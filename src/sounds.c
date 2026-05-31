#include "sounds.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <libdragon.h>

#include "scene.h"        /* for SLUG_MAX */
#include "settings.h"     /* for sound_on */

/* 48 kHz mono, two mixer channels (current loop + crossfading predecessor),
 * Opus-compressed wav64 streamed directly from ROM. Opus only encodes at
 * fixed rates (8/12/16/24/48 kHz), so audioconv64 ignores --wav-resample on
 * the Opus path and stamps the output at 48 kHz; matching the mixer output
 * rate here avoids any runtime resampling. */
#define AUDIO_FREQ      48000
#define AUDIO_BUFFERS   4
#define MIX_CHANNELS    2
#define FADE_IN_S       2.0f     /* reference: targetFPS * 2 frames in */
#define FADE_OUT_S      0.4f     /* reference: targetFPS / 2 frames out */
#define MAX_SOUND_SLUGS 16       /* 13 unique loops in the reference */

/* Open each unique slug at most once and stream from ROM. */
static char    wav_slugs[MAX_SOUND_SLUGS][SLUG_MAX];
static wav64_t wav_handles[MAX_SOUND_SLUGS];
static int     wav_count;

/* Two-channel fade engine. `active_ch` is whichever mixer channel hosts the
 * current scene's loop; the other channel hosts whatever it just replaced and
 * is fading out. Channels are never swapped: when a scene changes, we flip
 * the label and let both loops keep streaming on their own channels. */
static int         active_ch;
static const char *ch_slug[MIX_CHANNELS];   /* NULL == channel idle */
static float       ch_vol[MIX_CHANNELS];    /* current envelope value */
static float       ch_target[MIX_CHANNELS]; /* envelope target */
static float       ch_rate[MIX_CHANNELS];   /* units/sec toward target */

void sounds_init(void)
{
    audio_init(AUDIO_FREQ, AUDIO_BUFFERS);
    mixer_init(MIX_CHANNELS);
    wav64_init_compression(3);   /* Opus loops (--wav-compress 3) */
}

/* Open (and configure for looping) on first use, then return from the cache. */
static wav64_t *sound_get(const char *slug)
{
    for (int i = 0; i < wav_count; i++)
        if (strcmp(wav_slugs[i], slug) == 0) return &wav_handles[i];

    assertf(wav_count < MAX_SOUND_SLUGS, "wav64 cache full (slug=%s)", slug);
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

void sounds_tick(float dt)
{
    update_channel(0, dt);
    update_channel(1, dt);
}

/* Same-slug transitions intentionally keep the existing loop playing rather
 * than restarting it: the reference's HTML5 Audio rebuild produces an audible
 * pop when neighbouring scenes share a track (e.g. Feb clear -> cloudy), and
 * keeping the stream avoids it. Volume retargets to the new scene's max. */
void start_scene_sound(const char *slug, float max_vol)
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
        wav64_t *w = sound_get(slug);
        ch_slug[active_ch]   = wav_slugs[w - wav_handles];
        ch_vol[active_ch]    = 0.0f;
        ch_target[active_ch] = max_vol;
        ch_rate[active_ch]   = max_vol / FADE_IN_S;
        mixer_ch_set_vol(active_ch, 0.0f, 0.0f);
        wav64_play(w, active_ch);
    }
}
