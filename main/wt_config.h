// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
enum { WT_KEY_CAP = 257, WT_SSID_CAP = 33, WT_PASSWORD_CAP = 65 };
typedef struct { uint32_t version; char ssid[WT_SSID_CAP];
    char password[WT_PASSWORD_CAP]; char key[WT_KEY_CAP];
    unsigned region; } wt_config_t;
bool wt_config_valid(const wt_config_t *config);
const char *wt_config_host(unsigned region);
const char *wt_config_model(void);
