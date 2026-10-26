// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "wt_controls.h"
#include <stdbool.h>
#include <stdint.h>
/* 全部跨模型钩子用 opaque 指针（真类型由先 include wt_model.h 的 .c 保证），
 * 打破 wt_model.h ↔ wt_platform.h 的循环包含。 */
void wt_platform_controls_get(wt_controls_t *out);
bool wt_platform_controls_set(wt_control_t control,int value);
// Submit must never block the render task. Messages and commands are copied.
bool wt_platform_submit(const void *command);
bool wt_platform_poll(void *message);
bool wt_platform_key(void);
bool wt_platform_simulated(void);
// Optional platform diagnostics; device implementation performs no work.
void wt_platform_presented(const void *snapshot);
// Content: the verified wordbook blob, or NULL when unavailable. The pointer
// is valid for the process lifetime (device: rodata; host: malloc'd file).
const void *wt_platform_content(void);
// Progress persistence (UI-task callers only mark dirty; worker writes).
bool wt_platform_progress_load(const char *book_key,uint32_t word_count,uint8_t *bits);
bool wt_platform_progress_save(const char *book_key,const uint8_t *bits,uint32_t word_count);
bool wt_platform_resume_load(const void *blob,void *out);
void wt_platform_resume_save(const void *resume);
// UI 任务把脏进度（wt_save_msg_t）交给平台（worker 队列）落盘。
void wt_platform_progress_submit(const void *save);
// Study streak: touch today's check-in and return the consecutive day count.
// Worker-task only (NVS write; never call from the render task).
unsigned wt_platform_streak_touch(void);
// UI 任务把脏进度交给平台（worker 队列）。队列满时丢弃——下次脏标记会再来。
void wt_platform_progress_submit(const void *save);
