// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gsp_sim_bridge.h"
#include "wt_ui.h"
#include "wt_platform.h"
#include "wt_content.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Explicit PC fixtures. No network, microphone or device credentials are used.
static wt_ui_handle_t app;
static esp_gsp_handle_t app_ui;
static wt_message_t pending;
static bool available = true;
static unsigned polls;
static int connection_mode=2;
static unsigned record_requests,transcribe_requests,answer_requests,speech_requests,word_requests;
static wt_controls_t controls;
static bool controls_ready;
// 模拟进度：内存位图 + 断点 + 打卡。
static const wt_blob_t *sim_blob;
struct sim_progress { char key[12]; uint8_t *bits; uint32_t count; };
static struct sim_progress sim_progress[WT_BOOKS];
static wt_resume_t sim_resume;
static bool sim_resume_valid;
static unsigned sim_streak;
void wt_platform_controls_get(wt_controls_t *out)
{if(!controls_ready){controls=wt_controls_defaults();controls_ready=true;}*out=controls;}
bool wt_platform_controls_set(wt_control_t c,int value)
{wt_controls_t v;wt_platform_controls_get(&v);bool ok=wt_controls_change(&v,c,value,0);if(ok)controls=v;return ok;}
bool wt_platform_simulated(void) { return true; }
bool wt_platform_key(void) {
    const char *path=getenv("WT_SIM_KEY");if(!path)return false;
    FILE *f=fopen(path,"rb");if(!f)return false;int c=fgetc(f);fclose(f);return c=='1';
}
void wt_platform_presented(const wt_snapshot_t *s) {
    const char *path=getenv("WT_SIM_TRACE");if(!path)return;
    FILE *f=fopen(path,"wb");if(!f)return;
    wt_controls_t v;wt_platform_controls_get(&v);bool drawer=false;
    drawer=wt_ui_quick_is_open(app);
    fprintf(f,"{\"page\":%u,\"generation\":%u,\"page_index\":%u,\"page_count\":%u,\"recording\":%s,\"muted\":%s,\"brightness\":%u,\"volume\":%u,\"automatic\":%s,\"drawer\":%s,\"online\":%s,\"api_state\":%u,\"review_index\":%u,\"review_count\":%u,\"record_requests\":%u,\"transcribe_requests\":%u,\"answer_requests\":%u,\"speech_requests\":%u,\"grade\":%u,\"word_requests\":%u,\"book\":%u,\"word_pos\":%u,\"word_count\":%u,\"known\":%s,\"quiz_total\":%u,\"quiz_score\":%u,\"streak\":%u}\n",
        (unsigned)s->page,(unsigned)s->generation,s->page_index,s->page_count,s->recording?"true":"false",s->muted?"true":"false",v.brightness,v.volume,v.automatic?"true":"false",drawer?"true":"false",s->online?"true":"false",(unsigned)s->api_state,s->review_index,s->review_count,record_requests,transcribe_requests,answer_requests,speech_requests,v.grade,word_requests,s->book_index,s->word_pos,s->word_count,s->known?"true":"false",s->quiz_total,s->quiz_score,s->streak);fclose(f);
}
// ---- 词库与进度（模拟器用磁盘 blob，进度全在内存） ----
const wt_blob_t *wt_platform_content(void)
{
    if(!sim_blob){
        const char *path=WT_CONTENT_BIN;
        FILE *f=fopen(path,"rb");
        if(!f){fprintf(stderr,"wt_sim: 词库打不开 %s\n",path);return NULL;}
        fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
        uint8_t *data=malloc((size_t)n);
        if(!data||fread(data,1,(size_t)n,f)!=(size_t)n){free(data);fclose(f);return NULL;}
        fclose(f);
        char reason[128];
        static wt_blob_t blob;
        if(wt_manifest_open(data,(size_t)n,&blob)==WT_MANIFEST_OK&&
           wt_content_verify(&blob,NULL)==WT_CONTENT_OK)sim_blob=&blob;
        else{fprintf(stderr,"wt_sim: 词库校验失败 %s\n",reason);free(data);}
    }
    return sim_blob;
}
bool wt_platform_progress_load(const char *book_key,uint32_t word_count,uint8_t *bits)
{
    if(!book_key||!bits)return false;
    wt_progress_clear(bits,word_count);
    for(unsigned i=0;i<WT_BOOKS;++i)
        if(sim_progress[i].key[0]&&!strcmp(sim_progress[i].key,book_key)&&sim_progress[i].count==word_count)
            memcpy(bits,sim_progress[i].bits,wt_progress_bitmap_bytes(word_count));
    return true;
}
bool wt_platform_progress_save(const char *book_key,const uint8_t *bits,uint32_t word_count)
{
    if(!book_key||!bits)return false;
    for(unsigned i=0;i<WT_BOOKS;++i){
        if(!sim_progress[i].key[0]){
            snprintf(sim_progress[i].key,sizeof(sim_progress[i].key),"%s",book_key);
            sim_progress[i].bits=calloc(1,wt_progress_bitmap_bytes(word_count)?wt_progress_bitmap_bytes(word_count):1);
            sim_progress[i].count=word_count;
        }
        if(!strcmp(sim_progress[i].key,book_key)&&sim_progress[i].count==word_count){
            memcpy(sim_progress[i].bits,bits,wt_progress_bitmap_bytes(word_count));return true;
        }
    }
    return false;
}
bool wt_platform_resume_load(const wt_blob_t *blob,wt_resume_t *out)
{
    (void)blob;
    if(!sim_resume_valid)return false;
    *out=sim_resume;return true;
}
void wt_platform_resume_save(const wt_resume_t *resume)
{if(resume){sim_resume=*resume;sim_resume_valid=true;}}
unsigned wt_platform_streak_touch(void)
{return ++sim_streak;}
bool wt_platform_submit(const wt_command_t *c)
{
    wt_platform_controls_set(WT_CONTROL_SPEECH,c->muted?0:1);
    if(c->kind==WT_CMD_MUTE)return true;
    memset(&pending, 0, sizeof(pending)); pending.generation = c->generation;
    if(c->kind==WT_CMD_RECORD)++record_requests;
    if(c->kind==WT_CMD_SPEAK)++speech_requests;
    if(c->kind==WT_CMD_SEND_AUDIO)++transcribe_requests;
    if(c->kind==WT_CMD_SEND_TEXT||c->kind==WT_CMD_EXPLAIN)++answer_requests;
    if(c->kind==WT_CMD_SPEAK_WORD||c->kind==WT_CMD_SPEAK_SENT)++word_requests;
    switch(c->kind) {
    case WT_CMD_RECORD:available=false;return true;
    case WT_CMD_SEND_AUDIO:
        pending.kind=WT_MSG_TRANSCRIPT;
        snprintf(pending.text,sizeof(pending.text),"cancel 这个词和 recent 有什么区别？请各给一个例句。\n分两段说明。\n注意搭配。\n确认后发送。");break;
    case WT_CMD_SEND_TEXT:
        pending.kind=WT_MSG_ANSWER;
        snprintf(pending.text,sizeof(pending.text),"【模拟讲解】\ncancel 取消、作废。\n记忆：canc + el，\n计划有变就 cancel。\n\n例句 1：They cancelled\nthe flight yesterday.\n他们昨天取消了航班。\n例句 2：Please cancel\nmy order. 请取消我的订单。\n常见搭配：cancel a plan");break;
    case WT_CMD_EXPLAIN:
        pending.kind = WT_MSG_ANSWER;
        snprintf(pending.text, sizeof(pending.text), "【模拟换个讲法】\n把 cancel 想成一条被划掉的线。\n计划划掉 = 取消。\n机票划掉 = 航班取消。\n订单划掉 = 订单作废。\n近义词 call off 更口语。"); break;
    case WT_CMD_SETUP:
        pending.kind = WT_MSG_STATUS; pending.online = pending.configured = true; pending.api_state=WT_API_READY;
        snprintf(pending.text, sizeof(pending.text), "模拟预览没有热点 · 真机将显示连接信息"); break;
    case WT_CMD_SPEAK_WORD: case WT_CMD_SPEAK_SENT:
        pending.kind = WT_MSG_SPEECH_DONE;
        snprintf(pending.text, sizeof(pending.text), "模拟预览没有播放声音"); break;
    case WT_CMD_SPEAK:
        pending.kind = WT_MSG_SPEECH_DONE;
        snprintf(pending.text, sizeof(pending.text), "模拟预览没有播放声音"); break;
    case WT_CMD_CANCEL: case WT_CMD_MUTE: case WT_CMD_NONE:
        available = false; return true;
    default:
        pending.kind = WT_MSG_ERROR; snprintf(pending.text, sizeof(pending.text), "语音输入需要真机 AI 键与麦克风"); break;
    }
    available = true; polls = 0; return true;
}
bool wt_platform_poll(wt_message_t *out)
{
    const char *connection=getenv("WT_SIM_CONNECTION");
    if(connection){FILE *f=fopen(connection,"rb");if(f){int mode=fgetc(f)-'0';fclose(f);
        if(mode>=0&&mode<=3&&mode!=connection_mode){connection_mode=mode;
            memset(out,0,sizeof(*out));out->kind=WT_MSG_STATUS;out->configured=true;out->online=mode!=0;
            out->api_state=mode==2?WT_API_READY:mode==3?WT_API_ERROR:WT_API_UNKNOWN;return true;
        }
    }}
    if (!available || ++polls < 4) return false;
    *out = pending; available = false; return true;
}
esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    app_ui=ui;
    pending.kind = WT_MSG_STATUS; pending.online = pending.configured = true; pending.api_state=WT_API_READY;
    snprintf(pending.text, sizeof(pending.text), "模拟预览 · 不连接云端");
    return wt_ui_create(ui, &app);
}
void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{ (void)ui; wt_ui_delete(app); fprintf(stderr, "wt_backend: stopped\n"); }
