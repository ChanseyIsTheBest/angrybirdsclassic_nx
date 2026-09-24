// locale_patch.c -- at boot, patch the STOCK gamelogic.lua to force the UI
// language from config.txt (or the Switch system language when "auto"), limited
// to the 11 locales the game actually ships. Pipeline: AES-256-CBC decrypt ->
// LZMA(alone) decompress -> edit Lua bytecode -> LZMA compress -> AES encrypt,
// written next to the .nro and served in place of the stock file via
// locale_patch_redirect() (consulted by resolve_asset_path()/fopen_fake()).
//
// Simplified Chinese additionally gets its own title logo (愤怒的小鸟). The game
// ships the art -- MENU_LOGO_CN, the MENU_LOGO_*_CN composites and the
// SPLASH_ANGRY_BIRDS_CN splash -- but its scripts hardcode the English names,
// so for zh_CN we also:
//   * rename the logo string constants in scripts_common/menus/splashes.lua
//     (boot/loading screen) and scripts_common/menus/main_menu.lua (title menu);
//   * fix 1024x600_splash/SPLASHES_COMPOSPRITES.dat, the splash profile the
//     engine picks on a 16:9 screen, whose CN composite is misnamed "COMPO".
// Every CN name is checked against the sprite data first, so a different asset
// set falls back to the English logo instead of requesting a missing sprite.
// English is the only other language with its own logo art, so no other locale
// needs this.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "libc_shim.h"      // resolve_asset_path, DATA_DIR
#include "locale_patch.h"
#include "patch_bytecode.h"
#include "sprite_patch.h"
#include "aes.h"

size_t lzma_alone_decode(const unsigned char*, size_t, unsigned char*, size_t);
size_t lzma_alone_encode(const unsigned char*, size_t, unsigned char*, size_t);

// 32-byte AES-256 key used by the game's script encryption.
static const uint8_t KEY[32] = "USCaPQpA4TSNVxMI1v9SK9UC0yZuAnb2";

// ---------------------------------------------------------------------------
// Redirect table: engine-relative path suffix -> patched host file.
// Filled only during locale_patch_init() (before the engine starts), read-only
// afterwards. Empty => every lookup falls through to the stock asset.
// ---------------------------------------------------------------------------
#define MAX_REDIRECTS 12
static struct { char suffix[96]; char path[256]; } g_redirects[MAX_REDIRECTS];
static int g_nredirects = 0;

static void add_redirect(const char *suffix, const char *path) {
  for (int i = 0; i < g_nredirects; i++)
    if (strcasecmp(g_redirects[i].suffix, suffix) == 0) {       // replace
      snprintf(g_redirects[i].path, sizeof g_redirects[i].path, "%s", path);
      return;
    }
  if (g_nredirects >= MAX_REDIRECTS) { debugPrintf("locale_patch: redirect table full\n"); return; }
  snprintf(g_redirects[g_nredirects].suffix, sizeof g_redirects[0].suffix, "%s", suffix);
  snprintf(g_redirects[g_nredirects].path, sizeof g_redirects[0].path, "%s", path);
  g_nredirects++;
}

const char *locale_patch_redirect(const char *rel) {
  if (!rel || g_nredirects == 0) return NULL;
  size_t rl = strlen(rel);
  for (int i = 0; i < g_nredirects; i++) {
    size_t sl = strlen(g_redirects[i].suffix);
    // whole path components only: "x/scripts/menus/a.lua" must not match a
    // suffix of "scripts_common/menus/a.lua" or vice versa.
    if (rl >= sl && strcasecmp(rel + rl - sl, g_redirects[i].suffix) == 0 &&
        (rl == sl || rel[rl - sl - 1] == '/'))
      return g_redirects[i].path;
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Language selection
// ---------------------------------------------------------------------------

// Map a language code (config value like "fr","pt_BR","es_419","zh_TW", or a
// Switch system code like "en-US","zh-Hant","es-419") to one of the game's
// shipped locales. Returns NULL for English (no patch needed) or unsupported.
static const char *map_to_game_locale(const char *raw) {
  if (!raw || !raw[0]) return NULL;
  char l0 = (char)tolower((unsigned char)raw[0]);
  char l1 = raw[1] ? (char)tolower((unsigned char)raw[1]) : 0;
  #define IS(a,b) (l0==(a) && l1==(b))
  if (IS('e','n')) return NULL;            // English is the stock default
  if (IS('f','r')) return "fr_FR";
  if (IS('i','t')) return "it_IT";
  if (IS('d','e')) return "de_DE";
  if (IS('r','u')) return "ru_RU";
  if (IS('j','a')) return "ja_JP";
  if (IS('p','t')) return "pt_BR";         // game ships Brazilian only
  if (IS('e','s')) {                       // Spain vs Latin American
    if (strstr(raw,"419")||strstr(raw,"MX")||strstr(raw,"mx")||strstr(raw,"US")||
        strstr(raw,"us")||strstr(raw,"AR")||strstr(raw,"CO")||strstr(raw,"CL")||
        strstr(raw,"PE")||strstr(raw,"VE")) return "es_419";
    return "es_ES";
  }
  if (IS('z','h')) {                       // Simplified vs Traditional
    if (strstr(raw,"TW")||strstr(raw,"tw")||strstr(raw,"Hant")||strstr(raw,"hant")||
        strstr(raw,"HK")||strstr(raw,"hk")||strstr(raw,"MO")||strstr(raw,"mo")) return "zh_TW";
    return "zh_CN";
  }
  #undef IS
  return NULL;                             // not shipped by the game
}

// Determine the raw language code: config.language, or the system language when "auto".
static void resolve_raw_lang(char *out, size_t n) {
  out[0] = 0;
  if (config.language[0] && strcmp(config.language, "auto") != 0) {
    snprintf(out, n, "%s", config.language);
    return;
  }
  u64 code = 0;
  if (R_SUCCEEDED(setInitialize())) {
    if (R_SUCCEEDED(setGetSystemLanguage(&code))) {
      char buf[9] = {0};
      memcpy(buf, &code, sizeof(u64));     // e.g. "en-US","de","zh-Hant"
      snprintf(out, n, "%s", buf);
    }
    setExit();
  }
  if (!out[0]) snprintf(out, n, "en");
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

enum { CAP = 512 * 1024 };
static unsigned char *g_enc, *g_bc, *g_pbc, *g_cs;   // CAP-sized work buffers

static long read_file(const char *path, unsigned char *buf, long cap) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > cap) { fclose(f); return -1; }
  long got = (long)fread(buf, 1, (size_t)sz, f);
  fclose(f);
  return got == sz ? sz : -1;
}

static int write_file(const char *path, const unsigned char *buf, size_t len) {
  FILE *o = fopen(path, "wb");
  if (!o) { debugPrintf("locale_patch: cannot write %s\n", path); return 0; }
  size_t wrote = fwrite(buf, 1, len, o);
  fclose(o);
  if (wrote != len) { debugPrintf("locale_patch: short write %s\n", path); return 0; }
  return 1;
}

// Read a stock asset (engine-relative path) into buf. Returns length or -1.
static long read_asset(const char *rel, unsigned char *buf, long cap) {
  char path[512];
  if (!resolve_asset_path(rel, path, sizeof path)) return -1;
  return read_file(path, buf, cap);
}

// ---------------------------------------------------------------------------
// Encrypted-script patching
// ---------------------------------------------------------------------------

typedef size_t (*BcXform)(const uint8_t *bc, size_t n, uint8_t *out, size_t cap, void *ctx);

// Decrypt + decompress the stock script `rel`, run `xf` over its bytecode,
// recompress + re-encrypt to DATA_DIR/out_name and redirect `suffix` to it.
static int patch_script(const char *rel, const char *suffix, const char *out_name,
                        BcXform xf, void *ctx) {
  long elen = read_asset(rel, g_enc, CAP);
  if (elen < 16) { debugPrintf("locale_patch: %s: stock script not found/readable\n", rel); return 0; }
  long blk = (elen / 16) * 16;

  struct AES_ctx aes; uint8_t iv[16] = {0};
  AES_init_ctx_iv(&aes, KEY, iv);
  AES_CBC_decrypt_buffer(&aes, g_enc, (size_t)blk);

  size_t bclen = lzma_alone_decode(g_enc, (size_t)blk, g_bc, CAP);
  if (!bclen || !(g_bc[0] == 0x1b && g_bc[1] == 'L')) {
    debugPrintf("locale_patch: %s: decompress failed (wrong key or format?)\n", rel); return 0;
  }
  size_t plen = xf(g_bc, bclen, g_pbc, CAP, ctx);
  if (!plen) { debugPrintf("locale_patch: %s: bytecode patch failed\n", rel); return 0; }

  size_t cslen = lzma_alone_encode(g_pbc, plen, g_cs, CAP - 16);
  if (!cslen) { debugPrintf("locale_patch: %s: recompress failed\n", rel); return 0; }
  // PKCS#7, exactly like every stock script: always 1..16 bytes, each equal to
  // the pad length (a full block when already aligned). The engine takes the
  // last decrypted byte as the pad length and rejects the script if it's > 16.
  // Zero padding only worked by accident (a trailing 0 strips nothing): when
  // the compressed data happened to be block-aligned -- main_menu.lua did --
  // the file ended on a raw LZMA byte and the game refused to load it.
  size_t pad = 16 - (cslen % 16);
  memset(g_cs + cslen, (int)pad, pad);
  size_t enclen = cslen + pad;
  uint8_t iv2[16] = {0};
  AES_init_ctx_iv(&aes, KEY, iv2);
  AES_CBC_encrypt_buffer(&aes, g_cs, enclen);

  char outp[512];
  snprintf(outp, sizeof outp, "%s/%s", DATA_DIR, out_name);
  if (!write_file(outp, g_cs, enclen)) return 0;
  add_redirect(suffix, outp);
  debugPrintf("locale_patch: wrote %s (%zu bytes) for %s\n", outp, enclen, rel);
  return 1;
}

static size_t xf_gamelogic(const uint8_t *bc, size_t n, uint8_t *out, size_t cap, void *ctx) {
  return patch_gamelogic_bytecode(bc, n, (const char *)ctx, out, cap);
}

typedef struct { const LuaStrMap *map; int nmap; int min_hits; } RenameCtx;

static size_t xf_rename(const uint8_t *bc, size_t n, uint8_t *out, size_t cap, void *vctx) {
  RenameCtx *r = vctx;
  int hits = 0;
  size_t len = patch_rename_string_consts(bc, n, r->map, r->nmap, out, cap, &hits);
  debugPrintf("locale_patch: renamed %d logo constant(s)\n", hits);
  return (len && hits >= r->min_hits) ? len : 0;   // no hit => script changed; keep stock
}

// ---------------------------------------------------------------------------
// Simplified Chinese title logo
// ---------------------------------------------------------------------------

// Does any of `files` (in data/images/<dir>/) define sprite/composite `name`?
static int images_define(const char *dir, const char *const *files, int nfiles, const char *name) {
  char rel[160];
  for (int i = 0; i < nfiles; i++) {
    snprintf(rel, sizeof rel, "data/images/%s/%s", dir, files[i]);
    long n = read_asset(rel, g_enc, CAP);
    if (n > 0 && ka3d_has_name(g_enc, (size_t)n, name)) return 1;
  }
  return 0;
}

// Boot/loading screen: SPLASH_ANGRY_BIRDS -> SPLASH_ANGRY_BIRDS_CN.
static void patch_cn_splash(void) {
  // The engine chooses the splash profile from the screen aspect ratio (<=4:3
  // 1024x768, <=3:2 1440x960, <=1.8 1024x600, wider 1024x550). Make sure every
  // shipped profile resolves SPLASH_ANGRY_BIRDS_CN before switching the script.
  static const char *const profiles[] = {
    "1024x768_splash", "1440x960_splash", "1024x600_splash", "1024x550_splash" };
  static const char *const sheets[] = {
    "SPLASHES_SHEET_0.dat", "SPLASHES_SHEET_1.dat", "SPLASHES_SHEET_2.dat", "SPLASHES_SHEET_3.dat" };

  for (size_t i = 0; i < sizeof profiles / sizeof *profiles; i++) {
    char rel[160];
    snprintf(rel, sizeof rel, "data/images/%s/SPLASHES_COMPOSPRITES.dat", profiles[i]);
    long n = read_asset(rel, g_enc, CAP);
    if (n <= 0) continue;                  // profile not shipped: engine can't pick it either

    if (!images_define(profiles[i], sheets, 4, "MENU_LOGO_CN")) {
      debugPrintf("locale_patch: %s has no MENU_LOGO_CN; keeping English splash\n", profiles[i]);
      return;
    }
    // images_define() reused g_enc; read the composite table again
    n = read_asset(rel, g_enc, CAP);
    if (n <= 0) return;
    if (ka3d_has_name(g_enc, (size_t)n, "SPLASH_ANGRY_BIRDS_CN")) continue;   // already fine

    size_t fixed = fix_splash_cn_composite(g_enc, (size_t)n, g_pbc, CAP);
    if (!fixed) {
      debugPrintf("locale_patch: %s has no usable CN splash composite; keeping English splash\n",
                  profiles[i]);
      return;
    }
    char outp[512];
    snprintf(outp, sizeof outp, "%s/%s_SPLASHES_COMPOSPRITES_patched.dat", DATA_DIR, profiles[i]);
    if (!write_file(outp, g_pbc, fixed)) return;
    add_redirect(rel + 5 /* drop "data/" */, outp);
    debugPrintf("locale_patch: %s: renamed CN splash composite -> SPLASH_ANGRY_BIRDS_CN\n", profiles[i]);
  }

  static const LuaStrMap map[] = { { "SPLASH_ANGRY_BIRDS", "SPLASH_ANGRY_BIRDS_CN" } };
  RenameCtx ctx = { map, 1, 1 };
  patch_script("data/scripts_common/menus/splashes.lua", "scripts_common/menus/splashes.lua",
               "splashes_patched.lua", xf_rename, &ctx);
}

// Title (main) menu: MENU_LOGO[_FREE][_HD] -> ..._CN. The Switch always renders
// at 1080p, so the menu art comes from the 1024x768 set.
static void patch_cn_main_menu(void) {
  static const char *const menu_files[] = {
    "MENU_COMPOSPRITES.dat", "MENU_ELEMENTS_1.dat", "MENU_ELEMENTS_2.dat" };
  static const LuaStrMap all[] = {
    { "MENU_LOGO",         "MENU_LOGO_CN"         },
    { "MENU_LOGO_HD",      "MENU_LOGO_HD_CN"      },
    { "MENU_LOGO_FREE",    "MENU_LOGO_FREE_CN"    },
    { "MENU_LOGO_FREE_HD", "MENU_LOGO_FREE_HD_CN" },
  };
  LuaStrMap map[sizeof all / sizeof *all];
  int nmap = 0;
  for (size_t i = 0; i < sizeof all / sizeof *all; i++) {
    if (images_define("1024x768", menu_files, 3, all[i].to)) map[nmap++] = all[i];
    else debugPrintf("locale_patch: %s missing; keeping %s\n", all[i].to, all[i].from);
  }
  if (!nmap) return;
  RenameCtx ctx = { map, nmap, 1 };
  patch_script("data/scripts_common/menus/main_menu.lua", "scripts_common/menus/main_menu.lua",
               "main_menu_patched.lua", xf_rename, &ctx);
}

// ---------------------------------------------------------------------------

void locale_patch_init(void) {
  char raw[24];
  resolve_raw_lang(raw, sizeof raw);
  const char *loc = map_to_game_locale(raw);
  if (!loc) {
    debugPrintf("locale_patch: '%s' -> English/unsupported; using stock script\n", raw);
    return;
  }
  debugPrintf("locale_patch: '%s' -> %s\n", raw, loc);

  g_enc = malloc(CAP); g_bc = malloc(CAP); g_pbc = malloc(CAP); g_cs = malloc(CAP);
  if (g_enc && g_bc && g_pbc && g_cs) {
    int ok = patch_script("data/scripts_common/gamelogic.lua", "scripts_common/gamelogic.lua",
                          "gamelogic_patched.lua", xf_gamelogic, (void *)loc);
    // Only Simplified Chinese has its own logo art (besides English). Skip it if
    // the language patch failed, since the game would then be running in English.
    // config zh_logo: "all" (default) | "splash" (boot screen only) | "off".
    if (ok && strcmp(loc, "zh_CN") == 0) {
      int logo_off = !strcasecmp(config.zh_logo, "off");
      int splash_only = !strcasecmp(config.zh_logo, "splash");
      debugPrintf("locale_patch: zh_logo=%s\n", config.zh_logo);
      if (!logo_off) patch_cn_splash();
      if (!logo_off && !splash_only) patch_cn_main_menu();
    }
  }
  free(g_enc); free(g_bc); free(g_pbc); free(g_cs);
  g_enc = g_bc = g_pbc = g_cs = NULL;
}
