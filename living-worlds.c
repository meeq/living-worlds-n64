/*
 * Living Worlds - an N64 CI8 palette-cycling viewer (libdragon).
 *
 * Reproduces Joseph Huckaby's "Living Worlds" demo (color-cycling pixel art by
 * Mark Ferrari) to showcase the N64 CI8 texture format. The 256-color indexed
 * image is uploaded once; the picture never changes. Two animations run purely
 * by rebuilding the RGBA16 TLUT each frame:
 *
 *   - Color cycling: fast rotation of palette ranges (Huckaby's "BlendShift"
 *     sub-index interpolation), driven by the scene's cycle table.
 *   - Time of day: a slow morph of the whole palette. Each scene ships a set of
 *     keyframed palettes and a timeline mapping seconds-since-midnight to a
 *     palette; the displayed palette is a per-channel lerp between the two
 *     timeline palettes bracketing the current time (the reference's
 *     setTimeOfDayPalette + Palette.fade). Cycling is then layered on top.
 *
 * The day clock has three sources, cycled with C-up: AUTO (advances ~24 min per
 * simulated day, like the reference), RTC (follows the hardware/software clock
 * via the RTC subsystem), and HOLD (frozen). C-left/C-right scrub time by hand.
 *
 * Asset: rom:/ *.lw (format v2), produced by tools/convert_scene.py.
 */

#include <libdragon.h>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CYCLE_SPEED   280.0f    /* matches the reference engine (palette.js) */
#define SECS_PER_DAY  86400
#define SIM_RATE      60.0f     /* simulated seconds per real second (~24 min/day) */
#define SCRUB_RATE    10800.0f  /* scrub speed while C-left/right held (3 h/s) */
#define MAX_COLORS    256
#define MAX_SCENES    32
#define MAX_TL        64        /* timeline entries (largest scene has 46) */
#define FONT_HUD      1

typedef struct {
    uint8_t  reverse;
    uint8_t  low;
    uint8_t  high;
    uint16_t rate;
} cycle_t;

typedef struct {
    uint32_t off;    /* seconds since midnight */
    uint16_t pidx;   /* index into the palette table */
} tl_entry_t;

typedef enum { TIME_AUTO, TIME_RTC, TIME_HOLD } time_src_t;

/* scene data (rebuilt on every load_scene) */
static void     *scene_buf;            /* asset_load buffer (kept alive) */
static int       img_w, img_h;
static int       num_colors, num_cycles, num_palettes, num_tl;
static char      scene_title[64];      /* embedded title, shown in the HUD */
static const uint8_t *pal_table;       /* num_palettes * num_colors * 3, in scene_buf */
static cycle_t   cycles[MAX_COLORS];
static tl_entry_t timeline[MAX_TL];    /* sorted by offset */
static uint8_t   base_r[MAX_COLORS];   /* per-frame time-of-day palette (pre-cycling) */
static uint8_t   base_g[MAX_COLORS];
static uint8_t   base_b[MAX_COLORS];
static uint16_t *work_pal;             /* 8-byte aligned; uploaded to TLUT each frame */
static surface_t idx_surf;             /* FMT_CI8 view over the full-res pixel bytes */

/* scene catalog (rom:/ *.lw enumerated at startup) */
static char      scene_paths[MAX_SCENES][96];
static int       scene_count;
static int       scene_idx;

/* runtime state */
static bool       cycling    = true;
static bool       blendshift = true;
static bool       paused     = false;
static bool       show_hud   = true;
static uint32_t   scene_ms   = 0;          /* pausable clock driving the cycling */
static float      time_of_day = 43200.0f;  /* seconds since midnight, [0, 86400) */
static time_src_t time_src   = TIME_AUTO;
static bool       rtc_present = false;

static inline uint16_t rd16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}
static inline uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

/* Seconds since midnight from the RTC subsystem (hardware clock if present,
 * software clock otherwise - either way time(NULL) is hooked by rtc_init). */
static int rtc_seconds_of_day(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    return tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec;
}

static void load_scene(const char *path)
{
    if (scene_buf) {
        /* Reload: the RDP may still be DMAing the previous index map / TLUT, so
         * let it drain before we free those buffers out from under it. */
        rspq_wait();
        free(scene_buf);
        free(work_pal);
    }

    int sz;
    scene_buf = asset_load(path, &sz);
    assertf(scene_buf, "could not load %s", path);

    const uint8_t *b = scene_buf;
    assertf(memcmp(b, "LWL2", 4) == 0, "%s: bad magic / stale asset", path);

    img_w        = rd16(b + 4);
    img_h        = rd16(b + 6);
    num_colors   = rd16(b + 8);
    num_cycles   = rd16(b + 10);
    num_palettes = rd16(b + 12);
    num_tl       = rd16(b + 14);
    uint32_t pix = rd32(b + 20);

    assertf(num_colors <= MAX_COLORS, "%s: too many colors (%d)", path, num_colors);
    assertf(num_tl <= MAX_TL, "%s: too many timeline entries (%d)", path, num_tl);
    assertf(num_palettes > 0 && num_tl > 0, "%s: missing time-of-day data", path);

    pal_table = b + 24;

    const uint8_t *cyc = pal_table + (uint32_t)num_palettes * num_colors * 3;
    for (int i = 0; i < num_cycles; i++) {
        const uint8_t *c = cyc + i * 8;
        cycles[i].reverse = c[0];
        cycles[i].low     = c[1];
        cycles[i].high    = c[2];
        cycles[i].rate    = rd16(c + 4);
    }

    const uint8_t *tl = cyc + num_cycles * 8;
    for (int i = 0; i < num_tl; i++) {
        const uint8_t *e = tl + i * 8;
        timeline[i].off  = rd32(e);
        timeline[i].pidx = rd16(e + 4);
    }

    /* Title block (u16 len + UTF-8 bytes) sits between the timeline and the
     * 8-byte-aligned pixel data. */
    const uint8_t *t = tl + num_tl * 8;
    int tlen = rd16(t);
    if (tlen > (int)sizeof(scene_title) - 1)
        tlen = sizeof(scene_title) - 1;
    memcpy(scene_title, t + 2, tlen);
    scene_title[tlen] = '\0';

    const uint8_t *pixels = b + pix;
    idx_surf = surface_make_linear((void *)pixels, FMT_CI8, img_w, img_h);
    /* The RDP DMAs the index bytes; make sure they are flushed from the CPU cache. */
    data_cache_hit_writeback((void *)pixels, (uint32_t)img_w * img_h);

    work_pal = memalign(16, num_colors * sizeof(uint16_t));
    assertf(work_pal, "out of memory for palette");
}

static int cmp_path(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* Enumerate rom:/ *.lw into scene_paths[], sorted so the NN_ slugs play back in
 * calendar order (lexicographic == numeric for the zero-padded prefixes). */
static void enumerate_scenes(void)
{
    dir_t dir;
    for (int r = dir_findfirst("rom:/", &dir); r == 0 && scene_count < MAX_SCENES;
         r = dir_findnext("rom:/", &dir)) {
        size_t len = strlen(dir.d_name);
        bool is_lw_file = (
            dir.d_type == DT_REG && len >= 4 &&
            strcmp(dir.d_name + len - 3, ".lw") == 0
        );
        if (!is_lw_file) continue;
        snprintf(scene_paths[scene_count], sizeof(scene_paths[0]),
                 "rom:/%s", dir.d_name);
        scene_count++;
    }
    qsort(scene_paths, scene_count, sizeof(scene_paths[0]), cmp_path);
}

static void switch_scene(int delta)
{
    if (scene_count <= 1) return;
    scene_idx = (scene_idx + delta + scene_count) % scene_count;
    load_scene(scene_paths[scene_idx]);
    scene_ms = 0;   /* restart cycling from the new scene's t=0 */
}

/* Resolve the time-of-day palette for `t` (seconds since midnight) into
 * base_r/g/b. Mirrors the reference's setTimeOfDayPalette: find the timeline
 * entries bracketing `t` (wrapping across midnight), then lerp the two palettes
 * per channel by the fractional position between them. */
static void build_tod_palette(float t)
{
    int ti = (int)t % SECS_PER_DAY;
    if (ti < 0) ti += SECS_PER_DAY;

    int bi = -1, ai = -1;          /* before / after timeline indices */
    int boff = 0, aoff = 0;
    uint32_t bdist = SECS_PER_DAY, adist = SECS_PER_DAY;
    for (int i = 0; i < num_tl; i++) {
        int off = (int)timeline[i].off;
        if (off <= ti && (uint32_t)(ti - off) < bdist) { bdist = ti - off; bi = i; boff = off; }
        if (off >= ti && (uint32_t)(off - ti) < adist) { adist = off - ti; ai = i; aoff = off; }
    }
    if (bi < 0) {                  /* wrap: nearest entry is yesterday's last */
        int best = 0;
        for (int i = 1; i < num_tl; i++) if (timeline[i].off > timeline[best].off) best = i;
        bi = best; boff = (int)timeline[best].off - SECS_PER_DAY;
    }
    if (ai < 0) {                  /* wrap: nearest entry is tomorrow's first */
        int best = 0;
        for (int i = 1; i < num_tl; i++) if (timeline[i].off < timeline[best].off) best = i;
        ai = best; aoff = (int)timeline[best].off + SECS_PER_DAY;
    }

    const uint8_t *bp = pal_table + (uint32_t)timeline[bi].pidx * num_colors * 3;
    const uint8_t *ap = pal_table + (uint32_t)timeline[ai].pidx * num_colors * 3;
    int span = aoff - boff;
    int frac = ti - boff;
    for (int i = 0; i < num_colors; i++) {
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

/* Rebuild work_pal from the time-of-day palette, applying every active cycle.
 * Mirrors palette.js shiftColors/blendShiftColors: after rotating a range up by
 * the integer part of the amount, slot k blends toward its lower neighbour by the
 * fractional part (slot 0 wraps to the top of the range). */
static void build_palette(void)
{
    for (int i = 0; i < num_colors; i++)
        work_pal[i] = color_to_packed16(RGBA32(base_r[i], base_g[i], base_b[i], 0xFF));

    if (cycling) {
        for (int ci = 0; ci < num_cycles; ci++) {
            cycle_t *c = &cycles[ci];
            int size = c->high - c->low + 1;
            if (c->rate == 0 || size <= 1) continue;

            float rate_hz = c->rate / CYCLE_SPEED;
            float amt = fmodf((float)scene_ms * rate_hz / 1000.0f, (float)size);
            int   shift = (int)floorf(amt);
            float frac  = amt - shift;

            for (int k = 0; k < size; k++) {
                int a = ((k - shift) % size + size) % size;       /* color now in slot k */
                int b = ((k - shift - 1) % size + size) % size;   /* neighbour blending in */
                int ia = c->low + a, ib = c->low + b;
                if (blendshift && frac > 0.0f) {
                    int r = base_r[ia] + (int)((base_r[ib] - base_r[ia]) * frac);
                    int g = base_g[ia] + (int)((base_g[ib] - base_g[ia]) * frac);
                    int bl = base_b[ia] + (int)((base_b[ib] - base_b[ia]) * frac);
                    work_pal[c->low + k] = color_to_packed16(RGBA32(r, g, bl, 0xFF));
                } else {
                    work_pal[c->low + k] =
                        color_to_packed16(RGBA32(base_r[ia], base_g[ia], base_b[ia], 0xFF));
                }
            }
        }
    }

    data_cache_hit_writeback(work_pal, num_colors * sizeof(uint16_t));
}

static void draw_hud(void)
{
    int t = (int)time_of_day;
    int hh = t / 3600, mm = (t / 60) % 60;
    const char *src =
        time_src == TIME_AUTO ? "auto" :
        time_src == TIME_RTC  ? (rtc_present ? "rtc" : "rtc(soft)") : "hold";

    rdpq_text_printf(NULL, FONT_HUD, 8, 14,
        "%s  [%d/%d]", scene_title, scene_idx + 1, scene_count);
    rdpq_text_printf(NULL, FONT_HUD, 8, 26,
        "%02d:%02d %s   cycle:%s blend:%s%s",
        hh, mm, src,
        cycling ? "on" : "off",
        blendshift ? "on" : "off",
        paused ? "  PAUSED" : "");
    rdpq_text_printf(NULL, FONT_HUD, 8, 38,
        "A:cyc B:bld C<>:scrub Cup:time St:pause L/R:scene Z:hud");
}

int main(void)
{
    debug_init_isviewer();
    debug_init_usblog();

    dfs_init(DFS_DEFAULT_LOCATION);
    rdpq_init();
    rdpq_debug_start();
    joypad_init();
    timer_init();

    /* Hooks time(NULL) into the RTC (hardware clock if present, else software). */
    rtc_present = rtc_init();
    time_of_day = rtc_seconds_of_day();   /* open at the real time of day */

    enumerate_scenes();
    assertf(scene_count > 0, "no rom:/*.lw scenes found");
    load_scene(scene_paths[scene_idx]);

    rdpq_text_register_font(FONT_HUD, rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO));

    /* The full-res index map blits 1:1 into a native 640x480 framebuffer, so the
     * RDP never scales (no fractional-blit banding). Interlaced so all 480 lines
     * are shown. Color is 16bpp: the TLUT is RGBA16, so 32bpp adds no precision. */
    display_init(RESOLUTION_640x480, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);

    uint32_t last = get_ticks_ms();
    while (1) {
        joypad_poll();
        joypad_buttons_t p1   = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);
        if (p1.a)     cycling    = !cycling;
        if (p1.b)     blendshift = !blendshift;
        if (p1.start) paused     = !paused;
        if (p1.z)     show_hud   = !show_hud;
        if (p1.l)     switch_scene(-1);
        if (p1.r)     switch_scene(+1);
        if (p1.c_up)  time_src   = (time_src + 1) % 3;   /* auto -> rtc -> hold */

        uint32_t now = get_ticks_ms();
        uint32_t dms = now - last;
        last = now;
        float dt = dms / 1000.0f;

        /* Advance the day clock. Scrubbing always wins and "grabs" the clock out
         * of RTC mode so the scrubbed time sticks. */
        if (held.c_left || held.c_right) {
            time_of_day += (held.c_right ? SCRUB_RATE : -SCRUB_RATE) * dt;
            if (time_src == TIME_RTC) time_src = TIME_HOLD;
        } else if (!paused) {
            if (time_src == TIME_AUTO)      time_of_day += SIM_RATE * dt;
            else if (time_src == TIME_RTC)  time_of_day = rtc_seconds_of_day();
        }
        time_of_day = fmodf(time_of_day, (float)SECS_PER_DAY);
        if (time_of_day < 0) time_of_day += SECS_PER_DAY;

        if (!paused) scene_ms += dms;

        build_tod_palette(time_of_day);
        build_palette();

        surface_t *disp = display_get();
        rdpq_attach(disp, NULL);
        rdpq_clear(RGBA32(0, 0, 0, 255));

        rdpq_set_mode_standard();
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_mode_filter(FILTER_POINT);
        rdpq_tex_upload_tlut(work_pal, 0, num_colors);
        rdpq_tex_blit(&idx_surf, 0, 0, NULL);

        if (show_hud) draw_hud();

        rdpq_detach_show();
    }
}
