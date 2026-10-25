// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// wt_model —— AI 单词老师的 UI 状态机（纯 C，宿主可测）。
//
// 继承 learning-assistant 的骨架：单一所有者（UI 任务）、generation 代号防串台、
// 分页/裁行/录音时长这些真机验证过的机制全部保留。变化：相机与认题页面换成
// 词书/词条/测验/进度；词条数据来自本地词库 blob（wt_content），进度是按书的
// 掌握位图（wt_progress），持久化通过 wt_platform_progress_* 钩子。
#include "wt_model.h"
#include "wt_content.h"
#include "wt_manifest.h"
#include "wt_progress.h"
#include "wt_quiz.h"
#include "wt_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MIN_RECORD_MS = 300, LINE_COLUMNS = 18, MIN_QUIZ_BOOK_WORDS = 8 };
// 预设快捷问题：围绕当前词，一键即发，不用孩子打字。
static const char *const quick_questions[WT_QUICK_COUNT] = {
    "这个词为什么这么用？请结合它的释义和例句解释。",
    "这个词和最常见的近义词有什么区别？各给一个例句。",
    "请再给两个用到这个词的新例句，英文加中文。",
    "用这个词出 3 道选择题，先给题，最后统一给答案。",
};
struct wt_model_t {
    wt_snapshot_t view;
    bool simulated, key_held, conversation, awaiting_transcript;
    wt_page_t return_page;
    wt_action_t retry;
    char answer[WT_TEXT_CAP];
    size_t offsets[WT_MAX_PAGES + 1], review_offsets[WT_MAX_PAGES + 1];
    // Wordbook state.
    const wt_blob_t *blob;
    uint8_t *bits[WT_BOOKS];
    bool dirty[WT_BOOKS];
    unsigned book;
    uint32_t offset;
    bool word_loaded;
    wt_resume_t resume;
    wt_quiz_t quiz;
    unsigned quiz_score, quiz_total;
    bool reset_armed;
};
static bool valid_utf8(const char *text,size_t capacity)
{
    const unsigned char *p=(const unsigned char*)text;
    const char *end=memchr(text,0,capacity);if(!end)return false;
    while((const char*)p<end){
        unsigned c=*p++;if(c<0x80)continue;
        unsigned n=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:0;
        if(!n||p+n>(const unsigned char*)end)return false;
        if((c==0xe0&&p[0]<0xa0)||(c==0xed&&p[0]>=0xa0)||
            (c==0xf0&&p[0]<0x90)||(c==0xf4&&p[0]>=0x90))return false;
        for(unsigned i=0;i<n;++i)if((p[i]&0xc0)!=0x80)return false;
        p+=n;
    }
    return true;
}
static void hint(wt_model_handle_t m, const char *text)
{
    size_t n=strlen(text),cap=sizeof(m->view.hint)-1;
    if(n>cap){n=cap;while(n&&((unsigned char)text[n]&0xc0)==0x80)--n;}
    memcpy(m->view.hint,text,n);m->view.hint[n]=0;
}
static void command(wt_model_handle_t m, wt_command_t *out, wt_command_kind_t kind)
{ out->kind = kind; out->generation = m->view.generation; out->muted = m->view.muted;
  out->followup = m->conversation; out->param = 0; }
static void cancel(wt_model_handle_t m, wt_command_t *out);
static void page_lines(wt_model_handle_t m);
static void review_lines(wt_model_handle_t m);
static void cancel(wt_model_handle_t m, wt_command_t *out)
{
    ++m->view.generation; m->view.recording = false; m->view.speaking = false;
    m->view.tap_recording=false;m->awaiting_transcript=false;
    command(m, out, WT_CMD_CANCEL);
}
// 复制 blob 字段到定长快照缓冲，超长在 UTF-8 边界截断。
static void copy_field(char *out,size_t cap,const char *text)
{
    size_t used=0;
    while(*text){
        size_t n=*text<0x80?1:(unsigned char)*text<0xe0?2:(unsigned char)*text<0xf0?3:4;
        if(used+n>=cap){while(used&&((unsigned char)out[used]&0xc0)==0x80)--used;break;}
        memcpy(out+used,text,n);used+=n;text+=n;
    }
    out[used]=0;
}
static void present_word(wt_model_handle_t m)
{
    wt_word_t w;
    wt_snapshot_t *v=&m->view;
    wt_book_t book;
    if(!m->blob||!wt_manifest_book_at(m->blob,m->book,&book)||
       !wt_content_get_in_book(m->blob,m->book,m->offset,&w)){
        m->word_loaded=false;v->word[0]=v->ipa[0]=v->trans[0]=0;
        v->sent_en[0]=v->sent_cn[0]=0;return;
    }
    m->word_loaded=true;
    copy_field(v->word,sizeof(v->word),wt_content_field(&w,WT_FIELD_WORD));
    copy_field(v->ipa,sizeof(v->ipa),wt_content_field(&w,WT_FIELD_IPA));
    copy_field(v->trans,sizeof(v->trans),wt_content_field(&w,WT_FIELD_TRANS));
    copy_field(v->sent_en,sizeof(v->sent_en),wt_content_field(&w,WT_FIELD_SENT_EN));
    copy_field(v->sent_cn,sizeof(v->sent_cn),wt_content_field(&w,WT_FIELD_SENT_CN));
    copy_field(v->book_name,sizeof(v->book_name),book.name);
    copy_field(v->book_short,sizeof(v->book_short),book.short_name);
    v->book_index=m->book;v->word_pos=m->offset+1;v->word_count=book.count;
    v->known=m->bits[m->book]&&wt_progress_get(m->bits[m->book],book.count,m->offset);
}
static void save_resume(wt_model_handle_t m)
{
    if(!m->blob)return;
    m->resume.book_index=m->book;m->resume.offset=m->offset;
    wt_platform_resume_save(&m->resume);
}
static void load_word(wt_model_handle_t m,uint32_t offset)
{
    wt_book_t book;
    if(!m->blob||!wt_manifest_book_at(m->blob,m->book,&book))return;
    if(offset>=book.count)offset=book.count?book.count-1:0;
    m->offset=offset;
    present_word(m);
    save_resume(m);
}
static void fill_progress(wt_model_handle_t m)
{
    wt_snapshot_t *v=&m->view;
    v->streak=m->simulated?3:wt_platform_streak_touch();
    for(unsigned i=0;i<WT_BOOKS;++i){
        wt_book_t book;
        if(!m->blob||!wt_manifest_book_at(m->blob,i,&book)){v->prog_name[i][0]=0;v->prog_learned[i]=v->prog_total[i]=0;continue;}
        copy_field(v->prog_name[i],sizeof(v->prog_name[i]),book.name);
        v->prog_total[i]=book.count;
        v->prog_learned[i]=m->bits[i]?wt_progress_mastered(m->bits[i],book.count):0;
    }
}
static bool teacher_ready(wt_model_handle_t m)
{
    if(!m->view.online||!m->view.configured){hint(m,"请先完成联网与模型配置");return false;}
    return true;
}
static void ask(wt_model_handle_t m,wt_command_t *out,const char *question,bool followup)
{
    ++m->view.generation;
    m->conversation=followup;m->view.transcript[0]=0;m->awaiting_transcript=false;
    command(m,out,WT_CMD_SEND_TEXT);
    snprintf(out->anchor,sizeof(out->anchor),"%s",m->view.word);
    snprintf(out->text,sizeof(out->text),"%s",question);
    m->view.speaking=false;m->view.page=WT_WAITING;m->retry=WT_ASK;
    hint(m,followup?"已确认发送，正在思考":"老师正在讲解这个词，可以取消");
}
static void build_teach_prompt(char *out,size_t cap,const wt_snapshot_t *v)
{
    snprintf(out,cap,
        "请围绕我正在学习的单词讲解，先复述这个词的核心含义，再讲词根词缀或记忆方法，"
        "然后给两个新例句（英文加中文翻译），最后点出最常见的搭配或易错点。"
        "单词：%s。音标：%s。词典释义：%s。教材例句：%s（%s）。",
        v->word,v->ipa[0]?v->ipa:"（无音标）",v->trans,
        v->sent_en[0]?v->sent_en:"（无例句）",v->sent_cn[0]?v->sent_cn:"");
}
static void start_quiz(wt_model_handle_t m,uint32_t answer_offset)
{
    wt_snapshot_t *v=&m->view;
    wt_book_t book;
    if(!m->blob||!wt_manifest_book_at(m->blob,m->book,&book)||book.count<MIN_QUIZ_BOOK_WORDS){
        hint(m,"这本书词太少，没法出四选一");return;
    }
    m->resume.seed=m->resume.seed*1664525u+1013904223u;
    if(!wt_quiz_start(&m->quiz,m->blob,m->book,answer_offset,m->resume.seed)){
        hint(m,"这一题出不出来，请换一个词再试");return;
    }
    wt_word_t w;
    if(!wt_content_get_in_book(m->blob,m->book,answer_offset,&w)){hint(m,"词条读取失败，请重试");return;}
    copy_field(v->quiz_prompt,sizeof(v->quiz_prompt),wt_content_field(&w,WT_FIELD_TRANS));
    for(unsigned i=0;i<WT_QUIZ_CHOICES;++i){
        wt_word_t c;
        if(wt_content_get_in_book(m->blob,m->book,m->quiz.choice[i],&c))
            copy_field(v->quiz_options[i],sizeof(v->quiz_options[i]),wt_content_field(&c,WT_FIELD_WORD));
        else v->quiz_options[i][0]=0;
    }
    v->quiz_ready=wt_quiz_ready(&m->quiz);v->quiz_answered=false;v->quiz_correct=false;
    v->quiz_picked=m->quiz.picked;v->quiz_slot_of_answer=wt_quiz_slot_of_answer(&m->quiz);
    v->quiz_score=m->quiz_score;v->quiz_total=m->quiz_total;v->quiz_done=false;
    v->page=WT_QUIZ;hint(m,"选出与释义对应的单词");save_resume(m);
}
static void refresh_quiz(wt_model_handle_t m)
{
    wt_snapshot_t *v=&m->view;
    v->quiz_ready=wt_quiz_ready(&m->quiz);v->quiz_answered=wt_quiz_answered(&m->quiz);
    v->quiz_correct=m->quiz.phase==WT_QUIZ_PHASE_CORRECT;
    v->quiz_picked=m->quiz.picked;v->quiz_slot_of_answer=wt_quiz_slot_of_answer(&m->quiz);
    v->quiz_score=m->quiz_score;v->quiz_total=m->quiz_total;
}
static void quiz_next(wt_model_handle_t m)
{
    wt_book_t book;
    if(!m->blob||!wt_manifest_book_at(m->blob,m->book,&book)){hint(m,"词库不可用");return;}
    uint32_t next=wt_progress_next_unmastered(m->bits[m->book],book.count,(m->quiz.answer_offset+1)%book.count);
    if(next>=book.count){hint(m,"这本书已全部掌握，回首页换一本吧");m->view.page=WT_HOME;return;}
    start_quiz(m,next);
}
static void mark_mastered(wt_model_handle_t m,uint32_t offset,bool mastered)
{
    wt_book_t book;
    if(!m->blob||!m->bits[m->book]||!wt_manifest_book_at(m->blob,m->book,&book))return;
    wt_progress_set(m->bits[m->book],book.count,offset,mastered);
    m->dirty[m->book]=true;
    wt_platform_progress_save(book.key,m->bits[m->book],book.count);
}
wt_model_handle_t wt_model_create(bool simulated)
{
    wt_model_handle_t m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->simulated = simulated; m->view.page = WT_HOME; m->view.generation = 1;
    m->view.revision = 1; m->return_page = WT_HOME;
    m->blob = wt_platform_content();
    if(m->blob){
        for(unsigned i=0;i<WT_BOOKS;++i){
            wt_book_t book;
            if(!wt_manifest_book_at(m->blob,i,&book))continue;
            uint32_t bytes=wt_progress_bitmap_bytes(book.count);
            m->bits[i]=calloc(1,bytes?bytes:1);
            if(m->bits[i]){
                wt_progress_clear(m->bits[i],book.count);
                wt_platform_progress_load(book.key,book.count,m->bits[i]);
            }
        }
        wt_resume_t resume;
        if(wt_platform_resume_load(m->blob,&resume)){
            m->resume=resume;
            if(resume.book_index<WT_BOOKS)m->book=resume.book_index;
            wt_book_t book;
            if(wt_manifest_book_at(m->blob,m->book,&book)&&resume.offset<book.count)m->offset=resume.offset;
        }
    }
    hint(m, simulated ? "模拟预览 · 不连接云端" : "选择一本词书开始学习");
    return m;
}
void wt_model_delete(wt_model_handle_t m)
{
    if(!m)return;
    for(unsigned i=0;i<WT_BOOKS;++i)free(m->bits[i]);
    free(m);
}
bool wt_model_action(wt_model_handle_t m, wt_action_t action, wt_command_t *out)
{
    if (!m || !out) return false;
    memset(out, 0, sizeof(*out));
    if(action==WT_VOICE_TOGGLE){
        if(m->key_held)return false;
        if(m->view.tap_recording){m->view.tap_recording=false;return wt_model_action(m,WT_KEY_UP,out);}
        if(m->view.page!=WT_VOICE)return false;
        bool ok=wt_model_action(m,WT_KEY_DOWN,out);
        if(m->view.recording){m->key_held=false;m->view.tap_recording=true;}
        return ok;
    }
    if (action == WT_KEY_UP) {
        if(m->view.tap_recording)return false;
        m->key_held = false;
        if (!m->view.recording) return false;
        m->view.recording = false;
        if (m->view.recording_ms < MIN_RECORD_MS) {
            cancel(m, out); m->view.page = WT_VOICE; hint(m, "请按住 AI 键说完一句话再松开");
        } else { command(m, out, WT_CMD_SEND_AUDIO); m->awaiting_transcript=true; m->view.page = WT_WAITING; hint(m, "正在识别语音，完成后请确认发送"); }
        ++m->view.revision; return true;
    }
    if (action == WT_KEY_DOWN) {
        if (m->key_held||m->view.recording) return false;
        if(m->view.page==WT_QUIZ||m->view.page==WT_PROGRESS||m->view.page==WT_SETTINGS){hint(m,"此页面不能提问");++m->view.revision;return true;}
        m->key_held = true;
        if (m->view.page == WT_SETTINGS || !m->view.configured || !m->view.online) {
            hint(m, "请先完成联网与模型配置"); ++m->view.revision; return true;
        }
        if(m->view.page==WT_HOME)m->conversation=false;
        ++m->view.generation; m->awaiting_transcript=false; m->view.speaking = false; m->view.page = WT_VOICE;
        m->retry=WT_ASK;
        m->view.recording = true; m->view.recording_ms = 0; m->view.transcript[0]=0;
        hint(m, "松开后识别文字 · 最长 30 秒"); command(m, out, WT_CMD_RECORD);
        ++m->view.revision; return true;
    }
    switch (action) {
    case WT_BOOK_0: case WT_BOOK_1: case WT_BOOK_2:
    case WT_BOOK_3: case WT_BOOK_4: case WT_BOOK_5:
        if(!m->blob){hint(m,"词库不可用，请重启设备");break;}
        m->conversation=false;m->reset_armed=false;
        m->book=(unsigned)(action-WT_BOOK_0);
        load_word(m,m->offset);
        if(!m->word_loaded)break;
        m->view.page=WT_WORD;hint(m,"点发音听读音，点问老师让 AI 讲这个词");break;
    case WT_WORD_PREV:
        if(m->view.page!=WT_WORD||!m->word_loaded)return false;
        load_word(m,m->offset>0?m->offset-1:0);break;
    case WT_WORD_NEXT:
        if(m->view.page!=WT_WORD||!m->word_loaded)return false;
        load_word(m,m->offset+1);break;
    case WT_SPEAK_WORD:
        if(m->view.page!=WT_WORD&&m->view.page!=WT_READING)return false;
        if(!m->word_loaded)return false;
        if(!m->view.online){hint(m,"第一次发音需要联网，请先连接网络");break;}
        command(m,out,WT_CMD_SPEAK_WORD);snprintf(out->text,sizeof(out->text),"%s",m->view.word);break;
    case WT_SPEAK_SENT:
        if(m->view.page!=WT_WORD)return false;
        if(!m->word_loaded||!m->view.sent_en[0]){hint(m,"这个词没有例句");break;}
        if(!m->view.online){hint(m,"第一次发音需要联网，请先连接网络");break;}
        command(m,out,WT_CMD_SPEAK_SENT);snprintf(out->text,sizeof(out->text),"s:%s",m->view.sent_en);break;
    case WT_TEACH: {
        if(m->view.page!=WT_WORD||!m->word_loaded)return false;
        if(!teacher_ready(m))break;
        char prompt[WT_VOICE_TEXT_CAP];
        build_teach_prompt(prompt,sizeof(prompt),&m->view);
        ask(m,out,prompt,false);break;
    }
    case WT_QUICK_0: case WT_QUICK_1: case WT_QUICK_2: case WT_QUICK_3: {
        if(m->view.page!=WT_WORD&&m->view.page!=WT_READING)return false;
        if(!m->word_loaded)return false;
        if(!teacher_ready(m))break;
        char prompt[WT_VOICE_TEXT_CAP];
        snprintf(prompt,sizeof(prompt),"关于单词「%s」（释义：%s）：%s",
            m->view.word,m->view.trans,quick_questions[action-WT_QUICK_0]);
        ask(m,out,prompt,m->view.page==WT_READING);break;
    }
    case WT_MARK_KNOWN: {
        if(m->view.page!=WT_WORD||!m->word_loaded)return false;
        mark_mastered(m,m->offset,true);
        wt_book_t book;
        wt_manifest_book_at(m->blob,m->book,&book);
        uint32_t next=wt_progress_next_unmastered(m->bits[m->book],book.count,m->offset);
        present_word(m);
        if(next>=book.count){hint(m,"这本书全部掌握！回首页换一本吧");m->view.page=WT_HOME;break;}
        load_word(m,next);
        hint(m,"已记住，自动跳到下一个未掌握的词");break;
    }
    case WT_OPEN_QUIZ:
        if(!m->blob){hint(m,"词库不可用");break;}
        if(!m->word_loaded){wt_book_t book;if(!wt_manifest_book_at(m->blob,m->book,&book))break;
            load_word(m,wt_progress_next_unmastered(m->bits[m->book],book.count,0));
            if(!m->word_loaded)break;}
        m->conversation=false;m->reset_armed=false;
        start_quiz(m,m->offset);break;
    case WT_QUIZ_PICK_0: case WT_QUIZ_PICK_1: case WT_QUIZ_PICK_2: case WT_QUIZ_PICK_3: {
        // wt_quiz_answer 的返回值是"是否答对"而不是"是否受理"：答错也要记分。
        if(m->view.page!=WT_QUIZ||!wt_quiz_ready(&m->quiz))return false;
        unsigned slot=(unsigned)(action-WT_QUIZ_PICK_0);
        bool correct=wt_quiz_answer(&m->quiz,slot);
        ++m->quiz_total;
        if(correct)++m->quiz_score;
        if(m->quiz.phase==WT_QUIZ_PHASE_CORRECT)mark_mastered(m,m->quiz.answer_offset,true);
        refresh_quiz(m);++m->view.revision;return true;
    }
    case WT_QUIZ_NEXT:
        if(m->view.page!=WT_QUIZ||!wt_quiz_answered(&m->quiz))return false;
        quiz_next(m);break;
    case WT_OPEN_PROGRESS:
        m->reset_armed=false;m->view.reset_armed=false;fill_progress(m);m->view.page=WT_PROGRESS;hint(m,"点两次「清空进度」才会真的重置");break;
    case WT_RESET:
        if(m->view.page!=WT_PROGRESS)return false;
        if(!m->reset_armed){m->reset_armed=true;m->view.reset_armed=true;hint(m,"再点一次确认清空全部进度");++m->view.revision;return true;}
        m->reset_armed=false;m->view.reset_armed=false;
        for(unsigned i=0;i<WT_BOOKS;++i){
            wt_book_t book;
            if(m->bits[i]&&wt_manifest_book_at(m->blob,i,&book)){
                wt_progress_clear(m->bits[i],book.count);
                wt_platform_progress_save(book.key,m->bits[i],book.count);
            }
        }
        m->quiz_score=m->quiz_total=0;fill_progress(m);
        hint(m,"进度已清空");break;
    case WT_SEND_TEXT:
        if(m->view.page!=WT_VOICE_REVIEW||!m->view.transcript[0])return false;
        if(!m->view.online||!m->view.configured){hint(m,"请先完成联网与模型配置");++m->view.revision;return true;}
        ++m->view.generation;command(m,out,WT_CMD_SEND_TEXT);
        snprintf(out->anchor,sizeof(out->anchor),"%s",m->view.word);
        snprintf(out->text,sizeof(out->text),"%s",m->view.transcript);
        m->view.page=WT_WAITING;m->awaiting_transcript=false;
        hint(m,"已确认发送，正在思考");break;
    case WT_ASK:
        if(m->view.page==WT_HOME)m->conversation=false;
        cancel(m, out); m->view.page = WT_VOICE; m->view.transcript[0]=0;
        hint(m, m->conversation?"继续问这个词 · 按住 AI 键说话":"按住 AI 键或点声波录音，识别后确认"); break;
    case WT_BACK:
    case WT_CANCEL: {
        wt_page_t target=m->view.page==WT_SETTINGS?m->return_page:
            m->view.page==WT_WORD||m->view.page==WT_QUIZ||m->view.page==WT_PROGRESS?WT_HOME:
            m->view.page==WT_READING?WT_WORD:
            m->conversation&&(m->view.page==WT_VOICE||m->view.page==WT_WAITING||m->view.page==WT_VOICE_REVIEW)?WT_READING:WT_HOME;
        if(target==WT_HOME){m->conversation=false;fill_progress(m);}
        cancel(m, out);m->view.page=target;
        if(target==WT_READING)page_lines(m);
        hint(m, m->simulated ? "模拟预览 · 不连接云端" :
            target==WT_WORD?"点发音听读音，点问老师让 AI 讲这个词":"选择一本词书开始学习");
        break;
    }
    case WT_OPEN_SETTINGS:
        m->return_page = m->view.page==WT_READING||m->view.page==WT_VOICE_REVIEW?m->view.page:WT_HOME;
        cancel(m, out); m->view.page = WT_SETTINGS; hint(m, "家长用手机连接设备热点后填写密钥"); break;
    case WT_SETUP:
        if (m->view.page != WT_SETTINGS) return false;
        command(m, out, WT_CMD_SETUP); hint(m, "正在开启手机配置，请稍候"); break;
    case WT_PREVIOUS:
    case WT_NEXT:
        if(m->view.page==WT_VOICE_REVIEW){
            if(action==WT_NEXT&&m->view.review_index+1<m->view.review_count)++m->view.review_index;
            else if(action==WT_PREVIOUS&&m->view.review_index)--m->view.review_index;
            else return false;
            break;
        }
        if (m->view.page != WT_READING) return false;
        if (action == WT_NEXT && m->view.page_index + 1 < m->view.page_count) ++m->view.page_index;
        else if (action == WT_PREVIOUS && m->view.page_index) --m->view.page_index;
        else return false;
        cancel(m, out); page_lines(m); break;
    case WT_EXPLAIN:
        if (m->view.page != WT_READING) return false;
        if(!m->word_loaded)return false;
        ++m->view.generation; m->view.speaking = false;
        m->view.transcript[0]=0;
        command(m, out, WT_CMD_EXPLAIN);
        snprintf(out->anchor,sizeof(out->anchor),"%s",m->view.word);
        snprintf(out->text,sizeof(out->text),
            "关于单词「%s」（释义：%s），这一页我没看懂，请换一种方式再讲一遍：\n",
            m->view.word,m->view.trans);
        for (unsigned i = 0; i < WT_LINES; ++i) {
            size_t used = strlen(out->text);
            snprintf(out->text + used, sizeof(out->text) - used, "%s\n", m->view.lines[i]);
        }
        m->view.page = WT_WAITING; hint(m, "换一种方式解释这一页，可以取消"); m->retry = WT_ASK; break;
    case WT_READ:
        if (m->view.page != WT_READING) return false;
        if (m->view.speaking) { cancel(m, out); break; }
        // Each explicit read owns a fresh cancellation generation. Muted only
        // suppresses the UI's automatic read; it does not block this action.
        ++m->view.generation;
        command(m, out, WT_CMD_SPEAK);
        for (unsigned i = 0; i < WT_LINES; ++i) {
            size_t used = strlen(out->text);
            snprintf(out->text + used, sizeof(out->text) - used, "%s\n", m->view.lines[i]);
        }
        m->view.speaking = true; break;
    case WT_MUTE:
        m->view.muted = !m->view.muted; m->view.speaking = false;
        command(m, out, WT_CMD_MUTE); break;
    case WT_RETRY:
        if (m->view.page != WT_ERROR) return false;
        return wt_model_action(m, m->retry, out);
    default: return false;
    }
    ++m->view.revision; return true;
}
static void fill_lines(wt_model_handle_t m,const char *p,const char *end)
{
    memset(m->view.lines,0,sizeof(m->view.lines));
    unsigned line=0,width=0;size_t used=0;
    while(p<end&&line<WT_LINES){
        unsigned char c=(unsigned char)*p;size_t bytes=c<0x80?1:c<0xe0?2:c<0xf0?3:4;
        if(p+bytes>end)break;
        if(*p=='\r'){++p;continue;}
        if(*p=='\n'){++p;++line;width=0;used=0;continue;}
        if(width==LINE_COLUMNS||used+bytes>=WT_LINE_CAP){++line;width=0;used=0;if(line==WT_LINES)break;}
        memcpy(m->view.lines[line]+used,p,bytes);used+=bytes;++width;p+=bytes;
    }
}
static void page_lines(wt_model_handle_t m)
{
    if(m->view.page_count)fill_lines(m,m->answer+m->offsets[m->view.page_index],m->answer+m->offsets[m->view.page_index+1]);
}
static void review_lines(wt_model_handle_t m)
{fill_lines(m,m->view.transcript+m->review_offsets[m->view.review_index],m->view.transcript+m->review_offsets[m->view.review_index+1]);}
static unsigned paginate_offsets(const char *text,size_t *offsets)
{
    size_t i=0;unsigned line=0,width=0,pages=0;offsets[pages++]=0;
    while(text[i]){
        unsigned char c=(unsigned char)text[i];size_t bytes=c<0x80?1:c<0xe0?2:c<0xf0?3:4;
        if(c=='\r'){++i;continue;}
        if(c=='\n'||width==LINE_COLUMNS){
            ++line;width=0;if(c=='\n')++i;
            if(line>=WT_LINES){if(pages>=WT_MAX_PAGES||!text[i])break;offsets[pages++]=i;line=0;}
            if(c=='\n')continue;
        }
        ++width;i+=bytes;
    }
    offsets[pages]=i;return pages;
}
static void paginate(wt_model_handle_t m)
{m->view.page_count=paginate_offsets(m->answer,m->offsets);m->view.page_index=0;page_lines(m);}
void wt_model_message(wt_model_handle_t m, const wt_message_t *msg)
{
    if (!m || !msg) return;
    if(!valid_utf8(msg->text,sizeof(msg->text))){
        if(msg->generation==m->view.generation){m->view.page=WT_ERROR;m->view.recording=false;m->view.speaking=false;hint(m,"回答文字格式异常，请重新提问");++m->view.revision;}
        return;
    }
    if (msg->kind == WT_MSG_STATUS) {
        m->view.online = msg->online; m->view.configured = msg->configured;
        m->view.api_state=msg->online?msg->api_state:WT_API_UNKNOWN;
        if (msg->text[0]) hint(m, msg->text);
    } else {
        if (msg->generation != m->view.generation) return;
        switch (msg->kind) {
        case WT_MSG_SETUP: {
            memset(m->view.setup_lines,0,sizeof(m->view.setup_lines));
            const char *p=msg->text;
            for(unsigned i=0;i<3&&*p;++i){
                const char *end=strchr(p,'\n');size_t n=end?(size_t)(end-p):strlen(p);
                if(n>=WT_LINE_CAP)n=WT_LINE_CAP-1;
                memcpy(m->view.setup_lines[i],p,n);p=end?end+1:p+n;
            }
            m->view.page=WT_SETTINGS;hint(m,"用手机连接此热点后打开配置页");break;
        }
        case WT_MSG_ANSWER:
            if(m->view.page!=WT_WAITING)return;
            if (!msg->text[0]) { m->view.page = WT_ERROR; hint(m, "没有收到完整回答，请重试"); break; }
            snprintf(m->answer, sizeof(m->answer), "%s", msg->text);
            m->conversation=true;
            m->view.page = WT_READING; m->view.speaking = false; m->view.recording=false;
            paginate(m);
            hint(m, "可以翻页，也可以按住 AI 键追问"); break;
        case WT_MSG_TRANSCRIPT: {
            if(!m->awaiting_transcript||m->view.page!=WT_WAITING)return;
            m->awaiting_transcript=false;
            size_t n=strlen(msg->text);
            if(!n||n>=sizeof(m->view.transcript)){m->view.page=WT_ERROR;hint(m,"识别内容为空或过长，请分段重说");break;}
            memcpy(m->view.transcript,msg->text,n+1);
            m->view.review_count=paginate_offsets(m->view.transcript,m->review_offsets);m->view.review_index=0;
            if(m->review_offsets[m->view.review_count]!=n){m->view.page=WT_ERROR;hint(m,"识别内容分段过多，请重新简短说一次");break;}
            m->view.page=WT_VOICE_REVIEW;review_lines(m);
            hint(m,"请确认识别文字，再点击发送");break;
        }
        case WT_MSG_ERROR:
            m->view.page = WT_ERROR; m->view.recording = false; m->view.speaking = false;
            hint(m, msg->text[0] ? msg->text : "操作失败，请重试"); break;
        case WT_MSG_SPEECH_DONE: m->view.speaking = false; if (msg->text[0]) hint(m, msg->text); break;
        default: break;
        }
    }
    ++m->view.revision;
}
bool wt_model_tick(wt_model_handle_t m, unsigned elapsed, wt_command_t *out)
{
    if (!m || !out || !m->view.recording) return false;
    m->view.recording_ms += elapsed;
    if (m->view.recording_ms >= WT_MAX_RECORD_MS) {
        cancel(m, out); m->view.page = WT_VOICE; m->view.transcript[0]=0;
        hint(m, "录音已达 30 秒，请松开后重新分段提问"); ++m->view.revision; return true;
    }
    return false;
}
void wt_model_snapshot(wt_model_handle_t m, wt_snapshot_t *out)
{ if (m && out) *out = m->view; }
