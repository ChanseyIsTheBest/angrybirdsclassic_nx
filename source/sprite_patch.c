/* sprite_patch.c -- small read/patch helpers for the game's KA3D sprite data
 * (*_SHEET_n.dat / *_ELEMENTS_n.dat "SPRT" sheets and *_COMPOSPRITES.dat "COMP"
 * composite tables). Plain big-endian data; no crypto or compression.
 *
 * Used by the Simplified Chinese title-logo patch (locale_patch.c):
 *   - ka3d_has_name(): confirm a CN sprite/composite really exists before the
 *     scripts are pointed at it, so a different asset set can't end up asking
 *     the engine for a sprite that isn't there.
 *   - fix_splash_cn_composite(): the 1024x600 splash profile -- the one the
 *     engine picks for a 16:9 screen, i.e. every Switch -- ships the Chinese
 *     splash composite (background + MENU_LOGO_CN) under the placeholder name
 *     "COMPO" instead of "SPLASH_ANGRY_BIRDS_CN" (1024x550, 1024x768 and
 *     1440x960 all name it correctly). This renames it so the CN splash works.
 *
 * COMP layout (all big-endian):
 *   "KA3D" u32 size-8 | "COMP" u32 size-16 | u16 version | u16 count |
 *   count x { u16 len, name | u16 nchild | nchild x { u16 len, name, s16 x, s16 y } | u16 0 }
 */
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "sprite_patch.h"

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

int ka3d_has_name(const uint8_t *buf, size_t len, const char *name) {
  size_t n = strlen(name);
  if (!buf || n == 0 || n > 0xffff || len < 16 + 2 + n) return 0;
  if (memcmp(buf, "KA3D", 4) != 0) return 0;
  /* Every name in both SPRT and COMP files is a u16 BE length + bytes, so a
   * match needs the exact 2-byte length in front; a longer name that merely
   * contains `name` can't satisfy that. */
  for (size_t i = 16; i + 2 + n <= len; i++) {
    if (buf[i] == (uint8_t)(n >> 8) && buf[i + 1] == (uint8_t)n &&
        memcmp(buf + i + 2, name, n) == 0)
      return 1;
  }
  return 0;
}

/* Walks the COMP table. For each entry reports the name span and whether its
 * children include both wanted sprites. Returns 0 if the file is malformed. */
typedef struct {
  const uint8_t *name; uint16_t name_len;
  int has_bg, has_logo_cn;
} CompEntry;

static int comp_walk(const uint8_t *buf, size_t len,
                     int (*cb)(const CompEntry *e, void *ctx), void *ctx) {
  if (len < 20 || memcmp(buf, "KA3D", 4) != 0 || memcmp(buf + 8, "COMP", 4) != 0) return 0;
  if (be32(buf + 4) != len - 8 || be32(buf + 12) != len - 16) return 0;
  const uint8_t *p = buf + 16, *end = buf + len;
  uint16_t count = be16(p + 2);
  p += 4;
  for (uint16_t i = 0; i < count; i++) {
    CompEntry e = {0};
    if (end - p < 2) return 0;
    e.name_len = be16(p); p += 2;
    if (end - p < e.name_len + 2) return 0;
    e.name = p; p += e.name_len;
    uint16_t nchild = be16(p); p += 2;
    for (uint16_t c = 0; c < nchild; c++) {
      if (end - p < 2) return 0;
      uint16_t cl = be16(p); p += 2;
      if (end - p < cl + 4) return 0;
      if (cl == 29 && memcmp(p, "SPLASH_ANGRY_BIRDS_BACKGROUND", 29) == 0) e.has_bg = 1;
      if (cl == 12 && memcmp(p, "MENU_LOGO_CN", 12) == 0) e.has_logo_cn = 1;
      p += cl + 4;                                 /* name + s16 x + s16 y */
    }
    if (end - p < 2) return 0;
    p += 2;                                        /* trailing u16 */
    if (cb && !cb(&e, ctx)) break;
  }
  return p == end;                                 /* must consume the file exactly */
}

#define CN_SPLASH "SPLASH_ANGRY_BIRDS_CN"

typedef struct { const uint8_t *target; uint16_t target_len; int already; } FindCtx;

static int find_cb(const CompEntry *e, void *vctx) {
  FindCtx *f = vctx;
  if (e->name_len == sizeof(CN_SPLASH) - 1 && memcmp(e->name, CN_SPLASH, e->name_len) == 0) {
    f->already = 1;
    return 0;                                      /* correctly named: nothing to do */
  }
  if (!f->target && e->has_bg && e->has_logo_cn) {
    f->target = e->name;                           /* background + CN logo = the CN splash */
    f->target_len = e->name_len;
  }
  return 1;
}

size_t fix_splash_cn_composite(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap) {
  FindCtx f = {0};
  if (!comp_walk(in, in_len, find_cb, &f)) {
    /* a malformed file, or the walk stopped early because the name exists */
    if (!f.already) return 0;
  }
  if (f.already || !f.target) return 0;

  const size_t new_len = sizeof(CN_SPLASH) - 1;
  const size_t name_off = (size_t)(f.target - in);   /* first byte of the old name */
  const size_t out_len = in_len - f.target_len + new_len;
  if (out_len > out_cap || out_len > 0xffffffffu) return 0;

  memcpy(out, in, name_off - 2);                              /* up to the length prefix */
  out[name_off - 2] = (uint8_t)(new_len >> 8);
  out[name_off - 1] = (uint8_t)new_len;
  memcpy(out + name_off, CN_SPLASH, new_len);                 /* new name */
  memcpy(out + name_off + new_len, in + name_off + f.target_len,
         in_len - name_off - f.target_len);                   /* rest of the file */
  put_be32(out + 4, (uint32_t)(out_len - 8));                 /* KA3D chunk size */
  put_be32(out + 12, (uint32_t)(out_len - 16));               /* COMP chunk size */

  /* sanity: the result must parse and now contain the CN composite */
  FindCtx chk = {0};
  comp_walk(out, out_len, find_cb, &chk);
  return chk.already ? out_len : 0;
}
