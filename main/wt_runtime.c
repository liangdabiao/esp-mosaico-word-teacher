// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// wt_runtime —— worker 任务（云端、发音、麦克风录音）与平台钩子的设备实现。
// 继承 learning-assistant 的单所有者结构：UI 独占模型；worker 独占云端/扬声器；
// capture 独占麦克风。跨任务只有队列、原子量与音频锁。相机整体移除。
#include "wt_runtime.h"
#include "wt_platform.h"
#include "wt_provider.h"
#include "wt_network.h"
#include "wt_speech.h"
#include "wt_conversation.h"
#include "wt_voice.h"
#include "wt_cache.h"
#include "wt_store.h"
#include "wt_content_blob.h"
#include "bsp/esp_mosaico.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "esp_timer.h"
#include "esp_iris.h"
#include "esp_log.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { RATE=16000, SAMPLE_BYTES=2, AUDIO_CHUNK=512,
       WAV_HEADER=44, AUDIO_CAP=RATE*SAMPLE_BYTES*30,
       COMMAND_DEPTH=8, MESSAGE_DEPTH=4, WORKER_STACK=8192 };
struct runtime_t {
    QueueHandle_t commands,messages,saves;
    SemaphoreHandle_t audio_lock;
    wt_provider_handle_t provider;
    wt_conversation_handle_t conversation;
    esp_gsp_handle_t ui;
    esp_codec_dev_handle_t mic;
    button_handle_t buttons[BSP_BUTTON_NUM];
    TaskHandle_t capture_task,worker_task;
    atomic_uint generation;
    atomic_bool key,recording,wants_record,overflow,ready,speech_stopped;
    atomic_uint record_after_ms;
    atomic_uint visible_page;
    bool mic_open,previewing,last_online,last_configured;
    unsigned applied_volume;
    wt_api_state_t api_state,last_api_state;
    uint8_t *wav;
    size_t recorded;
    int16_t samples[AUDIO_CHUNK/2];
    wt_command_t command;
    wt_message_t message;
    char transcript[WT_VOICE_TEXT_CAP+128];
};
// App-lifetime singleton. UI owns model; worker owns cloud/voice/mic config;
// capture owns mic reads. Atomics, queues and audio_lock are the only
// cross-task state.
static struct runtime_t *runtime;
static bool current(void *ctx,uint32_t gen)
{struct runtime_t *r=ctx;return atomic_load(&r->generation)==gen;}
static void publish(struct runtime_t *r,wt_message_kind_t kind,uint32_t gen,const char *text)
{
    memset(&r->message,0,sizeof(r->message));r->message.kind=kind;r->message.generation=gen;
    if(text)snprintf(r->message.text,sizeof(r->message.text),"%s",text);
    if(xQueueSend(r->messages,&r->message,pdMS_TO_TICKS(100))!=pdTRUE){
        // Preserve a terminal event by dropping the oldest progress item.
        wt_message_t *discard=malloc(sizeof(*discard));
        if(discard){xQueueReceive(r->messages,discard,0);free(discard);xQueueSend(r->messages,&r->message,0);}
    }
}
static void failure(struct runtime_t *r,uint32_t gen,const char *text)
{if(current(r,gen))publish(r,WT_MSG_ERROR,gen,text);}
static void put16(uint8_t *p,unsigned x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static void put32(uint8_t *p,uint32_t x){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(x>>(8*i));}
static void header(struct runtime_t *r)
{
    uint8_t *p=r->wav;memset(p,0,WAV_HEADER);
    memcpy(p,"RIFF",4);put32(p+4,(uint32_t)r->recorded+36);memcpy(p+8,"WAVEfmt ",8);
    put32(p+16,16);put16(p+20,1);put16(p+22,1);put32(p+24,RATE);
    put32(p+28,RATE*2);put16(p+32,2);put16(p+34,16);memcpy(p+36,"data",4);put32(p+40,(uint32_t)r->recorded);
}
// 麦克风与扬声器共用配对 I2S，必须同采样率：两者都开 48k，这里 3:1 抽取成
// 16k 给 ASR（ES8311 16k 直播失真，48k 是扬声器唯一健康通路）。
#define MIC_DECIMATE 3
static void capture(void *ctx)
{
    struct runtime_t *r=ctx;ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    static int16_t raw[MIC_DECIMATE*AUDIO_CHUNK/2];
    for(;;){
        if(!r->mic_open){vTaskDelay(pdMS_TO_TICKS(100));continue;}
        uint32_t gen=atomic_load(&r->generation);bool rec=atomic_load(&r->recording);
        int err=esp_codec_dev_read(r->mic,raw,sizeof(raw));
        if(err!=ESP_CODEC_DEV_OK){if(rec)atomic_store(&r->overflow,true);vTaskDelay(pdMS_TO_TICKS(20));continue;}
        if(!rec||(int32_t)((uint32_t)(esp_timer_get_time()/1000)-atomic_load(&r->record_after_ms))<0)continue;
        for(unsigned i=0;i<AUDIO_CHUNK/2;++i){
            int32_t sum=0;
            for(unsigned k=0;k<MIC_DECIMATE;++k)sum+=raw[i*MIC_DECIMATE+k];
            r->samples[i]=(int16_t)(sum/MIC_DECIMATE);
        }
        xSemaphoreTake(r->audio_lock,portMAX_DELAY);
        if(atomic_load(&r->recording)&&current(r,gen)){
            if(r->recorded+sizeof(r->samples)>AUDIO_CAP)atomic_store(&r->overflow,true);
            else {memcpy(r->wav+WAV_HEADER+r->recorded,r->samples,sizeof(r->samples));r->recorded+=sizeof(r->samples);}
        }
        xSemaphoreGive(r->audio_lock);
    }
}
// TTS 回放：百炼 WAV（16k/24k）→ wt_voice_play_pcm（低采样率自动升到 48k）。
static esp_err_t play(struct runtime_t *r,uint32_t gen,uint8_t *wav,size_t size)
{
    wt_speech_info_t info;
    esp_err_t parsed=wt_speech_prepare(wav,size,&info);
    ESP_LOGI("wt_voice","WAV: total=%u format=%u channels=%u rate=%u bits=%u declared=%u streaming=%u pcm=%u result=%s",
        (unsigned)size,info.format,info.channels,info.rate,info.bits,(unsigned)info.declared_bytes,
        info.streaming_length,(unsigned)info.bytes,esp_err_to_name(parsed));
    if(parsed!=ESP_OK)return parsed;
    size_t pcm=info.offset,bytes=info.bytes;
    for(size_t sent=0;sent<bytes;){
        if(!current(r,gen)||atomic_load(&r->speech_stopped))return ESP_ERR_INVALID_STATE;
        wt_controls_t controls;wt_platform_controls_get(&controls);
        if((int)controls.volume!=r->applied_volume){
            wt_voice_apply_volume((int)controls.volume);
            r->applied_volume=controls.volume;
        }
        size_t chunk=bytes-sent>1024?1024:bytes-sent;
        esp_err_t written=wt_voice_play_pcm(wav+pcm+sent,chunk,info.rate);
        if(written!=ESP_OK){ESP_LOGE("wt_voice","Speaker write failed: offset=%u",(unsigned)sent);return ESP_FAIL;}
        sent+=chunk;
    }
    return ESP_OK;
}
static void speak(struct runtime_t *r,const wt_command_t *c,const wt_config_t *config)
{
    if(!wt_voice_ready()){publish(r,WT_MSG_SPEECH_DONE,c->generation,"扬声器未就绪，文字已保留");return;}
    uint8_t *wav=NULL;size_t size=0;
    esp_err_t err=wt_provider_audio(r->provider,config,c->generation,c->text,&wav,&size);
    if(err==ESP_OK)err=play(r,c->generation,wav,size);
    free(wav);
    ESP_LOGI("wt_voice","Playback finished: result=%s",esp_err_to_name(err));
    if(current(r,c->generation))publish(r,WT_MSG_SPEECH_DONE,c->generation,
        err==ESP_OK||err==ESP_ERR_INVALID_STATE||atomic_load(&r->speech_stopped)?NULL:"朗读失败，文字已保留，可重试");
}
static void speak_word(struct runtime_t *r,const wt_command_t *c,bool sentence)
{
    if(!wt_voice_ready()){publish(r,WT_MSG_SPEECH_DONE,c->generation,"扬声器未就绪");return;}
    wt_controls_t controls;wt_platform_controls_get(&controls);
    wt_voice_apply_volume((int)controls.volume);
    // 例句整句：有道 type=2 服务端 500、百度 gettts 的 TLS 证书不被设备信任
    //（本网络实测 100% 失败）——整句统一走 DashScope qwen3-tts（keep-alive 常驻
    // 连接，问老师朗读同通路）；单词仍走有道 HTTP + NAND 缓存。
    if(sentence){
        const wt_config_t *config=wt_network_config();
        if(!config||!wt_network_online()){publish(r,WT_MSG_SPEECH_DONE,c->generation,"例句需要联网");return;}
        uint8_t *wav=NULL;size_t size=0;
        esp_err_t err=wt_provider_audio(r->provider,config,c->generation,c->text,&wav,&size);
        if(err==ESP_OK)err=play(r,c->generation,wav,size);
        free(wav);
        ESP_LOGI("wt_voice","Sentence(DashScope) playback: result=%s",esp_err_to_name(err));
        if(current(r,c->generation))publish(r,WT_MSG_SPEECH_DONE,c->generation,
            err==ESP_OK||err==ESP_ERR_INVALID_STATE?NULL:"例句发音失败，请检查网络后重试");
        return;
    }
    esp_err_t err=wt_voice_speak_word(c->text);
    ESP_LOGI("wt_voice","Word playback: result=%s",esp_err_to_name(err));
    if(current(r,c->generation))publish(r,WT_MSG_SPEECH_DONE,c->generation,
        err==ESP_OK||err==ESP_ERR_INVALID_STATE?NULL:
        err==ESP_ERR_NOT_FOUND?"没有找到这个词的发音":"发音失败，请检查网络后重试");
}
static void network_status(struct runtime_t *r,bool force)
{
    bool online=wt_network_online(),configured=wt_network_configured();
    if(!force&&online==r->last_online&&configured==r->last_configured&&r->api_state==r->last_api_state)return;
    if(!online)r->api_state=WT_API_UNKNOWN;
    r->last_online=online;r->last_configured=configured;r->last_api_state=r->api_state;
    memset(&r->message,0,sizeof(r->message));r->message.kind=WT_MSG_STATUS;
    r->message.online=online;r->message.configured=configured;r->message.api_state=r->api_state;
    // streak 的 NVS 打卡在 worker 任务做一次，随状态消息带给模型；
    // UI 任务（12KB 渲染栈）不碰 flash。
    r->message.streak=wt_platform_streak_touch();
    if(!online)snprintf(r->message.text,sizeof(r->message.text),"%s",configured?"网络未连接，可在设置中重新配置":"请先用手机配置 Wi-Fi 和模型");
    xQueueSend(r->messages,&r->message,pdMS_TO_TICKS(100));
}
static void setup_message(struct runtime_t *r,uint32_t gen)
{
    const char *ssid=NULL,*password=NULL;
    if(wt_network_setup_info(&ssid,&password)){
        memset(&r->message,0,sizeof(r->message));r->message.kind=WT_MSG_SETUP;r->message.generation=gen;
        snprintf(r->message.text,sizeof(r->message.text),"%s\n无需密码，手机直接连接\n打开 192.168.4.1",ssid);
        xQueueSend(r->messages,&r->message,pdMS_TO_TICKS(100));
    }
}
static void answer_question(struct runtime_t *r,const wt_command_t *c,
                            const wt_config_t *config)
{
    if(!config||!wt_network_online()){failure(r,c->generation,"网络或模型未配置，请先打开连接设置");return;}
    if(!c->followup){
        wt_conversation_reset(r->conversation);
        wt_conversation_select(r->conversation,c->anchor[0]?c->anchor:"自由问答");
    }
    char *answer=calloc(1,WT_TEXT_CAP),*history=calloc(1,WT_HISTORY_CAP);
    if(!answer||!history){free(answer);free(history);failure(r,c->generation,"设备内存不足，请稍后重试");return;}
    wt_conversation_history(r->conversation,history,WT_HISTORY_CAP);
    ESP_LOGI("wt_teacher","Question: followup=%u history_bytes=%u",c->followup,(unsigned)strlen(history));
    wt_controls_t controls;wt_platform_controls_get(&controls);
    if(!wt_provider_set_grade(r->provider,controls.grade)){
        free(answer);free(history);failure(r,c->generation,"年级设置异常，请重新选择");return;
    }
    esp_err_t err=wt_provider_answer(r->provider,config,c->generation,c->text,NULL,0,history,answer,WT_TEXT_CAP);
    if(current(r,c->generation))r->api_state=err==ESP_OK?WT_API_READY:WT_API_ERROR;
    if(err==ESP_OK&&current(r,c->generation)){
        wt_conversation_answer(r->conversation,c->text,answer);publish(r,WT_MSG_ANSWER,c->generation,answer);
    }else failure(r,c->generation,wt_provider_error(r->provider));
    free(answer);free(history);
}
static void worker(void *ctx)
{
    struct runtime_t *r=ctx;ulTaskNotifyTake(pdTRUE,portMAX_DELAY);network_status(r,true);setup_message(r,1);
    unsigned heap_ticks=0;
    for(;;){
        network_status(r,false);
        wt_save_msg_t save;
        while(xQueueReceive(r->saves,&save,0)==pdTRUE){
            if(save.resume_save)wt_platform_resume_save(&save.resume);
            if(save.book_key[0])wt_platform_progress_save(save.book_key,save.bits,save.word_count);
        }
        if(xQueueReceive(r->commands,&r->command,pdMS_TO_TICKS(200))!=pdTRUE)continue;
        wt_command_t *c=&r->command;
        if(!current(r,c->generation))continue;
        const wt_config_t *config=wt_network_config();
        if(c->kind==WT_CMD_CANCEL){
            if(!c->followup)wt_conversation_reset(r->conversation);
            continue;
        }
        if(c->kind==WT_CMD_SETUP){
            if(wt_network_setup()!=ESP_OK)failure(r,c->generation,"无法开启配置热点，请稍后重试");
            else setup_message(r,c->generation);
            continue;
        }
        if(c->kind==WT_CMD_MUTE){continue;}
        if(c->kind==WT_CMD_RECORD){
            if(!c->followup)wt_conversation_reset(r->conversation);
            wt_voice_stop();
            if(!r->mic_open){failure(r,c->generation,"麦克风未就绪，请稍后重试");continue;}
            // Capture starts at nonblocking submit, even if an interrupted
            // HTTP request is still leaving its bounded socket timeout.
            continue;
        }
        if(c->kind==WT_CMD_SPEAK_WORD||c->kind==WT_CMD_SPEAK_SENT){
            speak_word(r,c,c->kind==WT_CMD_SPEAK_SENT);continue;
        }
        if(c->kind==WT_CMD_SPEAK){if(!atomic_load(&r->speech_stopped))speak(r,c,config);continue;}
        if(c->kind==WT_CMD_SEND_AUDIO){
            atomic_store(&r->recording,false);
            xSemaphoreTake(r->audio_lock,portMAX_DELAY);
            if(!current(r,c->generation)){xSemaphoreGive(r->audio_lock);continue;}
            bool valid=!atomic_load(&r->overflow)&&r->recorded>=3200;
            header(r);size_t size=r->recorded+WAV_HEADER;
            uint8_t *recording=valid?malloc(size):NULL;
            if(recording)memcpy(recording,r->wav,size);
            xSemaphoreGive(r->audio_lock);
            if(!valid){failure(r,c->generation,"录音不完整或太短，请重新提问");continue;}
            if(!recording){failure(r,c->generation,"录音内存不足，请稍后重试");continue;}
            esp_err_t transcribed=wt_provider_transcribe(r->provider,config,c->generation,recording,size,r->transcript,WT_VOICE_TEXT_CAP);
            free(recording);
            if(transcribed!=ESP_OK){if(current(r,c->generation))r->api_state=WT_API_ERROR;failure(r,c->generation,wt_provider_error(r->provider));continue;}
            r->api_state=WT_API_READY;
            publish(r,WT_MSG_TRANSCRIPT,c->generation,r->transcript);continue;
        }
        if(c->kind==WT_CMD_SEND_TEXT||c->kind==WT_CMD_EXPLAIN){
            answer_question(r,c,config);continue;
        }
    }
}
static void key_changed(void *button,void *ctx)
{struct runtime_t *r=ctx;atomic_store(&r->key,iot_button_get_event(button)==BUTTON_PRESS_DOWN);}
bool wt_platform_simulated(void){return false;}
void wt_platform_presented(const void *snapshot)
{const wt_snapshot_t *v=(const wt_snapshot_t *)snapshot;
 if(runtime&&v)atomic_store(&runtime->visible_page,v->page);}
bool wt_platform_key(void){return runtime&&atomic_load(&runtime->key);}
bool wt_platform_poll(void *out)
{return runtime&&xQueueReceive(runtime->messages,out,0)==pdTRUE;}
bool wt_platform_submit(const void *command)
{
    const wt_command_t *c=(const wt_command_t *)command;
    struct runtime_t *r=runtime;if(!r||!atomic_load(&r->ready))return false;
    if(c->generation<atomic_load(&r->generation))return true;
    if(c->kind==WT_CMD_MUTE){
        atomic_store(&r->speech_stopped,true);
        wt_platform_controls_set(WT_CONTROL_SPEECH,c->muted?0:1);return true;
    }
    atomic_store(&r->recording,false);
    atomic_store(&r->wants_record,c->kind==WT_CMD_RECORD);
    atomic_store(&r->generation,c->generation);
    // A new explicit read has a new generation, so it cannot revive an old
    // stream when clearing this latch. Mute never cancels recording/ASR.
    if(c->kind==WT_CMD_SPEAK)atomic_store(&r->speech_stopped,false);
    // 任何新操作都掐断正在进行的发音/朗读。
    wt_voice_set_generation(c->generation);
    wt_platform_controls_set(WT_CONTROL_SPEECH,c->muted?0:1);
    if(c->kind==WT_CMD_RECORD){
        if(!r->mic_open||xSemaphoreTake(r->audio_lock,0)!=pdTRUE)return false;
        r->recorded=0;atomic_store(&r->overflow,false);
        atomic_store(&r->record_after_ms,(uint32_t)(esp_timer_get_time()/1000)+150);
        atomic_store(&r->recording,true);xSemaphoreGive(r->audio_lock);
    }
    bool queued=xQueueSend(r->commands,c,0)==pdTRUE;
    if(!queued)atomic_store(&r->recording,false);
    return queued;
}
// ---- 平台钩子的设备实现：词库 blob、进度 NVS、打卡 ----
static wt_blob_t s_blob;
static bool s_blob_ready;
const void *wt_platform_content(void)
{
    if(!s_blob_ready){
        char reason[128];
        s_blob_ready=wt_content_open(&s_blob,reason,sizeof(reason));
        if(!s_blob_ready)ESP_LOGE("word_teacher","词库加载失败：%s",reason);
    }
    return s_blob_ready?&s_blob:NULL;
}
bool wt_platform_progress_load(const char *book_key,uint32_t word_count,uint8_t *bits)
{
    if(!book_key||!bits||!word_count)return false;
    wt_progress_clear(bits,word_count);
    return wt_store_load_bitmap(book_key,word_count,bits);
}
bool wt_platform_progress_save(const char *book_key,const uint8_t *bits,uint32_t word_count)
{
    if(!book_key||!bits||!word_count)return false;
    return wt_store_save_bitmap(book_key,bits,word_count);
}
bool wt_platform_resume_load(const void *blob,void *out)
{
    if(!blob||!out)return false;
    memset(out,0,sizeof(wt_resume_t));
    return wt_store_load_resume((const wt_blob_t *)blob,(wt_resume_t *)out)==WT_RESUME_OK;
}
void wt_platform_resume_save(const void *resume)
{
    if(resume)wt_store_save_resume((const wt_resume_t *)resume);
}
unsigned wt_platform_streak_touch(void)
{
    return wt_store_touch_streak();
}
void wt_platform_progress_submit(const void *save)
{
    if(runtime&&save&&runtime->saves)xQueueSend(runtime->saves,save,0);
}
esp_err_t wt_runtime_start(esp_gsp_handle_t ui)
{
    if(runtime||!ui)return ESP_ERR_INVALID_ARG;
    struct runtime_t *r=calloc(1,sizeof(*r));if(!r)return ESP_ERR_NO_MEM;
    r->ui=ui;
    atomic_init(&r->generation,1);atomic_init(&r->key,false);atomic_init(&r->recording,false);
    atomic_init(&r->overflow,false);atomic_init(&r->ready,false);atomic_init(&r->speech_stopped,true);atomic_init(&r->wants_record,false);
    atomic_init(&r->record_after_ms,0);
    atomic_init(&r->visible_page,WT_SETTINGS);
    r->commands=xQueueCreate(COMMAND_DEPTH,sizeof(wt_command_t));
    r->messages=xQueueCreate(MESSAGE_DEPTH,sizeof(wt_message_t));
    r->saves=xQueueCreate(4,sizeof(wt_save_msg_t));r->audio_lock=xSemaphoreCreateMutex();
    r->wav=heap_caps_malloc(AUDIO_CAP+WAV_HEADER,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    esp_err_t err=ESP_ERR_NO_MEM;
    if(!r->commands||!r->messages||!r->saves||!r->audio_lock||!r->wav)goto fail;
    err=wt_provider_create(current,r,&r->provider);if(err!=ESP_OK)goto fail;
    err=wt_conversation_create(&r->conversation);if(err!=ESP_OK)goto fail;
    r->mic=bsp_audio_codec_microphone_init();
    esp_codec_dev_sample_info_t format={.sample_rate=48000,.bits_per_sample=16,.channel=1};
    if(r->mic&&esp_codec_dev_open(r->mic,&format)==ESP_CODEC_DEV_OK){
        r->mic_open=true;
        if(esp_codec_dev_set_in_gain(r->mic,24.0f)!=ESP_CODEC_DEV_OK){esp_codec_dev_close(r->mic);r->mic_open=false;}}
    // 发音通路（NAND 缓存 + 48kHz 扬声器）；失败只降级，不阻断启动。
    if(!wt_cache_init())ESP_LOGW("wt_cache","发音缓存不可用（在线发音不受影响）");
    if(wt_voice_start()!=ESP_OK)ESP_LOGW("wt_voice","发音通路不可用（仅影响单词/例句发音）");
    const wt_blob_t *blob=wt_platform_content();
    if(!wt_store_init(blob?blob->version:0))ESP_LOGW("word_teacher","进度存储不可用（NVS），学习进度不会保存");
    int count=0;err=bsp_iot_button_create(r->buttons,&count,BSP_BUTTON_NUM);
    if(err!=ESP_OK)ESP_LOGW("word_teacher","AI 键初始化失败(%s)，语音键不可用，触摸照常",esp_err_to_name(err));
    else if(iot_button_register_cb(r->buttons[BSP_BUTTON_AI],BUTTON_PRESS_DOWN,NULL,key_changed,r)!=ESP_OK||
            iot_button_register_cb(r->buttons[BSP_BUTTON_AI],BUTTON_PRESS_UP,NULL,key_changed,r)!=ESP_OK)
        ESP_LOGW("word_teacher","AI 键回调注册失败，语音键不可用");
    err=ESP_OK;
    if(xTaskCreate(capture,"wt_mic",4096,r,5,&r->capture_task)!=pdPASS){err=ESP_ERR_NO_MEM;goto fail;}
    if(xTaskCreate(worker,"wt_worker",WORKER_STACK,r,4,&r->worker_task)!=pdPASS){err=ESP_ERR_NO_MEM;goto fail;}
    runtime=r;atomic_store(&r->ready,true);xTaskNotifyGive(r->capture_task);xTaskNotifyGive(r->worker_task);return ESP_OK;
fail:
    if(r->capture_task)vTaskDelete(r->capture_task);
    if(r->worker_task)vTaskDelete(r->worker_task);
    for(unsigned i=0;i<BSP_BUTTON_NUM;++i)if(r->buttons[i])iot_button_delete(r->buttons[i]);
    if(r->mic_open)esp_codec_dev_close(r->mic);
    wt_provider_delete(r->provider);
    wt_conversation_delete(r->conversation);
    if(r->commands)vQueueDelete(r->commands);
    if(r->messages)vQueueDelete(r->messages);
    if(r->saves)vQueueDelete(r->saves);
    if(r->audio_lock)vSemaphoreDelete(r->audio_lock);
    free(r->wav);free(r);return err;
}
