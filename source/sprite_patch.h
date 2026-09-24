#ifndef SPRITE_PATCH_H
#define SPRITE_PATCH_H
/* sprite_patch.h -- helpers for the game's KA3D sprite data files. */
#include <stddef.h>
#include <stdint.h>

/* 1 if a KA3D sprite sheet (SPRT) or composite table (COMP) defines `name`. */
int ka3d_has_name(const uint8_t *buf, size_t len, const char *name);

/* If a SPLASHES_COMPOSPRITES.dat carries the Chinese splash composite
 * (SPLASH_ANGRY_BIRDS_BACKGROUND + MENU_LOGO_CN) under a wrong name -- the
 * 1024x600 profile ships it as "COMPO" -- write a copy with it renamed to
 * SPLASH_ANGRY_BIRDS_CN and return the new length. Returns 0 when no fix is
 * needed (already named correctly, or no such composite) or on malformed input. */
size_t fix_splash_cn_composite(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap);

#endif
