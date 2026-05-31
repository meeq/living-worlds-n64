#pragma once

/* Read EEPROM and apply to the settings globals (silent no-op on carts
 * without EEPROM or with a stale/missing magic). Must run after scene_init()
 * so the persisted scene index can be range-checked. */
void save_load(void);

/* Persist the current settings if `save_dirty` is set. Cheap memcpy-class
 * call into libdragon's RAM-cached EEPROM driver; rate-limited to once per
 * frame so a held scrub doesn't burst writes. */
void save_flush(void);
