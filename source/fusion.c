/* fusion.c -- platform ("Java wrapper") implementations for the Fusion engine
 *
 * The engine calls back into ~12 Java classes (see the class list decoded from
 * the binary). For a first boot we need only a handful to answer sensibly:
 *
 *   AudioOutput          -- createAudioOutput(rate,bits,channels) negotiates the
 *                           PCM format; start/stopAudioOutput gate playback. The
 *                           engine then pulls PCM through nativeMixData (audio.c).
 *   Globals / DeviceInfo -- getActivity/getAssets/getClassLoader (opaque tokens
 *                           the engine forwards to AAssetManager_fromJava, which
 *                           ignores them), getDisplayWidth/Height, locale, etc.
 *   DeviceIDCreator      -- a stable fake device id / advertising id.
 *   SystemFontRenderer   -- Android renders some UI text through Typeface/Canvas.
 *                           We CANNOT do that here; we return neutral metrics and
 *                           log every call. Text that depends on this path will
 *                           be wrong/absent until a real font rasteriser (e.g.
 *                           FreeType) is wired in. See README "Known gaps".
 *   TextInput            -- routes to the Switch software keyboard.
 *
 * Everything else (ads, video, web, notifications, accelerometer, cloud) is
 * logged and given a safe default so the engine proceeds with those features
 * effectively disabled.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/stat.h>
#include <switch.h>

#include "jni.h"
#include "jni_fake.h"
#include "fusion.h"
#include "config.h"
#include "util.h"

// negotiated audio format (defaults are the Fusion norm; corrected on the real
// createAudioOutput call)
static int   s_rate = 44100, s_channels = 2, s_bits = 16;
static void *s_peer = NULL;
static int   s_audio_wanted = 0;

int   fusion_audio_rate(void)     { return s_rate; }
int   fusion_audio_channels(void) { return s_channels; }
int   fusion_audio_bits(void)     { return s_bits; }
void *fusion_audio_peer(void)     { return s_peer; }
int   fusion_audio_wanted(void)   { return s_audio_wanted; }

void fusion_set_audio_peer(void *p) {
  if (!p || p == s_peer) return;
  s_peer = p;
  // The engine's mixer reads its output format straight from the peer struct
  // (confirmed by disassembly): peer[0]=channels, peer[4]=bits/sample,
  // peer[8]=sample rate. Read them so we open SDL at exactly the right format
  // instead of guessing. Values outside sane ranges keep the defaults.
  volatile int *pf = (volatile int *)p;
  int ch = pf[0], bits = pf[1], rate = pf[2];
  debugPrintf("audio peer=%p fmt: ch=%d bits=%d rate=%d\n", p, ch, bits, rate);
  if (ch == 1 || ch == 2) s_channels = ch;
  if (bits == 8 || bits == 16) s_bits = bits;
  if (rate >= 8000 && rate <= 48000) s_rate = rate;
}

// ---------------------------------------------------------------------------
// small helpers for reading call args by JNI signature
// ---------------------------------------------------------------------------

// advance past '(' ... yield the next arg type char, skipping object/array
// descriptors as single logical args. Returns 0 at end of args (')').
static const char *sig_args(const char *sig) {
  if (!sig) return "";
  const char *p = strchr(sig, '(');
  return p ? p + 1 : sig;
}

// read one argument from ap given its JNI type char; store into out (as int64 /
// double / pointer). Returns the char consumed's "kind": 'i','j','f','d','l'.
// NOTE: va_list is an array type on AArch64, so passing it by value here still
// lets va_arg advance the caller's shared state across successive calls.
static char read_arg(const char **pp, va_list ap, int64_t *iv, double *dv, void **pv) {
  const char *p = *pp;
  char c = *p ? *p++ : ')';
  switch (c) {
    case 'Z': case 'B': case 'C': case 'S': case 'I':
      *iv = va_arg(ap, int); *pp = p; return 'i';
    case 'J':
      *iv = va_arg(ap, int64_t); *pp = p; return 'j';
    case 'F':
      *dv = va_arg(ap, double); *pp = p; return 'f'; // float promoted to double
    case 'D':
      *dv = va_arg(ap, double); *pp = p; return 'd';
    case 'L':
      while (*p && *p != ';') p++;
      if (*p == ';') p++;
      *pv = va_arg(ap, void *); *pp = p; return 'l';
    case '[':
      // one array descriptor = one arg; skip the element type too
      if (*p == 'L') { while (*p && *p != ';') p++; if (*p == ';') p++; }
      else if (*p) p++;
      *pv = va_arg(ap, void *); *pp = p; return 'l';
    default:
      *pp = p; return 0;
  }
}

// ---------------------------------------------------------------------------
// dispatch
// ---------------------------------------------------------------------------

#define IS(m) (strcmp(method, m) == 0)

// --- language selection --------------------------------------------------
// The engine picks its UI language from the locale string we return here
// (getCurrentLocale). config.txt's "language" is "auto" (follow the Switch
// system language) or a code like "de" / "pt_BR". Which languages actually
// appear depends on what translations the game's asset files contain.
static char s_lang[4] = "";      // 2-letter language, e.g. "de"
static char s_country[4] = "";   // 2-letter country,  e.g. "DE"
static char s_locale[8] = "";    // "de_DE"

static const char *country_for(const char *lang) {
  static const struct { const char *l, *c; } map[] = {
    {"en","US"},{"de","DE"},{"fr","FR"},{"es","ES"},{"it","IT"},{"pt","BR"},
    {"ru","RU"},{"ja","JP"},{"ko","KR"},{"zh","CN"},{"nl","NL"},{"sv","SE"},
    {"da","DK"},{"no","NO"},{"fi","FI"},{"pl","PL"},{"tr","TR"},
  };
  for (unsigned i = 0; i < sizeof(map)/sizeof(*map); i++)
    if (!strncmp(lang, map[i].l, 2)) return map[i].c;
  static char up[3];
  up[0] = toupper((unsigned char)lang[0]);
  up[1] = lang[1] ? toupper((unsigned char)lang[1]) : 0;
  up[2] = 0;
  return up;
}

static void resolve_language(void) {
  if (s_lang[0]) return;   // resolve once
  char lang[4] = "en";
  const char *country = NULL;

  if (config.language[0] && strcmp(config.language, "auto") != 0) {
    lang[0] = config.language[0];
    lang[1] = config.language[1] ? config.language[1] : 0;
    lang[2] = 0;
    const char *us = strchr(config.language, '_');   // allow "pt_BR", "zh_TW"
    if (us && us[1] && us[2]) { static char c[3]; c[0]=us[1]; c[1]=us[2]; c[2]=0; country = c; }
  } else {
    // follow the Switch system language
    u64 code = 0;
    if (R_SUCCEEDED(setInitialize())) {
      if (R_SUCCEEDED(setGetSystemLanguage(&code))) {
        char buf[9] = {0};
        memcpy(buf, &code, sizeof(u64));            // e.g. "en-US" / "de" / "ja"
        if (buf[0] && buf[1]) { lang[0]=buf[0]; lang[1]=buf[1]; lang[2]=0; }
      }
      setExit();
    }
  }

  if (!country) country = country_for(lang);
  s_lang[0]=lang[0]; s_lang[1]=lang[1]; s_lang[2]=0;
  s_country[0]=country[0]; s_country[1]=country[1]; s_country[2]=0;
  snprintf(s_locale, sizeof(s_locale), "%s_%s", s_lang, s_country);
  debugPrintf("language: %s (locale %s)\n", s_lang, s_locale);
}

jvalue fusion_call(const char *cls, const char *method, const char *sig,
                   void *self, va_list ap) {
  (void)self;
  jvalue r; r.j = 0;

  // -------- AudioOutput --------
  if (IS("createAudioOutput")) {
    // Observed native use: nativeMixData receives a native stream pointer as its
    // first param, which comes from here. Signature varies by build; parse it.
    // Common shapes: (J III) -> peer,rate,bits,channels  or  (III) -> rate,bits,channels.
    const char *p = sig_args(sig);
    int64_t iv = 0; double dv = 0; void *pv = NULL;
    int ints[4]; int ni = 0; void *firstptr = NULL;
    while (*p && *p != ')') {
      char k = read_arg(&p, ap, &iv, &dv, &pv);
      if (k == 'j') { if (!firstptr && iv) firstptr = (void *)(intptr_t)iv; }
      else if (k == 'l') { if (!firstptr && pv) firstptr = pv; }
      else if (k == 'i' && ni < 4) ints[ni++] = (int)iv;
    }
    if (firstptr) s_peer = firstptr;
    if (ni >= 3) { s_rate = ints[ni-3]; s_bits = ints[ni-2]; s_channels = ints[ni-1]; }
    // sanity clamps
    if (s_rate != 22050 && s_rate != 32000 && s_rate != 44100 && s_rate != 48000) s_rate = 44100;
    if (s_channels < 1 || s_channels > 2) s_channels = 2;
    if (s_bits != 8 && s_bits != 16) s_bits = 16;
    s_audio_wanted = 1;
    debugPrintf("Fusion.createAudioOutput sig=%s -> rate=%d bits=%d ch=%d peer=%p\n",
                sig ? sig : "?", s_rate, s_bits, s_channels, s_peer);
    r.l = jni_make_string("AudioOutput"); // any non-null jobject stand-in
    return r;
  }
  if (IS("startAudioOutput") || IS("resumeAudioOutput") ||
      IS("startOutput") || IS("resumeOutput")) {
    s_audio_wanted = 1;
    debugPrintf("Fusion.%s -> audio ON (peer=%p)\n", method, s_peer);
    r.z = JNI_TRUE;   // startOutput()Z: report success
    return r;
  }
  if (IS("stopAudioOutput") || IS("pauseAudioOutput") ||
      IS("stopOutput") || IS("pauseOutput")) {
    s_audio_wanted = 0;
    debugPrintf("Fusion.%s\n", method);
    return r;
  }

  // -------- opaque platform handles forwarded to the NDK / ignored --------
  if (IS("getActivity") || IS("getAssets") || IS("getClassLoader") ||
      IS("getApplicationContext") || IS("getContext") || IS("getPackageManager") ||
      IS("getInstance") || IS("getPackageInfo") || IS("getCurrentContext")) {
    r.l = jni_make_string(method); // non-null token; AAssetManager_fromJava ignores it
    return r;
  }

  // -------- display geometry --------
  if (IS("getDisplayWidth") || IS("getWidth"))  { r.i = screen_width;  return r; }
  if (IS("getDisplayHeight") || IS("getHeight")) { r.i = screen_height; return r; }
  if (IS("getDisplayDensity")) { r.f = 2.0f; return r; }             // ~xhdpi
  if (IS("getDisplayDensityGroup")) { r.i = 2; return r; }
  if (IS("getDisplayConfigurationGroup")) { r.i = 0; return r; }
  if (IS("getSafeInsetTop") || IS("getSafeInsetBottom") ||
      IS("getSafeInsetLeft") || IS("getSafeInsetRight")) { r.i = 0; return r; }

  // -------- writable storage / cache directories --------
  // The engine writes its cache and saves under these. Point them at the game
  // directory (persistent) and a cache subdir, both created on demand.
  if (IS("getPathToFileCacheDirectory") || IS("getPathToExternalFileCacheDirectory") ||
      IS("getCacheDir") || IS("getPathToCache")) {
    mkdir(DATA_DIR "/cache", 0777);
    r.l = jni_make_string(DATA_DIR "/cache");
    return r;
  }
  if (IS("getInternalStoragePath") || IS("getExternalStoragePath") ||
      IS("getPathToInternalFiles") || IS("getPathToExternalFiles") ||
      IS("getFilesDir") || IS("getPathToFileDirectory") || IS("getWritablePath") ||
      IS("getPathToDocuments") || IS("getPackageResourcePath")) {
    r.l = jni_make_string(DATA_DIR);
    return r;
  }

  // -------- device / locale / identity --------
  if (IS("getDeviceID") || IS("getDeviceId")) { r.l = jni_make_string("SWITCH-0000-0000-0000"); return r; }
  if (IS("getUniqueId")) { r.l = jni_make_string("SWITCH-AB-0001-0002-000000000003"); return r; }
  // UUID fallback: only reached if the engine still generates its own id. We
  // hand back stable string tokens; toString echoes the receiver string.
  if (IS("randomUUID") || IS("nameUUIDFromBytes") || IS("fromString"))
    { r.l = jni_make_string("00000000-0000-4000-8000-000000000001"); return r; }
  if (IS("toString")) { r.l = self ? self : jni_make_string(""); return r; }
  if (IS("getAdvertisementId") || IS("getAdvertisingId")) { r.l = jni_make_string("00000000-0000-0000-0000-000000000000"); return r; }
  if (IS("getModel"))        { r.l = jni_make_string("Switch"); return r; }
  if (IS("getManufacturer")) { r.l = jni_make_string("Nintendo"); return r; }
  if (IS("getOSVersion") || IS("getSystemVersion")) { r.l = jni_make_string("10"); return r; }
  if (IS("getLanguage") || IS("getCurrentLanguage")) { resolve_language(); r.l = jni_make_string(s_lang); return r; }
  if (IS("getCurrentLocale") || IS("getDefault") || IS("getLocale")) { resolve_language(); r.l = jni_make_string(s_locale); return r; }
  if (IS("getCountry"))      { resolve_language(); r.l = jni_make_string(s_country); return r; }
  if (IS("getCurrencyCode")) { r.l = jni_make_string("USD"); return r; }
  if (IS("getClipboardText")) { r.l = jni_make_string(""); return r; }
  if (IS("getCurrentTime"))  { r.j = 0; return r; }

  // -------- SystemFontRenderer (HONEST STUB -- see README "Known gaps") -------
  // These normally rasterise glyphs via android.graphics. We can't; return
  // neutral metrics so the engine's text layout doesn't divide-by-zero, and log
  // so the dependency is visible. Text using this path will not render until a
  // FreeType-based rasteriser is added.
  if (!strncmp(method, "createSystemFont", 16) || IS("createBitmapFont")) {
    debugPrintf("Fusion.%s (SystemFontRenderer: STUB, no glyphs)\n", method);
    r.l = jni_make_string("Font"); return r;
  }
  if (IS("getAscender") || IS("getFontMaxAscending")) { r.f = 24.0f; return r; }
  if (IS("getDescender") || IS("getFontMaxDescending")) { r.f = 6.0f; return r; }
  if (IS("getFontHeight")) { r.f = 30.0f; return r; }
  if (IS("getFontLeading")) { r.f = 4.0f; return r; }
  if (IS("getAvailableSystemFonts")) { r.l = NULL; return r; } // empty list

  // -------- TextInput -> Switch software keyboard --------
  if (IS("showKeyboard") || IS("showTextInput") || IS("createTextInputField") ||
      IS("show") /* TextInput.show */) {
    g_kbd_requested = 1;
    debugPrintf("Fusion.%s -> soft keyboard requested\n", method);
    return r;
  }
  if (IS("hideKeyboard") || IS("hideTextInput")) { return r; }

  // -------- lifecycle / misc that the engine polls --------
  if (IS("getPossibleOrientations")) { r.i = 0; return r; }
  if (IS("isTablet")) { r.z = JNI_TRUE; return r; }
  if (IS("hasNotch")) { r.z = JNI_FALSE; return r; }

  // -------- everything else: log once-ish and return 0/null --------
  {
    static int spam = 0;
    if (spam < 400) { debugPrintf("JNI call unhandled: %s.%s %s\n", cls, method, sig ? sig : ""); spam++; }
  }
  return r;
}
