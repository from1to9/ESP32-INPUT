// SPDX-License-Identifier: Apache-2.0
#define main interaction_suite_main
#include "../test_interaction.c"
#undef main

static void wide_fraction_survives_only_transient_busyness(void)
{
    for(unsigned i=0;i<3;++i) {
        const unsigned divisor=i==0 ? 1U:i==1 ? 4U:8U;
        interaction_t m;setup(&m,INPUT_MOUSE);
        interaction_set_suppressed(&m,true,false,false,0);
        assert(interaction_set_touch_divisor(&m,divisor));
        input_state_t control=m.control;
        interaction_sync(&m,&control,false,10);
        interaction_set_touch_motion_ready(&m,true);
        assert(!m.output_ready && m.touch_motion_ready);
        int x=0,y=0;
        for(unsigned n=0;n<divisor;++n) {
            const interaction_touch_delta_t delta=interaction_touch_displacement_owned(&m,INPUT_LOCAL,301,-303);
            x+=delta.x;y+=delta.y;
            interaction_sync(&m,&control,false,10U+n);
            interaction_set_touch_motion_ready(&m,true);
        }
        assert(x==301 && y==-303 && m.touch_carry[0]==0 && m.touch_carry[1]==0);
        assert(!m.view.motion_enabled);
        control.locked=true;
        interaction_sync(&m,&control,false,30);interaction_set_touch_motion_ready(&m,true);
        const interaction_touch_delta_t blocked=interaction_touch_displacement_owned(&m,INPUT_LOCAL,300,300);
        assert(!m.touch_motion_ready && !blocked.x && !blocked.y);
    }
}

int main(void)
{
    assert(interaction_suite_main()==0);
    wide_fraction_survives_only_transient_busyness();
    puts("PASS: wide touch precision partitions, transient carry and actual unavailability gates");
    return 0;
}
