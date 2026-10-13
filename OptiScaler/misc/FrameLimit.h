#pragma once
#include "SysUtils.h"

class FrameLimit
{
    static uint64_t get_timestamp();
    static int timer_sleep(int64_t hundred_ns);
    static int busywait_sleep(int64_t ns);
    static int combined_sleep(int64_t ns);

  public:
    // The cap counts shown frames: `shownPerRendered` is how many frame generation shows per frame the game renders (1 when
    // it generates none)
    static void sleep(uint32_t shownPerRendered = 1);
};
