// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// wt_model_test —— AI 单词老师状态机的宿主测试（真实词库 blob + 内存进度）。
// 覆盖：选书/取词/断点恢复、标记掌握自动跳词、四选一出题判分、
// 讲解的联网守卫、进度重置双击确认、语音页路径。
#include "wt_model.h"
#include "wt_content.h"
#include "wt_manifest.h"
#include "wt_progress.h"
#include "wt_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- 平台钩子：真实 blob 文件 + 内存进度 ----
static wt_blob_t blob;
static int blob_ready;
const void *wt_platform_content(void) { return blob_ready ? &blob : NULL; }
struct saved { char key[12]; uint8_t *bits; uint32_t count; };
static struct saved saved_bits[WT_BOOKS];
bool wt_platform_progress_load(const char *key,uint32_t count,uint8_t *bits)
{
    if(!key||!bits)return false;
    wt_progress_clear(bits,count);
    for(unsigned i=0;i<WT_BOOKS;++i)
        if(saved_bits[i].key[0]&&!strcmp(saved_bits[i].key,key)&&saved_bits[i].count==count)
            memcpy(bits,saved_bits[i].bits,wt_progress_bitmap_bytes(count));
    return true;
}
bool wt_platform_progress_save(const char *key,const uint8_t *bits,uint32_t count)
{
    if(!key||!bits)return false;
    for(unsigned i=0;i<WT_BOOKS;++i){
        if(!saved_bits[i].key[0]){
            snprintf(saved_bits[i].key,sizeof(saved_bits[i].key),"%s",key);
            saved_bits[i].bits=calloc(1,wt_progress_bitmap_bytes(count));
            saved_bits[i].count=count;
        }
        if(!strcmp(saved_bits[i].key,key)&&saved_bits[i].count==count){
            memcpy(saved_bits[i].bits,bits,wt_progress_bitmap_bytes(count));return true;
        }
    }
    return false;
}
static wt_resume_t resume_store;static int resume_valid;
bool wt_platform_resume_load(const void *b,void *out)
{(void)b;if(!resume_valid)return false;*(wt_resume_t *)out=resume_store;return true;}
void wt_platform_resume_save(const void *r){if(r){resume_store=*(const wt_resume_t *)r;resume_valid=1;}}
unsigned wt_platform_streak_touch(void){return 5;}
// 控制项与平台其余钩子：模型测试不经过 UI 提交路径，给空实现。
static wt_controls_t controls_store;
void wt_platform_controls_get(wt_controls_t *out)
{if(!controls_store.grade)controls_store=wt_controls_defaults();*out=controls_store;}
bool wt_platform_controls_set(wt_control_t c,int value)
{wt_controls_t v;wt_platform_controls_get(&v);bool ok=wt_controls_change(&v,c,value,0);if(ok)controls_store=v;return ok;}
bool wt_platform_submit(const void *c){(void)c;return true;}
bool wt_platform_poll(void *out){(void)out;return false;}
bool wt_platform_key(void){return false;}
void wt_platform_presented(const void *s){(void)s;}
void wt_platform_progress_submit(const void *save){(void)save;}

static wt_command_t cmd;
static wt_message_t msg;
int main(void){
    FILE *f=fopen("../assets/content/wt_content.bin","rb");
    assert(f&&"词库 blob 打不开");
    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
    uint8_t *data=malloc((size_t)n);
    assert(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);
    assert(wt_manifest_open(data,(size_t)n,&blob)==WT_MANIFEST_OK);
    assert(wt_content_verify(&blob,NULL)==WT_CONTENT_OK);
    blob_ready=1;
    wt_book_t book;assert(wt_manifest_book_at(&blob,0,&book)&&book.count>=100);

    // 1. 选书 → 词条页，字段齐全，断点保存
    wt_model_handle_t m=wt_model_create(false);
    assert(wt_model_action(m,WT_BOOK_0,&cmd)&&cmd.kind==WT_CMD_NONE);
    wt_snapshot_t v;wt_model_snapshot(m,&v);
    assert(v.page==WT_WORD&&v.word[0]&&v.book_short[0]&&v.word_pos==1&&v.word_count==book.count);
    // 首页进度已随创建填充（bug1 回归断言）
    assert(v.prog_total[0]==book.count&&v.prog_name[0][0]);
    char first_word[sizeof(v.word)];snprintf(first_word,sizeof(first_word),"%s",v.word);
    assert(wt_model_action(m,WT_WORD_NEXT,&cmd));
    wt_model_snapshot(m,&v);assert(v.word_pos==2);
    // 断点为脏标记：收口（模拟 worker）后再重建
    wt_save_msg_t sv;
    while(wt_model_take_pending(m,&sv)){
        if(sv.resume_save)wt_platform_resume_save(&sv.resume);
        if(sv.book_key[0])wt_platform_progress_save(sv.book_key,sv.bits,sv.word_count);
    }
    wt_model_delete(m);

    // 2. 重建模型 → 断点恢复到第 2 个词
    m=wt_model_create(false);
    assert(wt_model_action(m,WT_BOOK_0,&cmd));
    wt_model_snapshot(m,&v);assert(v.word_pos==2&&!v.known);
    wt_model_delete(m);

    // 3. 标记掌握 → 自动跳到下一个未掌握；进度跨实例保留
    m=wt_model_create(false);
    assert(wt_model_action(m,WT_BOOK_0,&cmd));
    wt_model_snapshot(m,&v);
    char second_word[sizeof(v.word)];snprintf(second_word,sizeof(second_word),"%s",v.word);
    assert(wt_model_action(m,WT_MARK_KNOWN,&cmd));
    wt_model_snapshot(m,&v);
    assert(v.word_pos==3&&strcmp(v.word,second_word));
    wt_save_msg_t sv3;
    while(wt_model_take_pending(m,&sv3)){
        if(sv3.resume_save)wt_platform_resume_save(&sv3.resume);
        if(sv3.book_key[0])wt_platform_progress_save(sv3.book_key,sv3.bits,sv3.word_count);
    }
    wt_model_delete(m);
    m=wt_model_create(false);
    assert(wt_model_action(m,WT_BOOK_0,&cmd));
    wt_model_snapshot(m,&v);assert(v.word_pos==3);
    assert(wt_model_action(m,WT_WORD_PREV,&cmd));
    wt_model_snapshot(m,&v);assert(v.known&&!strcmp(v.word,second_word));
    assert(wt_model_action(m,WT_SPEAK_WORD,&cmd)&&v.page==WT_WORD);
    assert(wt_model_action(m,WT_OPEN_QUIZ,&cmd));
    wt_model_snapshot(m,&v);assert(v.page==WT_QUIZ);
    wt_model_delete(m);

    // 4. 联机后出题、判分、记分
    m=wt_model_create(false);
    msg.kind=WT_MSG_STATUS;msg.generation=1;msg.online=true;msg.configured=true;msg.api_state=WT_API_READY;msg.streak=5;
    wt_model_message(m,&msg);
    assert(wt_model_action(m,WT_BOOK_0,&cmd));
    wt_model_snapshot(m,&v);
    assert(wt_model_action(m,WT_SPEAK_WORD,&cmd));
    assert(cmd.kind==WT_CMD_SPEAK_WORD&&!strcmp(cmd.text,v.word));
    assert(wt_model_action(m,WT_SPEAK_SENT,&cmd));
    assert(cmd.kind==WT_CMD_SPEAK_SENT&&!strcmp(cmd.text,v.sent_en));
    assert(strncmp(cmd.text,"s:",2));
    assert(wt_model_action(m,WT_OPEN_QUIZ,&cmd));
    wt_model_snapshot(m,&v);
    assert(v.page==WT_QUIZ&&v.quiz_ready&&v.quiz_prompt[0]);
    for(unsigned i=0;i<WT_QUIZ_CHOICES;++i)assert(v.quiz_options[i][0]);
    unsigned answer_slot=v.quiz_slot_of_answer;
    unsigned wrong_slot=(answer_slot+1)%WT_QUIZ_CHOICES;
    assert(wt_model_action(m,(wt_action_t)(WT_QUIZ_PICK_0+wrong_slot),&cmd));
    wt_model_snapshot(m,&v);assert(v.quiz_answered&&!v.quiz_correct&&v.quiz_total==1&&!v.quiz_score);
    assert(wt_model_action(m,WT_QUIZ_NEXT,&cmd));
    wt_model_snapshot(m,&v);assert(v.quiz_ready&&v.quiz_total==1);
    answer_slot=v.quiz_slot_of_answer;
    assert(wt_model_action(m,(wt_action_t)(WT_QUIZ_PICK_0+answer_slot),&cmd));
    wt_model_snapshot(m,&v);assert(v.quiz_correct&&v.quiz_score==1&&v.quiz_total==2);
    assert(!wt_model_action(m,(wt_action_t)(WT_QUIZ_PICK_0+answer_slot),&cmd));
    wt_model_snapshot(m,&v);assert(v.quiz_score==1&&v.quiz_total==2);
    assert(wt_model_action(m,WT_BACK,&cmd));
    wt_model_snapshot(m,&v);assert(v.page==WT_HOME);
    assert(wt_model_action(m,WT_BOOK_0,&cmd));
    assert(wt_model_action(m,WT_TEACH,&cmd));
    wt_model_snapshot(m,&v);
    assert(cmd.kind==WT_CMD_SEND_TEXT&&cmd.text[0]&&strstr(cmd.text,"围绕我正在学习的单词"));
    assert(v.page==WT_WAITING);
    msg.kind=WT_MSG_ANSWER;msg.generation=v.generation;
    snprintf(msg.text,sizeof(msg.text),"1 cancel 取消\n2 记忆法\n3 例句一\n4 例句二\n5 易错点\n6 小结");
    wt_model_message(m,&msg);
    wt_model_snapshot(m,&v);
    assert(v.page==WT_READING&&v.page_count>=1&&v.lines[0][0]);
    assert(wt_model_action(m,WT_QUICK_0,&cmd));
    assert(cmd.kind==WT_CMD_SEND_TEXT&&cmd.followup);
    wt_save_msg_t sv4;
    while(wt_model_take_pending(m,&sv4)){
        if(sv4.resume_save)wt_platform_resume_save(&sv4.resume);
        if(sv4.book_key[0])wt_platform_progress_save(sv4.book_key,sv4.bits,sv4.word_count);
    }
    wt_model_delete(m);

    // 5. 进度重置：双击确认
    m=wt_model_create(false);
    assert(wt_model_action(m,WT_OPEN_PROGRESS,&cmd));
    wt_model_snapshot(m,&v);
    assert(v.page==WT_PROGRESS&&v.prog_learned[0]==2);
    assert(wt_model_action(m,WT_RESET,&cmd));
    wt_model_snapshot(m,&v);assert(v.reset_armed&&v.prog_learned[0]==2);
    assert(wt_model_action(m,WT_RESET,&cmd));
    wt_model_snapshot(m,&v);assert(!v.reset_armed&&v.prog_learned[0]==0);
    wt_model_delete(m);
    puts("PASS: book select, home progress fill, resume, mark-known advance, quiz scoring, online guards, teach prompt, double-tap reset");
    return 0;
}
