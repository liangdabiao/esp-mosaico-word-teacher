// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "wt_controls.h"
wt_controls_t wt_controls_defaults(void)
{return (wt_controls_t){.brightness=80,.volume=80,.speech_enabled=true,.automatic=true,.grade=WT_GRADE_DEFAULT};}
const char *wt_grade_name(unsigned grade)
{
    static const char *const names[]={"一年级","二年级","三年级","四年级","五年级","六年级","初一","初二","初三","高一","高二","高三"};
    return names[(grade>=WT_GRADE_MIN&&grade<=WT_GRADE_MAX?grade:WT_GRADE_DEFAULT)-1];
}
bool wt_controls_change(wt_controls_t *v,wt_control_t c,int value,unsigned rotation)
{
    if(!v)return false;
    switch(c){
    case WT_CONTROL_BRIGHTNESS:if(value<10||value>100)return false;v->brightness=(unsigned)value;break;
    case WT_CONTROL_VOLUME:if(value<0||value>100)return false;v->volume=(unsigned)value;break;
    case WT_CONTROL_SPEECH:if(value!=0&&value!=1)return false;v->speech_enabled=value!=0;break;
    case WT_CONTROL_GRADE:if(value<WT_GRADE_MIN||value>WT_GRADE_MAX)return false;v->grade=(unsigned)value;break;
    case WT_CONTROL_AUTO:
        if((value!=0&&value!=1)||rotation>270||rotation%90)return false;
        v->automatic=value!=0;if(!v->automatic)v->fixed_rotation=rotation;break;
    case WT_CONTROL_ACCENT:if(value!=0&&value!=1)return false;v->accent=value!=0;break;
    default:return false;
    }
    return true;
}
uint32_t wt_controls_pack(const wt_controls_t *v)
{return 0xa5000000U|v->brightness|(v->volume<<7)|((uint32_t)v->speech_enabled<<14)|
    ((uint32_t)v->automatic<<15)|((v->fixed_rotation/90)<<16)|(v->grade<<18)|
    ((uint32_t)(v->accent?1:0)<<22);}
bool wt_controls_unpack(uint32_t p,wt_controls_t *v)
{
    if(!v||(p&0xffc00000U)!=0xa5000000U)return false;
    wt_controls_t n={.brightness=p&127U,.volume=(p>>7)&127U,
        .speech_enabled=(p&(1U<<14))!=0,.automatic=(p&(1U<<15))!=0,.fixed_rotation=((p>>16)&3U)*90};
    n.grade=(p>>18)&15U;if(!n.grade)n.grade=WT_GRADE_DEFAULT;
    n.accent=(p&(1U<<22))!=0;
    if(n.brightness<10||n.brightness>100||n.volume>100||n.grade>WT_GRADE_MAX)return false;
    *v=n;return true;
}
