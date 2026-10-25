// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// 发音的本地缓存（/nand LittleFS）。听过一次就留一份；再次播放同一内容
// 命中缓存就完全不联网，离线也能重播。缓存内容是原始 MP3 字节。
bool wt_cache_init(void);
bool wt_cache_ready(void);
bool wt_cache_path(const char *key, char *out, size_t capacity);
bool wt_cache_has(const char *key);
void wt_cache_touch(const char *path);
void wt_cache_remove(const char *path);
// 一次下载对应一个 writer。begin 创建 <hash>.mp3.tmp；feed 可反复调用；
// commit 只在整段解码成功时调用（fflush + rename）；abort 删除临时文件。
// 任何一步失败都不影响播放，只是这次不进缓存。
bool wt_cache_writer_begin(const char *key);
void wt_cache_writer_feed(const void *data, size_t len);
bool wt_cache_writer_commit(void);
void wt_cache_writer_abort(void);
