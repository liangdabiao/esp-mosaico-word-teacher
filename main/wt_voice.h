// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <stdbool.h>
#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>
// 单词/例句发音（有道公开接口 + NAND 缓存）。所有函数在 worker 任务调用；
// generation 由 runtime 维护，新操作自动掐断上一次播放。
// 启动时调一次（NAND 挂载 + 扬声器 48kHz 通路）；失败不致命，仅发音不可用。
esp_err_t wt_voice_start(void);
void wt_voice_set_generation(uint32_t generation);
void wt_voice_stop(void);
bool wt_voice_ready(void);
// 发单词（key=单词原文）；发例句（内部加 "s:" 前缀做缓存键）。
esp_err_t wt_voice_speak_word(const char *word);
esp_err_t wt_voice_speak_sentence(const char *sentence);
// 构建有道 URL（sentence=false 用单词接口，口音取 controls.accent）。
esp_err_t wt_voice_build_url(bool sentence,const char *text,char *out,size_t cap);
// 播放已解码 PCM（16 位单声道；16k/24k 自动插值升到 48k）。供 TTS 通路复用。
esp_err_t wt_voice_play_pcm(const uint8_t *pcm,size_t bytes,unsigned rate);
bool wt_voice_ready(void);
void wt_voice_apply_volume(int volume);
