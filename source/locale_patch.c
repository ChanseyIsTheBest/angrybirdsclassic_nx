// locale_patch.c -- at boot, patch the STOCK gamelogic.lua to force the UI
// language from config.txt (or the Switch system language when "auto"), limited
// to the 11 locales the game actually ships. Pipeline: AES-256-CBC decrypt ->
// LZMA(alone) decompress -> inject getCurrentLocale override -> LZMA compress ->
// AES encrypt, written to DATA_DIR/gamelogic_patched.lua and served via a
// redirect in resolve_asset_path().
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "libc_shim.h"      // resolve_asset_path, DATA_DIR
#include "locale_patch.h"
#include "aes.h"

size_t lzma_alone_decode(const unsigned char*, size_t, unsigned char*, size_t);
size_t lzma_alone_encode(const unsigned char*, size_t, unsigned char*, size_t);
size_t patch_gamelogic_bytecode(const unsigned char*, size_t, const char*, unsigned char*, size_t);

char g_patched_gamelogic[512] = "";

// 32-byte AES-256 key used by the game's script encryption.
static const uint8_t KEY[32] = "USCaPQpA4TSNVxMI1v9SK9UC0yZuAnb2";

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

static long read_file(const char *path, unsigned char *buf, long cap) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > cap) { fclose(f); return -1; }
  long got = (long)fread(buf, 1, (size_t)sz, f);
  fclose(f);
  return got == sz ? sz : -1;
}

void locale_patch_init(void) {
  char raw[24];
  resolve_raw_lang(raw, sizeof raw);
  const char *loc = map_to_game_locale(raw);
  if (!loc) {
    debugPrintf("locale_patch: '%s' -> English/unsupported; using stock script\n", raw);
    return;
  }
  debugPrintf("locale_patch: '%s' -> %s\n", raw, loc);

  // Resolve the STOCK gamelogic.lua now, before the redirect is armed.
  char stock[512];
  if (!resolve_asset_path("data/scripts_common/gamelogic.lua", stock, sizeof stock)) {
    debugPrintf("locale_patch: stock gamelogic.lua not found; skipping\n");
    return;
  }

  enum { CAP = 512 * 1024 };
  unsigned char *enc = malloc(CAP), *bc = malloc(CAP), *pbc = malloc(CAP), *cs = malloc(CAP);
  if (!enc || !bc || !pbc || !cs) { free(enc); free(bc); free(pbc); free(cs); return; }

  int ok = 0;
  do {
    long elen = read_file(stock, enc, CAP);
    if (elen < 16) { debugPrintf("locale_patch: read stock failed\n"); break; }
    long blk = (elen / 16) * 16;

    struct AES_ctx ctx; uint8_t iv[16] = {0};
    AES_init_ctx_iv(&ctx, KEY, iv);
    AES_CBC_decrypt_buffer(&ctx, enc, (size_t)blk);

    size_t bclen = lzma_alone_decode(enc, (size_t)blk, bc, CAP);
    if (!bclen || !(bc[0] == 0x1b && bc[1] == 'L')) {
      debugPrintf("locale_patch: decompress failed (wrong key or format?)\n"); break;
    }
    size_t plen = patch_gamelogic_bytecode(bc, bclen, loc, pbc, CAP);
    if (!plen) { debugPrintf("locale_patch: bytecode patch failed\n"); break; }

    size_t cslen = lzma_alone_encode(pbc, plen, cs, CAP);
    if (!cslen) { debugPrintf("locale_patch: recompress failed\n"); break; }
    size_t pad = (16 - (cslen % 16)) % 16;
    memset(cs + cslen, 0, pad);
    size_t enclen = cslen + pad;
    uint8_t iv2[16] = {0};
    AES_init_ctx_iv(&ctx, KEY, iv2);
    AES_CBC_encrypt_buffer(&ctx, cs, enclen);

    char outp[512];
    snprintf(outp, sizeof outp, "%s/gamelogic_patched.lua", DATA_DIR);
    FILE *o = fopen(outp, "wb");
    if (!o) { debugPrintf("locale_patch: cannot write %s\n", outp); break; }
    size_t wrote = fwrite(cs, 1, enclen, o);
    fclose(o);
    if (wrote != enclen) { debugPrintf("locale_patch: short write\n"); break; }

    snprintf(g_patched_gamelogic, sizeof g_patched_gamelogic, "%s", outp);
    debugPrintf("locale_patch: wrote %s (%zu bytes), locale %s\n", outp, enclen, loc);
    ok = 1;
  } while (0);

  free(enc); free(bc); free(pbc); free(cs);
  if (!ok) g_patched_gamelogic[0] = 0;
}
