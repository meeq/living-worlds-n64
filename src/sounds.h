#pragma once

/* Initialise libdragon's audio + mixer + wav64 subsystems at the rates
 * matched to our streamed Opus loops. */
void sounds_init(void);

/* Advance the two-channel fade envelope (gated by sound_on from settings). */
void sounds_tick(float dt);

/* Begin playing `slug` (or NULL/empty for silence) with a ~2 s fade-in,
 * fading out whatever was previously playing over ~0.4 s. */
void start_scene_sound(const char *slug, float max_vol);
