/*
 * Living Worlds - an N64 CI8 palette-cycling demonstration.
 *
 * Reproduces color-cycling pixel art by Mark Ferrari to showcase the N64 CI8
 * texture format. The 256-color indexed image is uploaded once; the picture
 * never changes. Two animations run purely by changing the TLUT each frame:
 *
 *   - Color cycling: fast rotation of palette ranges driven by a cycle table.
 *   - Time of day: a slow morph of the whole palette. Each scene ships a set
 *     of keyframed palettes and a timeline mapping seconds-since-midnight to
 *     a palette; the displayed palette is a per-channel lerp between the two
 *     timeline palettes bracketing the current time. Cycling layers on top.
 *
 * The day clock has three sources: AUTO (advances time at a configurable speed),
 * RTC (follows the hardware/software clock), and HOLD (frozen). All three, plus
 * a manual time control and speed control, are exposed through the in-game menu.
 *
 * Scenes may also reference an ambient audio loop with a per-scene max volume.
 * On scene change the outgoing loop fades out while the new one fades in.
 *
 * The user interface is a very simple state machine:
 * UI_TITLE shows a credits overlay over the live first scene and fades out on
 * any button (or after 10 s); UI_SCENE is * the bare scene with no overlay;
 * UI_MENU is a translucent control panel overlaid on the scene and navigated
 * with the D-pad or analog stick.
 */

#include <sys/time.h>

#include <libdragon.h>

#include "clock.h"
#include "palette.h"
#include "scene.h"
#include "settings.h"
#include "sounds.h"
#include "ui.h"

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
        // Start the scene at the middle of the day so the demo looks good
        settimeofday(&(struct timeval){ .tv_sec = SECS_MID_DAY }, NULL);
    }

    sounds_init();

    scene_init();

    /* Pull persisted settings before the first scene load so the saved
     * scene_idx wins. For AUTO/RTC modes, snap time_of_day back to the real
     * clock -- the saved value is only authoritative when source is HOLD. */
    settings_load();
    if (time_src != TIME_HOLD) time_of_day = rtc_seconds_of_day();

    scene_load(scene_idx);

    ui_init();

    uint32_t last = get_ticks_ms();
    while (1) {
        joypad_poll();
        joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

        uint32_t now = get_ticks_ms();
        uint32_t dms = now - last;
        last = now;
        float dt = dms / 1000.0f;

        ui_tick(pressed, dt);

        palette_tick(dms);
        advance_time_of_day(dt);
        palette_rebuild(time_of_day);

        sounds_tick(dt);
        mixer_try_play();

        surface_t *disp = display_get();
        rdpq_attach_clear(disp, NULL);

        scene_draw();
        ui_draw();

        mixer_try_play();
        rdpq_detach_show();
        mixer_try_play();

        settings_flush();
    }
}
