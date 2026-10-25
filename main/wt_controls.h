// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
enum { WT_GRADE_MIN=1, WT_GRADE_MAX=12, WT_GRADE_DEFAULT=7 };
typedef struct { unsigned brightness,volume,fixed_rotation,grade;bool speech_enabled,automatic,accent; } wt_controls_t;
typedef enum { WT_CONTROL_BRIGHTNESS,WT_CONTROL_VOLUME,WT_CONTROL_SPEECH,WT_CONTROL_AUTO,WT_CONTROL_GRADE,WT_CONTROL_ACCENT } wt_control_t;
const char *wt_grade_name(unsigned grade);
wt_controls_t wt_controls_defaults(void);
bool wt_controls_change(wt_controls_t *v,wt_control_t control,int value,unsigned rotation);
uint32_t wt_controls_pack(const wt_controls_t *v);
bool wt_controls_unpack(uint32_t packed,wt_controls_t *v);
