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
 * Each scene also references an ambient audio loop (rom:/<slug>.wav64, streamed
 * by libdragon's wav64 module) with a per-scene max volume. On scene change the
 * outgoing loop fades out (~0.4 s) while the new one fades in (~2 s), matching
 * the reference's startSceneAudio / stopSceneAudio. C-down toggles sound; pause
 * and the global mute both gate per-channel volumes to zero without disturbing
 * the underlying stream positions, so resuming is seamless.
 *
 * Asset: rom:/ *.lw (format LWL3), produced by tools/convert_scene.py.
 */

#include <libdragon.h>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define CYCLE_SPEED   280.0f    /* matches the reference engine (palette.js) */
#define SECS_PER_DAY  86400
#define SECS_MID_DAY (SECS_PER_DAY / 2)
#define SIM_RATE      60.0f     /* simulated seconds per real second (~24 min/day) */
#define SCRUB_RATE    10800.0f  /* scrub speed while C-left/right held (3 h/s) */
#define MAX_COLORS    256
#define MAX_SCENES    32
#define FONT_HUD      1

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

typedef enum { TIME_AUTO, TIME_RTC, TIME_HOLD } time_src_t;

/* working buffers */
static surface_t idx_surf;            /* FMT_CI8 view over the full-res pixel bytes */
static uint8_t   base_r[MAX_COLORS];
static uint8_t   base_g[MAX_COLORS];
static uint8_t   base_b[MAX_COLORS];
static uint16_t  tlut[MAX_COLORS] __attribute__((aligned(16)));

/* scene data */
static void              *scene_buf;  /* asset_load buffer (kept alive) */
static const lw_header_t *hdr;        /* overlay over scene_buf; source of truth for dims */
static const uint8_t     *pal_table;  /* hdr->num_palettes * hdr->num_colors * 3, in scene_buf */
static const lw_cycle_t  *cycles;     /* hdr->num_cycles entries, in scene_buf */
static const lw_tl_t     *timeline;   /* hdr->num_tl entries, sorted by offset, in scene_buf */

/* scene catalog (*.lw enumerated at startup) */
static char      scene_paths[MAX_SCENES][96];
static int       scene_count;
static int       scene_idx;

/* runtime state */
static bool       cycling     = true;
static bool       blendshift  = true;
static bool       paused      = false;
static bool       show_hud    = true;
static bool       sound_on    = true;
static uint32_t   scene_ms    = 0;             /* pausable clock driving the cycling */
static float      time_of_day = SECS_MID_DAY;  /* seconds since midnight, [0, 86400) */
static time_src_t time_src    = TIME_AUTO;
static bool       rtc_present = false;

/* wav64 cache: open each unique slug at most once and stream from ROM. */
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

/* Enumerate *.lw into scene_paths[], sorted by filename. */
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
    scene_ms = 0; // restart cycling from the new scene's t=0
    start_scene_audio(hdr->audio_slug, hdr->audio_volume_q8 / 256.0f);
}

/* Resolve the time-of-day palette for `t` (seconds since midnight) into
 * base_r/g/b. Mirrors the reference's setTimeOfDayPalette: find the timeline
 * entries bracketing `t` (wrapping across midnight), then lerp the two palettes
 * per channel by the fractional position between them. */
static void rebuild_base_rgb(float t)
{
    int ti = (int)t % SECS_PER_DAY;
    if (ti < 0) ti += SECS_PER_DAY;

    /* timeline is sorted by offset: bi/ai bracket ti, wrapping across midnight. */
    int n_tl = hdr->num_tl, n_col = hdr->num_colors;
    int ai = 0;
    while (ai < n_tl && (int)timeline[ai].off <= ti) ai++;
    int bi = ai - 1;
    int boff = (bi < 0)      ? (int)timeline[bi = n_tl - 1].off - SECS_PER_DAY
                             : (int)timeline[bi].off;
    int aoff = (ai >= n_tl)  ? (int)timeline[ai = 0].off + SECS_PER_DAY
                             : (int)timeline[ai].off;

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

/* Find a cached wav64 for `slug`, opening (and looping) it if first use. */
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

/* Step `ch_vol[ch]` toward `ch_target[ch]` at `ch_rate[ch]` and push the
 * (sound_on/!paused-gated) volume to the mixer. Stops the channel when an
 * outgoing fade reaches zero. */
static void update_channel(int ch, float dt)
{
    if (!ch_slug[ch]) return;
    if (ch_vol[ch] != ch_target[ch]) {
        float step = ch_rate[ch] * dt;
        if (ch_vol[ch] < ch_target[ch])
            ch_vol[ch] = (ch_vol[ch] + step > ch_target[ch]) ? ch_target[ch] : ch_vol[ch] + step;
        else
            ch_vol[ch] = (ch_vol[ch] - step < ch_target[ch]) ? ch_target[ch] : ch_vol[ch] - step;
    }
    float gated = (sound_on && !paused) ? ch_vol[ch] : 0.0f;
    mixer_ch_set_vol(ch, gated, gated);
    if (ch_vol[ch] == 0.0f && ch_target[ch] == 0.0f) {
        mixer_ch_stop(ch);
        ch_slug[ch] = NULL;
    }
}

/* Begin playing `slug` (or NULL for silence). Crossfades by flipping
 * `active_ch` to the other mixer channel: the outgoing loop continues
 * streaming on its existing channel and fades out, while the new loop fades
 * in on the freshly-claimed channel. Mirrors reference startSceneAudio /
 * stopSceneAudio (~2 s in, ~0.4 s out).
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
        float delta = fabsf(max_vol - ch_vol[active_ch]);
        ch_rate[active_ch]   = delta > 0 ? delta / FADE_IN_S : 0.0f;
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

static void draw_hud(void)
{
    int t = (int)time_of_day;
    int hh = t / 3600, mm = (t / 60) % 60;
    const char *src =
        time_src == TIME_AUTO ? "auto" :
        time_src == TIME_RTC  ? (rtc_present ? "rtc" : "rtc(soft)") : "hold";

    rdpq_text_printf(NULL, FONT_HUD, 8, 14,
        "%s  [%d/%d]", hdr->title, scene_idx + 1, scene_count);
    rdpq_text_printf(NULL, FONT_HUD, 8, 26,
        "%02d:%02d %s  cycle:%s blend:%s snd:%s%s",
        hh, mm, src,
        cycling ? "on" : "off",
        blendshift ? "on" : "off",
        sound_on  ? (hdr->audio_slug[0] ? hdr->audio_slug : "off") : "off",
        paused ? "  PAUSED" : "");
    rdpq_text_printf(NULL, FONT_HUD, 8, 38,
        "A:cyc B:bld C<>:scrub Cup:time Cdn:snd St:pause L/R:scene Z:hud");
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
    load_scene(scene_paths[scene_idx]);
    start_scene_audio(hdr->audio_slug, hdr->audio_volume_q8 / 256.0f);

    rdpq_text_register_font(FONT_HUD, rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO));

    uint32_t last = get_ticks_ms();
    while (1) {
        joypad_poll();
        joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        joypad_buttons_t held    = joypad_get_buttons_held(JOYPAD_PORT_1);
        if (pressed.a)      cycling    = !cycling;
        if (pressed.b)      blendshift = !blendshift;
        if (pressed.start)  paused     = !paused;
        if (pressed.z)      show_hud   = !show_hud;
        if (pressed.c_up)   time_src   = (time_src + 1) % 3;
        if (pressed.c_down) sound_on   = !sound_on;
        if (pressed.l)      switch_scene(-1);
        if (pressed.r)      switch_scene(+1);

        uint32_t now = get_ticks_ms();
        uint32_t dms = now - last;
        last = now;
        float dt = dms / 1000.0f;
        if (!paused) scene_ms += dms;

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

        if (show_hud) draw_hud();

        mixer_try_play();
        rdpq_detach_show();
        mixer_try_play();
    }
}
