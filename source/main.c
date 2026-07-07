/* main.c -- Angry Birds Classic (Rovio "Fusion" engine) Switch wrapper
 *
 * Fusion is a single native library. Its Android Java layer loaded it, ran
 * JNI_OnLoad, created a GLSurfaceView (GLES2) and drove the engine through
 * NativeApplication lifecycle calls: nativeConfig, nativeInit(w,h),
 * nativeResize(w,h), then per frame nativeUpdate (which also renders -- the
 * separate nativeRender is a stub in this build). Touch came in through
 * MyInputHandler.nativeInput(action, pointerId, x, y). Audio is pulled from the
 * engine via AudioOutput.nativeMixData (see audio.c). We recreate all of that:
 * load + link the library, stand up a GLES2 context and a fake JNI, run
 * JNI_OnLoad, then drive the same lifecycle from a libnx main loop.
 *
 * Data you provide (from a copy of the game you own; nothing bundled):
 *   libAngryBirdsClassic.so   -- lib/arm64-v8a/ in the APK
 *   assets/...                -- the APK's assets/ folder, unpacked next to the .nro
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "libc_shim.h"
#include "jni_fake.h"
#include "fusion.h"
#include "audio.h"

int screen_width = 1280, screen_height = 720;

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;
so_module game_mod;

// Heap split: reserve a fixed zone for the .so (code + data + relocation scratch)
// and give ALL remaining memory to the newlib heap, which is where the engine's
// malloc goes (textures, audio, Lua/Box2D). See config.h SO_ZONE_MB.
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  size_t so_zone = (size_t)SO_ZONE_MB * 1024 * 1024;
  if (so_zone > size / 2) so_zone = size / 2;         // never starve the game
  size_t newlib_size = size - so_zone;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + newlib_size;

  heap_so_base = (void *)ALIGN_MEM((uintptr_t)((char *)addr + newlib_size), 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nExtract it from your APK's lib/arm64-v8a\nand place it next to the .nro.", SO_NAME);
}

// ---------------------------------------------------------------------------
// EGL / GLES2 context on the default NWindow
// ---------------------------------------------------------------------------
static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;
static EGLConfig  s_egl_config = 0;   // kept so we can rebuild the surface on resize

// (re)create the window surface at the current screen_width/height and make it
// current. Used at startup and whenever the resolution changes (dock/undock).
static int egl_make_surface(void) {
  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  EGLSurface surf = eglCreateWindowSurface(s_display, s_egl_config, win, NULL);
  if (!surf) { debugPrintf("egl: no surface (%dx%d)\n", screen_width, screen_height); return 0; }
  eglMakeCurrent(s_display, surf, surf, s_context);
  if (s_surface && s_surface != surf) eglDestroySurface(s_display, s_surface);
  s_surface = surf;
  eglSwapInterval(s_display, 1);
  return 1;
}

static int egl_init(void) {
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) { debugPrintf("egl: no display\n"); return 0; }
  eglInitialize(s_display, NULL, NULL);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { debugPrintf("egl: bindAPI failed\n"); goto fail; }

  const EGLint cfg_attr[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,   // GLES2 (shaders)
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  EGLint num = 0;
  if (!eglChooseConfig(s_display, cfg_attr, &s_egl_config, 1, &num) || num < 1) {
    debugPrintf("egl: no config\n"); goto fail;
  }

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  s_context = eglCreateContext(s_display, s_egl_config, EGL_NO_CONTEXT, ctx_attr);
  if (!s_context) { debugPrintf("egl: no context\n"); goto fail; }

  if (!egl_make_surface()) goto fail;
  return 1;

fail:
  // Release whatever we grabbed so the default window is free again -- otherwise
  // fatal_error()'s consoleInit() would fight EGL for it and crash.
  if (s_surface) { eglDestroySurface(s_display, s_surface); s_surface = EGL_NO_SURFACE; }
  if (s_context) { eglDestroyContext(s_display, s_context); s_context = EGL_NO_CONTEXT; }
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
  return 0;
}

static void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context) eglDestroyContext(s_display, s_context);
  if (s_surface) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
}

// ---------------------------------------------------------------------------
// engine entry points (exact exported symbol names)
// ---------------------------------------------------------------------------
#define APP(name) "Java_com_rovio_fusion_NativeApplication_" name
#define INP(name) "Java_com_rovio_fusion_MyInputHandler_" name
#define AUD(name) "Java_com_rovio_fusion_AudioOutput_" name

typedef void (*fn_v)(JNIEnv, void *);
typedef void (*fn_cfg)(JNIEnv, void *, jobject config);
typedef void (*fn_wh)(JNIEnv, void *, jint w, jint h);
typedef void (*fn_input)(JNIEnv, void *, jint action, jint pointerId, jfloat x, jfloat y);
typedef void (*fn_key)(JNIEnv, void *, jint keyCode, jint action, jint a4, jint a5);
typedef jint (*fn_onload)(JavaVM, void *);

static fn_cfg   e_nativeConfig;
static fn_wh    e_nativeInit;
static fn_wh    e_nativeResize;
static fn_v     e_nativeUpdate;
static fn_v     e_nativePause;
static fn_v     e_nativeResume;
static fn_v     e_nativeDeinit;
static fn_input e_nativeInput;
static fn_key   e_nativeKeyInput;
static void    *e_nativeMixData;
static fn_onload e_JNI_OnLoad;

static void *thiz;

static void resolve_entry_points(void) {
  e_JNI_OnLoad   = (fn_onload)so_find_addr_rx(&game_mod, "JNI_OnLoad");
  e_nativeConfig = (fn_cfg)so_find_addr_rx(&game_mod, APP("nativeConfig"));
  e_nativeInit   = (fn_wh)so_find_addr_rx(&game_mod, APP("nativeInit"));
  e_nativeResize = (fn_wh)so_find_addr_rx(&game_mod, APP("nativeResize"));
  e_nativeUpdate = (fn_v)so_find_addr_rx(&game_mod, APP("nativeUpdate"));
  e_nativePause  = (fn_v)so_try_find_addr_rx(&game_mod, APP("nativePause"));
  e_nativeResume = (fn_v)so_try_find_addr_rx(&game_mod, APP("nativeResume"));
  e_nativeDeinit = (fn_v)so_try_find_addr_rx(&game_mod, APP("nativeDeinit"));
  e_nativeInput  = (fn_input)so_find_addr_rx(&game_mod, INP("nativeInput"));
  e_nativeKeyInput = (fn_key)so_try_find_addr_rx(&game_mod, INP("nativeKeyInput"));
  e_nativeMixData = (void *)so_try_find_addr_rx(&game_mod, AUD("nativeMixData"));
  if (!e_nativeMixData) debugPrintf("nativeMixData not found -- audio disabled\n");
}

// ---------------------------------------------------------------------------
// input: touch panel + a left-stick/A pointer for docked play
//   Fusion nativeInput actions (from disassembly): 0=DOWN 1=UP 2=MOVE 3=CANCEL
// ---------------------------------------------------------------------------
#define ACT_DOWN 0
#define ACT_UP   1
#define ACT_MOVE 2

static PadState pad;
static int touch_down = 0; static float last_x = 0, last_y = 0;
static float cur_x, cur_y; static int cur_init = 0, btn_down = 0;
static int g_docked = 0;      // updated from the operation mode each frame
static int back_held = 0;

#define AKEYCODE_BACK 4       // Android KEYCODE_BACK
#define AKEY_DOWN 0
#define AKEY_UP   1

static void update_touch(void) {
  HidTouchScreenState st = {0};
  const float sx = (float)screen_width / 1280.0f;
  const float sy = (float)screen_height / 720.0f;
  if (hidGetTouchScreenStates(&st, 1) && st.count > 0) {
    const float x = st.touches[0].x * sx, y = st.touches[0].y * sy;
    if (!touch_down) { touch_down = 1; e_nativeInput(fake_env, thiz, ACT_DOWN, 0, x, y); }
    else if (x != last_x || y != last_y) { e_nativeInput(fake_env, thiz, ACT_MOVE, 0, x, y); }
    last_x = x; last_y = y;
  } else if (touch_down) {
    touch_down = 0; e_nativeInput(fake_env, thiz, ACT_UP, 0, last_x, last_y);
  }
}

// Left stick drives a virtual cursor; A taps/drags at it (press-move-release
// gives slingshot control). This is the docked-mode input path since there's no
// touchscreen, but it's harmless in handheld too. B / + send the Android Back
// key (cancel / menu-back).
static void update_stick_pointer(void) {
  padUpdate(&pad);
  if (!cur_init) { cur_x = screen_width * 0.5f; cur_y = screen_height * 0.5f; cur_init = 1; }
  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  const float dz = 0.15f * 32767.0f;
  const float speed = screen_height / 720.0f * 12.0f;   // scale cursor speed with resolution
  float dx = (fabsf((float)ls.x) > dz) ? (ls.x / 32767.0f) : 0.0f;
  float dy = (fabsf((float)ls.y) > dz) ? (ls.y / 32767.0f) : 0.0f;
  if (dx != 0.0f || dy != 0.0f) {
    cur_x += dx * speed; cur_y -= dy * speed;
    if (cur_x < 0) cur_x = 0; if (cur_x > screen_width)  cur_x = screen_width;
    if (cur_y < 0) cur_y = 0; if (cur_y > screen_height) cur_y = screen_height;
    if (btn_down) e_nativeInput(fake_env, thiz, ACT_MOVE, 0, cur_x, cur_y);
  }
  const u64 down = padGetButtonsDown(&pad), up = padGetButtonsUp(&pad);
  const u64 held = padGetButtons(&pad);
  if ((down & HidNpadButton_A) && !btn_down) { btn_down = 1; e_nativeInput(fake_env, thiz, ACT_DOWN, 0, cur_x, cur_y); }
  if ((up & HidNpadButton_A) && btn_down)    { btn_down = 0; e_nativeInput(fake_env, thiz, ACT_UP, 0, cur_x, cur_y); }

  // B / + -> Android Back key
  if (e_nativeKeyInput) {
    if ((down & (HidNpadButton_B | HidNpadButton_Plus)) && !back_held) {
      back_held = 1; e_nativeKeyInput(fake_env, thiz, AKEYCODE_BACK, AKEY_DOWN, 0, 0);
    }
    if (back_held && !(held & (HidNpadButton_B | HidNpadButton_Plus))) {
      back_held = 0; e_nativeKeyInput(fake_env, thiz, AKEYCODE_BACK, AKEY_UP, 0, 0);
    }
  }
}

// Draw the virtual cursor as a small white crosshair with a dark outline, using
// scissor+clear (no shaders needed). Docked only -- handheld uses the touch
// screen. Called after the engine renders, before the buffer swap.
static void draw_cursor(void) {
  if (!g_docked) return;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);        // engine uses FBOs (pp.fx); draw to screen
  const int cx = (int)cur_x;
  const int cy = screen_height - (int)cur_y;   // GL origin is bottom-left
  const int L = screen_height / 40;            // arm length, scales with res
  int t = screen_height / 240; if (t < 2) t = 2;  // arm thickness
  glEnable(GL_SCISSOR_TEST);
  // outline (black), slightly larger
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glScissor(cx - L - 1, cy - t/2 - 1, 2*L + 2, t + 2); glClear(GL_COLOR_BUFFER_BIT);
  glScissor(cx - t/2 - 1, cy - L - 1, t + 2, 2*L + 2); glClear(GL_COLOR_BUFFER_BIT);
  // white core
  glClearColor(1.f, 1.f, 1.f, 1.f);
  glScissor(cx - L, cy - t/2, 2*L, t); glClear(GL_COLOR_BUFFER_BIT);
  glScissor(cx - t/2, cy - L, t, 2*L); glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
}

// Software keyboard: when the engine (TextInput) requests it, pop the Switch
// keyboard. Delivering the text back into the engine is build-specific (Fusion
// has a nativeKeyInput / TextInput callback whose exact signature isn't known
// without the Java source), so for now we log the result; wire it in once the
// device log shows how the engine expects the string. See README.
static void handle_keyboard(void) {
  if (!g_kbd_requested) return;
  g_kbd_requested = 0;
  SwkbdConfig kbd;
  if (R_FAILED(swkbdCreate(&kbd, 0))) return;
  swkbdConfigMakePresetDefault(&kbd);
  swkbdConfigSetStringLenMax(&kbd, 32);
  char out[64] = {0};
  Result rc = swkbdShow(&kbd, out, sizeof(out));
  swkbdClose(&kbd);
  if (R_SUCCEEDED(rc)) debugPrintf("swkbd result: \"%s\" (delivery TODO)\n", out);
}

// ---------------------------------------------------------------------------
// single-module load
// ---------------------------------------------------------------------------
static void load_module(void) {
  if (so_load(&game_mod, SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);
  so_relocate(&game_mod);
  update_imports();
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);
  resolve_entry_points();
  so_finalize(&game_mod);
  so_flush_caches(&game_mod);
  so_execute_init_array(&game_mod);   // C++ constructors + JNI_OnLoad prerequisites
  so_free_temp(&game_mod);
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
static void start_engine(void) {
  // JNI_OnLoad first: the engine caches the JavaVM + its wrapper classes/methods.
  if (e_JNI_OnLoad) {
    jint v = e_JNI_OnLoad(fake_vm, NULL);
    debugPrintf("JNI_OnLoad -> 0x%x\n", v);
  }

  // nativeConfig: exact config-object shape is build-specific; we pass an empty
  // config token and log. If the engine reads fields off it they resolve to 0
  // via the fake JNI (defaults). Refine from the device log if needed.
  if (e_nativeConfig) {
    jobject cfg = jni_make_string(""); // non-null stand-in
    debugPrintf("nativeConfig(...)\n");
    e_nativeConfig(fake_env, thiz, cfg);
  }

  debugPrintf("nativeInit(%dx%d)\n", screen_width, screen_height);
  e_nativeInit(fake_env, thiz, screen_width, screen_height);
  e_nativeResize(fake_env, thiz, screen_width, screen_height);
  if (e_nativeResume) e_nativeResume(fake_env, thiz);

  if (e_nativeMixData) audio_set_mixer(e_nativeMixData, thiz);
}

static AppletHookCookie s_applet_cookie;
static void applet_hook_fn(AppletHookType type, void *param) {
  (void)param;
  if (type == AppletHookType_OnExitRequest) jni_quit_requested = 1;
}

static void boot_mark(const char *stage) {
  FILE *f = fopen(DATA_DIR "/angrybirds_nx_stage.txt", "w");
  if (f) { fputs(stage, f); fputc('\n', f); fclose(f); }
}

// ---------------------------------------------------------------------------
// resolution: automatic per dock state, or forced by config.txt
// ---------------------------------------------------------------------------
static void compute_resolution(int *w, int *h) {
  g_docked = (appletGetOperationMode() == AppletOperationMode_Console);
  if (config.screen_width > 0 && config.screen_height > 0) {
    *w = config.screen_width;  *h = config.screen_height;    // forced by config
  } else if (g_docked) {
    *w = (config.docked_width  > 0) ? config.docked_width  : 1920;
    *h = (config.docked_height > 0) ? config.docked_height : 1080;
  } else {
    *w = 1280; *h = 720;                                     // handheld panel
  }
  if (*w > 1920) *w = 1920;   if (*h > 1080) *h = 1080;      // Switch display cap
  if (*w < 640)  *w = 1280;   if (*h < 360)  *h = 720;
}

// Re-apply the resolution for the current dock state; recreates the EGL surface
// and tells the engine to resize. Called every frame but only acts on a change,
// so docking/undocking live switches between 1080p and 720p.
static void update_resolution(void) {
  int w, h;
  compute_resolution(&w, &h);                 // also refreshes g_docked
  if (w == screen_width && h == screen_height) return;
  const int ow = screen_width, oh = screen_height;
  screen_width = w; screen_height = h;
  if (!egl_make_surface()) {                   // e.g. mid dock transition
    screen_width = ow; screen_height = oh;     // revert, retry next frame
    debugPrintf("resize: surface recreate failed, retrying\n");
    return;
  }
  glViewport(0, 0, screen_width, screen_height);
  if (e_nativeResize) e_nativeResize(fake_env, thiz, screen_width, screen_height);
  debugPrintf("resolution -> %dx%d (%s)\n", w, h, g_docked ? "docked" : "handheld");
}

int main(int argc, char *argv[]) {
  // Make sure the log/breadcrumb directory exists before anything writes to it
  // (fopen won't create parent folders). Harmless if it already exists.
  mkdir("sdmc:/switch", 0777);
  mkdir(DATA_DIR, 0777);

  // Bulletproof breadcrumb: overwrite a tiny file with the last stage reached,
  // independent of DEBUG_LOG and flushed on every fclose. If the app vanishes,
  // this file tells us exactly how far it got before dying.
  boot_mark("main_entry");

  // Resolve the game directory (where the .so and assets/ live). hbloader passes
  // the .nro's full path as argv[0]; strip the filename. Fall back to DATA_DIR.
  // Assets are then found under <base>/assets/... regardless of the process cwd,
  // and we chdir there so the engine's own relative reads/writes resolve too.
  {
    char base[512];
    base[0] = 0;
    if (argc >= 1 && argv && argv[0] && strchr(argv[0], '/')) {
      strncpy(base, argv[0], sizeof(base) - 1);
      base[sizeof(base) - 1] = 0;
      char *slash = strrchr(base, '/');
      if (slash) *slash = 0; // cut "/file.nro", keep the directory
    }
    if (!base[0]) strcpy(base, DATA_DIR);
    set_asset_base(base);
    chdir(base);
  }

  cpu_boost(1);
  appletHook(&s_applet_cookie, applet_hook_fn, NULL);

  check_syscalls();
  check_data();

  // Load config.txt (create it with defaults on first run / if it names retired
  // keys), then pick the initial resolution for the current dock state.
  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);
  { int w, h; compute_resolution(&w, &h); screen_width = w; screen_height = h; }
  debugPrintf("config: screen=%dx%d docked_pref=%dx%d -> boot %dx%d (%s)\n",
      config.screen_width, config.screen_height, config.docked_width, config.docked_height,
      screen_width, screen_height, g_docked ? "docked" : "handheld");

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO) < 0)
    debugPrintf("SDL_Init(audio) failed: %s\n", SDL_GetError());
  boot_mark("after_sdl");

  if (!egl_init())
    fatal_error("Failed to create an OpenGL ES 2 context.\n\nInstall switch-mesa (pacman -S switch-mesa\nswitch-libdrm_nouveau) and rebuild.");
  boot_mark("after_egl");

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  debugPrintf("heap: .so zone %u KB at %p\n",
      (unsigned)(heap_so_limit / 1024), heap_so_base);

  load_module();
  boot_mark("after_load_module");
  jni_init();
  thiz = jni_make_thiz();
  start_engine();
  boot_mark("after_start_engine");

  int frame = 0, boot_frames = 0;
  const u64 frame_ticks = armNsToTicks(1000000000ull / 60);
  u64 frame_deadline = armGetSystemTick() + frame_ticks;

  while (appletMainLoop() && !jni_quit_requested) {
    update_resolution();                  // live dock/undock -> 1080p / 720p
    update_touch();
    update_stick_pointer();
    handle_keyboard();
    audio_poll();

    e_nativeUpdate(fake_env, thiz);       // update + render (this build)
    draw_cursor();                        // virtual cursor overlay (docked)
    eglSwapBuffers(s_display, s_surface);

    if (frame < 5 || (frame % 300) == 0) {
      debugPrintf("frame %d\n", frame);
      if (frame == 0) boot_mark("first_frame");
    }

    // hold to ~60 Hz
    const s64 remain = (s64)(frame_deadline - armGetSystemTick());
    if (remain > 0) {
      const u64 remain_ns = armTicksToNs((u64)remain);
      if (remain_ns > 1500000ull) svcSleepThread((s64)(remain_ns - 1000000ull));
      while ((s64)(frame_deadline - armGetSystemTick()) > 0) ;
      frame_deadline += frame_ticks;
    } else {
      frame_deadline = armGetSystemTick() + frame_ticks;
    }
    frame++;
    if (boot_frames < 10 && ++boot_frames == 10) cpu_boost(0);
  }

  debugPrintf("shutting down\n");
  boot_mark("exiting");
  if (e_nativePause)  e_nativePause(fake_env, thiz);
  if (e_nativeDeinit) e_nativeDeinit(fake_env, thiz);   // must stop engine threads
  audio_shutdown();      // closes the SDL device (joins its callback thread)
  SDL_Quit();
  egl_deinit();          // release the GL context + window before we hand back
  cpu_boost(0);          // restore default clocks
  appletUnhook(&s_applet_cookie);
  debugLogFlush();

  // Return normally so libnx runs its standard teardown (services, gpu, applet)
  // and hands control cleanly back to the homebrew menu. Calling __libnx_exit
  // here instead would skip that and can leave the menu in a state that crashes
  // when it resumes.
  return 0;
}
