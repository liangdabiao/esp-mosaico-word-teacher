// main/wt_store.c —— 见 wt_store.h。
//
// 为什么每次都 nvs_open / nvs_close，而不是持有长句柄：NVS 句柄持有的是
// 分区与页缓存，长期持有会在擦除/OTA 时挡住别人。这里的调用频率是
// "翻几十个词一次"，开一次的开销完全可以忽略。
#include "wt_store.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"

#define TAG "wt_store"

// 命名空间名。7 个字符，远在 15 的上限内。
#define WT_NVS_NAMESPACE "wdstore"

// 内容版本的键。见头文件里对 cver 的说明。
#define WT_KEY_CONTENT_VERSION "cver"
#define WT_KEY_RESUME "resume"
#define WT_KEY_VOLUME "volume"
#define WT_KEY_BOOK "book"
#define WT_KEY_STREAK "wtstreak"
#define WT_KEY_STREAK_DAY "wtstday"

// 位图键的前缀。长度写死在编译期断言里（见 wt_manifest.h）。
#define WT_BITMAP_PREFIX "wdp."

// 默认最大音量：真机上 ES8311 的 BSP 增益是 -3.6 dB，旧默认 80% 实测偏小。
// 用户手动调过的值仍以 NVS 为准。
#define WT_DEFAULT_VOLUME 100

// ---------------------------------------------------------------------------
// 小工具：开 / 关，键名组装
// ---------------------------------------------------------------------------

static bool open_read(nvs_handle_t *handle) {
    return nvs_open(WT_NVS_NAMESPACE, NVS_READONLY, handle) == ESP_OK;
}

static bool open_write(nvs_handle_t *handle) {
    return nvs_open(WT_NVS_NAMESPACE, NVS_READWRITE, handle) == ESP_OK;
}

// 组装位图键名。book_key 来自已校验的 blob（wt_manifest 已保证是
// 0x21..0x7e 的可见 ASCII 且长度受限），所以这里只做一次防御性长度检查 ——
// 万一哪天有人从别处喂进来一个更长的 key，写 NVS 会静默失败，
// 宁可在这里就拒绝并留一行日志。
static bool bitmap_key(const char *book_key, char *out, size_t capacity) {
    if (!book_key || !book_key[0]) return false;
    const size_t key_len = strlen(book_key);
    if (key_len > WT_BOOK_KEY_MAX - 1u) {
        ESP_LOGW(TAG, "书 key 太长（%u 字符），无法装进 NVS 键名：%s",
                 (unsigned)key_len, book_key);
        return false;
    }
    const int written = snprintf(out, capacity, WT_BITMAP_PREFIX "%s", book_key);
    if (written < 0 || (size_t)written >= capacity) return false;
    return true;
}

static uint32_t bitmap_bytes(uint32_t word_count) {
    return wt_progress_bitmap_bytes(word_count);
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------

bool wt_store_init(uint32_t content_version) {
    nvs_handle_t handle = 0;
    if (!open_write(&handle)) {
        ESP_LOGW(TAG, "NVS 命名空间打不开 —— 本次运行不会记住进度");
        return false;
    }

    uint32_t stored_version = 0;
    const esp_err_t read_result =
        nvs_get_u32(handle, WT_KEY_CONTENT_VERSION, &stored_version);

    if (read_result == ESP_OK && stored_version == content_version) {
        nvs_close(handle);
        ESP_LOGI(TAG, "进度可用（内容版本 %lu）", (unsigned long)content_version);
        return true;
    }

    if (read_result != ESP_OK && read_result != ESP_ERR_NVS_NOT_FOUND) {
        // 读失败不是"没存过"，不该顺手把数据清了 —— 先报清楚再继续。
        ESP_LOGW(TAG, "读内容版本失败: %s", esp_err_to_name(read_result));
    }

    // 走到这里只有两种情况：① 第一次启动（没存过）② 词库换过（版本不符）。
    // 两种情况都要把旧数据清掉：① 无所谓，② 是必须 ——
    // 旧位图指在新词表上会给出一个"看起来正常、其实错位"的进度，
    // 那比"进度清零"糟糕得多（用户会以为自己在复习，其实在背别人的词）。
    if (read_result == ESP_OK) {
        ESP_LOGW(TAG, "内容版本变了（%lu -> %lu），清空旧进度",
                 (unsigned long)stored_version, (unsigned long)content_version);
        nvs_erase_all(handle);
    }

    if (nvs_set_u32(handle, WT_KEY_CONTENT_VERSION, content_version) != ESP_OK) {
        nvs_close(handle);
        ESP_LOGW(TAG, "写内容版本失败 —— 本次运行不会记住进度");
        return false;
    }
    const esp_err_t committed = nvs_commit(handle);
    nvs_close(handle);
    if (committed != ESP_OK) {
        ESP_LOGW(TAG, "提交内容版本失败: %s", esp_err_to_name(committed));
        return false;
    }
    ESP_LOGI(TAG, "进度初始化完成（内容版本 %lu）", (unsigned long)content_version);
    return true;
}

// ---------------------------------------------------------------------------
// 掌握位图
// ---------------------------------------------------------------------------

bool wt_store_load_bitmap(const char *book_key, uint32_t word_count, uint8_t *bits) {
    if (!bits || word_count == 0) return false;
    const uint32_t expected = bitmap_bytes(word_count);
    if (!expected) return false;

    char key[WT_BOOK_KEY_MAX + 4];
    if (!bitmap_key(book_key, key, sizeof(key))) return false;

    nvs_handle_t handle = 0;
    if (!open_read(&handle)) return false;

    size_t length = expected;
    const esp_err_t result = nvs_get_blob(handle, key, bits, &length);
    nvs_close(handle);

    // 长度不符 = 这本书的词数变了（换了词库版本）。cver 已经在 init 里
    // 把这种情况挡在前面了，这里再判一次是因为 cver 能挡住"整库换版本"，
    // 挡不住"某一本书的词数被单独改过"。
    if (result != ESP_OK) return false;
    if (length != expected) {
        ESP_LOGW(TAG, "位图长度不符：期望 %u，实得 %u（%s）",
                 (unsigned)expected, (unsigned)length, book_key);
        return false;
    }
    return true;
}

bool wt_store_save_bitmap(const char *book_key, const uint8_t *bits,
                          uint32_t word_count) {
    if (!bits || word_count == 0) return false;
    const uint32_t bytes = bitmap_bytes(word_count);
    if (!bytes) return false;

    char key[WT_BOOK_KEY_MAX + 4];
    if (!bitmap_key(book_key, key, sizeof(key))) return false;

    nvs_handle_t handle = 0;
    if (!open_write(&handle)) return false;

    esp_err_t result = nvs_set_blob(handle, key, bits, bytes);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);

    if (result != ESP_OK) {
        // 写失败一定要说：这是"进度丢了但界面看起来正常"的唯一线索。
        ESP_LOGW(TAG, "写位图失败（%s, %u 字节）：%s", book_key, (unsigned)bytes,
                 esp_err_to_name(result));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 断点
// ---------------------------------------------------------------------------

wt_resume_err_t wt_store_load_resume(const wt_blob_t *blob, wt_resume_t *out) {
    if (!blob || !out) return WT_RESUME_ERR_NULL;

    nvs_handle_t handle = 0;
    // 打不开/没有记录都等价于"从没存过断点"，用 MAGIC 表示；
    // 若这里返回 SHORT，全新设备每次开机都会报一条吓人的"记录长度不足"。
    if (!open_read(&handle)) return WT_RESUME_ERR_MAGIC;

    uint8_t frame[WT_RESUME_BYTES];
    size_t length = sizeof(frame);
    const esp_err_t result = nvs_get_blob(handle, WT_KEY_RESUME, frame, &length);
    nvs_close(handle);

    // 长度不符说明是旧版布局存的（WT_RESUME_FORMAT 变了）。不猜、不迁移 ——
    // 一个"猜出来"的断点会把用户丢到一个他没去过的词上。
    if (result == ESP_ERR_NVS_NOT_FOUND) return WT_RESUME_ERR_MAGIC;
    if (result != ESP_OK || length != WT_RESUME_BYTES) return WT_RESUME_ERR_SHORT;

    // 校验顺序（长度 → 魔数 → 布局版本 → 校验和 → 内容版本 → 范围）由
    // wt_resume_decode 负责，那是它存在的理由，这里不重复实现。
    return wt_resume_decode(frame, length, blob, out);
}

bool wt_store_save_resume(const wt_resume_t *resume) {
    if (!resume) return false;

    uint8_t frame[WT_RESUME_BYTES];
    const size_t written = wt_resume_encode(resume, frame, sizeof(frame));
    if (written != WT_RESUME_BYTES) return false;   // encode 保证不会写半个记录

    nvs_handle_t handle = 0;
    if (!open_write(&handle)) return false;

    esp_err_t result = nvs_set_blob(handle, WT_KEY_RESUME, frame, written);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "写断点失败：%s", esp_err_to_name(result));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 音量
// ---------------------------------------------------------------------------

uint8_t wt_store_load_volume(void) {
    nvs_handle_t handle = 0;
    if (!open_read(&handle)) return WT_DEFAULT_VOLUME;

    uint8_t volume = WT_DEFAULT_VOLUME;
    const esp_err_t result = nvs_get_u8(handle, WT_KEY_VOLUME, &volume);
    nvs_close(handle);

    if (result != ESP_OK) return WT_DEFAULT_VOLUME;
    // 夹一下：NVS 里可能留着上一版固件写的 0（静音）或 >100 的值。
    // 0 会让用户以为"喇叭坏了"，而这一版没有静音这个功能。
    if (volume < 10) return 10;
    if (volume > 100) return 100;
    return volume;
}

void wt_store_save_volume(uint8_t volume) {
    if (volume > 100) volume = 100;

    nvs_handle_t handle = 0;
    if (!open_write(&handle)) return;
    esp_err_t result = nvs_set_u8(handle, WT_KEY_VOLUME, volume);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "写音量失败：%s", esp_err_to_name(result));
    }
}

// ---------------------------------------------------------------------------
// 上次在学哪本书
// ---------------------------------------------------------------------------

bool wt_store_load_book(char *out, size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = '\0';

    nvs_handle_t handle = 0;
    if (!open_read(&handle)) return false;

    size_t length = capacity;
    const esp_err_t result = nvs_get_str(handle, WT_KEY_BOOK, out, &length);
    nvs_close(handle);

    if (result != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return out[0] != '\0';
}

void wt_store_save_book(const char *book_key) {
    if (!book_key || !book_key[0]) return;

    nvs_handle_t handle = 0;
    if (!open_write(&handle)) return;
    esp_err_t result = nvs_set_str(handle, WT_KEY_BOOK, book_key);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "写当前书失败：%s", esp_err_to_name(result));
    }
}

// ---------------------------------------------------------------------------
// 连续学习打卡。以"天"为粒度（本地时间，SNTP 已由网络模块对时）：
// 键里存"上次打卡的 epoch 天 + 当前连续天数"。同一天重复调用只返回现值；
// 昨天打过 → 连续 +1；再往前 → 重新从 1 计。数据坏了当 0 处理，不报错。
// ---------------------------------------------------------------------------
unsigned wt_store_touch_streak(void) {
    nvs_handle_t handle = 0;
    if (!open_read(&handle)) return 0;
    uint32_t last_day = 0, streak = 0;
    nvs_get_u32(handle, WT_KEY_STREAK_DAY, &last_day);
    nvs_get_u32(handle, WT_KEY_STREAK, &streak);
    nvs_close(handle);

    time_t now = time(NULL);
    if (now < 1700000000) {
        // 时钟还没同步：宁可不计这一天，也别把 1970 年存成断签。
        return (unsigned)streak;
    }
    const uint32_t today = (uint32_t)(now / 86400);
    if (today == last_day) return (unsigned)streak;
    uint32_t next = (last_day && today == last_day + 1) ? streak + 1 : 1;
    if (!open_write(&handle)) return (unsigned)streak;
    esp_err_t result = nvs_set_u32(handle, WT_KEY_STREAK_DAY, today);
    if (result == ESP_OK) result = nvs_set_u32(handle, WT_KEY_STREAK, next);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "写打卡失败：%s", esp_err_to_name(result));
        return (unsigned)streak;
    }
    return (unsigned)next;
}
