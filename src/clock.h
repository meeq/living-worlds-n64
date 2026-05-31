#pragma once

#include <stdbool.h>

typedef enum { TIME_AUTO, TIME_RTC, TIME_HOLD, TIME_SRC_COUNT } time_src_t;

extern bool rtc_present;

/* Seconds since midnight from the RTC subsystem (hardware clock if present,
 * software clock otherwise -- either way time(NULL) is hooked by rtc_init). */
int   rtc_seconds_of_day(void);

/* Wrap a time-of-day value into the [0, 86400) range. */
float wrap_time_of_day(float t);

/* Shift time_of_day by delta_s and pin source to TIME_HOLD. */
void  scrub_time_of_day(float delta_s);

/* Per-frame clock tick. AUTO advances at the selected DAY_SPEED preset;
 * RTC tracks the real clock; HOLD freezes. */
void  advance_time_of_day(float dt);

const char *time_src_label(void);
