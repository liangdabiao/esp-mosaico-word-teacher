// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "wt_config.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct wt_provider_t *wt_provider_handle_t;
typedef bool (*wt_current_cb_t)(void *context, uint32_t generation);
esp_err_t wt_provider_create(wt_current_cb_t current, void *context,
                                  wt_provider_handle_t *out);
void wt_provider_delete(wt_provider_handle_t provider);
bool wt_provider_set_grade(wt_provider_handle_t provider,unsigned grade);
esp_err_t wt_provider_answer(wt_provider_handle_t provider,
    const wt_config_t *config, uint32_t generation, const char *question,
    const void *jpeg, size_t jpeg_size, const char *previous_answer,
    char *answer, size_t capacity);
esp_err_t wt_provider_transcribe(wt_provider_handle_t provider,
    const wt_config_t *config, uint32_t generation, const void *wav,
    size_t wav_size, char *text, size_t capacity);
esp_err_t wt_provider_audio(wt_provider_handle_t provider,
    const wt_config_t *config, uint32_t generation, const char *text,
    uint8_t **wav, size_t *size);
const char *wt_provider_error(wt_provider_handle_t provider);
// Maintenance probe: fixed provider host, no credentials or user content.
esp_err_t wt_provider_probe(unsigned region, int *http_status);
