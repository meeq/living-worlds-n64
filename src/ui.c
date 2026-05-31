#include "ui.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <libdragon.h>

#include "clock.h"
#include "scene.h"
#include "settings.h"

#define FONT_HUD     1
#define FONT_TITLE   2

/* Button-icon spritemap: 48x48 PNG arranged as a 4x4 grid of 12x12 cells
 * (rom:/buttons.sprite). Row-major btn_t indices below. The icons take the
 * place of "L/Z", "Start/A", "C-Up", etc. in the menu cheat sheet, and the
 * Start icon also appears on the title-screen prompt. */
#define BTN_SIZE     12

/* Title screen: ~600 px text band centered on a 640 px screen, dark-vignetted
 * over the live scene, fading out at TITLE_FADE_S either on any-button or
 * after TITLE_HOLD_S of inactivity. Pressing Start jumps straight to the
 * menu instead. */
#define TITLE_HOLD_S 10.0f
#define TITLE_FADE_S 0.6f
#define TITLE_VIG_A  160       /* peak vignette alpha (0..255) */

/* Action toast: one-line confirmation in the bottom-right corner whenever an
 * off-menu binding fires. Anchored inside a ~10% NTSC action-safe inset so it
 * stays visible on overscanning CRTs. Re-styled per-frame on FONT_HUD style 2
 * so the color/outline alpha can track the fade. */
#define TOAST_HOLD_S 1.5f
#define TOAST_FADE_S 0.4f
#define TOAST_STYLE  2
#define TOAST_SAFE_R 32        /* right action-safe inset (px) */
#define TOAST_SAFE_B 32        /* bottom action-safe inset (px) */

/* Menu panel: 360 px wide, centered on 640x480, sized to fit header + 7 rows
 * + 3-line cheat sheet with the bottom edge well inside the NTSC action-safe. */
#define MENU_X0      140
#define MENU_Y0      60
#define MENU_X1      500
#define MENU_Y1      410
#define MENU_PAD     14        /* inner padding (px) */
#define MENU_ROW_H   24        /* row spacing */
#define MENU_PANEL_A 190       /* panel alpha (0..255) */

#define SCRUB_RATE   10800.0f  /* hold-to-scrub speed: 3 h/s */

typedef enum {
    BTN_D_UP,    BTN_D_RIGHT, BTN_D_LEFT, BTN_D_DOWN,
    BTN_L,       BTN_Z,       BTN_R,      BTN_DPAD,
    BTN_A,       BTN_B,       BTN_START,  BTN_CTRL,
    BTN_C_UP,    BTN_C_RIGHT, BTN_C_LEFT, BTN_C_DOWN,
} btn_t;

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

static sprite_t *btn_sprite;
static int       title_press_w;   /* "Press " width in FONT_TITLE (px) */
static int       title_options_w; /* " for Options" width in FONT_TITLE (px) */

static ui_state_t ui_state      = UI_TITLE;
static float      title_alpha   = 1.0f;        /* 1.0 visible, 0.0 gone */
static float      title_t       = 0.0f;        /* seconds in UI_TITLE */
static bool       title_dismiss = false;       /* fade-out requested */
static int        menu_focus    = ROW_SCENE;
static char       toast_msg[64] = "";
static float      toast_t       = TOAST_HOLD_S + TOAST_FADE_S;  /* start hidden */

/* ---------- low-level draw helpers ---------- */

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

/* Blit one 12x12 cell from btn_sprite, modulated by `a` (1.0 = opaque).
 * Resets RDP mode each call: the surrounding text renderer leaves the
 * pipeline in a text-glyph mode that produces garbled output for raw
 * sprite blits. */
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

/* ---------- toast ---------- */

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

/* ---------- title ---------- */

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

/* ---------- menu ---------- */

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

/* ---------- input handlers ---------- */

static void menu_change(int row, int dir)
{
    switch (row) {
        case ROW_SCENE:       scene_advance(dir); break;
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
        case ROW_SCENE:       scene_advance(+1); break;
        case ROW_TIME_OF_DAY: /* no toggle for scrub-only row */ break;
        default:              menu_change(row, +1); break;
    }
}

/* Returns true to close the menu. `dt` drives the analog-stick scrub on the
 * Time of day row. */
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
        scene_advance(scene_dir);
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

/* ---------- public ---------- */

void ui_init(void)
{
    /* Two fonts: the small mono font is the menu's body text; the variable-
     * width debug font is reserved for the title credits. Style 0 on FONT_HUD
     * is the default white-on-black; style 1 is the focused-row yellow.
     * Selected mid-string with the ^00 / ^01 escapes from rdpq_text. */
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
     * draw_title can re-center "Press [Start] for Options" every frame
     * without allocating. */
    title_press_w   = title_measure("Press ");
    title_options_w = title_measure(" for Options");
}

void ui_tick(joypad_buttons_t pressed, float dt)
{
    toast_t += dt;

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
}

void ui_draw(void)
{
    if      (ui_state == UI_TITLE) draw_title(title_alpha);
    else if (ui_state == UI_MENU)  draw_menu();
    else                           draw_toast();
}
