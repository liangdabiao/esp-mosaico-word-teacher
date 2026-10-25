// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "wt_model.h"
#include "esp_err.h"
enum { WT_HISTORY_CAP=WT_TEXT_CAP*2+WT_VOICE_TEXT_CAP*2+512 };
typedef struct wt_conversation_t *wt_conversation_handle_t;
// Worker-only owner of the bounded first/latest Q&A context that keeps
// teacher follow-ups anchored to the word being studied.
esp_err_t wt_conversation_create(wt_conversation_handle_t *out);
void wt_conversation_delete(wt_conversation_handle_t c);
void wt_conversation_reset(wt_conversation_handle_t c);
// Anchor the context to a word (no-op text clears the anchor).
void wt_conversation_select(wt_conversation_handle_t c,const char *selection);
void wt_conversation_answer(wt_conversation_handle_t c,const char *question,const char *answer);
void wt_conversation_history(wt_conversation_handle_t c,char *out,size_t cap);
