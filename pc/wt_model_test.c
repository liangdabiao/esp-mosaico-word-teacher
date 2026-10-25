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
const wt_blob_t *wt_platform_content(void) { return blob_ready ? &blob : NULL; }
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
bool wt_platform_resume_load(const wt_blob_t *b,wt_resume_t *out)
{(void)b;if(!resume_valid)return false;*out=resume_store;return true;}
void wt_platform_resume_save(const wt_resume_t *r){if(r){resume_store=*r;resume_valid=1;}}
unsigned wt_platform_streak_touch(void){return 5;}
// 控制项与平台其余钩子：模型测试不经过 UI 提交路径，给空实现。
static wt_controls_t controls_store;
void wt_platform_controls_get(wt_controls_t *out)
{if(!controls_store.grade)controls_store=wt_controls_defaults();*out=controls_store;}
bool wt_platform_controls_set(wt_control_t c,int value)
{wt_controls_t v;wt_platform_controls_get(&v);bool ok=wt_controls_change(&v,c,value,0);if(ok)controls_store=v;return ok;}
bool wt_platform_submit(const wt_command_t *c){(void)c;return true;}
bool wt_platform_poll(wt_message_t *out){(void)out;return false;}
bool wt_platform_key(void){return false;}
void wt_platform_presented(const wt_snapshot_t *s){(void)s;}

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
    char first_word[sizeof(v.word)];snprintf(first_word,sizeof(first_word),"%s",v.word);
    assert(wt_model_action(m,WT_WORD_NEXT,&cmd));
    wt_model_snapshot(m,&v);assert(v.word_pos==2);
    assert(resume_valid&&resume_store.book_index==0&&resume_store.offset==1);
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
    // 进度写进了 saved_bits：新实例不应再回到第 2 词
    wt_model_delete(m);
    m=wt_model_create(false);
    assert(wt_model_action(m,WT_BOOK_0,&cmd));
    wt_model_snapshot(m,&v);assert(v.word_pos==3);
    // 跳回第 2 词并翻看：known 标志点亮
    assert(wt_model_action(m,WT_WORD_PREV,&cmd));
    wt_model_snapshot(m,&v);assert(v.known&&!strcmp(v.word,second_word));
    // 离线守卫：讲解/发音只提示不跳页
    assert(wt_model_action(m,WT_TEACH,&cmd)&&v.page==WT_WORD);
    assert(wt_model_action(m,WT_SPEAK_WORD,&cmd)&&v.page==WT_WORD);
    assert(wt_model_action(m,WT_OPEN_QUIZ,&cmd));
    wt_model_snapshot(m,&v);assert(v.page==WT_QUIZ);
    wt_model_delete(m);

    // 4. 联机后出题、判分、记分
    m=wt_model_create(false);
    msg.kind=WT_MSG_STATUS;msg.generation=1;msg.online=true;msg.configured=true;msg.api_state=WT_API_READY;
    wt_model_message(m,&msg);
    assert(wt_model_action(m,WT_BOOK_0,&cmd));
    assert(wt_model_action(m,WT_OPEN_QUIZ,&cmd));
    wt_model_snapshot(m,&v);
    assert(v.page==WT_QUIZ&&v.quiz_ready&&v.quiz_prompt[0]);
    for(unsigned i=0;i<WT_QUIZ_CHOICES;++i)assert(v.quiz_options[i][0]);
    unsigned answer_slot=v.quiz_slot_of_answer;
    // 故意答错再答对：错答不记分；答对标记掌握并计分
    unsigned wrong_slot=(answer_slot+1)%WT_QUIZ_CHOICES;
    assert(wt_model_action(m,(wt_action_t)(WT_QUIZ_PICK_0+wrong_slot),&cmd));
    wt_model_snapshot(m,&v);assert(v.quiz_answered&&!v.quiz_correct&&v.quiz_total==1&&!v.quiz_score);
    assert(wt_model_action(m,WT_QUIZ_NEXT,&cmd));
    wt_model_snapshot(m,&v);assert(v.quiz_ready&&v.quiz_total==1);
    answer_slot=v.quiz_slot_of_answer;
    assert(wt_model_action(m,(wt_action_t)(WT_QUIZ_PICK_0+answer_slot),&cmd));
    wt_model_snapshot(m,&v);assert(v.quiz_correct&&v.quiz_score==1&&v.quiz_total==2);
    // 重复点击同一选项被拒绝，不刷分
    assert(!wt_model_action(m,(wt_action_t)(WT_QUIZ_PICK_0+answer_slot),&cmd));
    wt_model_snapshot(m,&v);assert(v.quiz_score==1&&v.quiz_total==2);
    // 讲解：联网后进入等待页，命令带词上下文
    assert(wt_model_action(m,WT_BACK,&cmd));
    wt_model_snapshot(m,&v);assert(v.page==WT_HOME);
    assert(wt_model_action(m,WT_BOOK_0,&cmd));
    assert(wt_model_action(m,WT_TEACH,&cmd));
    wt_model_snapshot(m,&v);
    assert(cmd.kind==WT_CMD_SEND_TEXT&&cmd.text[0]&&strstr(cmd.text,"围绕我正在学习的单词"));
    assert(v.page==WT_WAITING);
    // 模拟回答到达 → 讲解页可翻页
    msg.kind=WT_MSG_ANSWER;msg.generation=v.generation;
    snprintf(msg.text,sizeof(msg.text),"1 cancel 取消\n2 记忆法\n3 例句一\n4 例句二\n5 易错点\n6 小结");
    wt_model_message(m,&msg);
    wt_model_snapshot(m,&v);
    assert(v.page==WT_READING&&v.page_count>=1&&v.lines[0][0]);
    // 快捷问题：从讲解页追问，带 followup
    assert(wt_model_action(m,WT_QUICK_0,&cmd));
    assert(cmd.kind==WT_CMD_SEND_TEXT&&cmd.followup);
    wt_model_delete(m);

    // 5. 进度重置：双击确认
    m=wt_model_create(false);
    assert(wt_model_action(m,WT_OPEN_PROGRESS,&cmd));
    wt_model_snapshot(m,&v);
    // 已掌握 2 词：词条页"记住了"1 个 + 测验答对 1 个
    assert(v.page==WT_PROGRESS&&v.prog_learned[0]==2);
    assert(wt_model_action(m,WT_RESET,&cmd));
    wt_model_snapshot(m,&v);assert(v.reset_armed&&v.prog_learned[0]==2);
    assert(wt_model_action(m,WT_RESET,&cmd));
    wt_model_snapshot(m,&v);assert(!v.reset_armed&&v.prog_learned[0]==0);
    wt_model_delete(m);
    puts("PASS: book select, resume, mark-known advance, quiz scoring, online guards, teach prompt, double-tap reset");
    return 0;
}
