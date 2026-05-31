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

/* Read EEPROM and apply to the globals above (silent no-op on carts without
 * EEPROM or with a stale/missing magic). Must run after scene_init() so the
 * persisted scene index can be range-checked. */
void settings_load(void);

/* Persist the current settings if `save_dirty` is set. Cheap memcpy-class
 * call into libdragon's RAM-cached EEPROM driver; rate-limited to once per
 * frame so a held scrub doesn't burst writes. */
void settings_flush(void);
