// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "wt_controls.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    wt_controls_t v=wt_controls_defaults(),loaded;
    assert(v.brightness==80&&v.volume==50&&v.speech_enabled&&v.automatic);
    assert(!wt_controls_change(&v,WT_CONTROL_BRIGHTNESS,0,0));
    assert(!wt_controls_change(&v,WT_CONTROL_VOLUME,101,0));
    assert(wt_controls_change(&v,WT_CONTROL_BRIGHTNESS,10,0));
    assert(wt_controls_change(&v,WT_CONTROL_VOLUME,0,0));
    assert(wt_controls_change(&v,WT_CONTROL_SPEECH,0,0));
    assert(wt_controls_change(&v,WT_CONTROL_AUTO,0,270));
    assert(wt_controls_change(&v,WT_CONTROL_VOLUME,75,90)&&v.fixed_rotation==270);
    assert(wt_controls_unpack(wt_controls_pack(&v),&loaded));
    assert(loaded.brightness==10&&loaded.volume==75&&!loaded.speech_enabled&&!loaded.automatic&&loaded.fixed_rotation==270);
    assert(!wt_controls_unpack(0,&loaded));
    assert(!wt_controls_unpack(wt_controls_pack(&v)|(1U<<22),&loaded));
    assert(wt_controls_change(&v,WT_CONTROL_AUTO,1,0)&&v.automatic);
    assert(!wt_controls_change(&v,WT_CONTROL_GRADE,0,0));
    assert(!wt_controls_change(&v,WT_CONTROL_GRADE,13,0));
    for(unsigned grade=1;grade<=12;++grade){
        assert(wt_controls_change(&v,WT_CONTROL_GRADE,(int)grade,0));
        assert(wt_controls_unpack(wt_controls_pack(&v),&loaded)&&loaded.grade==grade);
    }
    assert(wt_controls_unpack(0xa500c000U|80U|(50U<<7),&loaded)&&loaded.grade==7);
    puts("PASS: brightness floor, volume range, speech switch, fixed orientation capture and saved settings roundtrip");
}
