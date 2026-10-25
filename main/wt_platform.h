// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "wt_model.h"
#include "wt_controls.h"
#include "wt_progress.h"
void wt_platform_controls_get(wt_controls_t *out);
bool wt_platform_controls_set(wt_control_t control,int value);
// Submit must never block the render task. Messages and commands are copied.
bool wt_platform_submit(const wt_command_t *command);
bool wt_platform_poll(wt_message_t *message);
bool wt_platform_key(void);
bool wt_platform_simulated(void);
// Optional platform diagnostics; device implementation performs no work.
void wt_platform_presented(const wt_snapshot_t *snapshot);
// Content: the verified wordbook blob, or NULL when unavailable. The pointer
// is valid for the process lifetime (device: rodata; host: malloc'd file).
const wt_blob_t *wt_platform_content(void);
// Progress persistence. Called from the model (UI task); implementations must
// not block for long. Missing storage degrades to in-memory-only progress.
bool wt_platform_progress_load(const char *book_key,uint32_t word_count,uint8_t *bits);
bool wt_platform_progress_save(const char *book_key,const uint8_t *bits,uint32_t word_count);
bool wt_platform_resume_load(const wt_blob_t *blob,wt_resume_t *out);
void wt_platform_resume_save(const wt_resume_t *resume);
// Study streak: touch today's check-in and return the consecutive day count.
unsigned wt_platform_streak_touch(void);
