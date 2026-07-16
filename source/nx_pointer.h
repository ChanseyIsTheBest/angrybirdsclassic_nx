/* nx_pointer.h -- reusable pointer input + on-screen cursor for Switch ports of
 * Android games.
 *
 * Self-contained: owns the pad, the touchscreen, a USB mouse, a stick-driven
 * virtual cursor, and the GL overlay that draws it. Drop nx_pointer.{c,h} into
 * any so-loader style port, call nxp_init() once, nxp_update() per frame,
 * nxp_poll() to drain events, and nxp_draw() right before eglSwapBuffers().
 *
 * Controls
 * --------
 *   Touchscreen  handheld only, always live
 *   '+'          toggle the cursor on / off   (handheld and docked)
 *   '-'          toggle GYRO pointing on / off
 *   Left stick   move the cursor
 *   L / R        recenter the cursor to the middle of the screen (aids gyro)
 *   A / ZR / ZL  tap / confirm at the cursor (ZL/ZR = one-handed play)
 *   D-pad U/D    decrease / increase sensitivity -- applies to whichever device
 *                is ACTUALLY driving the cursor: mouse if one is plugged in,
 *                else gyro if active, else the stick. One control, one job; it
 *                never tunes a device that isn't there.
 *   USB mouse    moves the cursor (handheld or docked); left button taps; the
 *                wheel adjusts sensitivity where the HID layer reports it.
 *   Gyro         tilt/turn the controller to point. Yaw moves X, pitch moves Y.
 *                A connected mouse takes priority: gyro is switched off while
 *                one is plugged in.
 *
 * Custom cursor
 * -------------
 * If <data_dir>/cursor.png exists and is at most 64x64, it is used as the
 * cursor (alpha respected). Otherwise a built-in vector arrow is drawn.
 *
 * Settings
 * --------
 * The three sensitivities (stick, mouse, gyro) are loaded from
 * <data_dir>/pointer.cfg at startup and saved back 3 seconds after the last
 * change -- so a burst of D-pad presses costs exactly one write, not twenty.
 */
#include <stdio.h>   /* FILE, for the optional locked-I/O hooks below */
#ifndef NX_POINTER_H
#define NX_POINTER_H

#include <stdint.h>

/* Pointer phases -- deliberately the same values the ports already use. */
enum { NXP_DOWN = 1, NXP_MOVE = 2, NXP_UP = 3 };

/* Layout-compatible with the usual `PtrEvent` so a port can cast directly. */
typedef struct { int id; float x, y; int phase; } NxpEvent;

typedef struct {
  int   screen_w, screen_h;      /* render space, e.g. 1920x1080            */
  int   panel_w,  panel_h;       /* touch panel space; 0 => 1280x720        */
  const char *data_dir;          /* where cursor.png lives, e.g. "sdmc:/switch/btd5" */

  int   cursor_id;               /* pointer id for the cursor; 0 => 8       */
  int   max_touch_slots;         /* touch ids 0..n-1;        0 => 8         */

  float stick_speed;             /* px/frame at full deflection; 0 => 14    */
  float mouse_sens;              /* mouse px multiplier;        0 => 1.0    */

  void (*log)(const char *msg);  /* optional; may be NULL                   */

  /* OPTIONAL locked file I/O.
   * so-loader ports MUST serialise newlib file calls: devkitPro's handle table
   * is not thread-safe, and the game's worker threads hammer it constantly. If
   * the port has locked fopen/fclose wrappers, pass them here and settings I/O
   * will go through them. Leave NULL to use plain fopen/fclose (fine for a
   * single-threaded host, unsafe next to a live engine). */
  FILE *(*fopen_fn)(const char *path, const char *mode);
  int   (*fclose_fn)(FILE *f);
} NxpConfig;

/* Call once, EARLY (before the engine spawns threads): it reads cursor.png off
 * the SD card. Decoding and GL upload happen lazily on the render thread. */
void nxp_init(const NxpConfig *cfg);

/* Once per frame, before nxp_poll(). Reads pad/touch/mouse and builds events. */
void nxp_update(void);

/* Drain this frame's pointer events. Returns how many were written. */
int  nxp_poll(NxpEvent *out, int max);

/* Draw the cursor. Call with the engine's GL context current -- the natural
 * place is inside your eglSwapBuffers wrapper, just before the real swap, so
 * the cursor lands on top of the finished frame. Saves/restores all GL state. */
void nxp_draw(void);

/* Queries / misc */
int   nxp_cursor_visible(void);
void  nxp_cursor_pos(float *x, float *y);
float nxp_stick_speed(void);     /* current scroll rate (px/frame)          */
float nxp_mouse_sens(void);      /* current mouse sensitivity               */
float nxp_gyro_sens(void);       /* current gyro sensitivity                */
int   nxp_gyro_enabled(void);    /* gyro pointing active right now?         */
int   nxp_mouse_connected(void); /* a USB mouse is present                  */

/* Flush any pending settings write immediately (e.g. on shutdown). Normally the
 * save happens by itself 3s after the last change. */
void  nxp_save_settings(void);

#endif /* NX_POINTER_H */
