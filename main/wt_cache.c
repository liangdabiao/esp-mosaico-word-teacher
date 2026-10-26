// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// wt_cache —— 发音缓存（english-dict wd_cache 的 C 移植，行为与限额一致）。
// SPI NAND → WL 块设备 → LittleFS(/nand)；文件名 = 64 位 FNV-1a，前两位做分片；
// 每片限额 400 KiB / 48 文件，超出按最旧淘汰。
#include "wt_cache.h"
#include "bsp/esp_mosaico.h"
#include "bsp/nand_flash.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "spi_nand_flash.h"
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>

static const char kTag[] = "wt_cache";
static const char kMountPath[] = "/nand";
static const char kCacheRoot[] = "/nand/wdvoice";
enum { kPathMax = 96, kShardBytes = 400 * 1024, kShardFiles = 48,
       kMaxFileBytes = 192 * 1024, kShards = 256 };
static bool s_init_done, s_mounted, s_bus_owned, s_device_added;
static esp_blockdev_handle_t s_blockdev;
static spi_device_handle_t s_spi;
static FILE *s_writer;
static char s_writer_final[kPathMax];
static atomic_bool s_writer_active;
struct cache_entry { char name[112]; long mtime; uint32_t bytes; };
static uint64_t fnv1a64(const char *text)
{
    uint64_t hash = 1469598103934665603ull;
    while (*text) { hash ^= (unsigned char)*text++; hash *= 1099511628211ull; }
    return hash;
}
// HOLD#/WP# 低有效：先把输出锁存器拉高，再开 pad，避免配置瞬间误触发。
static esp_err_t release_control_pin(gpio_num_t pin)
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << (unsigned)pin;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_set_level(pin, 1);
    if (err != ESP_OK) return err;
    err = gpio_config(&config);
    if (err != ESP_OK) return err;
    return gpio_set_level(pin, 1);
}
// 挂载失败也要释放已分配的 SPI 设备/总线/磨损均衡块设备，否则这些内部
// RAM 永久泄漏（english-dict wd_cache 的 nand_cleanup_partial 同款）。
static void nand_cleanup_partial(void)
{
    if (s_blockdev) {
        s_blockdev->ops->release(s_blockdev);
        s_blockdev = NULL;
    }
    if (s_device_added && s_spi) {
        (void)spi_bus_remove_device(s_spi);
    }
    s_spi = NULL;
    s_device_added = false;
    if (s_bus_owned) {
        (void)spi_bus_free(BSP_NAND_SPI_HOST);
    }
    s_bus_owned = false;
}
bool wt_cache_init(void)
{
    if (s_init_done) return s_mounted;
    s_init_done = true;
    esp_err_t err = release_control_pin(BSP_NAND_HOLD);
    if (err == ESP_OK) err = release_control_pin(BSP_NAND_WP);
    if (err != ESP_OK) { ESP_LOGW(kTag, "释放 NAND HOLD/WP 失败: %s", esp_err_to_name(err)); }
    spi_bus_config_t bus = {};
    bus.mosi_io_num = BSP_NAND_D; bus.miso_io_num = BSP_NAND_Q;
    bus.sclk_io_num = BSP_NAND_CLK;
    bus.quadwp_io_num = GPIO_NUM_NC; bus.quadhd_io_num = GPIO_NUM_NC;
    bus.max_transfer_sz = BSP_NAND_FLASH_DEFAULT_MAX_TRANSFER_SZ;
    err = spi_bus_initialize(BSP_NAND_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) ESP_LOGW(kTag, "SPI3 已初始化，沿用现有总线");
    else if (err != ESP_OK) { ESP_LOGW(kTag, "NAND SPI 总线初始化失败: %s", esp_err_to_name(err)); return false; }
    else s_bus_owned = true;
    spi_device_interface_config_t device = {};
    device.clock_speed_hz = BSP_NAND_FLASH_DEFAULT_CLOCK_HZ;
    device.mode = 0; device.spics_io_num = BSP_NAND_CS;
    device.queue_size = BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE;
    device.flags = SPI_DEVICE_HALFDUPLEX;
    if (spi_bus_add_device(BSP_NAND_SPI_HOST, &device, &s_spi) != ESP_OK) {
        ESP_LOGW(kTag, "NAND SPI 设备添加失败");
        nand_cleanup_partial(); return false;
    }
    s_device_added = true;
    spi_nand_flash_config_t nand = {};
    nand.device_handle = s_spi; nand.gc_factor = 0;
    nand.io_mode = SPI_NAND_IO_MODE_SIO; nand.flags = SPI_DEVICE_HALFDUPLEX;
    if (spi_nand_flash_init_with_layers(&nand, &s_blockdev) != ESP_OK) {
        ESP_LOGW(kTag, "NAND 初始化失败"); s_blockdev = NULL;
        nand_cleanup_partial(); return false;
    }
    esp_vfs_littlefs_conf_t mount = {};
    mount.base_path = kMountPath; mount.blockdev = s_blockdev;
    // 绝不自动格式化：卷坏了应在日志里暴露，而不是悄悄清空。
    mount.format_if_mount_failed = false; mount.read_only = false;
    mount.dont_mount = false; mount.grow_on_mount = false;
    if (esp_vfs_littlefs_register(&mount) != ESP_OK) {
        ESP_LOGW(kTag, "LittleFS 挂载失败（发音缓存禁用，仍可在线发音）");
        // register 失败时已消费并释放块设备；清掉 SPI 残留。
        s_blockdev = NULL;
        nand_cleanup_partial(); return false;
    }
    if (mkdir(kCacheRoot, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(kTag, "创建缓存目录失败: %s", strerror(errno)); return false;
    }
    s_mounted = true;
    ESP_LOGI(kTag, "NAND LittleFS 已挂载，缓存目录 %s", kCacheRoot);
    return true;
}
bool wt_cache_ready(void) { return s_mounted; }
static void shard_dir(const char *key, char *out, size_t cap)
{
    uint64_t hash = fnv1a64(key);
    snprintf(out, cap, "%s/%02llx", kCacheRoot, (unsigned long long)(hash & 0xff));
}
bool wt_cache_path(const char *key, char *out, size_t capacity)
{
    if (!key || !out || capacity < kPathMax) return false;
    char dir[kPathMax]; shard_dir(key, dir, sizeof(dir));
    snprintf(out, capacity, "%s/%016llx.mp3", dir, (unsigned long long)fnv1a64(key));
    return true;
}
bool wt_cache_has(const char *key)
{
    char path[kPathMax]; struct stat st;
    return wt_cache_path(key, path, sizeof(path)) && stat(path, &st) == 0 && st.st_size > 0;
}
void wt_cache_touch(const char *path)
{ utime(path, NULL); }
void wt_cache_remove(const char *path)
{ if (path[0]) remove(path); }
static int entry_compare(const void *lhs, const void *rhs)
{
    const struct cache_entry *a = lhs, *b = rhs;
    if (a->mtime != b->mtime) return a->mtime < b->mtime ? -1 : 1;
    return strcmp(a->name, b->name);
}
// 分片超限就按 mtime 淘汰最旧，直到回到限额之内。
static void prune_shard(const char *file_path)
{
    char dir[kPathMax + 8];
    snprintf(dir, sizeof(dir), "%s", file_path);
    char *slash = strrchr(dir, '/'); if (!slash) return;
    *slash = 0;
    DIR *handle = opendir(dir); if (!handle) return;
    struct cache_entry *entries = calloc(kShardFiles + 8, sizeof(*entries));
    if (!entries) { closedir(handle); return; }
    unsigned count = 0; uint32_t bytes = 0;
    struct dirent *item;
    while ((item = readdir(handle)) && count < kShardFiles + 8) {
        if (item->d_name[0] == '.') continue;
        struct cache_entry *e = &entries[count];
        size_t dl = strlen(dir), dn = strnlen(item->d_name, sizeof(item->d_name));
        if (dl + 1 + dn + 1 > sizeof(e->name)) continue;
        memcpy(e->name, dir, dl); e->name[dl] = '/';
        memcpy(e->name + dl + 1, item->d_name, dn + 1);
        struct stat st;
        if (stat(e->name, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        e->mtime = (long)st.st_mtime; e->bytes = (uint32_t)st.st_size;
        bytes += e->bytes; ++count;
    }
    closedir(handle);
    if (bytes <= kShardBytes && count <= kShardFiles) { free(entries); return; }
    qsort(entries, count, sizeof(*entries), entry_compare);
    for (unsigned i = 0; i < count && (bytes > kShardBytes || count > kShardFiles); ++i) {
        remove(entries[i].name); bytes -= entries[i].bytes; --count;
    }
    free(entries);
}
bool wt_cache_writer_begin(const char *key)
{
    if (!s_mounted || !key || !key[0] || atomic_load(&s_writer_active)) return false;
    char dir[kPathMax]; shard_dir(key, dir, sizeof(dir));
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) return false;
    if (!wt_cache_path(key, s_writer_final, sizeof(s_writer_final))) return false;
    char tmp[kPathMax + 5];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_writer_final);
    s_writer = fopen(tmp, "wb");
    if (!s_writer) return false;
    atomic_store(&s_writer_active, true);
    return true;
}
void wt_cache_writer_feed(const void *data, size_t len)
{
    if (!s_writer || !data || !len) return;
    if (fwrite(data, 1, len, s_writer) != len) { /* 写失败不致命，commit 时校验 */ }
}
bool wt_cache_writer_commit(void)
{
    if (!s_writer) return false;
    const bool ok = fflush(s_writer) == 0 && fclose(s_writer) == 0;
    s_writer = NULL;
    if (!ok) { wt_cache_writer_abort(); return false; }
    char tmp[kPathMax + 5];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_writer_final);
    if (rename(tmp, s_writer_final) != 0) { atomic_store(&s_writer_active, false); return false; }
    atomic_store(&s_writer_active, false);
    prune_shard(s_writer_final);
    return true;
}
void wt_cache_writer_abort(void)
{
    if (s_writer) { fclose(s_writer); s_writer = NULL; }
    if (atomic_exchange(&s_writer_active, false)) {
        char tmp[kPathMax + 5];
        snprintf(tmp, sizeof(tmp), "%s.tmp", s_writer_final);
        remove(tmp);
    }
}
