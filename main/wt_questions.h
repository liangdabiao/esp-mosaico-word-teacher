// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <stdint.h>
enum { WT_QUESTION_MAX=24, WT_QUESTION_ROWS=3,
       WT_QUESTION_NUMBER_CAP=49, WT_QUESTION_SUMMARY_CAP=97,
       WT_QUESTION_LOCATION_CAP=145 };
typedef struct {
    char number[WT_QUESTION_NUMBER_CAP];
    char summary[WT_QUESTION_SUMMARY_CAP];
    char location[WT_QUESTION_LOCATION_CAP];
} wt_question_t;
typedef struct {
    unsigned count;
    wt_question_t items[WT_QUESTION_MAX];
} wt_questions_t;
