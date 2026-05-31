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
 * The day clock has three sources: AUTO (advances ~24 min per simulated day,
 * like the reference), RTC (follows the hardware/software clock via the RTC
 * subsystem), and HOLD (frozen). All three, plus a manual scrub, are exposed
 * through the in-game menu.
 *
 * Each scene also references an ambient audio loop (rom:/<slug>.wav64, streamed
 * by libdragon's wav64 module) with a per-scene max volume. On scene change the
 * outgoing loop fades out (~0.4 s) while the new one fades in (~2 s), matching
 * the reference's startSceneAudio / stopSceneAudio. The menu's Sound row gates
 * per-channel volumes to zero without disturbing the underlying stream
 * positions, so resuming is seamless.
 *
 * The front end is a tiny state machine: UI_TITLE shows a credits overlay over
 * the live first scene and fades out on any button (or after 10 s); UI_SCENE is
 * the bare scene with no overlay; UI_MENU is a translucent panel of focusable
 * rows (Scene, Cycling, BlendShift, Time source, Time of day, Sound) navigated
 * with the D-pad or analog stick. Start toggles into and out of the menu; no
 * other bindings exist outside it.
 *
 * Asset: rom:/ *.lw (format LWL3), produced by tools/convert_scene.py.
 */

#include <libdragon.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define CYCLE_SPEED   280.0f    /* matches the reference engine (palette.js) */
#define SECS_PER_DAY  86400
#define SECS_MID_DAY (SECS_PER_DAY / 2)
/* AUTO speed is parameterised by real-time day length (how long a full
 * simulated day takes in real seconds): every preset is a clean round
 * duration, and adjacent steps roughly halve. Default is 1 min/day -- a
 * lively-but-watchable pace; anything longer drags. */
static const int DAY_SPEED_SECS[] = { 1200, 600, 300, 120, 60, 30, 15 };
#define DAY_SPEED_N        (sizeof(DAY_SPEED_SECS) / sizeof(DAY_SPEED_SECS[0]))
#define DAY_SPEED_DEFAULT  4    /* 60s -- 1 minute per simulated day */
#define SCRUB_RATE       10800.0f  /* scrub speed while C-left/right held (3 h/s) */
#define MAX_COLORS    256
#define MAX_SCENES    32
#define FONT_HUD      1
#define FONT_TITLE    2

/* Button-icon spritemap: 48x48 PNG arranged as a 4x4 grid of 12x12 cells
 * (rom:/buttons.sprite). Row-major btn_t indices below. The icons take the
 * place of "L/Z", "Start/A", "C-Up", etc. in the menu cheat sheet, and the
 * Start icon also appears on the title-screen prompt. */
#define BTN_SIZE      12
typedef enum {
    BTN_D_UP,    BTN_D_RIGHT, BTN_D_LEFT, BTN_D_DOWN,
    BTN_L,       BTN_Z,       BTN_R,      BTN_DPAD,
    BTN_A,       BTN_B,       BTN_START,  BTN_CTRL,
    BTN_C_UP,    BTN_C_RIGHT, BTN_C_LEFT, BTN_C_DOWN,
} btn_t;

/* Title screen: ~600 px text band centered on a 640 px screen, dark-vignetted
 * over the live scene, fading out at TITLE_FADE_S either on any-button or
 * after TITLE_HOLD_S of inactivity. Pressing Start jumps straight to the menu
 * instead. */
#define TITLE_HOLD_S  10.0f
#define TITLE_FADE_S  0.6f
#define TITLE_VIG_A   160       /* peak vignette alpha (0..255) */

/* Action toast: one-line confirmation in the bottom-right corner whenever an
 * off-menu binding fires. Anchored inside a ~10% NTSC action-safe inset so it
 * stays visible on overscanning CRTs. Re-styled per-frame on FONT_HUD style 2
 * so the color/outline alpha can track the fade. */
#define TOAST_HOLD_S     1.5f
#define TOAST_FADE_S     0.4f
#define TOAST_STYLE      2
#define TOAST_SAFE_R     32     /* right action-safe inset (px) */
#define TOAST_SAFE_B     32     /* bottom action-safe inset (px) */

/* Menu panel: 360 px wide, centered on 640x480, sized to fit header + 7 rows
 * + 3-line cheat sheet with the bottom edge well inside the NTSC action-safe */
#define MENU_X0       140
#define MENU_Y0       60
#define MENU_X1       500
#define MENU_Y1       410
#define MENU_PAD      14        /* inner padding (px) */
#define MENU_ROW_H    24        /* row spacing */
#define MENU_PANEL_A  190       /* panel alpha (0..255) */

/* Audio: 48 kHz mono, two mixer channels (current loop + crossfading
 * predecessor), Opus-compressed wav64 streamed directly from ROM. Opus only
 * encodes at fixed rates (8/12/16/24/48 kHz), so audioconv64 ignores
 * --wav-resample on the Opus path and stamps the output at 48 kHz; matching
 * the mixer output rate here avoids any runtime resampling. */
#define AUDIO_FREQ      48000
#define AUDIO_BUFFERS   4
#define MIX_CHANNELS    2
#define FADE_IN_S       2.0f     /* reference: targetFPS * 2 frames in */
#define FADE_OUT_S      0.4f     /* reference: targetFPS / 2 frames out */
#define MAX_AUDIO_SLUGS 16       /* 13 unique loops in the reference */
#define SLUG_MAX        16

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

typedef enum { TIME_AUTO, TIME_RTC, TIME_HOLD, TIME_SRC_COUNT } time_src_t;

/* On-cart save layout. Two EEPROM blocks (16 bytes); the trailing 4 bytes are
 * reserved so we can extend without bumping the magic. Persisted on every
 * user-visible state change (dirty-flag, flushed once per frame); the libdragon
 * EEPROM driver coalesces background writes. Magic + version gate prevents
 * reading garbage from a blank/foreign cart. */
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
    uint32_t        time_of_day_s;   /* seconds since midnight; only meaningful when time_src == TIME_HOLD */
    uint32_t        reserved;
} lw_save_t;                         /* sizeof == 16 */
_Static_assert(sizeof(lw_save_t) == 16, "lw_save_t must be 16 bytes (2 EEPROM blocks)");

static sprite_t *btn_sprite;
static int       title_press_w;   /* "Press " width in FONT_TITLE (px) */
static int       title_options_w; /* " for Options" width in FONT_TITLE (px) */

static surface_t idx_surf;            /* FMT_CI8 view over the full-res pixel bytes */
static uint8_t   base_r[MAX_COLORS];
static uint8_t   base_g[MAX_COLORS];
static uint8_t   base_b[MAX_COLORS];
static uint16_t  tlut[MAX_COLORS] __attribute__((aligned(16)));

static void              *scene_buf;  /* asset_load buffer (kept alive) */
static const lw_header_t *hdr;        /* overlay over scene_buf; source of truth for dims */
static const uint8_t     *pal_table;  /* hdr->num_palettes * hdr->num_colors * 3, in scene_buf */
static const lw_cycle_t  *cycles;
static const lw_tl_t     *timeline;   /* sorted by offset */

static char      scene_paths[MAX_SCENES][96];
static int       scene_count;
static int       scene_idx;

static bool       cycling     = true;
static bool       blendshift  = true;
static bool       sound_on    = true;
static uint32_t   scene_ms    = 0;             /* pausable clock driving the cycling */
static float      time_of_day = SECS_MID_DAY;  /* seconds since midnight, [0, 86400) */
static time_src_t time_src    = TIME_AUTO;
static int        day_speed_idx = DAY_SPEED_DEFAULT;
static bool       rtc_present = false;

/* UI_TITLE: credits over live scene. UI_SCENE: bare scene. UI_MENU: panel.
 * Start toggles into and out of the menu; the scene animates in every state. */
typedef enum { UI_TITLE, UI_SCENE, UI_MENU } ui_state_t;

typedef enum {
    ROW_SCENE,
    ROW_SOUND,
    ROW_CYCLING,
    ROW_BLENDSHIFT,
    ROW_TIME_SOURCE,
    ROW_SPEED,
    ROW_TIME_OF_DAY,
    ROW_COUNT
} menu_row_t;

static ui_state_t ui_state      = UI_TITLE;
static float      title_alpha   = 1.0f;        /* 1.0 visible, 0.0 gone */
static float      title_t       = 0.0f;        /* seconds in UI_TITLE */
static bool       title_dismiss = false;       /* fade-out requested */
static int        menu_focus    = ROW_SCENE;
static bool       save_dirty    = false;       /* pending EEPROM flush */
static char       toast_msg[64] = "";
static float      toast_t       = TOAST_HOLD_S + TOAST_FADE_S;  /* start hidden */

/* Open each unique slug at most once and stream from ROM. */
static char    wav_slugs[MAX_AUDIO_SLUGS][SLUG_MAX];
static wav64_t wav_handles[MAX_AUDIO_SLUGS];
static int     wav_count;

/* Two-channel fade engine. `active_ch` is whichever mixer channel hosts the
 * current scene's loop; the other channel hosts whatever it just replaced and
 * is fading out. Channels are never swapped: when a scene changes, we flip the
 * label and let both loops keep streaming on their own channels. */
static int         active_ch;
static const char *ch_slug[MIX_CHANNELS];   /* NULL == channel idle */
static float       ch_vol[MIX_CHANNELS];    /* current envelope value */
static float       ch_target[MIX_CHANNELS]; /* envelope target */
static float       ch_rate[MIX_CHANNELS];   /* units/sec toward target */

/* Seconds since midnight from the RTC subsystem (hardware clock if present,
 * software clock otherwise - either way time(NULL) is hooked by rtc_init). */
static int rtc_seconds_of_day(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    return tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec;
}

static float wrap_time_of_day(float t)
{
    t = fmodf(t, (float)SECS_PER_DAY);
    return t < 0 ? t + SECS_PER_DAY : t;
}

static void scrub_time_of_day(float delta_s)
{
    time_of_day = wrap_time_of_day(time_of_day + delta_s);
    time_src    = TIME_HOLD;
    save_dirty  = true;
}

/* Wrap-around add for cycling through a fixed-size enum with -1/+1 dirs. */
static inline int cycle_enum(int v, int n, int dir) { return (v + n + dir) % n; }

/* Per-frame clock tick. AUTO advances at the selected DAY_SPEED_SECS preset;
 * RTC tracks the real clock; HOLD freezes (where manual scrub leaves us). */
static void advance_time_of_day(float dt)
{
    if (time_src == TIME_AUTO)     time_of_day += (float)SECS_PER_DAY * dt / (float)DAY_SPEED_SECS[day_speed_idx];
    else if (time_src == TIME_RTC) time_of_day = rtc_seconds_of_day();
    time_of_day = wrap_time_of_day(time_of_day);
}

static void load_scene(const char *path)
{
    if (scene_buf) {
        /* On reload, wait for the RDP to finish DMAing before freeing. */
        rspq_wait();
        free(scene_buf);
    }

    scene_buf = asset_load(path, NULL);
    assertf(scene_buf, "could not load %s", path);

    hdr = scene_buf;
    assertf(memcmp(hdr->magic, "LWL3", 4) == 0, "%s: bad magic / stale asset", path);
    assertf(hdr->num_colors <= MAX_COLORS, "%s: too many colors (%d)", path, hdr->num_colors);
    assertf(hdr->num_palettes > 0 && hdr->num_tl > 0, "%s: missing time-of-day data", path);

    pal_table = (const uint8_t *)(hdr + 1);
    cycles    = (const void *)(pal_table + (uint32_t)hdr->num_palettes * hdr->num_colors * 3);
    timeline  = (const void *)(cycles + hdr->num_cycles);

    const uint8_t *pixels = (const uint8_t *)scene_buf + hdr->pixel_offset;
    idx_surf = surface_make_linear((void *)pixels, FMT_CI8, hdr->width, hdr->height);
    /* The RDP DMAs the index bytes; make sure they are flushed from the CPU cache. */
    data_cache_hit_writeback((void *)pixels, (uint32_t)hdr->width * hdr->height);
}

static int cmp_path(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void enumerate_scenes(void)
{
    dir_t dir;
    for (int r = dir_findfirst("rom:/", &dir); r == 0 && scene_count < MAX_SCENES;
         r = dir_findnext("rom:/", &dir)) {
        const char *ext = strrchr(dir.d_name, '.');
        if (dir.d_type != DT_REG || !ext || strcmp(ext, ".lw") != 0) continue;
        snprintf(scene_paths[scene_count], sizeof(scene_paths[0]),
                 "rom:/%.90s", dir.d_name);
        scene_count++;
    }
    qsort(scene_paths, scene_count, sizeof(scene_paths[0]), cmp_path);
}

/* Forward declaration: switch_scene crossfades audio after loading the new
 * scene, but start_scene_audio is defined further down with the rest of the
 * audio engine. */
static void start_scene_audio(const char *slug, float max_vol);

static void switch_scene(int delta)
{
    if (scene_count <= 1) return;
    scene_idx = (scene_idx + delta + scene_count) % scene_count;
    load_scene(scene_paths[scene_idx]);
    scene_ms = 0;
    start_scene_audio(hdr->audio_slug, hdr->audio_volume_q8 / 256.0f);
}

/* Mirrors the reference's setTimeOfDayPalette: per-channel lerp between the
 * two timeline palettes bracketing `t`, wrapping across midnight. */
static void rebuild_base_rgb(float t)
{
    int ti = (int)t % SECS_PER_DAY;
    if (ti < 0) ti += SECS_PER_DAY;

    /* timeline is sorted by offset: bi/ai bracket ti, wrapping across midnight. */
    int n_tl = hdr->num_tl, n_col = hdr->num_colors;
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

/* Rebuild tlut from the time-of-day palette, applying every active cycle.
 * Mirrors palette.js shiftColors/blendShiftColors: after rotating a range up by
 * the integer part of the amount, slot k blends toward its lower neighbour by the
 * fractional part (slot 0 wraps to the top of the range). */
static void rebuild_tlut(void)
{
    int slots = hdr->num_colors;
    for (int i = 0; i < slots; i++)
        tlut[i] = color_to_packed16(RGBA32(base_r[i], base_g[i], base_b[i], 0xFF));

    if (cycling) {
        for (int ci = 0; ci < hdr->num_cycles; ci++) {
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

/* Open (and configure for looping) on first use, then return from the cache. */
static wav64_t *audio_get(const char *slug)
{
    for (int i = 0; i < wav_count; i++)
        if (strcmp(wav_slugs[i], slug) == 0) return &wav_handles[i];

    assertf(wav_count < MAX_AUDIO_SLUGS, "wav64 cache full (slug=%s)", slug);
    char path[32];
    snprintf(path, sizeof(path), "rom:/%.*s.wav64", SLUG_MAX - 1, slug);
    wav64_open(&wav_handles[wav_count], path);
    wav64_set_loop(&wav_handles[wav_count], true);
    snprintf(wav_slugs[wav_count], SLUG_MAX, "%s", slug);
    return &wav_handles[wav_count++];
}

static void update_channel(int ch, float dt)
{
    if (!ch_slug[ch]) return;
    float step = ch_rate[ch] * dt;
    if      (ch_vol[ch] < ch_target[ch]) ch_vol[ch] = fminf(ch_vol[ch] + step, ch_target[ch]);
    else if (ch_vol[ch] > ch_target[ch]) ch_vol[ch] = fmaxf(ch_vol[ch] - step, ch_target[ch]);

    float gated = sound_on ? ch_vol[ch] : 0.0f;
    mixer_ch_set_vol(ch, gated, gated);
    /* Reaching zero on an outgoing fade releases the channel for reuse. */
    if (ch_vol[ch] == 0.0f && ch_target[ch] == 0.0f) {
        mixer_ch_stop(ch);
        ch_slug[ch] = NULL;
    }
}

/* Begin playing `slug` (or NULL for silence). Mirrors reference
 * startSceneAudio / stopSceneAudio (~2 s in, ~0.4 s out).
 *
 * Same-slug transitions intentionally keep the existing loop playing rather
 * than restarting it: the reference's HTML5 Audio rebuild produces an audible
 * pop when neighbouring scenes share a track (e.g. Feb clear -> cloudy), and
 * keeping the stream avoids it. Volume retargets to the new scene's max. */
static void start_scene_audio(const char *slug, float max_vol)
{
    bool have_new = slug && slug[0];

    if (have_new && ch_slug[active_ch] && strcmp(ch_slug[active_ch], slug) == 0) {
        ch_target[active_ch] = max_vol;
        ch_rate[active_ch]   = fabsf(max_vol - ch_vol[active_ch]) / FADE_IN_S;
        return;
    }

    /* Fade out whatever currently owns `active_ch`. It stays on its mixer
     * channel; only our label moves. */
    if (ch_slug[active_ch]) {
        ch_target[active_ch] = 0.0f;
        ch_rate[active_ch]   = ch_vol[active_ch] / FADE_OUT_S;
    }

    int next_ch = (MIX_CHANNELS - 1) - active_ch;
    /* The other channel might still be fading out from an earlier transition;
     * cut it off cleanly before reusing the slot. */
    if (ch_slug[next_ch]) {
        mixer_ch_stop(next_ch);
        ch_slug[next_ch] = NULL;
        ch_vol[next_ch]  = 0.0f;
    }

    active_ch = next_ch;
    if (have_new) {
        wav64_t *w = audio_get(slug);
        ch_slug[active_ch]   = wav_slugs[w - wav_handles];
        ch_vol[active_ch]    = 0.0f;
        ch_target[active_ch] = max_vol;
        ch_rate[active_ch]   = max_vol / FADE_IN_S;
        mixer_ch_set_vol(active_ch, 0.0f, 0.0f);
        wav64_play(w, active_ch);
    }
}

/* Silent no-op on carts without EEPROM or with a stale/missing magic. Must
 * run after enumerate_scenes() so scene_idx can be range-checked. */
static void save_load(void)
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

/* Serialize the current settings into 2 EEPROM blocks. libdragon writes
 * through a RAM cache + background flusher, so this is a cheap memcpy-class
 * call; rate-limiting the flush to once per frame via save_dirty keeps the
 * write burst bounded during a held scrub. */
static void save_flush(void)
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

/* Fill a rectangle with a flat (combiner) color, blending against the
 * framebuffer via the prim alpha. Caller must call rdpq_set_mode_standard()
 * again before resuming normal sprite/text draws. */
static void fill_rect_alpha(int x0, int y0, int x1, int y1, color_t c)
{
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(c);
    rdpq_fill_rectangle(x0, y0, x1, y1);
}

static void title_set_alpha(float a)
{
    uint8_t aa = (uint8_t)(a * 255.0f);
    rdpq_font_t *f = (rdpq_font_t *)rdpq_text_get_font(FONT_TITLE);
    rdpq_font_style(f, 0, &(rdpq_fontstyle_t){
        .color         = RGBA32(255, 255, 255, aa),
        .outline_color = RGBA32(0,   0,   0,   aa),
    });
}

static int title_measure(const char *s)
{
    int n = strlen(s);
    rdpq_paragraph_t *p = rdpq_paragraph_build(&(rdpq_textparms_t){0}, FONT_TITLE, s, &n);
    int w = (int)p->advance_x;
    rdpq_paragraph_free(p);
    return w;
}

/* Forward declaration: draw_title embeds a Start icon in its prompt; the
 * sprite helpers live further down with the menu code. */
static void draw_btn_fade(int x, int y, btn_t b, float a);

static void draw_title(float alpha)
{
    if (alpha <= 0.0f) return;

    fill_rect_alpha(0, 0, 640, 480,
        RGBA32(0, 0, 0, (uint8_t)(TITLE_VIG_A * alpha)));

    title_set_alpha(alpha);

    rdpq_textparms_t centered = { .width = 640, .align = ALIGN_CENTER };

    rdpq_text_printf(&centered, FONT_TITLE, 0, 170, "LIVING WORLDS");
    rdpq_text_printf(&centered, FONT_TITLE, 0, 230,
        "Color-cycling pixel art by Mark Ferrari");
    rdpq_text_printf(&centered, FONT_TITLE, 0, 250,
        "Original code by Ian Gilman and Joseph Huckaby");
    rdpq_text_printf(&centered, FONT_TITLE, 0, 270,
        "N64 port by Christopher Bonhage");
    /* Fragment widths were measured at startup; draw_btn_fade dims in
     * lockstep with the surrounding title text. */
    int prompt_total = title_press_w + BTN_SIZE + title_options_w;
    int prompt_x     = (640 - prompt_total) / 2;
    rdpq_text_printf(NULL, FONT_TITLE, prompt_x, 430, "Press ");
    draw_btn_fade(prompt_x + title_press_w, 430 - 9, BTN_START, alpha);
    rdpq_text_printf(NULL, FONT_TITLE,
        prompt_x + title_press_w + BTN_SIZE, 430, " for Options");
}

static void toast_show(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast_msg, sizeof(toast_msg), fmt, ap);
    va_end(ap);
    toast_t = 0.0f;
}

static float toast_alpha(void)
{
    if (toast_t <= TOAST_HOLD_S) return 1.0f;
    float a = 1.0f - (toast_t - TOAST_HOLD_S) / TOAST_FADE_S;
    return a < 0.0f ? 0.0f : a;
}

static void draw_toast(void)
{
    float a = toast_alpha();
    if (a <= 0.0f || toast_msg[0] == 0) return;

    uint8_t aa = (uint8_t)(a * 255.0f);
    rdpq_font_t *f = (rdpq_font_t *)rdpq_text_get_font(FONT_HUD);
    rdpq_font_style(f, TOAST_STYLE, &(rdpq_fontstyle_t){
        .color         = RGBA32(255, 220,  80, aa),
        .outline_color = RGBA32(0,   0,   0,   aa),
    });

    /* Right-align inside the action-safe column [TOAST_SAFE_R, 640 - TOAST_SAFE_R]
     * with the baseline at 480 - TOAST_SAFE_B. The style escape lets us pick
     * style 2 without a global font-style flip. */
    rdpq_textparms_t parms = {
        .width = 640 - 2 * TOAST_SAFE_R,
        .align = ALIGN_RIGHT,
        .style_id = TOAST_STYLE,
    };
    rdpq_text_printf(&parms, FONT_HUD, TOAST_SAFE_R, 480 - TOAST_SAFE_B,
        "%s", toast_msg);
}

/* Blit one 12x12 cell from btn_sprite, modulated by `a` (1.0 = opaque). Resets
 * RDP mode each call: the surrounding text renderer leaves the pipeline in a
 * text-glyph mode that produces garbled output for raw sprite blits. */
static void draw_btn_fade(int x, int y, btn_t b, float a)
{
    uint8_t aa = (uint8_t)(a * 255.0f);
    int col = (int)b & 3;
    int row = (int)b >> 2;
    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_NONE);
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(RGBA32(255, 255, 255, aa));
    rdpq_sprite_blit(btn_sprite, x, y, &(rdpq_blitparms_t){
        .s0 = col * BTN_SIZE, .t0 = row * BTN_SIZE,
        .width = BTN_SIZE,    .height = BTN_SIZE,
    });
}

static void draw_btn(int x, int y, btn_t b) { draw_btn_fade(x, y, b, 1.0f); }

/* Format DAY_SPEED_SECS[idx] as a compact human label ("1h", "20m", "30s"). The
 * preset table is hand-picked so every entry divides cleanly into hours,
 * minutes, or seconds — no mixed-unit values like "1m30s" can appear. */
static const char *day_speed_label(int idx)
{
    static char buf[16];
    int s = DAY_SPEED_SECS[idx];
    if      (s >= 3600 && s % 3600 == 0) snprintf(buf, sizeof(buf), "%dh", s / 3600);
    else if (s >= 60   && s % 60   == 0) snprintf(buf, sizeof(buf), "%dm", s / 60);
    else                                  snprintf(buf, sizeof(buf), "%ds", s);
    return buf;
}

static const char *time_src_label(void)
{
    switch (time_src) {
        case TIME_AUTO: return "AUTO";
        case TIME_RTC:  return rtc_present ? "RTC" : "RTC*";
        default:        return "HOLD";
    }
}

/* One-token-at-a-time left-to-right layout for the cheat sheet rows. Each
 * helper draws at x and returns the next x cursor. Icons sit 9 px above the
 * text baseline so the 12 px sprite overlaps the 12 px line of mono text. */
static int cheat_btn(int x, int y, btn_t b)
{
    draw_btn(x, y - 9, b);
    return x + BTN_SIZE;
}

static int cheat_text(int x, int y, const char *s)
{
    rdpq_text_printf(NULL, FONT_HUD, x, y, "%s", s);
    return x + (int)strlen(s) * 8;
}

static void draw_menu(void)
{
    fill_rect_alpha(MENU_X0, MENU_Y0, MENU_X1, MENU_Y1,
        RGBA32(0, 0, 0, MENU_PANEL_A));
    color_t edge = RGBA32(255, 255, 255, 220);
    fill_rect_alpha(MENU_X0,     MENU_Y0,     MENU_X1,     MENU_Y0 + 2, edge);
    fill_rect_alpha(MENU_X0,     MENU_Y1 - 2, MENU_X1,     MENU_Y1,     edge);
    fill_rect_alpha(MENU_X0,     MENU_Y0,     MENU_X0 + 2, MENU_Y1,     edge);
    fill_rect_alpha(MENU_X1 - 2, MENU_Y0,     MENU_X1,     MENU_Y1,     edge);

    /* fill_rect_alpha leaves the pipeline in a FLAT combiner; reset for text. */
    rdpq_set_mode_standard();

    const int label_x = MENU_X0 + MENU_PAD;
    const int value_x = MENU_X0 + 160;
    int       y       = MENU_Y0 + MENU_PAD + 18;

    int t = (int)time_of_day;
    int hh = t / 3600, mm = (t / 60) % 60;

    /* ROW_SCENE gets its own two-line block below; labels/values here only
     * cover the single-line rows. */
    const char *labels[ROW_COUNT] = {
        "Scene", "Sound", "Color Cycling", "Color Blending",
        "Time Source", "AUTO Day Speed", "Time of Day",
    };
    char values[ROW_COUNT][32];
    values[ROW_SCENE][0] = 0;
    snprintf(values[ROW_CYCLING],     sizeof(values[0]), "[%s]", cycling    ? "ON" : "OFF");
    snprintf(values[ROW_BLENDSHIFT],  sizeof(values[0]), "[%s]", blendshift ? "ON" : "OFF");
    snprintf(values[ROW_TIME_SOURCE], sizeof(values[0]), "< %s >", time_src_label());
    snprintf(values[ROW_SPEED],       sizeof(values[0]), "< %s >", day_speed_label(day_speed_idx));
    snprintf(values[ROW_TIME_OF_DAY], sizeof(values[0]), "< %02d:%02d >", hh, mm);
    snprintf(values[ROW_SOUND],       sizeof(values[0]), "[%s]", sound_on   ? "ON" : "OFF");

    for (int i = 0; i < ROW_COUNT; i++) {
        bool focused = (i == menu_focus);
        const char *style_open  = focused ? "^01" : "";
        const char *style_close = focused ? "^00" : "";

        if (i == ROW_SCENE) {
            /* Two lines so the full hdr->title fits regardless of length. */
            rdpq_text_printf(NULL, FONT_HUD, label_x, y,
                "%s%s Scene  %d/%d%s",
                style_open, focused ? ">" : " ",
                scene_idx + 1, scene_count, style_close);
            y += MENU_ROW_H;
            rdpq_text_printf(NULL, FONT_HUD, label_x + 16, y,
                "%s< %s >%s", style_open, hdr->title, style_close);
            y += MENU_ROW_H;
            continue;
        }

        rdpq_text_printf(NULL, FONT_HUD, label_x, y,
            "%s%s %s%s", style_open, focused ? ">" : " ", labels[i], style_close);
        rdpq_text_printf(NULL, FONT_HUD, value_x, y,
            "%s%s%s", style_open, values[i], style_close);
        y += MENU_ROW_H;
    }

    /* Off-menu controls cheat sheet. Rows are sized to fit in the 332 px
     * panel inner width (12 px icons, 8 px mono chars). */
    y += 6;
    rdpq_text_printf(NULL, FONT_HUD, label_x, y, "Controls (when menu is closed)");
    y += MENU_ROW_H;
    int cx = label_x;
    cx = cheat_btn (cx, y, BTN_L);
    cx = cheat_text(cx, y, "/");
    cx = cheat_btn (cx, y, BTN_Z);
    cx = cheat_text(cx, y, ": Previous Scene  ");
    cx = cheat_btn (cx, y, BTN_R);
    cheat_text     (cx, y, ": Next Scene");
    y += MENU_ROW_H;

    cx = label_x;
    cx = cheat_btn (cx, y, BTN_START);
    cx = cheat_text(cx, y, "/");
    cx = cheat_btn (cx, y, BTN_A);
    cx = cheat_text(cx, y, ": Menu  ");
    cx = cheat_btn (cx, y, BTN_B);
    cx = cheat_text(cx, y, ": Sound  ");
    cx = cheat_btn (cx, y, BTN_D_UP);
    cx = cheat_text(cx, y, "/");
    cx = cheat_btn (cx, y, BTN_D_DOWN);
    cheat_text     (cx, y, ": Speed");
    y += MENU_ROW_H;

    cx = label_x;
    cx = cheat_btn (cx, y, BTN_C_UP);
    cx = cheat_text(cx, y, ": Color Cycling  ");
    cx = cheat_btn (cx, y, BTN_C_LEFT);
    cx = cheat_text(cx, y, "/");
    cx = cheat_btn (cx, y, BTN_C_RIGHT);
    cheat_text     (cx, y, ": Time of Day");
    y += MENU_ROW_H;

    cx = label_x;
    cx = cheat_btn (cx, y, BTN_C_DOWN);
    cx = cheat_text(cx, y, ": Color Blending  ");
    cx = cheat_btn (cx, y, BTN_D_LEFT);
    cx = cheat_text(cx, y, "/");
    cx = cheat_btn (cx, y, BTN_D_RIGHT);
    cheat_text     (cx, y, ": Time Source");
}

static void menu_change(int row, int dir)
{
    switch (row) {
        case ROW_SCENE:       switch_scene(dir); break;
        case ROW_CYCLING:     cycling    = !cycling;    break;
        case ROW_BLENDSHIFT:  blendshift = !blendshift; break;
        case ROW_TIME_SOURCE: time_src   = cycle_enum(time_src, TIME_SRC_COUNT, dir); break;
        case ROW_SPEED:       day_speed_idx = cycle_enum(day_speed_idx, DAY_SPEED_N, dir); break;
        case ROW_TIME_OF_DAY: scrub_time_of_day(dir * 3600.0f); break;
        case ROW_SOUND:       sound_on   = !sound_on;   break;
    }
    save_dirty = true;
}

static void menu_activate(int row)
{
    switch (row) {
        case ROW_SCENE:       switch_scene(+1); break;
        case ROW_TIME_OF_DAY: /* no toggle for scrub-only row */ break;
        default:              menu_change(row, +1); break;
    }
}

/* `dt` drives the analog-stick scrub on the Time of day row. Returns true
 * if the menu should close. */
static bool menu_handle_input(joypad_buttons_t pressed, float dt)
{
    if (pressed.start || pressed.b) return true;

    int dy = 0;
    if (pressed.d_up)   dy--;
    if (pressed.d_down) dy++;
    /* Stick Y is positive-up in libdragon, so invert to match d_down=+1. */
    dy -= joypad_get_axis_pressed(JOYPAD_PORT_1, JOYPAD_AXIS_STICK_Y);
    if (dy) menu_focus = cycle_enum(menu_focus, ROW_COUNT, dy > 0 ? 1 : -1);

    int dx = 0;
    if (pressed.d_left)  dx--;
    if (pressed.d_right) dx++;
    dx += joypad_get_axis_pressed(JOYPAD_PORT_1, JOYPAD_AXIS_STICK_X);
    if (dx) menu_change(menu_focus, dx > 0 ? +1 : -1);

    if (pressed.a) menu_activate(menu_focus);

    /* Hold-to-scrub on the Time of day row. Forces TIME_HOLD so the value sticks. */
    if (menu_focus == ROW_TIME_OF_DAY) {
        int hx = 0;
        joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);
        if (held.d_left)  hx--;
        if (held.d_right) hx++;
        hx += joypad_get_axis_held(JOYPAD_PORT_1, JOYPAD_AXIS_STICK_X);
        if (hx) scrub_time_of_day(hx * SCRUB_RATE * dt);
    }

    return false;
}

/* Returns true to enter the menu. Off-menu bindings fire a bottom-right
 * toast so the action is visible without opening the menu. */
static bool scene_handle_input(joypad_buttons_t pressed, float dt)
{
    if (pressed.start || pressed.a) return true;

    int scene_dir = pressed.r ? +1 : (pressed.l || pressed.z) ? -1 : 0;
    if (scene_dir) {
        switch_scene(scene_dir);
        toast_show(scene_dir < 0 ? "< Scene %d/%d  %s" : "Scene %d/%d >  %s",
                   scene_idx + 1, scene_count, hdr->title);
        save_dirty = true;
        return false;
    }

    if (pressed.b) {
        sound_on = !sound_on;
        toast_show("Sound: %s", sound_on ? "ON" : "OFF");
        save_dirty = true;
    }
    if (pressed.c_up) {
        cycling = !cycling;
        toast_show("Color Cycling: %s", cycling ? "ON" : "OFF");
        save_dirty = true;
    }
    if (pressed.c_down) {
        blendshift = !blendshift;
        toast_show("Color Blending: %s", blendshift ? "ON" : "OFF");
        save_dirty = true;
    }

    /* C-Left/Right scrubs time-of-day continuously while held. Toast refreshes
     * each frame so the HH:MM readout follows the scrub and stays visible until
     * release. */
    joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);
    int hx = (held.c_right ? 1 : 0) - (held.c_left ? 1 : 0);
    if (hx) {
        scrub_time_of_day(hx * SCRUB_RATE * dt);
        int t = (int)time_of_day;
        toast_show("Time of Day: %02d:%02d", t / 3600, (t / 60) % 60);
    }

    int dx = (pressed.d_right ? 1 : 0) - (pressed.d_left ? 1 : 0);
    if (dx) {
        time_src = cycle_enum(time_src, TIME_SRC_COUNT, dx);
        toast_show("Time Source: %s", time_src_label());
        save_dirty = true;
    }

    int dy = (pressed.d_down ? 1 : 0) - (pressed.d_up ? 1 : 0);
    if (dy) {
        day_speed_idx = cycle_enum(day_speed_idx, DAY_SPEED_N, dy);
        time_src = TIME_AUTO;
        toast_show("AUTO Day Speed: %s", day_speed_label(day_speed_idx));
        save_dirty = true;
    }

    return false;
}

int main(void)
{
    debug_init_isviewer();
    debug_init_usblog();

    dfs_init(DFS_DEFAULT_LOCATION);
    rdpq_init();
    joypad_init();
    timer_init();

    display_init(RESOLUTION_640x480, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);

    rtc_present = rtc_init();
    if (!rtc_present) {
        settimeofday(&(struct timeval){ .tv_sec = SECS_MID_DAY }, NULL);
    }
    time_of_day = rtc_seconds_of_day();   /* open at the real time of day */

    audio_init(AUDIO_FREQ, AUDIO_BUFFERS);
    mixer_init(MIX_CHANNELS);
    wav64_init_compression(3);            /* Opus loops (--wav-compress 3) */

    enumerate_scenes();
    assertf(scene_count > 0, "no rom:/*.lw scenes found");

    /* Pull persisted settings before the first scene load so the saved
     * scene_idx wins. For AUTO/RTC modes, snap time_of_day back to the real
     * clock — the saved value is only authoritative when source is HOLD. */
    save_load();
    if (time_src != TIME_HOLD) time_of_day = rtc_seconds_of_day();

    load_scene(scene_paths[scene_idx]);
    start_scene_audio(hdr->audio_slug, hdr->audio_volume_q8 / 256.0f);

    /* Two fonts: the small mono font (existing) is the menu's body text; the
     * variable-width debug font is reserved for the title credits. Style 0 on
     * FONT_HUD is the default white-on-black; style 1 is the focused-row
     * yellow. Selected mid-string with the ^00 / ^01 escapes from rdpq_text. */
    rdpq_font_t *f_hud = rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO);
    rdpq_font_style(f_hud, 0, &(rdpq_fontstyle_t){
        .color = RGBA32(240, 240, 240, 255), .outline_color = RGBA32(0, 0, 0, 255),
    });
    rdpq_font_style(f_hud, 1, &(rdpq_fontstyle_t){
        .color = RGBA32(255, 220,  80, 255), .outline_color = RGBA32(0, 0, 0, 255),
    });
    rdpq_text_register_font(FONT_HUD, f_hud);

    rdpq_font_t *f_title = rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_VAR);
    rdpq_text_register_font(FONT_TITLE, f_title);
    title_set_alpha(1.0f);

    btn_sprite = sprite_load("rom:/buttons.sprite");
    assertf(btn_sprite, "could not load rom:/buttons.sprite");

    /* Measure the FONT_TITLE fragments straddling the Start icon once, so
     * draw_title can re-center "Press [Start] for Options" every frame without
     * allocating. */
    title_press_w   = title_measure("Press ");
    title_options_w = title_measure(" for Options");

    uint32_t last = get_ticks_ms();
    while (1) {
        joypad_poll();
        joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

        uint32_t now = get_ticks_ms();
        uint32_t dms = now - last;
        last = now;
        float dt = dms / 1000.0f;

        toast_t += dt;

        /* Per-state input. Only Start has any effect outside the menu;
         * everything else routes through menu_handle_input(). */
        switch (ui_state) {
            case UI_TITLE:
                title_t += dt;
                if (title_t >= TITLE_HOLD_S) title_dismiss = true;
                if (pressed.start) {
                    ui_state      = UI_MENU;
                    title_alpha   = 0.0f;
                    title_dismiss = false;
                } else if (pressed.raw) {
                    title_dismiss = true;
                }
                if (title_dismiss) {
                    title_alpha -= dt / TITLE_FADE_S;
                    if (title_alpha <= 0.0f) {
                        title_alpha = 0.0f;
                        ui_state    = UI_SCENE;
                    }
                }
                break;
            case UI_SCENE:
                if (scene_handle_input(pressed, dt)) ui_state = UI_MENU;
                break;
            case UI_MENU:
                if (menu_handle_input(pressed, dt)) ui_state = UI_SCENE;
                break;
        }

        scene_ms += dms;

        advance_time_of_day(dt);
        rebuild_base_rgb(time_of_day);
        rebuild_tlut();

        update_channel(0, dt);
        update_channel(1, dt);
        mixer_try_play();

        surface_t *disp = display_get();
        rdpq_attach_clear(disp, NULL);

        rdpq_set_mode_standard();
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_mode_filter(FILTER_POINT);
        rdpq_tex_upload_tlut(tlut, 0, hdr->num_colors);
        rdpq_tex_blit(&idx_surf, 0, 0, NULL);

        if      (ui_state == UI_TITLE) draw_title(title_alpha);
        else if (ui_state == UI_MENU)  draw_menu();
        else                           draw_toast();

        mixer_try_play();
        rdpq_detach_show();
        mixer_try_play();

        save_flush();
    }
}
