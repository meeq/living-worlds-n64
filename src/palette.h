#pragma once

#include <stdint.h>

#include "scene.h"   /* for lw_header_t, lw_cycle_t, lw_tl_t */

#define MAX_COLORS 256

/* Adopt a new scene's palette data and reset the cycling clock. Called by
 * scene_load() with pointers derived from the freshly-loaded asset. */
void palette_set_scene(const lw_header_t *h, const uint8_t *pal_table,
                       const lw_cycle_t *cycles, const lw_tl_t *timeline);

/* Advance the (pausable) clock that drives color cycling. */
void palette_tick(uint32_t dms);

/* Rebuild the TLUT for time-of-day `t`: lerp the bracketing keyframed
 * palettes, then apply every active cycle on top. */
void palette_rebuild(float t);

/* Upload the freshly-rebuilt TLUT to the RDP. */
void palette_upload(void);
