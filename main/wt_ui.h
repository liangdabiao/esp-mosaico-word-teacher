// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "esp_gsp.h"
typedef struct wt_ui_t *wt_ui_handle_t;
esp_gsp_err_t wt_ui_create(esp_gsp_handle_t ui, wt_ui_handle_t *out);
void wt_ui_delete(wt_ui_handle_t ui);

// UI-thread observer for native simulator evidence; no cross-task access.
bool wt_ui_quick_is_open(wt_ui_handle_t ui);
