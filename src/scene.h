#pragma once

#include <stdint.h>

#include <libdragon.h>

#define SLUG_MAX 16

typedef struct {
    char     magic[4];                 /* "LWL3" */
    uint16_t width, height, num_colors, num_cycles, num_palettes, num_tl;
    uint32_t pixel_offset;
    char     title[64];                /* NUL-terminated UTF-8 */
    char     audio_slug[SLUG_MAX];     /* NUL-terminated ASCII, "" == silent */
    uint16_t audio_volume_q8;          /* max volume * 256 */
    uint16_t pad;
} lw_header_t;                         /* sizeof == 104 */

typedef struct {
    uint8_t  reverse, low, high, pad;
    uint16_t rate, pad2;
} lw_cycle_t;                          /* sizeof == 8 */

typedef struct {
    uint32_t off;                      /* seconds since midnight */
    uint16_t pidx, pad;
} lw_tl_t;                             /* sizeof == 8 */

extern const lw_header_t *hdr;         /* current scene's header */
extern surface_t          idx_surf;    /* FMT_CI8 view over the pixel bytes */
extern int                scene_count;

/* Enumerate rom:/ for *.lw files into the catalog; assertf if none found. */
void scene_init(void);

/* Load scene `idx` from the catalog, updating scene_idx and crossfading
 * its audio loop. */
void scene_load(int idx);

/* Wrap `delta` around the catalog and scene_load() the resulting index. */
void scene_advance(int delta);
