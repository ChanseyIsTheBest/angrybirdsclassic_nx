#ifndef POWERUP_PATCH_H
#define POWERUP_PATCH_H
#include <stddef.h>

// Called once at boot, after config load and set_asset_base(), before the
// engine starts. If config "powerups" is "max" or a number, tops every
// power-up in the save (<game dir>/settings.lua) up to that many available
// (max = 9999, the most the game displays). "off" (default): no-op.
void powerup_patch_init(void);

// "off"/"max"/number -> target count (0 = disabled, capped at 9999).
int powerups_target_from_config(const char *value);

// Pure text edit of a decrypted settings.lua: returns a malloc'd new text (and
// its length / number of power-ups changed), or NULL if nothing needs changing
// (*changed = 0) or the text isn't in the layout the game writes (*changed = -1).
char *powerups_edit_save_text(const char *in, size_t len, int target,
                              size_t *out_len, int *changed);
#endif
