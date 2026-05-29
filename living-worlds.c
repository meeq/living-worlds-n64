/*
 * Living Worlds - an N64 CI8 palette-cycling viewer (libdragon).
 *
 * Reproduces Joseph Huckaby's "Canvas Cycle" demo (color-cycling pixel art by
 * Mark Ferrari) to showcase the N64 CI8 texture format. The 256-color indexed
 * image is uploaded once; animation comes purely from rotating ranges of the
 * palette each frame and re-uploading the RDP TLUT, while the CI8 pixel bytes
 * never change. Smoothing uses Huckaby's "BlendShift" (sub-index interpolation).
 *
 * Asset: rom:/scene.lw, produced by tools/convert_scene.py (see that file and
 * the project plan for the binary layout).
 */

#include <libdragon.h>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CYCLE_SPEED   280.0f   /* matches the reference engine (palette.js) */
#define MAX_COLORS    256
#define MAX_SCENES    32
#define FONT_HUD      1

typedef struct {
    uint8_t  reverse;
    uint8_t  low;
    uint8_t  high;
    uint16_t rate;
} cycle_t;

/* scene data (rebuilt on every load_scene) */
static void     *scene_buf;            /* asset_load buffer (kept alive) */
static int       img_w, img_h;
static int       num_colors, num_cycles;
static char      scene_title[64];      /* embedded title, shown in the HUD */
static uint8_t   base_r[MAX_COLORS];   /* original palette, full 8-bit precision */
static uint8_t   base_g[MAX_COLORS];
static uint8_t   base_b[MAX_COLORS];
static cycle_t   cycles[MAX_COLORS];
static uint16_t *work_pal;             /* 8-byte aligned; uploaded to TLUT each frame */
static surface_t idx_surf;             /* FMT_CI8 view over the full-res pixel bytes */

/* scene catalog (rom:/ *.lw enumerated at startup) */
static char      scene_paths[MAX_SCENES][96];
static int       scene_count;
static int       scene_idx;

/* runtime state */
static bool      cycling    = true;
static bool      blendshift = true;
static bool      paused     = false;
static bool      show_hud   = true;
static uint32_t  scene_ms   = 0;       /* pausable clock driving the cycling */

static inline uint16_t rd16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}
static inline uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
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
    assertf(memcmp(b, "LWLD", 4) == 0, "%s: bad magic", path);

    img_w        = rd16(b + 4);
    img_h        = rd16(b + 6);
    num_colors   = rd16(b + 8);
    num_cycles   = rd16(b + 10);
    uint32_t pix = rd32(b + 12);

    const uint8_t *pal = b + 16;
    for (int i = 0; i < num_colors; i++) {
        base_r[i] = pal[i * 3 + 0];
        base_g[i] = pal[i * 3 + 1];
        base_b[i] = pal[i * 3 + 2];
    }

    const uint8_t *cyc = pal + num_colors * 3;
    for (int i = 0; i < num_cycles; i++) {
        const uint8_t *c = cyc + i * 8;
        cycles[i].reverse = c[0];
        cycles[i].low     = c[1];
        cycles[i].high    = c[2];
        cycles[i].rate    = rd16(c + 4);
    }

    /* Title block (u16 len + UTF-8 bytes) sits between the cycle table and the
     * 8-byte-aligned pixel data. */
    const uint8_t *t = cyc + num_cycles * 8;
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

/* Rebuild work_pal from base palette, applying every active cycle.
 * Mirrors palette.js shiftColors/blendShiftColors: after rotating a range up by
 * the integer part of the amount, slot k blends toward its lower neighbour by the
 * fractional part (slot 0 wraps to the top of the range). */
static void build_palette(void)
{
    for (int i = 0; i < num_colors; i++)
        work_pal[i] = color_to_packed16(RGBA32(base_r[i], base_g[i], base_b[i], 0xFF));

    if (!cycling) return;

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

    data_cache_hit_writeback(work_pal, num_colors * sizeof(uint16_t));
}

static void draw_hud(void)
{
    rdpq_text_printf(NULL, FONT_HUD, 8, 14,
        "%s  [%d/%d]", scene_title, scene_idx + 1, scene_count);
    rdpq_text_printf(NULL, FONT_HUD, 8, 26,
        "cycle:%s blend:%s%s",
        cycling ? "on" : "off",
        blendshift ? "on" : "off",
        paused ? "  PAUSED" : "");
    rdpq_text_printf(NULL, FONT_HUD, 8, 38,
        "A:cycle B:blend Start:pause L/R:scene Z:hud");
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
        joypad_buttons_t p1 = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        if (p1.a)     cycling    = !cycling;
        if (p1.b)     blendshift = !blendshift;
        if (p1.start) paused     = !paused;
        if (p1.z)     show_hud   = !show_hud;
        if (p1.l)     switch_scene(-1);
        if (p1.r)     switch_scene(+1);

        uint32_t now = get_ticks_ms();
        if (!paused) scene_ms += now - last;
        last = now;

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
