/* nx_pointer.c -- see nx_pointer.h.
 *
 * Everything pointer-related for a Switch port of an Android game, in one file:
 * touchscreen, USB mouse, stick cursor, and the GL overlay that draws it.
 */
#include <switch.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdarg.h>   /* va_list / va_start / va_end in logf_() */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>  /* png_jmpbuf / setjmp in the libpng error path */
#include <png.h>

#include "nx_pointer.h"

/* ------------------------------------------------------------------ config */

static NxpConfig s_cfg;
static int       s_ready = 0;

static void logf_(const char *fmt, ...) {
  if (!s_cfg.log) return;
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  s_cfg.log(buf);
}

/* ------------------------------------------------------------------- state */

static PadState s_pad;

/* touch (handheld) */
static int   s_touch_active[16];
static float s_touch_x[16], s_touch_y[16];

/* cursor */
static float s_cx, s_cy;
static int   s_visible   = -1;      /* -1 = not yet decided */
static int   s_was_docked = -1;
static int   s_tap_prev  = 0;       /* A or mouse-left held last frame */

/* tunables, adjusted live */
static float s_stick_speed;         /* px/frame at full stick deflection */
static float s_mouse_sens;          /* multiplier on mouse deltas */
static float s_gyro_sens;           /* multiplier on gyro angular velocity */

/* gyro (motion pointing). Handles differ per controller style, so we fetch one
 * for each and pick at read time based on what is actually connected. */
static HidSixAxisSensorHandle s_six[4];
static int s_gyro_ready = 0;        /* sensors started */
static int s_gyro_on    = 0;        /* toggled by '-' */
static int s_gyro_logged = 0;

/* A USB mouse takes priority: gyro is switched off while one is connected. */
static int s_mouse_connected = 0;

/* Raw angular velocity -> px/frame. The gyro reports a RATE, so this is the
 * per-frame gain; s_gyro_sens scales it and D-pad U/D tunes that live.
 * Measured on hardware: a normal turn gives |angular_velocity| ~0.14, so at the
 * original gain of 40 that was only ~6 px/frame -- far too slow. Tripled. */
#define GYRO_GAIN 120.0f

/* d-pad auto-repeat for the sensitivity adjustment */
static int   s_dpad_hold = 0;

/* Settings persistence. A save is queued whenever a sensitivity changes and
 * committed 3s after the LAST change, so holding the D-pad through twenty steps
 * writes the file once rather than twenty times. */
#define SETTINGS_DEBOUNCE_NS  3000000000ULL      /* 3 seconds */
static int s_settings_dirty = 0;
static u64 s_settings_tick  = 0;

/* events for this frame */
static NxpEvent s_ev[24];
static int      s_nev;

#define STICK_MIN   2.0f
#define STICK_MAX  60.0f
#define SENS_MIN    0.25f
#define SENS_MAX    8.0f
#define GYRO_MIN    0.10f
#define GYRO_MAX    8.0f

static int is_docked(void) {
  return appletGetOperationMode() == AppletOperationMode_Console;
}

/* ------------------------------------------------------- settings file ---
 * IMPORTANT: use the port's locked fopen/fclose when it supplies them. The
 * engine's worker threads are doing file I/O constantly, and devkitPro's newlib
 * handle table is not thread-safe -- an unlocked open/close from here would race
 * it and corrupt the fd table (which shows up as a Data Abort deep inside
 * newlib, nowhere near this code). */
static FILE *cfg_fopen(const char *path, const char *mode) {
  return s_cfg.fopen_fn ? s_cfg.fopen_fn(path, mode) : fopen(path, mode);
}
static int cfg_fclose(FILE *f) {
  return s_cfg.fclose_fn ? s_cfg.fclose_fn(f) : fclose(f);
}

static void clamp_settings(void) {
  if (s_stick_speed < STICK_MIN) s_stick_speed = STICK_MIN;
  if (s_stick_speed > STICK_MAX) s_stick_speed = STICK_MAX;
  if (s_mouse_sens  < SENS_MIN)  s_mouse_sens  = SENS_MIN;
  if (s_mouse_sens  > SENS_MAX)  s_mouse_sens  = SENS_MAX;
  if (s_gyro_sens   < GYRO_MIN)  s_gyro_sens   = GYRO_MIN;
  if (s_gyro_sens   > GYRO_MAX)  s_gyro_sens   = GYRO_MAX;
}

static void settings_path(char *out, size_t n) {
  snprintf(out, n, "%s/pointer.cfg", s_cfg.data_dir ? s_cfg.data_dir : ".");
}

static void settings_load(void) {
  if (!s_cfg.data_dir) return;
  char path[512];
  settings_path(path, sizeof path);

  FILE *f = cfg_fopen(path, "r");
  if (!f) { logf_("nxp: no pointer.cfg -- using defaults\n"); return; }

  char line[128];
  while (fgets(line, sizeof line, f)) {
    float v;
    if      (sscanf(line, "stick=%f", &v) == 1) s_stick_speed = v;
    else if (sscanf(line, "mouse=%f", &v) == 1) s_mouse_sens  = v;
    else if (sscanf(line, "gyro=%f",  &v) == 1) s_gyro_sens   = v;
  }
  cfg_fclose(f);
  clamp_settings();                      /* the file may have been hand-edited */
  logf_("nxp: settings loaded  stick=%.1f mouse=%.2f gyro=%.2f\n",
        s_stick_speed, s_mouse_sens, s_gyro_sens);
}

void nxp_save_settings(void) {
  if (!s_ready || !s_cfg.data_dir) return;
  char path[512];
  settings_path(path, sizeof path);

  FILE *f = cfg_fopen(path, "w");
  if (!f) { logf_("nxp: could not write %s\n", path); s_settings_dirty = 0; return; }

  fprintf(f, "# nx_pointer settings -- auto-saved, safe to edit\n");
  fprintf(f, "stick=%.2f\n", s_stick_speed);
  fprintf(f, "mouse=%.2f\n", s_mouse_sens);
  fprintf(f, "gyro=%.2f\n",  s_gyro_sens);
  cfg_fclose(f);

  s_settings_dirty = 0;
  logf_("nxp: settings saved  stick=%.1f mouse=%.2f gyro=%.2f\n",
        s_stick_speed, s_mouse_sens, s_gyro_sens);
}

/* Queue a save; it lands SETTINGS_DEBOUNCE_NS after the last change. */
static void settings_touch(void) {
  s_settings_dirty = 1;
  s_settings_tick  = armGetSystemTick();
}

/* Called once per frame from nxp_update(). */
static void settings_tick(void) {
  if (!s_settings_dirty) return;
  if (armTicksToNs(armGetSystemTick() - s_settings_tick) >= SETTINGS_DEBOUNCE_NS)
    nxp_save_settings();
}

/* --------------------------------------------------- custom cursor (PNG) */

#define CURSOR_MAX_DIM 64

/* Raw file bytes, slurped at init (single-threaded) so the render thread never
 * has to touch the filesystem. Decoded + uploaded lazily in nxp_draw(). */
static uint8_t *s_png_bytes = NULL;
static size_t   s_png_len   = 0;

/* decoded */
static GLuint s_cursor_tex = 0;
static int    s_cursor_w = 0, s_cursor_h = 0;
static int    s_png_tried = 0;      /* decode attempted (success or not) */

static void slurp_cursor_png(void) {
  if (!s_cfg.data_dir) return;
  char path[512];
  snprintf(path, sizeof path, "%s/cursor.png", s_cfg.data_dir);

  FILE *f = cfg_fopen(path, "rb");     /* locked path: engine threads are live */
  if (!f) { logf_("nxp: no cursor.png (using built-in arrow)\n"); return; }

  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len <= 0 || len > 4 * 1024 * 1024) { cfg_fclose(f); return; }

  s_png_bytes = malloc((size_t)len);
  if (!s_png_bytes) { cfg_fclose(f); return; }
  s_png_len = fread(s_png_bytes, 1, (size_t)len, f);
  cfg_fclose(f);

  if (s_png_len != (size_t)len) { free(s_png_bytes); s_png_bytes = NULL; s_png_len = 0; return; }
  logf_("nxp: cursor.png loaded (%zu bytes), decoding on first frame\n", s_png_len);
}

/* libpng reader over the in-memory buffer */
typedef struct { const uint8_t *p; size_t len, off; } PngSrc;

static void png_read_mem(png_structp png, png_bytep out, png_size_t n) {
  PngSrc *s = (PngSrc *)png_get_io_ptr(png);
  if (s->off + n > s->len) { png_error(png, "short read"); return; }
  memcpy(out, s->p + s->off, n);
  s->off += n;
}

/* Decode to RGBA8 and upload. Returns 1 on success. Render thread only. */
static int cursor_upload_png(void) {
  if (!s_png_bytes) return 0;

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) return 0;
  png_infop info = png_create_info_struct(png);
  if (!info) { png_destroy_read_struct(&png, NULL, NULL); return 0; }

  uint8_t   *pixels = NULL;
  png_bytep *rows   = NULL;
  if (setjmp(png_jmpbuf(png))) {          /* libpng error path */
    free(pixels); free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    logf_("nxp: cursor.png decode failed -- using built-in arrow\n");
    return 0;
  }

  PngSrc src = { s_png_bytes, s_png_len, 0 };
  png_set_read_fn(png, &src, png_read_mem);
  png_read_info(png, info);

  const png_uint_32 w = png_get_image_width(png, info);
  const png_uint_32 h = png_get_image_height(png, info);
  if (w == 0 || h == 0 || w > CURSOR_MAX_DIM || h > CURSOR_MAX_DIM) {
    logf_("nxp: cursor.png is %ux%u -- max is %dx%d, using built-in arrow\n",
          (unsigned)w, (unsigned)h, CURSOR_MAX_DIM, CURSOR_MAX_DIM);
    png_destroy_read_struct(&png, &info, NULL);
    return 0;
  }

  /* normalise anything to 8-bit RGBA so transparency always works */
  const int ct = png_get_color_type(png, info);
  const int bd = png_get_bit_depth(png, info);
  if (bd == 16)                       png_set_strip_16(png);
  if (ct == PNG_COLOR_TYPE_PALETTE)   png_set_palette_to_rgb(png);
  if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  /* ensure an alpha channel exists even for opaque RGB */
  png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  png_read_update_info(png, info);

  const size_t stride = (size_t)w * 4;
  pixels = malloc(stride * h);
  rows   = malloc(sizeof(png_bytep) * h);
  if (!pixels || !rows) { png_error(png, "oom"); }
  for (png_uint_32 y = 0; y < h; y++) rows[y] = pixels + y * stride;
  png_read_image(png, rows);
  png_read_end(png, NULL);
  png_destroy_read_struct(&png, &info, NULL);
  free(rows);

  /* Save the texture binding we are about to disturb -- this runs inside the
   * engine's context, and leaving its texture unbound would corrupt its frame. */
  GLint prev_active = 0, prev_tex = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

  glGenTextures(1, &s_cursor_tex);
  glBindTexture(GL_TEXTURE_2D, s_cursor_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GLint prev_align = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);   /* engine uploads assume 4 */
  free(pixels);

  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);   /* put the engine's back */
  glActiveTexture((GLenum)prev_active);

  s_cursor_w = (int)w;
  s_cursor_h = (int)h;
  logf_("nxp: custom cursor %dx%d ready (tex=%u)\n", s_cursor_w, s_cursor_h, s_cursor_tex);
  return 1;
}

/* ------------------------------------------------------------------- init */

void nxp_init(const NxpConfig *cfg) {
  if (s_ready) return;
  memset(&s_cfg, 0, sizeof s_cfg);
  if (cfg) s_cfg = *cfg;

  if (s_cfg.panel_w <= 0)         s_cfg.panel_w = 1280;
  if (s_cfg.panel_h <= 0)         s_cfg.panel_h = 720;
  if (s_cfg.cursor_id < 0)        s_cfg.cursor_id = 8;   // <0 => default 8; 0 is allowed
  if (s_cfg.max_touch_slots <= 0) s_cfg.max_touch_slots = 8;
  if (s_cfg.screen_w <= 0)        s_cfg.screen_w = 1920;
  if (s_cfg.screen_h <= 0)        s_cfg.screen_h = 1080;

  s_stick_speed = (s_cfg.stick_speed > 0.0f) ? s_cfg.stick_speed : 14.0f;
  s_mouse_sens  = (s_cfg.mouse_sens  > 0.0f) ? s_cfg.mouse_sens  : 1.0f;
  s_gyro_sens   = 1.0f;

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&s_pad);
  hidInitializeTouchScreen();
  hidInitializeMouse();               /* USB / dock mouse */

  /* Six-axis (gyro): each controller style has its own handle, so grab them all
   * and choose the right one per frame from the active style. */
  Result rc0 = hidGetSixAxisSensorHandles(&s_six[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
  Result rc1 = hidGetSixAxisSensorHandles(&s_six[1], 1, HidNpadIdType_No1,      HidNpadStyleTag_NpadFullKey);
  Result rc2 = hidGetSixAxisSensorHandles(&s_six[2], 2, HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyDual);
  if (R_SUCCEEDED(rc0) && R_SUCCEEDED(rc1) && R_SUCCEEDED(rc2)) {
    for (int i = 0; i < 4; i++) hidStartSixAxisSensor(s_six[i]);
    s_gyro_ready = 1;
  } else {
    logf_("nxp: gyro unavailable (sensor handles failed)\n");
  }

  s_cx = s_cfg.screen_w * 0.5f;
  s_cy = s_cfg.screen_h * 0.5f;

  settings_load();                    /* restores stick/mouse/gyro sensitivities */
  slurp_cursor_png();                 /* read now, decode on the render thread */

  s_ready = 1;
  logf_("nxp: init %dx%d (panel %dx%d), stick=%.1f mouse=%.2f\n",
        s_cfg.screen_w, s_cfg.screen_h, s_cfg.panel_w, s_cfg.panel_h,
        s_stick_speed, s_mouse_sens);
}

/* ---------------------------------------------------------------- helpers */

static void clamp_cursor(void) {
  if (s_cx < 0) s_cx = 0;
  if (s_cy < 0) s_cy = 0;
  if (s_cx > s_cfg.screen_w - 1) s_cx = (float)(s_cfg.screen_w - 1);
  if (s_cy > s_cfg.screen_h - 1) s_cy = (float)(s_cfg.screen_h - 1);
}

static void push(int id, float x, float y, int phase) {
  if (s_nev >= (int)(sizeof s_ev / sizeof s_ev[0])) return;
  NxpEvent *e = &s_ev[s_nev++];
  e->id = id; e->x = x; e->y = y; e->phase = phase;
}

/* Touch: the panel always reports in its own space (1280x720) regardless of the
 * resolution we render at, so scale into render space -- otherwise the right and
 * bottom edges (and the corners) are physically unreachable. */
static void do_touch(void) {
  HidTouchScreenState ts = {0};
  hidGetTouchScreenStates(&ts, 1);

  const int slots = s_cfg.max_touch_slots;
  int now[16] = {0};
  int count = ts.count > slots ? slots : ts.count;

  const float sx = (float)s_cfg.screen_w / (float)s_cfg.panel_w;
  const float sy = (float)s_cfg.screen_h / (float)s_cfg.panel_h;

  for (int i = 0; i < count; i++) {
    float x = (float)ts.touches[i].x * sx;
    float y = (float)ts.touches[i].y * sy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > s_cfg.screen_w - 1) x = (float)(s_cfg.screen_w - 1);
    if (y > s_cfg.screen_h - 1) y = (float)(s_cfg.screen_h - 1);
    now[i] = 1;
    push(i, x, y, s_touch_active[i] ? NXP_MOVE : NXP_DOWN);
    s_touch_x[i] = x; s_touch_y[i] = y;
  }
  for (int i = 0; i < slots; i++) {
    if (s_touch_active[i] && !now[i])
      push(i, s_touch_x[i], s_touch_y[i], NXP_UP);
    s_touch_active[i] = now[i];
  }
}

/* D-pad UP/DOWN adjusts sensitivity. That is the ONLY sensitivity control --
 * left/right used to also tune the stick, which just meant two buttons doing
 * overlapping jobs and no clear answer to "which one am I changing?".
 *
 * Up/Down applies to whichever device is ACTUALLY driving the cursor right now:
 *   a mouse is plugged in  -> mouse sensitivity
 *   else gyro is on        -> gyro sensitivity
 *   else                   -> stick speed
 * so it is never a no-op, and there is only ever one thing it can mean.
 * Auto-repeats while held. Any change queues a debounced save. */
static void do_dpad(u64 held, u64 pressed) {
  const u64 any = HidNpadButton_Up | HidNpadButton_Down;

  int repeat = 0;
  if (held & any) {
    s_dpad_hold++;
    if (s_dpad_hold > 24 && (s_dpad_hold % 3) == 0) repeat = 1;   /* ~0.4s, then repeat */
  } else {
    s_dpad_hold = 0;
  }

  int step = 0;
  if (pressed & HidNpadButton_Up)   step = +1;
  if (pressed & HidNpadButton_Down) step = -1;
  if (repeat && (held & HidNpadButton_Up))   step = +1;
  if (repeat && (held & HidNpadButton_Down)) step = -1;
  if (!step) return;

  const float f = (step > 0) ? 1.15f : (1.0f / 1.15f);

  if (s_mouse_connected) {
    s_mouse_sens *= f;
    clamp_settings();
    logf_("nxp: mouse sensitivity = %.2f\n", s_mouse_sens);
  } else if (s_gyro_on) {
    s_gyro_sens *= f;
    clamp_settings();
    logf_("nxp: gyro sensitivity = %.2f\n", s_gyro_sens);
  } else {
    s_stick_speed *= f;
    clamp_settings();
    logf_("nxp: stick speed = %.1f px/frame\n", s_stick_speed);
  }

  settings_touch();                 /* save 3s after the last change */
}

/* Pick the six-axis handle that matches whatever is actually connected, then
 * read its latest sample. Handheld, Pro Controller and dual Joy-Con all report
 * through different handles. */
static int read_gyro(HidSixAxisSensorState *out, float *sign_x, float *sign_y) {
  if (!s_gyro_ready) return 0;
  const u64 style = padGetStyleSet(&s_pad);

  /* AXIS SIGNS.
   * The Pro Controller's IMU frame gives the correct pointing direction with
   * both axes negated. A Joy-Con's IMU sits rotated 180 degrees about its
   * pointing axis relative to that, so BOTH axes come out mirrored -- which is
   * exactly what shows up in practice (Pro correct, Joy-Con reversed on both).
   * Keep the signs in one place so a single controller can be re-flipped
   * without touching the rest of the maths. */
  *sign_x = -1.0f;
  *sign_y = -1.0f;                                  /* Pro Controller */

  if (style & HidNpadStyleTag_NpadFullKey)
    return hidGetSixAxisSensorStates(s_six[1], out, 1) > 0;

  if (style & HidNpadStyleTag_NpadHandheld) {
    *sign_x = +1.0f; *sign_y = +1.0f;               /* Joy-Con frame: mirrored */
    return hidGetSixAxisSensorStates(s_six[0], out, 1) > 0;
  }

  if (style & HidNpadStyleTag_NpadJoyDual) {
    *sign_x = +1.0f; *sign_y = +1.0f;               /* Joy-Con frame: mirrored */
    const u64 attr = padGetAttributes(&s_pad);
    if (attr & HidNpadAttribute_IsRightConnected)
      return hidGetSixAxisSensorStates(s_six[3], out, 1) > 0;
    if (attr & HidNpadAttribute_IsLeftConnected)
      return hidGetSixAxisSensorStates(s_six[2], out, 1) > 0;
  }
  return 0;
}

/* Motion pointing: tilt/turn the controller to move the cursor.
 *
 * The gyro reports an angular VELOCITY, so we treat it as a per-frame rate:
 *   yaw   (rotation about the controller's up axis)    -> cursor X
 *   pitch (rotation about the controller's right axis) -> cursor Y
 * Signs are negated so the cursor follows where you point the controller.
 * A connected mouse wins, so gyro is skipped entirely while one is plugged in. */
static void do_gyro(void) {
  if (!s_gyro_on || s_mouse_connected) return;

  HidSixAxisSensorState st = {0};
  float sx = -1.0f, sy = -1.0f;
  if (!read_gyro(&st, &sx, &sy)) return;

  if (s_gyro_logged < 5) {
    s_gyro_logged++;
    logf_("nxp: gyro av=(%.3f, %.3f, %.3f) signs=(%+.0f,%+.0f)\n",
          st.angular_velocity.x, st.angular_velocity.y, st.angular_velocity.z, sx, sy);
  }

  const float g = GYRO_GAIN * s_gyro_sens;
  const float dx = sx * st.angular_velocity.y * g;   /* yaw   -> horizontal */
  const float dy = sy * st.angular_velocity.x * g;   /* pitch -> vertical   */

  /* small deadzone so a resting controller does not creep */
  if (fabsf(dx) > 0.05f || fabsf(dy) > 0.05f) {
    s_cx += dx;
    s_cy += dy;
    clamp_cursor();
  }
}

/* USB mouse: relative motion drives the cursor; the wheel tunes sensitivity.
 *
 * IMPORTANT: read the whole sample buffer, not just the newest state. HID samples
 * faster than we render, and a wheel tick shows up in exactly ONE sample -- if we
 * only look at the latest one each frame, most ticks land in samples we never see
 * and the wheel appears dead. Accumulate every sample we have not processed yet
 * (tracked by sampling_number). */
static u64 s_mouse_seen = 0;
static int s_mouse_logged = 0;

static int do_mouse(void) {
  HidMouseState st[16];
  int n = (int)hidGetMouseStates(st, 16);
  if (n <= 0) return 0;

  s32 dx = 0, dy = 0, wheel_y = 0, wheel_x = 0;
  u32 buttons = 0;
  u64 newest = s_mouse_seen, btn_sn = 0;

  for (int i = 0; i < n; i++) {
    const HidMouseState *m = &st[i];
    if (m->sampling_number > btn_sn) { btn_sn = m->sampling_number; buttons = m->buttons; }
    if (m->sampling_number <= s_mouse_seen) continue;      /* already handled */
    dx      += m->delta_x;
    dy      += m->delta_y;
    wheel_y += m->wheel_delta_y;
    wheel_x += m->wheel_delta_x;      /* some drivers put vertical scroll here */
    if (m->sampling_number > newest) newest = m->sampling_number;
  }

  /* Use whichever axis actually reports. On this hardware wheel_delta_y stayed 0
   * while the mouse otherwise worked perfectly, so don't assume either one. */
  s32 wheel = wheel_y ? wheel_y : wheel_x;

  /* Presence: trust the IsConnected attribute, but treat any real activity as
   * proof of a mouse too (in case a device never sets the flag). */
  u32 attrs = 0;
  for (int i = 0; i < n; i++)
    if (st[i].sampling_number == btn_sn) { attrs = st[i].attributes; break; }
  int conn = (attrs & HidMouseAttribute_IsConnected) ? 1 : 0;
  if (!conn && (dx || dy || wheel_x || wheel_y || buttons)) conn = 1;

  if (conn != s_mouse_connected) {
    s_mouse_connected = conn;
    logf_("nxp: mouse %s\n", conn ? "connected" : "disconnected");
    if (conn && s_gyro_on) {
      s_gyro_on = 0;                 /* a mouse takes priority over motion */
      logf_("nxp: gyro OFF (mouse connected)\n");
    }
  }

  if (s_mouse_seen == 0) {           /* first sight of the mouse: sync, don't jump */
    s_mouse_seen = newest;
    logf_("nxp: mouse detected (%d samples buffered)\n", n);
    return (buttons & HidMouseButton_Left) ? 1 : 0;
  }
  s_mouse_seen = newest;

  /* Log every event that carries a wheel value, plus a few movement samples, so
   * a dead wheel is immediately visible in the log rather than a mystery. */
  if ((wheel_x || wheel_y) || s_mouse_logged < 3) {
    if (s_mouse_logged < 40) {
      s_mouse_logged++;
      logf_("nxp: mouse dx=%d dy=%d wheel_y=%d wheel_x=%d buttons=0x%x\n",
            (int)dx, (int)dy, (int)wheel_y, (int)wheel_x, (unsigned)buttons);
    }
  }

  if (wheel != 0) {
    /* one notch = one step; several notches in a frame compound */
    for (int i = 0; i < (wheel > 0 ? wheel : -wheel) && i < 8; i++)
      s_mouse_sens *= (wheel > 0) ? 1.15f : (1.0f / 1.15f);
    if (s_mouse_sens < SENS_MIN) s_mouse_sens = SENS_MIN;
    if (s_mouse_sens > SENS_MAX) s_mouse_sens = SENS_MAX;
    logf_("nxp: mouse sensitivity = %.2f\n", s_mouse_sens);
  }

  if (dx || dy) {
    s_cx += (float)dx * s_mouse_sens;
    s_cy += (float)dy * s_mouse_sens;
    clamp_cursor();
    if (s_visible <= 0) s_visible = 1;     /* moving the mouse brings it up */
  }

  return (buttons & HidMouseButton_Left) ? 1 : 0;
}

/* ----------------------------------------------------------------- update */

void nxp_update(void) {
  if (!s_ready) return;
  padUpdate(&s_pad);

  const int docked = is_docked();
  if (docked != s_was_docked) {
    /* docked has no touchscreen, so make sure there is always a way to point */
    if (s_visible < 0 || docked) s_visible = docked ? 1 : 0;
    s_was_docked = docked;
  }

  const u64 held    = padGetButtons(&s_pad);
  const u64 pressed = padGetButtonsDown(&s_pad);

  /* '+' toggles the cursor; '-' toggles gyro pointing. */
  if (pressed & HidNpadButton_Plus) {
    s_visible = (s_visible > 0) ? 0 : 1;
    logf_("nxp: cursor %s\n", s_visible ? "ON" : "OFF");
  }
  if (pressed & HidNpadButton_Minus) {
    if (s_mouse_connected) {
      logf_("nxp: gyro stays OFF while a mouse is connected\n");
    } else if (!s_gyro_ready) {
      logf_("nxp: gyro unavailable on this controller\n");
    } else {
      s_gyro_on = !s_gyro_on;
      logf_("nxp: gyro %s\n", s_gyro_on ? "ON" : "OFF");
      if (s_gyro_on && s_visible <= 0) s_visible = 1;   /* pointing needs a cursor */
    }
  }

  /* L / R recenter the cursor to the middle of the screen. Handy with gyro:
   * point the controller where it's comfortable, recenter, and aim from there
   * (like recentering a VR view). Works in every mode. */
  if (pressed & (HidNpadButton_L | HidNpadButton_R)) {
    s_cx = s_cfg.screen_w * 0.5f;
    s_cy = s_cfg.screen_h * 0.5f;
    if (s_visible <= 0) s_visible = 1;    /* show it so you can see where it went */
    logf_("nxp: cursor recentered\n");
  }

  do_dpad(held, pressed);

  s_nev = 0;

  if (!docked) do_touch();                 /* touchscreen: handheld only */

  /* Mouse first: it sets s_mouse_connected, which gates the gyro below. */
  const int mouse_tap = do_mouse();
  do_gyro();                               /* no-op if off, or if a mouse is in */

  if (s_visible > 0) {
    HidAnalogStickState ls = padGetStickPos(&s_pad, 0);
    if (ls.x || ls.y) {
      s_cx += (ls.x / 32767.0f) * s_stick_speed;
      s_cy -= (ls.y / 32767.0f) * s_stick_speed;   /* stick +y is up */
      clamp_cursor();
    }

    /* A, ZR and ZL all confirm/tap (ZL/ZR let you play one-handed), as does the
     * mouse's left button. */
    const int tap = ((held & (HidNpadButton_A | HidNpadButton_ZR | HidNpadButton_ZL)) ? 1 : 0)
                    | mouse_tap;
    int phase = 0;
    if      ( tap && !s_tap_prev) phase = NXP_DOWN;
    else if ( tap &&  s_tap_prev) phase = NXP_MOVE;
    else if (!tap &&  s_tap_prev) phase = NXP_UP;
    s_tap_prev = tap;
    if (phase) push(s_cfg.cursor_id, s_cx, s_cy, phase);
  } else {
    s_tap_prev = 0;
  }

  settings_tick();          /* commits a queued save 3s after the last change */
}

int nxp_poll(NxpEvent *out, int max) {
  int n = s_nev < max ? s_nev : max;
  if (n > 0) memcpy(out, s_ev, (size_t)n * sizeof(NxpEvent));
  return n;
}

int   nxp_cursor_visible(void)          { return s_visible > 0; }
void  nxp_cursor_pos(float *x, float *y){ if (x) *x = s_cx; if (y) *y = s_cy; }
float nxp_stick_speed(void)             { return s_stick_speed; }
float nxp_mouse_sens(void)              { return s_mouse_sens; }
float nxp_gyro_sens(void)               { return s_gyro_sens; }
int   nxp_gyro_enabled(void)            { return s_gyro_on && !s_mouse_connected; }
int   nxp_mouse_connected(void)         { return s_mouse_connected; }


/* ======================= GL overlay ====================================== */

static GLuint s_prog = 0;
static GLint  s_u_screen, s_u_origin, s_u_scale, s_u_colour, s_u_tex, s_u_use_tex;
static int    s_gl_failed = 0;

/* Built-in arrow: tip at (0,0), y down, drawn as a fan from the tip. */
static const GLfloat s_arrow[] = {
   0.0f,  0.0f,   0.0f, 16.0f,   4.0f, 12.0f,   7.0f, 18.0f,
  10.0f, 16.5f,   7.0f, 10.5f,  12.0f, 10.0f,
};
#define ARROW_VERTS 7

static GLuint mkshader(GLenum t, const char *src) {
  GLuint s = glCreateShader(t);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { glDeleteShader(s); return 0; }
  return s;
}

static int gl_init(void) {
  if (s_prog) return 1;
  if (s_gl_failed) return 0;

  static const char *vs =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "uniform vec2 uScreen;\n"
    "uniform vec2 uOrigin;\n"
    "uniform float uScale;\n"
    "void main() {\n"
    "  vUV = aUV;\n"
    "  vec2 p = uOrigin + aPos * uScale;\n"
    "  gl_Position = vec4((p.x/uScreen.x)*2.0-1.0, 1.0-(p.y/uScreen.y)*2.0, 0.0, 1.0);\n"
    "}\n";
  static const char *fs =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform vec4 uColour;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uUseTex;\n"
    "void main() {\n"
    /* Branch on a uniform (uniform control flow -- always safe in GLES2) so the
     * built-in-arrow path never samples a texture. That means we don't have to
     * bind or disturb ANY texture state unless a custom cursor.png is in use. */
    "  if (uUseTex > 0.5) gl_FragColor = texture2D(uTex, vUV);\n"
    "  else               gl_FragColor = uColour;\n"
    "}\n";

  GLuint v = mkshader(GL_VERTEX_SHADER, vs), f = mkshader(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) { s_gl_failed = 1; logf_("nxp: cursor shader compile failed\n"); return 0; }

  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glBindAttribLocation(p, 0, "aPos");
  glBindAttribLocation(p, 1, "aUV");
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);

  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { glDeleteProgram(p); s_gl_failed = 1; logf_("nxp: cursor link failed\n"); return 0; }

  s_prog      = p;
  s_u_screen  = glGetUniformLocation(p, "uScreen");
  s_u_origin  = glGetUniformLocation(p, "uOrigin");
  s_u_scale   = glGetUniformLocation(p, "uScale");
  s_u_colour  = glGetUniformLocation(p, "uColour");
  s_u_tex     = glGetUniformLocation(p, "uTex");
  s_u_use_tex = glGetUniformLocation(p, "uUseTex");
  return 1;
}

/* A vertex attribute's FULL state. Saving only the enabled flag (as we did at
 * first) is not enough: glVertexAttribPointer also records size/type/stride/
 * pointer AND which ARRAY_BUFFER was bound at the time. The engine sets its
 * attribute pointers once and reuses them across frames, so overwriting slot 0
 * with our arrow array left every subsequent engine draw reading garbage
 * geometry -- 25 draw calls a frame, and a black screen. */
typedef struct {
  GLint enabled, size, type, norm, stride, buf;
  void *ptr;
} AttribState;

static void attrib_save(GLuint i, AttribState *a) {
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a->enabled);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a->size);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a->type);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a->norm);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a->stride);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a->buf);
  glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a->ptr);
}

static void attrib_restore(GLuint i, const AttribState *a) {
  /* glVertexAttribPointer captures whatever ARRAY_BUFFER is bound right now, so
   * re-bind the attribute's original buffer before restoring its pointer. */
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)a->buf);
  if (a->size > 0)
    glVertexAttribPointer(i, a->size, (GLenum)a->type,
                          (GLboolean)(a->norm ? GL_TRUE : GL_FALSE),
                          a->stride, a->ptr);
  if (a->enabled) glEnableVertexAttribArray(i);
  else            glDisableVertexAttribArray(i);
}

void nxp_draw(void) {
  if (!s_ready || s_visible <= 0) return;
  if (!gl_init()) return;

  /* decode + upload cursor.png on first draw (needs a live GL context) */
  if (!s_png_tried) {
    s_png_tried = 1;
    if (s_png_bytes) {
      if (!cursor_upload_png()) { s_cursor_tex = 0; }
      free(s_png_bytes); s_png_bytes = NULL; s_png_len = 0;   /* done with the bytes */
    }
  }

  /* ---- save every bit of state we touch ---- */
  AttribState a0, a1;
  attrib_save(0, &a0);
  attrib_save(1, &a1);

  GLint prev_prog = 0, prev_buf = 0;
  GLint bs_rgb = 0, bd_rgb = 0, bs_a = 0, bd_a = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buf);
  glGetIntegerv(GL_BLEND_SRC_RGB, &bs_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &bd_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &bs_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &bd_a);
  const GLboolean was_blend   = glIsEnabled(GL_BLEND);
  const GLboolean was_depth   = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean was_cull    = glIsEnabled(GL_CULL_FACE);
  const GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   /* honours PNG alpha */

  glUseProgram(s_prog);
  glUniform2f(s_u_screen, (GLfloat)s_cfg.screen_w, (GLfloat)s_cfg.screen_h);
  glUniform2f(s_u_origin, s_cx, s_cy);

  if (s_cursor_tex) {
    /* ---- custom PNG: textured quad, hotspot at the top-left ---- */
    const GLfloat w = (GLfloat)s_cursor_w, h = (GLfloat)s_cursor_h;
    const GLfloat quad[] = { 0,0,  w,0,  0,h,  w,h };
    const GLfloat uv[]   = { 0,0,  1,0,  0,1,  1,1 };

    /* Texture units: read the ACTIVE unit first, then switch to unit 0 and read
     * ITS binding. (Reading GL_TEXTURE_BINDING_2D before switching gives you the
     * active unit's binding, and restoring that onto unit 0 corrupts unit 0.) */
    GLint prev_active = 0, tex0 = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);                  /* client-side arrays */
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);

    glBindTexture(GL_TEXTURE_2D, s_cursor_tex);
    glUniform1i(s_u_tex, 0);
    glUniform1f(s_u_use_tex, 1.0f);
    glUniform1f(s_u_scale, (GLfloat)s_cfg.screen_h / 1080.0f);
    glUniform4f(s_u_colour, 1, 1, 1, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);        /* restore unit 0 */
    glActiveTexture((GLenum)prev_active);              /* then the active unit */
  } else {
    /* ---- built-in arrow: solid colour, no texture touched at all ---- */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, s_arrow);
    glUniform1f(s_u_use_tex, 0.0f);                    /* shader will not sample */

    const GLfloat sc = 2.4f * ((GLfloat)s_cfg.screen_h / 1080.0f);
    glUniform1f(s_u_scale, sc * 1.22f);
    glUniform4f(s_u_colour, 0.0f, 0.0f, 0.0f, 0.85f);  /* outline */
    glDrawArrays(GL_TRIANGLE_FAN, 0, ARROW_VERTS);

    glUniform1f(s_u_scale, sc);
    glUniform4f(s_u_colour, 1.0f, 1.0f, 1.0f, 1.0f);   /* fill */
    glDrawArrays(GL_TRIANGLE_FAN, 0, ARROW_VERTS);
  }

  /* ---- restore ----
   * Attributes first (each re-binds its own source buffer), then put the global
   * ARRAY_BUFFER binding back, then the program and the fixed-function bits. */
  attrib_restore(0, &a0);
  attrib_restore(1, &a1);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_buf);
  glUseProgram((GLuint)prev_prog);
  glBlendFuncSeparate((GLenum)bs_rgb, (GLenum)bd_rgb, (GLenum)bs_a, (GLenum)bd_a);
  if (!was_blend) glDisable(GL_BLEND); else glEnable(GL_BLEND);
  if (was_depth)   glEnable(GL_DEPTH_TEST);
  if (was_cull)    glEnable(GL_CULL_FACE);
  if (was_scissor) glEnable(GL_SCISSOR_TEST);
}
