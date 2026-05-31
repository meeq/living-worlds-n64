#include "clock.h"

#include <math.h>
#include <time.h>

#include "settings.h"

bool rtc_present = false;

int rtc_seconds_of_day(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    return tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec;
}

float wrap_time_of_day(float t)
{
    t = fmodf(t, (float)SECS_PER_DAY);
    return t < 0 ? t + SECS_PER_DAY : t;
}

void scrub_time_of_day(float delta_s)
{
    time_of_day = wrap_time_of_day(time_of_day + delta_s);
    time_src    = TIME_HOLD;
    save_dirty  = true;
}

void advance_time_of_day(float dt)
{
    if (time_src == TIME_AUTO)
        time_of_day += (float)SECS_PER_DAY * dt / (float)DAY_SPEED_SECS[day_speed_idx];
    else if (time_src == TIME_RTC)
        time_of_day = rtc_seconds_of_day();
    time_of_day = wrap_time_of_day(time_of_day);
}

const char *time_src_label(void)
{
    switch (time_src) {
        case TIME_AUTO: return "AUTO";
        case TIME_RTC:  return rtc_present ? "RTC" : "RTC*";
        default:        return "HOLD";
    }
}
