#pragma once

#include <libdragon.h>

/* Load fonts + button sprite and pre-measure title fragments. Must run
 * after rdpq_init / display_init. */
void ui_init(void);

/* Per-frame state machine + input dispatch. UI_TITLE fades on any button
 * (or after 10 s); UI_SCENE runs off-menu bindings; UI_MENU drives the
 * settings panel. Start toggles into and out of the menu. */
void ui_tick(joypad_buttons_t pressed, float dt);

/* Draw whichever overlay belongs to the current UI state (title/menu/toast). */
void ui_draw(void);
