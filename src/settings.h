#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "clock.h"

#define SECS_PER_DAY 86400
#define SECS_MID_DAY (SECS_PER_DAY / 2)

/* AUTO speed presets: how long a full simulated day takes in real seconds.
 * Each entry is a clean round duration (divides cleanly into hours, minutes,
 * or seconds) so day_speed_label can render it without mixed-unit values like
 * "1m30s". Default is 1 min/day -- a lively-but-watchable pace. */
#define DAY_SPEED_N        7
#define DAY_SPEED_DEFAULT  4   /* 60s -- 1 minute per simulated day */
extern const int DAY_SPEED_SECS[DAY_SPEED_N];

extern bool       cycling;
extern bool       blendshift;
extern bool       sound_on;
extern float      time_of_day;        /* seconds since midnight, [0, 86400) */
extern time_src_t time_src;
extern int        day_speed_idx;
extern int        scene_idx;
extern bool       save_dirty;         /* pending EEPROM flush */

/* Format DAY_SPEED_SECS[idx] as a compact human label ("1h", "20m", "30s"). */
const char *day_speed_label(int idx);

/* Wrap-around add for cycling through a fixed-size enum with -1/+1 dirs. */
static inline int cycle_enum(int v, int n, int dir) { return (v + n + dir) % n; }
