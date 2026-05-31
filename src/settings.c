#include "settings.h"

#include <stdio.h>

bool       cycling       = true;
bool       blendshift    = true;
bool       sound_on      = true;
float      time_of_day   = SECS_MID_DAY;
time_src_t time_src      = TIME_AUTO;
int        day_speed_idx = DAY_SPEED_DEFAULT;
int        scene_idx     = 0;
bool       save_dirty    = false;

const int DAY_SPEED_SECS[DAY_SPEED_N] = { 1200, 600, 300, 120, 60, 30, 15 };

const char *day_speed_label(int idx)
{
    static char buf[16];
    int s = DAY_SPEED_SECS[idx];
    if      (s >= 3600 && s % 3600 == 0) snprintf(buf, sizeof(buf), "%dh", s / 3600);
    else if (s >= 60   && s % 60   == 0) snprintf(buf, sizeof(buf), "%dm", s / 60);
    else                                 snprintf(buf, sizeof(buf), "%ds", s);
    return buf;
}
