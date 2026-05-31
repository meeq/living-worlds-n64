#include "palette.h"

#include <math.h>

#include <libdragon.h>

#include "settings.h"

#define CYCLE_SPEED 280.0f    /* matches the reference engine (palette.js) */

uint16_t tlut[MAX_COLORS] __attribute__((aligned(16)));

static const lw_header_t *cur_hdr;
static const uint8_t     *pal_table;
static const lw_cycle_t  *cycles;
static const lw_tl_t     *timeline;
static uint8_t            base_r[MAX_COLORS];
static uint8_t            base_g[MAX_COLORS];
static uint8_t            base_b[MAX_COLORS];
static uint32_t           scene_ms;    /* pausable clock driving cycling */

void palette_set_scene(const lw_header_t *h, const uint8_t *pt,
                       const lw_cycle_t *cy, const lw_tl_t *tl)
{
    cur_hdr   = h;
    pal_table = pt;
    cycles    = cy;
    timeline  = tl;
    scene_ms  = 0;
}

void palette_tick(uint32_t dms) { scene_ms += dms; }

/* Mirrors the reference's setTimeOfDayPalette: per-channel lerp between the
 * two timeline palettes bracketing `t`, wrapping across midnight. */
static void rebuild_base_rgb(float t)
{
    int ti = (int)t % SECS_PER_DAY;
    if (ti < 0) ti += SECS_PER_DAY;

    int n_tl = cur_hdr->num_tl, n_col = cur_hdr->num_colors;
    int ai = 0;
    while (ai < n_tl && (int)timeline[ai].off <= ti) ai++;
    int bi = ai - 1;
    int boff, aoff;
    if (bi < 0)     { bi = n_tl - 1; boff = (int)timeline[bi].off - SECS_PER_DAY; }
    else            { boff = (int)timeline[bi].off; }
    if (ai >= n_tl) { ai = 0;        aoff = (int)timeline[ai].off + SECS_PER_DAY; }
    else            { aoff = (int)timeline[ai].off; }

    const uint8_t *bp = pal_table + (uint32_t)timeline[bi].pidx * n_col * 3;
    const uint8_t *ap = pal_table + (uint32_t)timeline[ai].pidx * n_col * 3;
    int span = aoff - boff;
    int frac = ti - boff;
    for (int i = 0; i < n_col; i++) {
        int r = bp[i * 3 + 0], g = bp[i * 3 + 1], bl = bp[i * 3 + 2];
        if (span > 0) {
            r  += (ap[i * 3 + 0] - r)  * frac / span;
            g  += (ap[i * 3 + 1] - g)  * frac / span;
            bl += (ap[i * 3 + 2] - bl) * frac / span;
        }
        base_r[i] = r;
        base_g[i] = g;
        base_b[i] = bl;
    }
}

/* Rebuild tlut from base_r/g/b, applying every active cycle. Mirrors
 * palette.js shiftColors/blendShiftColors: after rotating a range up by the
 * integer part of the amount, slot k blends toward its lower neighbour by
 * the fractional part (slot 0 wraps to the top of the range). */
static void rebuild_tlut(void)
{
    int slots = cur_hdr->num_colors;
    for (int i = 0; i < slots; i++)
        tlut[i] = color_to_packed16(RGBA32(base_r[i], base_g[i], base_b[i], 0xFF));

    if (cycling) {
        for (int ci = 0; ci < cur_hdr->num_cycles; ci++) {
            const lw_cycle_t *c = &cycles[ci];
            int size = c->high - c->low + 1;
            if (c->rate == 0 || size <= 1) continue;

            float rate_hz = c->rate / CYCLE_SPEED;
            float amt = fmodf((float)scene_ms * rate_hz / 1000.0f, (float)size);
            int   shift = (int)floorf(amt);
            float blend = blendshift ? (amt - shift) : 0.0f;

            for (int k = 0; k < size; k++) {
                int a = (k - shift + size) % size;     /* color now in slot k */
                int b = (a - 1 + size) % size;         /* neighbour blending in */
                int ia = c->low + a, ib = c->low + b;
                int r  = base_r[ia] + (int)((base_r[ib] - base_r[ia]) * blend);
                int g  = base_g[ia] + (int)((base_g[ib] - base_g[ia]) * blend);
                int bl = base_b[ia] + (int)((base_b[ib] - base_b[ia]) * blend);
                tlut[c->low + k] = color_to_packed16(RGBA32(r, g, bl, 0xFF));
            }
        }
    }

    data_cache_hit_writeback(tlut, slots * sizeof(uint16_t));
}

void palette_rebuild(float t)
{
    rebuild_base_rgb(t);
    rebuild_tlut();
}
