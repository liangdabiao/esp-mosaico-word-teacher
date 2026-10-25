// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "wt_conversation.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct wt_conversation_t {
    char word[64],first[WT_TEXT_CAP],last[WT_TEXT_CAP],question[1024];
};
esp_err_t wt_conversation_create(wt_conversation_handle_t *out)
{if(!out)return ESP_ERR_INVALID_ARG;*out=calloc(1,sizeof(**out));return *out?ESP_OK:ESP_ERR_NO_MEM;}
void wt_conversation_reset(wt_conversation_handle_t c)
{if(c)memset(c,0,sizeof(*c));}
void wt_conversation_delete(wt_conversation_handle_t c)
{wt_conversation_reset(c);free(c);}
void wt_conversation_select(wt_conversation_handle_t c,const char *selection)
{
    // 换一个锚点即视为重新开始一轮问答：清掉旧讲解（与上游语义一致）。
    if(!c)return;
    snprintf(c->word,sizeof(c->word),"%s",selection?selection:"");
    c->first[0]=c->last[0]=c->question[0]=0;
}
void wt_conversation_answer(wt_conversation_handle_t c,const char *q,const char *a)
{
    if(!c)return;
    if(!c->first[0])snprintf(c->first,sizeof(c->first),"%s",a);
    snprintf(c->last,sizeof(c->last),"%s",a);snprintf(c->question,sizeof(c->question),"%s",q);
}
void wt_conversation_history(wt_conversation_handle_t c,char *out,size_t cap)
{
    if(!c){if(cap)out[0]=0;return;}
    if(!c->first[0]){if(cap)snprintf(out,cap,"%s",c->word);return;}
    snprintf(out,cap,"正在学的单词：%s\n首次讲解：\n%s\n最近提问：\n%s\n最近回答：\n%s",c->word,c->first,c->question,c->last);
}
