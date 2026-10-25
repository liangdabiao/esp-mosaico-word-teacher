// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { WT_TEXT_CAP = 8192, WT_LINE_CAP = 97, WT_LINES = 6,
       WT_VOICE_TEXT_CAP = 1024, WT_MAX_PAGES = 128, WT_MAX_RECORD_MS = 30000,
       WT_BOOKS = 6, WT_QUIZ_CHOICES = 4, WT_QUICK_COUNT = 4 };
// Field capacities follow the wordbook blob layout (wt_content.h): the five
// string fields are bounded by the generator; +1 for NUL.
enum { WT_WORD_CAP = 25, WT_IPA_CAP = 29, WT_TRANS_CAP = 65,
       WT_SENT_EN_CAP = 105, WT_SENT_CN_CAP = 40 };
typedef struct wt_model_t *wt_model_handle_t;
typedef enum { WT_HOME, WT_WORD, WT_READING, WT_VOICE, WT_WAITING,
    WT_SETTINGS, WT_ERROR, WT_VOICE_REVIEW, WT_QUIZ, WT_PROGRESS } wt_page_t;
typedef enum { WT_BOOK_0, WT_BOOK_1, WT_BOOK_2, WT_BOOK_3, WT_BOOK_4, WT_BOOK_5,
    WT_WORD_PREV, WT_WORD_NEXT, WT_SPEAK_WORD, WT_SPEAK_SENT, WT_TEACH,
    WT_QUICK_0, WT_QUICK_1, WT_QUICK_2, WT_QUICK_3, WT_MARK_KNOWN,
    WT_OPEN_QUIZ, WT_QUIZ_PICK_0, WT_QUIZ_PICK_1, WT_QUIZ_PICK_2, WT_QUIZ_PICK_3,
    WT_QUIZ_NEXT, WT_OPEN_PROGRESS, WT_RESET,
    WT_ASK, WT_KEY_DOWN, WT_KEY_UP, WT_BACK, WT_PREVIOUS,
    WT_NEXT, WT_EXPLAIN, WT_READ, WT_MUTE, WT_SETUP,
    WT_OPEN_SETTINGS, WT_CANCEL, WT_RETRY, WT_VOICE_TOGGLE, WT_SEND_TEXT } wt_action_t;
typedef enum { WT_CMD_NONE, WT_CMD_RECORD, WT_CMD_SEND_AUDIO,
    WT_CMD_CANCEL, WT_CMD_EXPLAIN, WT_CMD_SPEAK, WT_CMD_SETUP,
    WT_CMD_MUTE, WT_CMD_SEND_TEXT,
    WT_CMD_SPEAK_WORD, WT_CMD_SPEAK_SENT } wt_command_kind_t;
typedef struct { wt_command_kind_t kind; uint32_t generation;
    bool muted,followup; int param;
    char text[WT_VOICE_TEXT_CAP]; char anchor[32]; } wt_command_t;
typedef enum { WT_API_UNKNOWN, WT_API_READY, WT_API_ERROR } wt_api_state_t;

typedef enum { WT_MSG_ANSWER, WT_MSG_TRANSCRIPT,
    WT_MSG_ERROR, WT_MSG_SPEECH_DONE, WT_MSG_STATUS, WT_MSG_SETUP } wt_message_kind_t;
typedef struct { wt_message_kind_t kind; uint32_t generation;
    bool online, configured, speaking; wt_api_state_t api_state;
    char text[WT_TEXT_CAP]; } wt_message_t;
typedef struct { wt_page_t page; uint32_t generation, revision;
    bool online, configured, muted, recording, speaking;
    unsigned page_index, page_count, recording_ms;
    wt_api_state_t api_state;
    char transcript[WT_VOICE_TEXT_CAP];
    unsigned review_index,review_count;
    bool tap_recording;
    char lines[WT_LINES][WT_LINE_CAP];
    char setup_lines[3][WT_LINE_CAP];
    char hint[192];
    // Current word (page WT_WORD / WT_READING) and its book.
    char word[WT_WORD_CAP], ipa[WT_IPA_CAP], trans[WT_TRANS_CAP];
    char sent_en[WT_SENT_EN_CAP], sent_cn[WT_SENT_CN_CAP];
    unsigned book_index; char book_name[33], book_short[13];
    unsigned word_pos, word_count; bool known;
    // Quiz (page WT_QUIZ).
    bool quiz_ready, quiz_answered, quiz_correct, quiz_done;
    unsigned quiz_picked, quiz_slot_of_answer, quiz_score, quiz_total;
    char quiz_prompt[WT_TRANS_CAP], quiz_options[WT_QUIZ_CHOICES][WT_WORD_CAP];
    // Progress (page WT_PROGRESS); reset needs a second tap.
    unsigned streak;
    char prog_name[WT_BOOKS][33];
    unsigned prog_learned[WT_BOOKS], prog_total[WT_BOOKS];
    bool reset_armed; } wt_snapshot_t;

// The UI task alone owns the model. Drivers exchange copies through queues.
// The model reads the wordbook through wt_platform_content() and persists
// progress through wt_platform_progress_*().
wt_model_handle_t wt_model_create(bool simulated);
void wt_model_delete(wt_model_handle_t model);
bool wt_model_action(wt_model_handle_t model, wt_action_t action,
                           wt_command_t *command);
void wt_model_message(wt_model_handle_t model, const wt_message_t *message);
bool wt_model_tick(wt_model_handle_t model, unsigned elapsed_ms,
                         wt_command_t *command);
void wt_model_snapshot(wt_model_handle_t model, wt_snapshot_t *snapshot);
