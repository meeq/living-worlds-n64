#include "scene.h"

#include <stdlib.h>
#include <string.h>

#include <libdragon.h>

#include "palette.h"
#include "settings.h"
#include "sounds.h"

#define MAX_SCENES 32

const lw_header_t *hdr;
int                scene_count;

static surface_t idx_surf;    /* FMT_CI8 view over the current scene's pixel bytes */
static char      scene_paths[MAX_SCENES][96];
static void     *scene_buf;

static void load_scene_file(const char *path)
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

    const uint8_t    *pal_table = (const uint8_t *)(hdr + 1);
    const lw_cycle_t *cycles    = (const void *)(pal_table + (uint32_t)hdr->num_palettes * hdr->num_colors * 3);
    const lw_tl_t    *timeline  = (const void *)(cycles + hdr->num_cycles);
    palette_set_scene(hdr, pal_table, cycles, timeline);

    const uint8_t *pixels = (const uint8_t *)scene_buf + hdr->pixel_offset;
    idx_surf = surface_make_linear((void *)pixels, FMT_CI8, hdr->width, hdr->height);
    /* The RDP DMAs the index bytes; flush them from the CPU cache. */
    data_cache_hit_writeback((void *)pixels, (uint32_t)hdr->width * hdr->height);
}

static int cmp_path(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

void scene_init(void)
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
    assertf(scene_count > 0, "no rom:/*.lw scenes found");
}

void scene_load(int idx)
{
    scene_idx = idx;
    load_scene_file(scene_paths[idx]);
    start_scene_sound(hdr->audio_slug, hdr->audio_volume_q8 / 256.0f);
}

void scene_advance(int delta)
{
    if (scene_count <= 1) return;
    scene_load((scene_idx + delta + scene_count) % scene_count);
}

void scene_draw(void)
{
    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_filter(FILTER_POINT);
    palette_upload();
    rdpq_tex_blit(&idx_surf, 0, 0, NULL);
}
