// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// wt_ui —— GSP 视图层（learning-assistant 的 wt_ui 同骨架）。
// gsp_learn_* 访问器由 GSPC 从 ui/main.json 的 bind/callback 生成；
// UI 任务独占模型，50ms tick 拉快照渲染，事件回调查模型动作。
#include "wt_ui.h"
#include "wt_model.h"
#include "wt_platform.h"
#include "bundle_gsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TICK_MS = 50, PANEL_HEIGHT=480, PANEL_EDGE=24, PANEL_COMMIT=80, GRADE_ROWS=4, GRADE_ROW_HEIGHT=48,
       GRADE_TOP=246, GRADE_LEFT=24, GRADE_WIDTH=432, WAVE_PHASES=8, WAVE_STEP_MS=100 };
struct wt_ui_t {
    esp_gsp_handle_t ui;
    wt_model_handle_t model;
    wt_snapshot_t view;
    wt_message_t message;
    wt_command_t command;
    char error_text[384];
    void *timer;
    bool key, rendered;
    bool controls_rendered;
    bool panel_open,pointer_down,panel_drag,drag_was_open,grade_open;
    int press_y,last_y;
    unsigned wave_phase,wave_elapsed;
    unsigned grade_offset;
    int press_x;bool grade_drag;
    wt_controls_t controls;
    esp_gsp_err_t error;
};
static void checked(wt_ui_handle_t s, esp_gsp_err_t err)
{ if (err != ESP_GSP_OK) { s->error = err; fprintf(stderr, "wt_ui: error=%d\n", err); } }
static void render_grades(wt_ui_handle_t s)
{
    static esp_gsp_err_t (*const text[])(esp_gsp_handle_t,const char*)={gsp_learn_grade_item_0_set_text,gsp_learn_grade_item_1_set_text,gsp_learn_grade_item_2_set_text,gsp_learn_grade_item_3_set_text};
    static esp_gsp_err_t (*const color[])(esp_gsp_handle_t,uint32_t)={gsp_learn_grade_pick_0_set_color_rgb888,gsp_learn_grade_pick_1_set_color_rgb888,gsp_learn_grade_pick_2_set_color_rgb888,gsp_learn_grade_pick_3_set_color_rgb888};
    wt_controls_t v;wt_platform_controls_get(&v);
    for(unsigned i=0;i<GRADE_ROWS;++i){unsigned grade=s->grade_offset+i+1;
        checked(s,text[i](s->ui,wt_grade_name(grade)));
        checked(s,color[i](s->ui,grade==v.grade?0xC9F4E9:0xF4FCFD));
    }
}
static void panel_target(wt_ui_handle_t s,bool open)
{
    if(open){s->grade_open=false;checked(s,gsp_learn_grade_menu_set_visible(s->ui,false));}
    s->panel_open=open;
    checked(s,gsp_learn_quick_settings_set_visible(s->ui,open));
}
static void pointer(esp_gsp_handle_t ui,int32_t x,int32_t y,bool pressed,void *ctx)
{
    (void)ui;(void)x;wt_ui_handle_t s=ctx;
    if(pressed){
        if(!s->pointer_down){s->pointer_down=true;s->press_y=y;s->press_x=x;s->grade_drag=false;s->drag_was_open=s->panel_open;}
        s->last_y=y;int dy=y-s->press_y;
        bool edge=!s->drag_was_open?s->press_y<=PANEL_EDGE&&dy>PANEL_EDGE:
            s->press_y>=PANEL_HEIGHT-60&&dy<-PANEL_EDGE;
        if(edge)s->panel_drag=true;
        if(s->grade_open&&s->press_x>=GRADE_LEFT&&s->press_x<=GRADE_LEFT+GRADE_WIDTH&&s->press_y>=GRADE_TOP&&s->press_y<GRADE_TOP+GRADE_ROWS*GRADE_ROW_HEIGHT&&(dy>16||dy<-16))s->grade_drag=true;
    }else if(s->pointer_down){
        if(s->grade_open&&s->grade_drag){int dy=s->last_y-s->press_y;int rows=dy>0?-(dy+GRADE_ROW_HEIGHT/2)/GRADE_ROW_HEIGHT:-(dy-GRADE_ROW_HEIGHT/2)/GRADE_ROW_HEIGHT;
            int offset=(int)s->grade_offset+rows;s->grade_offset=(unsigned)(offset<0?0:offset>WT_GRADE_MAX-GRADE_ROWS?WT_GRADE_MAX-GRADE_ROWS:offset);render_grades(s);
        }
        if(s->panel_drag){int dy=s->last_y-s->press_y;panel_target(s,s->drag_was_open?dy>-PANEL_COMMIT:dy>=PANEL_COMMIT);}
        s->pointer_down=false;s->panel_drag=false;
    }
}
bool wt_ui_quick_is_open(wt_ui_handle_t s){return s&&s->panel_open;}
static void animate_recording(wt_ui_handle_t s)
{
    static esp_gsp_err_t (*const visible[])(esp_gsp_handle_t,bool)={
        gsp_learn_wave_0_set_visible,gsp_learn_wave_1_set_visible,gsp_learn_wave_2_set_visible,
        gsp_learn_wave_3_set_visible,gsp_learn_wave_4_set_visible,gsp_learn_wave_5_set_visible,
        gsp_learn_wave_6_set_visible,gsp_learn_wave_7_set_visible,gsp_learn_wave_8_set_visible};
    unsigned next=s->wave_phase;
    if(s->view.page==WT_VOICE&&s->view.recording){
        s->wave_elapsed+=TICK_MS;
        if(s->wave_elapsed>=WAVE_STEP_MS){s->wave_elapsed=0;next=1+s->wave_phase%WAVE_PHASES;}
    }else{next=0;s->wave_elapsed=0;}
    if(next!=s->wave_phase){
        checked(s,visible[s->wave_phase](s->ui,false));
        checked(s,visible[next](s->ui,true));s->wave_phase=next;
    }
}
static void render_controls(wt_ui_handle_t s,const wt_controls_t *v)
{
    char text[16];
    if(!s->controls_rendered||v->brightness!=s->controls.brightness){
        checked(s,gsp_learn_quick_brightness_set_value(s->ui,(int32_t)v->brightness));
        snprintf(text,sizeof(text),"%u%%",v->brightness);checked(s,gsp_learn_brightness_percent_set_text(s->ui,text));
    }
    if(!s->controls_rendered||v->volume!=s->controls.volume){
        checked(s,gsp_learn_quick_volume_set_value(s->ui,(int32_t)v->volume));
        snprintf(text,sizeof(text),"%u%%",v->volume);checked(s,gsp_learn_volume_percent_set_text(s->ui,text));
    }
    if(!s->controls_rendered||v->speech_enabled!=s->controls.speech_enabled){
        checked(s,gsp_learn_speech_on_set_visible(s->ui,v->speech_enabled));
        checked(s,gsp_learn_speech_off_set_visible(s->ui,!v->speech_enabled));
    }
    if(!s->controls_rendered||v->automatic!=s->controls.automatic){
        checked(s,gsp_learn_direction_auto_set_visible(s->ui,v->automatic));
        checked(s,gsp_learn_direction_locked_set_visible(s->ui,!v->automatic));
    }
    if(!s->controls_rendered||v->grade!=s->controls.grade){
        checked(s,gsp_learn_grade_label_set_text(s->ui,wt_grade_name(v->grade)));
    }
    s->controls=*v;s->controls_rendered=true;
}
// Labels use explicit bounded lines, matching the answer's 18-column layout.
static void wrap_progress(char *out,size_t capacity,const char *text)
{
    const unsigned char *p=(const unsigned char*)text;size_t used=0;unsigned col=0,lines=1;
    while(*p&&used+8<capacity){
        size_t n=*p<0x80?1:*p<0xe0?2:*p<0xf0?3:4;
        if(*p=='\r'){++p;continue;}
        if(col==18||*p=='\n'){
            if(lines==3){memcpy(out+used,"…",3);used+=3;break;}
            out[used++]='\n';col=0;++lines;if(*p=='\n'){++p;continue;}
        }
        memcpy(out+used,p,n);used+=n;p+=n;++col;
    }
    out[used]=0;
}
static void render(wt_ui_handle_t s)
{
    wt_snapshot_t *v = &s->view;
    if(v->page!=WT_SETTINGS&&s->grade_open){s->grade_open=false;checked(s,gsp_learn_grade_menu_set_visible(s->ui,false));}
    checked(s, gsp_learn_home_set_visible(s->ui, v->page == WT_HOME));
    checked(s, gsp_learn_word_set_visible(s->ui, v->page == WT_WORD));
    checked(s, gsp_learn_quiz_set_visible(s->ui, v->page == WT_QUIZ));
    checked(s, gsp_learn_progress_set_visible(s->ui, v->page == WT_PROGRESS));
    checked(s, gsp_learn_reading_set_visible(s->ui, v->page == WT_READING));
    checked(s, gsp_learn_voice_set_visible(s->ui, v->page == WT_VOICE));
    checked(s,gsp_learn_voice_review_set_visible(s->ui,v->page==WT_VOICE_REVIEW));
    checked(s, gsp_learn_waiting_set_visible(s->ui, v->page == WT_WAITING));
    checked(s, gsp_learn_settings_panel_set_visible(s->ui, v->page == WT_SETTINGS));
    checked(s, gsp_learn_error_panel_set_visible(s->ui, v->page == WT_ERROR));
    checked(s,gsp_learn_status_set_text(s->ui,v->page==WT_HOME||v->page==WT_SETTINGS||v->page==WT_VOICE||v->page==WT_VOICE_REVIEW||v->page==WT_PROGRESS?"":v->hint));
    if(v->page==WT_HOME){
        char text[80];
        for(unsigned i=0;i<WT_BOOKS;++i){
            static esp_gsp_err_t (*const name[])(esp_gsp_handle_t,const char*)={
                gsp_learn_book_name_0_set_text,gsp_learn_book_name_1_set_text,gsp_learn_book_name_2_set_text,
                gsp_learn_book_name_3_set_text,gsp_learn_book_name_4_set_text,gsp_learn_book_name_5_set_text};
            static esp_gsp_err_t (*const prog[])(esp_gsp_handle_t,const char*)={
                gsp_learn_book_prog_0_set_text,gsp_learn_book_prog_1_set_text,gsp_learn_book_prog_2_set_text,
                gsp_learn_book_prog_3_set_text,gsp_learn_book_prog_4_set_text,gsp_learn_book_prog_5_set_text};
            checked(s,name[i](s->ui,v->prog_name[i]));
            snprintf(text,sizeof(text),"已学 %u / %u",v->prog_learned[i],v->prog_total[i]);
            checked(s,prog[i](s->ui,v->prog_total[i]?text:"词库不可用"));
        }
    }
    checked(s,gsp_learn_wifi_dot_set_color_rgb888(s->ui,v->online?0x22C55E:0xA4AFB8));
    checked(s,gsp_learn_api_dot_set_color_rgb888(s->ui,v->online&&v->api_state==WT_API_READY?0x22C55E:
        v->online&&v->api_state==WT_API_ERROR?0xD79A32:0xA4AFB8));
    if(v->page==WT_WORD){
        checked(s,gsp_learn_word_book_set_text(s->ui,v->book_short));
        checked(s,gsp_learn_word_text_set_text(s->ui,v->word));
        checked(s,gsp_learn_word_ipa_set_text(s->ui,v->ipa));
        checked(s,gsp_learn_word_trans_set_text(s->ui,v->trans));
        checked(s,gsp_learn_word_sent_en_set_text(s->ui,v->sent_en));
        checked(s,gsp_learn_word_sent_cn_set_text(s->ui,v->sent_cn));
        char text[96];
        snprintf(text,sizeof(text),"%s · 第 %u / %u 个",v->book_name,v->word_pos,v->word_count);
        checked(s,gsp_learn_word_pos_set_text(s->ui,text));
    }
    if(v->page==WT_QUIZ){
        static esp_gsp_err_t (*const opt[])(esp_gsp_handle_t,const char*)={
            gsp_learn_quiz_opt_0_set_text,gsp_learn_quiz_opt_1_set_text,
            gsp_learn_quiz_opt_2_set_text,gsp_learn_quiz_opt_3_set_text};
        char text[48];
        snprintf(text,sizeof(text),"%u / %u",v->quiz_total?v->quiz_score:0,v->quiz_total);
        checked(s,gsp_learn_quiz_progress_set_text(s->ui,v->quiz_total?text:""));
        checked(s,gsp_learn_quiz_prompt_set_text(s->ui,v->quiz_prompt));
        for(unsigned i=0;i<WT_QUIZ_CHOICES;++i)checked(s,opt[i](s->ui,v->quiz_options[i]));
        if(v->quiz_answered){
            if(v->quiz_correct)snprintf(text,sizeof(text),"回答正确！");
            else snprintf(text,sizeof(text),"正确答案：%s",v->quiz_options[v->quiz_slot_of_answer]);
            checked(s,gsp_learn_quiz_result_set_text(s->ui,text));
        }else checked(s,gsp_learn_quiz_result_set_text(s->ui,""));
    }
    if(v->page==WT_PROGRESS){
        char text[64];
        snprintf(text,sizeof(text),v->streak?"连续学习 %u 天":"今天开始打卡吧",v->streak);
        checked(s,gsp_learn_prog_streak_set_text(s->ui,text));
        for(unsigned i=0;i<WT_BOOKS;++i){
            static esp_gsp_err_t (*const name[])(esp_gsp_handle_t,const char*)={
                gsp_learn_prog_name_0_set_text,gsp_learn_prog_name_1_set_text,gsp_learn_prog_name_2_set_text,
                gsp_learn_prog_name_3_set_text,gsp_learn_prog_name_4_set_text,gsp_learn_prog_name_5_set_text};
            static esp_gsp_err_t (*const count[])(esp_gsp_handle_t,const char*)={
                gsp_learn_prog_count_0_set_text,gsp_learn_prog_count_1_set_text,gsp_learn_prog_count_2_set_text,
                gsp_learn_prog_count_3_set_text,gsp_learn_prog_count_4_set_text,gsp_learn_prog_count_5_set_text};
            checked(s,name[i](s->ui,v->prog_name[i]));
            snprintf(text,sizeof(text),"%u / %u",v->prog_learned[i],v->prog_total[i]);
            checked(s,count[i](s->ui,text));
        }
        checked(s,gsp_learn_reset_label_set_text(s->ui,v->reset_armed?"再点一次确认清空":"清空进度"));
    }
    if (v->page == WT_READING) {
        checked(s,gsp_learn_read_label_set_text(s->ui,v->speaking?"停止":"朗读"));
        char text[48]; snprintf(text, sizeof(text), "%u / %u", v->page_index+1, v->page_count);
        checked(s, gsp_learn_page_hint_set_text(s->ui, text));
        checked(s,gsp_learn_previous_set_enabled(s->ui,v->page_index>0));
        checked(s,gsp_learn_next_set_enabled(s->ui,v->page_index+1<v->page_count));
        checked(s, gsp_learn_line_0_set_text(s->ui, v->lines[0]));
        checked(s, gsp_learn_line_1_set_text(s->ui, v->lines[1]));
        checked(s, gsp_learn_line_2_set_text(s->ui, v->lines[2]));
        checked(s, gsp_learn_line_3_set_text(s->ui, v->lines[3]));
        checked(s, gsp_learn_line_4_set_text(s->ui, v->lines[4]));
        checked(s, gsp_learn_line_5_set_text(s->ui, v->lines[5]));
    }
    if (v->page == WT_WAITING) {
        wrap_progress(s->error_text,sizeof(s->error_text),v->transcript[0]?v->transcript:v->hint);
        checked(s,gsp_learn_waiting_text_set_text(s->ui,s->error_text));
    }
    if(v->page==WT_VOICE){
        checked(s,gsp_learn_voice_record_label_set_text(s->ui,v->recording?"结束录音":"开始说话"));
    }
    if(v->page==WT_VOICE_REVIEW){
        checked(s,gsp_learn_review_instruction_set_text(s->ui,v->hint));
        char text[48];snprintf(text,sizeof(text),"%u / %u",v->review_index+1,v->review_count);
        checked(s,gsp_learn_review_page_set_text(s->ui,text));
        checked(s,gsp_learn_review_previous_set_enabled(s->ui,v->review_index>0));
        checked(s,gsp_learn_review_next_set_enabled(s->ui,v->review_index+1<v->review_count));
        checked(s,gsp_learn_review_line_0_set_text(s->ui,v->lines[0]));
        checked(s,gsp_learn_review_line_1_set_text(s->ui,v->lines[1]));
        checked(s,gsp_learn_review_line_2_set_text(s->ui,v->lines[2]));
        checked(s,gsp_learn_review_line_3_set_text(s->ui,v->lines[3]));
        checked(s,gsp_learn_review_line_4_set_text(s->ui,v->lines[4]));
        checked(s,gsp_learn_review_line_5_set_text(s->ui,v->lines[5]));
    }
    if (v->page == WT_SETTINGS) {
        checked(s, gsp_learn_network_set_text(s->ui, v->online ? "网络：已连接" : "网络：未连接"));
        checked(s, gsp_learn_model_status_set_text(s->ui, !v->configured?"API：尚未配置":v->api_state==WT_API_READY?"API：已连通":v->api_state==WT_API_ERROR?"API：连接异常":"API：已配置，待验证"));
        checked(s, gsp_learn_setup_a_set_text(s->ui, v->setup_lines[0]));
        checked(s, gsp_learn_setup_b_set_text(s->ui, v->setup_lines[1]));
        checked(s, gsp_learn_setup_c_set_text(s->ui, v->setup_lines[2]));
    }
    if (v->page == WT_ERROR) {
        const unsigned char *p=(const unsigned char*)v->hint;size_t used=0;unsigned count=0;
        while(*p&&used+6<sizeof(s->error_text)){
            size_t n=*p<0x80?1:*p<0xe0?2:*p<0xf0?3:4;
            if(count==18){s->error_text[used++]='\n';count=0;}
            memcpy(s->error_text+used,p,n);used+=n;p+=n;++count;
        }
        s->error_text[used]=0;
        checked(s,gsp_learn_error_text_set_text(s->ui,s->error_text));
    }
}
static void submit(wt_ui_handle_t s)
{
    if (s->command.kind != WT_CMD_NONE && !wt_platform_submit(&s->command)) {
        memset(&s->message, 0, sizeof(s->message));
        s->message.kind = WT_MSG_ERROR; s->message.generation = s->command.generation;
        snprintf(s->message.text, sizeof(s->message.text), "设备忙，请稍后重试");
        wt_model_message(s->model, &s->message);
    }
}
static void action(wt_ui_handle_t s, wt_action_t a)
{ if (wt_model_action(s->model, a, &s->command)) submit(s); }
static void event(esp_gsp_handle_t ui, const esp_gsp_event_t *e, void *ctx)
{
    (void)ui; wt_ui_handle_t s = ctx;
    if (e->type != ESP_GSP_EVENT_CALL) return;
    if(gsp_learn_event_is_grade_toggle(e)){
        s->grade_open=!s->grade_open;checked(s,gsp_learn_grade_menu_set_visible(s->ui,s->grade_open));
        if(s->grade_open){wt_controls_t v;wt_platform_controls_get(&v);s->grade_offset=v.grade>GRADE_ROWS?v.grade-GRADE_ROWS:0;render_grades(s);}
        return;
    }
    int grade_row=gsp_learn_event_is_grade_pick_0(e)?0:gsp_learn_event_is_grade_pick_1(e)?1:gsp_learn_event_is_grade_pick_2(e)?2:gsp_learn_event_is_grade_pick_3(e)?3:-1;
    if(grade_row>=0){
        if(!s->grade_open||s->grade_drag)return;
        wt_platform_controls_set(WT_CONTROL_GRADE,(int)s->grade_offset+grade_row+1);
        s->grade_open=false;checked(s,gsp_learn_grade_menu_set_visible(s->ui,false));return;
    }
    if(s->grade_open){s->grade_open=false;checked(s,gsp_learn_grade_menu_set_visible(s->ui,false));return;}
    if(gsp_learn_event_is_quick_close(e)){panel_target(s,false);return;}
    if(gsp_learn_event_is_quick_brightness(e)){wt_platform_controls_set(WT_CONTROL_BRIGHTNESS,(int32_t)e->arg);return;}
    if(gsp_learn_event_is_quick_volume(e)){wt_platform_controls_set(WT_CONTROL_VOLUME,(int32_t)e->arg);return;}
    if(gsp_learn_event_is_quick_speech(e)){action(s,WT_MUTE);return;}
    if(gsp_learn_event_is_direction(e)){
        wt_controls_t v;wt_platform_controls_get(&v);
        wt_platform_controls_set(WT_CONTROL_AUTO,!v.automatic);return;
    }
    if(gsp_learn_event_is_book_0(e))action(s,WT_BOOK_0);
    else if(gsp_learn_event_is_book_1(e))action(s,WT_BOOK_1);
    else if(gsp_learn_event_is_book_2(e))action(s,WT_BOOK_2);
    else if(gsp_learn_event_is_book_3(e))action(s,WT_BOOK_3);
    else if(gsp_learn_event_is_book_4(e))action(s,WT_BOOK_4);
    else if(gsp_learn_event_is_book_5(e))action(s,WT_BOOK_5);
    else if(gsp_learn_event_is_quiz_open(e))action(s,WT_OPEN_QUIZ);
    else if(gsp_learn_event_is_progress_open(e))action(s,WT_OPEN_PROGRESS);
    else if(gsp_learn_event_is_sound_word(e))action(s,WT_SPEAK_WORD);
    else if(gsp_learn_event_is_sound_sent(e))action(s,WT_SPEAK_SENT);
    else if(gsp_learn_event_is_mark_known(e))action(s,WT_MARK_KNOWN);
    else if(gsp_learn_event_is_teach(e))action(s,WT_TEACH);
    else if(gsp_learn_event_is_word_prev(e))action(s,WT_WORD_PREV);
    else if(gsp_learn_event_is_word_next(e))action(s,WT_WORD_NEXT);
    else if(gsp_learn_event_is_quiz_pick_0(e))action(s,WT_QUIZ_PICK_0);
    else if(gsp_learn_event_is_quiz_pick_1(e))action(s,WT_QUIZ_PICK_1);
    else if(gsp_learn_event_is_quiz_pick_2(e))action(s,WT_QUIZ_PICK_2);
    else if(gsp_learn_event_is_quiz_pick_3(e))action(s,WT_QUIZ_PICK_3);
    else if(gsp_learn_event_is_quiz_next(e))action(s,WT_QUIZ_NEXT);
    else if(gsp_learn_event_is_reset(e))action(s,WT_RESET);
    else if(gsp_learn_event_is_voice_toggle(e))action(s,WT_VOICE_TOGGLE);
    else if(gsp_learn_event_is_send_text(e))action(s,WT_SEND_TEXT);
    else if(gsp_learn_event_is_voice_retake(e))action(s,WT_ASK);
    else if (gsp_learn_event_is_followup(e)) action(s, WT_ASK);
    else if (gsp_learn_event_is_settings(e)){panel_target(s,false);action(s, WT_OPEN_SETTINGS);}
    else if (gsp_learn_event_is_next(e)) action(s, WT_NEXT);
    else if (gsp_learn_event_is_previous(e)) action(s, WT_PREVIOUS);
    else if (gsp_learn_event_is_explain(e)) action(s, WT_EXPLAIN);
    else if (gsp_learn_event_is_read(e)) action(s, WT_READ);
    else if (gsp_learn_event_is_setup(e)) action(s, WT_SETUP);
    else if (gsp_learn_event_is_retry(e)) action(s, WT_RETRY);
    else action(s, WT_BACK);
}
static void tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ui; wt_ui_handle_t s = ctx;
    uint32_t previous = s->view.revision;
    wt_model_snapshot(s->model,&s->view);
    wt_controls_t controls;wt_platform_controls_get(&controls);
    if(s->view.muted==controls.speech_enabled)action(s,WT_MUTE);
    bool key = wt_platform_key();
    if (key != s->key) { s->key = key; action(s, key ? WT_KEY_DOWN : WT_KEY_UP); }
    if (wt_model_tick(s->model, TICK_MS, &s->command)) submit(s);
    for (unsigned i = 0; i < 4 && wt_platform_poll(&s->message); ++i) {
        wt_model_message(s->model, &s->message);
        if(s->message.kind==WT_MSG_ANSWER){
            wt_model_snapshot(s->model,&s->view);
            if(s->message.generation==s->view.generation&&s->view.page==WT_READING&&!s->view.muted)action(s,WT_READ);
        }
    }
    wt_model_snapshot(s->model, &s->view);
    if (!s->rendered || previous != s->view.revision) { render(s); s->rendered = true; }
    wt_platform_controls_get(&controls);render_controls(s,&controls);
    animate_recording(s);
    wt_platform_presented(&s->view);
}
esp_gsp_err_t wt_ui_create(esp_gsp_handle_t ui, wt_ui_handle_t *out)
{
    if (!ui || !out) return ESP_GSP_ERR_INVALID_ARG;
    *out = NULL; wt_ui_handle_t s = calloc(1, sizeof(*s));
    if (!s) return ESP_GSP_ERR_NO_MEM;
    s->ui = ui; s->model = wt_model_create(wt_platform_simulated());
    if (!s->model) { free(s); return ESP_GSP_ERR_NO_MEM; }
    esp_gsp_err_t err = esp_gsp_on_event(ui, event, s);
    if (err != ESP_GSP_OK) { wt_ui_delete(s); return err; }
    err=esp_gsp_set_pointer_observer(ui,pointer,s);
    if(err!=ESP_GSP_OK){wt_ui_delete(s);return err;}
    s->timer = esp_gsp_timer_create(ui, TICK_MS, tick, s);
    if (!s->timer) { wt_ui_delete(s); return ESP_GSP_ERR_NO_MEM; }
    *out = s; return ESP_GSP_OK;
}
void wt_ui_delete(wt_ui_handle_t s)
{
    if (!s) return;
    if (s->timer) esp_gsp_timer_delete(s->ui, s->timer);
    esp_gsp_set_pointer_observer(s->ui,NULL,NULL);
    esp_gsp_on_event(s->ui, NULL, NULL);
    wt_model_delete(s->model); free(s);
}
