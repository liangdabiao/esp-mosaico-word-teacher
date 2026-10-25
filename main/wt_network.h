// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "esp_err.h"
#include "wt_config.h"
#include <stddef.h>
esp_err_t wt_network_start(void);
bool wt_network_online(void);
bool wt_network_configured(void);
const wt_config_t *wt_network_config(void);
esp_err_t wt_network_setup(void);
bool wt_network_setup_info(const char **ssid, const char **password);
// Worker-only provider smoke check used before atomically committing a candidate.
esp_err_t wt_provider_test(const wt_config_t *config, char *error, size_t capacity);
