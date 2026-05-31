#include "save.h"

#include <stdint.h>
#include <string.h>

#include <libdragon.h>

#include "clock.h"
#include "scene.h"
#include "settings.h"

/* Two EEPROM blocks (16 bytes). The trailing 4 bytes are reserved so we can
 * extend without bumping the magic. The libdragon EEPROM driver coalesces
 * background writes; magic + version gate prevents reading garbage from a
 * blank/foreign cart. */
typedef union {
    struct __attribute__((packed)) {
        unsigned cycling    : 1;
        unsigned blendshift : 1;
        unsigned sound_on   : 1;
        unsigned _reserved  : 5;
    };
    uint8_t raw;
} lw_save_flags_t;
_Static_assert(sizeof(lw_save_flags_t) == 1, "lw_save_flags_t must pack into a single byte");

typedef struct {
    char            magic[4];        /* "LWS2" */
    lw_save_flags_t flags;
    uint8_t         time_src;        /* time_src_t value */
    uint8_t         scene_idx;       /* index into the sorted scene catalog */
    uint8_t         day_speed_idx;   /* index into DAY_SPEED_SECS */
    uint32_t        time_of_day_s;   /* only meaningful when time_src == TIME_HOLD */
    uint32_t        reserved;
} lw_save_t;                         /* sizeof == 16 */
_Static_assert(sizeof(lw_save_t) == 16, "lw_save_t must be 16 bytes (2 EEPROM blocks)");

void save_load(void)
{
    if (eeprom_present() == EEPROM_NONE) return;
    lw_save_t s;
    eeprom_read_bytes(&s, 0, sizeof(s));
    if (memcmp(s.magic, "LWS2", 4) != 0) return;

    cycling    = s.flags.cycling;
    blendshift = s.flags.blendshift;
    sound_on   = s.flags.sound_on;
    if (s.time_src < TIME_SRC_COUNT)         time_src      = (time_src_t)s.time_src;
    if (s.scene_idx < (unsigned)scene_count) scene_idx     = s.scene_idx;
    if (s.time_of_day_s < SECS_PER_DAY)      time_of_day   = (float)s.time_of_day_s;
    if (s.day_speed_idx < DAY_SPEED_N)       day_speed_idx = s.day_speed_idx;
}

void save_flush(void)
{
    if (!save_dirty) return;
    save_dirty = false;
    if (eeprom_present() == EEPROM_NONE) return;

    lw_save_t s = {
        .magic         = { 'L', 'W', 'S', '2' },
        .flags         = { .cycling = cycling, .blendshift = blendshift, .sound_on = sound_on },
        .time_src      = (uint8_t)time_src,
        .scene_idx     = (uint8_t)scene_idx,
        .day_speed_idx = (uint8_t)day_speed_idx,
        .time_of_day_s = (uint32_t)time_of_day,
        .reserved      = 0,
    };
    eeprom_write_bytes(&s, 0, sizeof(s));
}
