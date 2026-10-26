// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// wt_voice —— 单词/例句发音（english-dict wd_audio 的 C 移植，方案 B 的免费通路）。
// 链路：缓存命中 → fread；未命中 → 有道 dictvoice HTTP 流
//   → 剥 ID3v2 → esp_mp3_dec(MP3) → 混单声道 → 低采样率插值升到 48k
//   → ES8311(48kHz)。边播边写缓存，整段成功才提交。
#include "wt_voice.h"
#include "bsp/esp_mosaico.h"
#include "bsp/audio.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_codec_dev.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "wt_cache.h"
#include "wt_platform.h"
#include <ctype.h>
#include <esp_err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "decoder/esp_audio_dec.h"
#include "decoder/esp_audio_dec_default.h"
#include "decoder/esp_audio_dec.h"
#include "decoder/impl/esp_mp3_dec.h"

static const char kTag[] = "wt_voice";
enum {
    kHttpTimeoutMs = 5000, kTotalBudgetMs = 30000, kStallMs = 5000,
    kInputSize = 2048, kOutputSize = 16384, kResampleSize = 49152,
    kPlaybackRate = 48000,
};
static const char kWordUrlUs[] = "http://dict.youdao.com/dictvoice?type=0&audio=";
static const char kWordUrlUk[] = "http://dict.youdao.com/dictvoice?type=1&audio=";
// 例句整句：有道 type=2 服务端 500（2026-10 实测，english-dict 文档同记录），
// 整句改走百度 gettts（16kHz MP3，播放器 3 倍升采到 48k）。
static const char kSentenceUrl[] = "https://fanyi.baidu.com/gettts?lan=en&spd=3&source=web&text=";
struct wt_voice_t {
    esp_codec_dev_handle_t codec;
    bool codec_open;
    int applied_volume;
    uint8_t *input, *output, *resample;
    uint32_t generation;
    // MP3 解码器开机常驻：运行中期内部堆碎片化后再 open 会 NO_MEM。
    void *decoder;   // esp_mp3_dec_* 底层句柄（绕开注册表，注册表里没有 MP3）
};
static struct wt_voice_t voice;
static bool still_current(uint32_t generation)
{ return voice.generation == generation; }
static uint32_t now_ms(void)
{ return (uint32_t)(esp_timer_get_time() / 1000); }
static void *open_mp3_decoder(void)
{
    if (voice.decoder) return voice.decoder;
    if (esp_mp3_dec_open(NULL, 0, &voice.decoder) != ESP_AUDIO_ERR_OK) {
        voice.decoder = NULL;
        ESP_LOGE(kTag, "MP3 解码器打开失败");
    }
    return voice.decoder;
}
// 句柄由 esp_mp3_dec_open 创建，必须配套使用 MP3 直接接口。
// 每段流结束后 reset，保留启动早期分配的常驻句柄，避免后续堆碎片化导致 NO_MEM。
static void reset_mp3_decoder(void *decoder)
{
    if (!decoder) return;
    if (esp_mp3_dec_reset(decoder) == ESP_AUDIO_ERR_OK) return;
    ESP_LOGW(kTag, "MP3 解码器复位失败，重新创建");
    esp_mp3_dec_close(decoder);
    if (voice.decoder == decoder) voice.decoder = NULL;
}
esp_err_t wt_voice_start(void)
{
    if (voice.codec_open) return ESP_OK;
    memset(&voice, 0, sizeof(voice));
    voice.input = heap_caps_malloc(kInputSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    voice.output = heap_caps_malloc(kOutputSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    voice.resample = heap_caps_malloc(kResampleSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!voice.input || !voice.output || !voice.resample) return ESP_ERR_NO_MEM;
    voice.codec = bsp_audio_codec_speaker_init();
    if (!voice.codec) return ESP_ERR_INVALID_STATE;
    esp_codec_dev_sample_info_t format = {
        .sample_rate = kPlaybackRate, .bits_per_sample = 16, .channel = 1};
    if (esp_codec_dev_open(voice.codec, &format) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    wt_controls_t controls; wt_platform_controls_get(&controls);
    if (esp_codec_dev_set_out_vol(voice.codec, (int)controls.volume) != ESP_CODEC_DEV_OK)
        return ESP_FAIL;
    voice.applied_volume = (int)controls.volume;
    voice.codec_open = true;
    // 注册解码器（MP3 不在 simple_dec 默认集里；english-dict 同款两连调，
    // 漏掉它 = "Decoder MP3 not registered"，发音永远失败）。
    {
        esp_audio_err_t a=esp_audio_dec_register_default();
        esp_audio_err_t b=ESP_AUDIO_ERR_OK; /* simple 默认集无 MP3，跳过 */
        esp_audio_err_t m=esp_mp3_dec_register();
        // simple_dec_open 查的是 common 注册表；把 MP3 ops 显式挂入
        // （esp_mp3_dec_register 只填它自己的内部表，两表不互通）。
        esp_audio_dec_ops_t mp3_ops=ESP_MP3_DEC_DEFAULT_OPS();
        esp_audio_err_t g=esp_audio_dec_register(ESP_AUDIO_TYPE_MP3,&mp3_ops);
        ESP_LOGW(kTag,"dec reg: default=%d simple=%d mp3=%d common=%d",(int)a,(int)b,(int)m,(int)g);
    }
    // 启动早期（堆未碎片化）打开 MP3 解码器并常驻。
    if (!open_mp3_decoder())ESP_LOGW(kTag, "发音解码器不可用，稍后重试打开");
    return ESP_OK;
}
void wt_voice_set_generation(uint32_t generation)
{ voice.generation = generation; }
void wt_voice_stop(void)
{ voice.generation++; }
// 每次流 open 都会把 codec 寄存器复位：曲线与音量必须在 open 之后重装
// （对齐 english-dict 实测曲线；无曲线时 set_out_vol 落在硬件极低档且 PA 静音）。
static bool audio_set_format(uint32_t rate)
{
    esp_codec_dev_sample_info_t format = {
        .sample_rate = rate, .bits_per_sample = 16, .channel = 1};
    if (esp_codec_dev_open(voice.codec, &format) != ESP_CODEC_DEV_OK) return false;
    static const uint8_t kCurveVol[6] = {0, 20, 40, 60, 80, 100};
    static const float kCurveDb[6] = {-40.0f, -26.0f, -14.0f, -6.0f, 0.0f, 6.0f};
    esp_codec_dev_vol_map_t map[6] = {};
    for (int i = 0; i < 6; ++i) { map[i].vol = kCurveVol[i]; map[i].db_value = kCurveDb[i]; }
    esp_codec_dev_vol_curve_t curve = {.vol_map = map, .count = 6};
    if (esp_codec_dev_set_vol_curve(voice.codec, &curve) != ESP_CODEC_DEV_OK)
        ESP_LOGW(kTag, "音量曲线装不上，沿用默认直线");
    esp_codec_dev_set_out_vol(voice.codec, voice.applied_volume);
    return true;
}
static bool audio_write(const uint8_t *data, size_t bytes)
{
    return voice.codec && esp_codec_dev_write(voice.codec, (void *)data, (int)bytes) ==
        ESP_CODEC_DEV_OK;
}
static size_t downmix_to_mono(uint8_t *pcm, size_t bytes, uint8_t channels)
{
    if (!pcm || channels == 0) return 0;
    if (channels == 1) return bytes & ~(size_t)1;
    int16_t *samples = (int16_t *)pcm;
    size_t frames = bytes / (sizeof(int16_t) * channels);
    for (size_t frame = 0; frame < frames; ++frame)
        samples[frame] = (int16_t)(((int32_t)samples[frame * channels] +
            samples[frame * channels + 1]) / 2);
    return frames * sizeof(int16_t);
}
// 线性插值升采。factor = kPlaybackRate / source_rate，仅整数倍（2 或 3）。
static size_t upsample_nx(const uint8_t *pcm, size_t bytes, uint8_t *out, unsigned factor)
{
    const int16_t *src = (const int16_t *)pcm;
    int16_t *dst = (int16_t *)out;
    size_t count = bytes / sizeof(int16_t);
    for (size_t i = 0; i < count; ++i) {
        int32_t a = src[i], b = (i + 1 < count) ? src[i + 1] : a;
        for (unsigned step = 0; step < factor; ++step)
            dst[i * factor + step] =
                (int16_t)(a + ((int32_t)(b - a) * (int32_t)step) / (int32_t)factor);
    }
    return count * factor * sizeof(int16_t);
}
// 解析 ID3v2 头返回要跳过的字节数（含头）；不是 ID3 返回 0。
static size_t parse_id3_skip(const uint8_t *head, size_t len)
{
    if (len < 10 || memcmp(head, "ID3", 3)) return 0;
    size_t size = ((size_t)(head[6] & 0x7f) << 21) | ((size_t)(head[7] & 0x7f) << 14) |
        ((size_t)(head[8] & 0x7f) << 7) | (size_t)(head[9] & 0x7f);
    return size + 10;
}
static void url_encode(const char *text, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        bool safe = isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        int n = safe ? snprintf(out + used, cap - used, "%c", *p)
                     : snprintf(out + used, cap - used, "%%%c%c",
                                hex[*p >> 4], hex[*p & 15]);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
    }
    out[used] = 0;
}
esp_err_t wt_voice_build_url(bool sentence, const char *text, char *out, size_t cap)
{
    const char *base;
    wt_controls_t controls; wt_platform_controls_get(&controls);
    if (sentence) base = kSentenceUrl;
    else base = controls.accent ? kWordUrlUk : kWordUrlUs;
    char encoded[512]; url_encode(text, encoded, sizeof(encoded));
    int n = snprintf(out, cap, "%s%s", base, encoded);
    return n < 0 || (size_t)n >= cap ? ESP_ERR_INVALID_SIZE : ESP_OK;
}
struct outcome { bool ok, cancelled; uint32_t bytes, pcm_bytes; const char *detail; };
// 主循环：读 → 剥 ID3 → 解码 → 混单声道 → 升采 → 写 I2S，边播边喂缓存 writer。
// cache_source 非空 = 缓存回放（client 为 NULL）；否则走网络。
static struct outcome stream_and_play(esp_http_client_handle_t client,
                                      void *decoder,
                                      uint32_t generation, FILE *cache_source,
                                      bool cache_write)
{
    struct outcome o = {false, false, 0, 0, "unknown"};
    bool format_ready = false, upsample = false, played = false, read_ended = false;
    uint8_t channels = 1;
    uint32_t sample_rate = 0;
    unsigned decode_failures = 0, write_failures = 0;
    size_t carried = 0;
    const uint32_t deadline = now_ms() + kTotalBudgetMs;
    uint32_t last_progress = now_ms();
    // 剥 ID3v2 头：先读 10 字节算跳过量，已进缓冲的部分就地前移。
    if (!cache_source) {
        size_t head_len = 0;
        while (head_len < 10 && still_current(generation)) {
            int got = esp_http_client_read(client, (char *)voice.input + head_len,
                                           (int)(10 - head_len));
            if (got <= 0) break;
            head_len += (size_t)got;
        }
        o.bytes += (uint32_t)head_len;
        size_t skip = head_len == 10 ? parse_id3_skip(voice.input, 10) : 0;
        size_t pending = head_len;
        if (skip >= pending) {
            skip -= pending; pending = 0;
            uint8_t trash[64];
            while (skip > 0 && still_current(generation)) {
                size_t want = skip < sizeof(trash) ? skip : sizeof(trash);
                int got = esp_http_client_read(client, (char *)trash, (int)want);
                if (got <= 0) break;
                size_t took = (size_t)got < skip ? (size_t)got : skip;
                skip -= took; o.bytes += (uint32_t)took;
            }
        } else {
            pending -= skip;
            memmove(voice.input, voice.input + skip, pending);
        }
        carried = pending;
        // 无 ID3v2 时，首 10 字节也是 MP3 数据；必须与后续网络字节一起写入缓存。
        if (cache_write && carried > 0) wt_cache_writer_feed(voice.input, carried);
        if (skip > 0) ESP_LOGD(kTag, "跳过 ID3v2 头 %u 字节", (unsigned)skip);
    }
    while (still_current(generation)) {
        const uint32_t now = now_ms();
        if ((int32_t)(deadline - now) <= 0) { o.detail = "budget"; return o; }
        if (now - last_progress >= kStallMs) {
            o.detail = played ? "stalled" : "no-audio"; return o;
        }
        if (carried >= kInputSize) { o.detail = "decoder-stuck"; return o; }
        int got;
        if (cache_source) {
            got = (int)fread(voice.input + carried, 1, kInputSize - carried, cache_source);
            if (got == 0 && ferror(cache_source)) got = -1;
        } else {
            got = esp_http_client_read(client, (char *)voice.input + carried,
                                       (int)(kInputSize - carried));
            if (got > 0 && cache_write) wt_cache_writer_feed(voice.input + carried, (size_t)got);
        }
        if (got < 0) { o.detail = "read"; return o; }
        if (got == 0) read_ended = true;
        else { carried += (size_t)got; o.bytes += (uint32_t)got; last_progress = now_ms(); }
        esp_audio_dec_in_raw_t raw = {
            .buffer = voice.input, .len = (uint32_t)carried, .consumed = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE};
        while (raw.len > 0 && still_current(generation)) {
            esp_audio_dec_out_frame_t frame = {
                .buffer = voice.output, .len = kOutputSize,
                .needed_size = 0, .decoded_size = 0};
            const uint32_t before = raw.len;
            esp_audio_dec_info_t dec_info = {};
            const esp_audio_err_t result = esp_mp3_dec_decode(decoder, &raw, &frame, &dec_info);
            if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) { o.detail = "decoder-buffer"; return o; }
            if (result != ESP_AUDIO_ERR_OK) {
                // 偶发坏帧放弃这一批重新同步；坏帧太多说明流本身不对。
                if (++decode_failures > 32) { o.detail = "decode"; return o; }
                raw.len = 0; break;
            }
            if (raw.consumed > raw.len) raw.consumed = raw.len;
            raw.buffer += raw.consumed; raw.len -= raw.consumed;
            if (frame.decoded_size > 0) {
                if (!format_ready && dec_info.sample_rate) {
                    channels = dec_info.channel < 1 ? 1 :
                        (dec_info.channel > 2 ? 2 : (uint8_t)dec_info.channel);
                    sample_rate = dec_info.sample_rate;
                    upsample = sample_rate < kPlaybackRate &&
                        (kPlaybackRate % sample_rate) == 0;
                    const uint32_t play_rate = upsample ? kPlaybackRate : sample_rate;
                    if (dec_info.bits_per_sample != 16 || sample_rate < 8000 ||
                        sample_rate > kPlaybackRate || !audio_set_format(play_rate)) {
                        ESP_LOGE(kTag, "不支持的流格式: %luHz/%ubit/%uch",
                            (unsigned long)sample_rate, dec_info.bits_per_sample,
                            dec_info.channel);
                        o.detail = "format"; return o;
                    }
                    format_ready = true;
                    ESP_LOGI(kTag, "%luHz/%uch -> 单声道%s",
                        (unsigned long)sample_rate, channels,
                        upsample ? "，插值升采到 48000Hz" : "");
                }
                if (format_ready) {
                    const size_t mono = downmix_to_mono(frame.buffer,
                                                        frame.decoded_size, channels);
                    const uint8_t *play = frame.buffer;
                    size_t play_bytes = mono;
                    if (upsample) {
                        play_bytes = upsample_nx(frame.buffer, mono, voice.resample,
                                                 kPlaybackRate / sample_rate);
                        play = voice.resample;
                    }
                    if (!audio_write(play, play_bytes)) {
                        // 写失败必须留痕：真机上"状态在播、喇叭没声"最难查。
                        if ((++write_failures & 0x07u) == 1)
                            ESP_LOGW(kTag, "写 I2S 失败 %u 次（本次 %u 字节）",
                                write_failures, (unsigned)play_bytes);
                        continue;
                    }
                    o.pcm_bytes += (uint32_t)play_bytes;
                    last_progress = now_ms();
                    if (!played) {
                        played = true;
                        ESP_LOGI(kTag, "首帧 PCM 已写入 I2S（%u 字节）", (unsigned)play_bytes);
                    }
                }
            }
            if (raw.len == before || raw.consumed == 0) break;
        }
        carried = raw.len;
        if (carried > 0 && raw.buffer != voice.input)
            memmove(voice.input, raw.buffer, carried);
        if (read_ended) break;
    }
    if (!still_current(generation)) { o.cancelled = true; o.detail = "cancelled"; return o; }
    // 收尾：让解码器把内部残留吐出来，否则末尾丢一帧（尾音被切）。
    if (format_ready) {
        esp_audio_dec_in_raw_t tail = {};
        tail.buffer = voice.input; tail.len = 0;
        tail.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;
        esp_audio_dec_info_t dec_info = {};
        for (unsigned guard = 0; guard < 32 && still_current(generation); ++guard) {
            esp_audio_dec_out_frame_t frame = {};
            frame.buffer = voice.output; frame.len = kOutputSize;
            const esp_audio_err_t r = esp_mp3_dec_decode(decoder, &tail, &frame, &dec_info);
            if (r != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) break;
            const size_t mono = downmix_to_mono(frame.buffer, frame.decoded_size, channels);
            size_t written = 0;
            if (upsample) {
                written = upsample_nx(frame.buffer, mono, voice.resample,
                                      kPlaybackRate / sample_rate);
                if (audio_write(voice.resample, written)) o.pcm_bytes += (uint32_t)written;
            } else if (audio_write(frame.buffer, mono)) {
                o.pcm_bytes += (uint32_t)mono;
            }
        }
    }
    if (!played) { o.detail = "no-audio"; return o; }
    if (!read_ended) { o.cancelled = true; o.detail = "cancelled"; return o; }
    o.ok = true; o.detail = "ok"; return o;
}
// 缓存命中走文件；未命中走网络并边播边写缓存。整段成功才提交缓存。
// 单次尝试；play_key 负责弱网重试。
static esp_err_t play_key_once(const char *key, const char *url)
{
    esp_http_client_handle_t client = NULL;
    void *decoder = open_mp3_decoder();
    FILE *cache_source = NULL;
    bool cache_write = false;
    if (!decoder) return ESP_ERR_NO_MEM;
    if (wt_cache_ready()) {
        char path[96];
        if (wt_cache_has(key) && wt_cache_path(key, path, sizeof(path)))
            cache_source = fopen(path, "rb");
        if (cache_source) ESP_LOGI(kTag, "缓存命中: %s", path);
    }
    if (!cache_source) {
        esp_http_client_config_t config = {};
        config.url = url; config.timeout_ms = kHttpTimeoutMs;
        config.buffer_size = 1024; config.buffer_size_tx = 512;
        config.user_agent = "word-teacher/1.0.0";
        config.keep_alive_enable = false;
        config.disable_auto_redirect = false; config.max_redirection_count = 3;
        // 明文接口挂证书包无害；若哪天强制跳 https 也有校验可用。
        config.crt_bundle_attach = esp_crt_bundle_attach;
        client = esp_http_client_init(&config);
        if (!client) { reset_mp3_decoder(decoder); return ESP_ERR_NO_MEM; }
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGW(kTag, "连接失败: %s", url);
            esp_http_client_cleanup(client);
            reset_mp3_decoder(decoder);
            return ESP_FAIL;
        }
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            // 404 有实际意义：词条没有音标的词多半也拿不到发音，静默处理。
            ESP_LOGW(kTag, "HTTP %d for %s", status, url);
            esp_http_client_close(client); esp_http_client_cleanup(client);
            reset_mp3_decoder(decoder);
            return status == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        }
        // 边播边写缓存：整段解码成功才 rename 提交，断流/取消就删临时文件。
        cache_write = wt_cache_writer_begin(key);
    }
    const uint32_t generation = voice.generation;
    struct outcome o = cache_source
        ? stream_and_play(NULL, decoder, generation, cache_source, false)
        : stream_and_play(client, decoder, generation, NULL, cache_write);
    reset_mp3_decoder(decoder);
    if (cache_source) {
        fclose(cache_source);
        if (!o.ok && !o.cancelled) {
            char path[96];
            if (wt_cache_path(key, path, sizeof(path))) {
                wt_cache_remove(path);
                ESP_LOGW(kTag, "缓存播放失败，已删除坏缓存，重试将走网络");
            }
        }
    }
    if (o.ok && cache_write) wt_cache_writer_commit();
    else wt_cache_writer_abort();
    if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
    ESP_LOGI(kTag, "播放完成: ok=%u bytes=%u pcm=%u detail=%s",
        o.ok, o.bytes, o.pcm_bytes, o.detail);
    return o.ok || o.cancelled ? ESP_OK : ESP_FAIL;
}
// 弱网（读超时/连接抖动）自动重试一次；GET 下载体积小，重发无害。
static esp_err_t play_key(const char *key, const char *url)
{
    esp_err_t err = play_key_once(key, url);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "发音重试一次");
        err = play_key_once(key, url);
    }
    return err;
}
esp_err_t wt_voice_speak_word(const char *word)
{
    if (!voice.codec_open || !word || !word[0]) return ESP_ERR_INVALID_STATE;
    char url[640];
    esp_err_t err = wt_voice_build_url(false, word, url, sizeof(url));
    if (err != ESP_OK) return err;
    return play_key(word, url);
}
esp_err_t wt_voice_speak_sentence(const char *sentence)
{
    if (!voice.codec_open || !sentence || !sentence[0]) return ESP_ERR_INVALID_STATE;
    char key[128], url[1024];
    snprintf(key, sizeof(key), "s:%s", sentence);
    esp_err_t err = wt_voice_build_url(true, sentence, url, sizeof(url));
    if (err != ESP_OK) return err;
    return play_key(key, url);
}
// 播放一段已解码 PCM（16 位单声道，rate 可为 16000/24000/48000）。
// 低采样率就地插值升到 48k（ES8311 16k 直播失真），统一 48kHz 通路。
esp_err_t wt_voice_play_pcm(const uint8_t *pcm, size_t bytes, unsigned rate)
{
    if (!voice.codec_open || !pcm || !bytes) return ESP_ERR_INVALID_STATE;
    if (rate != kPlaybackRate && (rate == 0 || kPlaybackRate % rate))
        return ESP_ERR_NOT_SUPPORTED;
    const unsigned factor = kPlaybackRate / rate;
    if (bytes & 1) bytes &= ~(size_t)1;
    if (factor == 1) return audio_write(pcm, bytes) ? ESP_OK : ESP_FAIL;
    if (bytes * factor > kResampleSize) return ESP_ERR_INVALID_SIZE;
    const size_t out = upsample_nx(pcm, bytes, voice.resample, factor);
    return audio_write(voice.resample, out) ? ESP_OK : ESP_FAIL;
}
bool wt_voice_ready(void) { return voice.codec_open; }
void wt_voice_apply_volume(int volume)
{
    if (voice.codec_open && volume != voice.applied_volume &&
        esp_codec_dev_set_out_vol(voice.codec, volume) == ESP_CODEC_DEV_OK)
        voice.applied_volume = volume;
}
