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
#include "locale_patch.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "libc_shim.h"
#include "jni_fake.h"
#include "fusion.h"
#include "audio.h"
#include "nx_pointer.h"

int screen_width = 1920, screen_height = 1080;   // fixed 1080p (see config.h)

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
// input: the nx_pointer module owns touch, USB mouse, the stick-driven cursor
// and gyro pointing, and draws the on-screen cursor. We translate its pointer
// events into the engine's nativeInput calls (Fusion actions, from disassembly:
// 0=DOWN 1=UP 2=MOVE). B is kept as a hardware Android "Back" -- the module uses
// +, -, L, R, A, ZR, ZL and the d-pad, but not B -- driven off its own pad.
// ---------------------------------------------------------------------------
#define ACT_DOWN 0
#define ACT_UP   1
#define ACT_MOVE 2
#define AKEYCODE_BACK 4       // Android KEYCODE_BACK

static PadState back_pad;
static int back_held = 0;

static int nxp_phase_to_act(int phase) {
  switch (phase) {
    case NXP_DOWN: return ACT_DOWN;
    case NXP_UP:   return ACT_UP;
    default:       return ACT_MOVE;   // NXP_MOVE
  }
}

// Feed this frame's pointer events (touch / stick cursor / mouse / gyro) to the
// engine as nativeInput calls.
static void feed_pointer(void) {
  NxpEvent ev[24];
  int n = nxp_poll(ev, 24);
  for (int i = 0; i < n; i++)
    e_nativeInput(fake_env, thiz, nxp_phase_to_act(ev[i].phase), ev[i].id, ev[i].x, ev[i].y);
}

// B -> Android Back (cancel / menu-back), as a down/up pair.
static void update_backkey(void) {
  padUpdate(&back_pad);
  if (!e_nativeKeyInput) return;
  const u64 down = padGetButtonsDown(&back_pad);
  const u64 held = padGetButtons(&back_pad);
  if ((down & HidNpadButton_B) && !back_held) {
    back_held = 1; e_nativeKeyInput(fake_env, thiz, AKEYCODE_BACK, 0, 0, 0);
  }
  if (back_held && !(held & HidNpadButton_B)) {
    back_held = 0; e_nativeKeyInput(fake_env, thiz, AKEYCODE_BACK, 1, 0, 0);
  }
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

  // nativeConfig's string is the app's writable files directory (the engine uses
  // it as its file-cache path via getPathToFileCacheDirectory) -- confirmed from
  // the Fusion Java layer (MySurfaceView passes getFilesDir().getAbsolutePath()).
  if (e_nativeConfig) {
    jobject cfg = jni_make_string(DATA_DIR);
    debugPrintf("nativeConfig(\"%s\")\n", DATA_DIR);
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

static void nxp_log(const char *msg) { debugPrintf("%s", msg); }

static void boot_mark(const char *stage) {
  FILE *f = fopen(DATA_DIR "/angrybirds_nx_stage.txt", "w");
  if (f) { fputs(stage, f); fputc('\n', f); fclose(f); }
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
  // Load config.txt (create it / add new keys on first run), then set the fixed
  // 1080p render size. Handheld downscales to the 720p panel automatically.
  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);
  screen_width = 1920; screen_height = 1080;
  debugPrintf("config: language=%s -> render %dx%d (fixed)\n",
      config.language, screen_width, screen_height);

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO) < 0)
    debugPrintf("SDL_Init(audio) failed: %s\n", SDL_GetError());
  boot_mark("after_sdl");

  if (!egl_init())
    fatal_error("Failed to create an OpenGL ES 2 context.\n\nInstall switch-mesa (pacman -S switch-mesa\nswitch-libdrm_nouveau) and rebuild.");
  boot_mark("after_egl");

  // Pointer input + on-screen cursor (touch, USB mouse, stick cursor, gyro). It
  // owns the pad/touch/mouse; we keep a small separate pad just for the B (Back)
  // button, which the module doesn't use. pointer.cfg + cursor.png live in the
  // game folder; route its file I/O through the engine's wrappers.
  NxpConfig np = {0};
  np.screen_w = screen_width; np.screen_h = screen_height;
  np.panel_w  = 1280; np.panel_h = 720;
  np.cursor_id = 0;     // Angry Birds only registers taps on low pointer ids
  np.data_dir = DATA_DIR;
  np.log = nxp_log;
  np.fopen_fn = fopen_fake; np.fclose_fn = fclose_fake;
  nxp_init(&np);
  padInitializeDefault(&back_pad);   // separate pad just for the B (Back) button

  debugPrintf("heap: .so zone %u KB at %p\n",
      (unsigned)(heap_so_limit / 1024), heap_so_base);

  load_module();
  boot_mark("after_load_module");
  jni_init();
  thiz = jni_make_thiz();
  // Patch stock gamelogic.lua to the configured language (before the engine
  // loads any script). No-op for English/unsupported or if the key/format differ.
  locale_patch_init();
  boot_mark("after_locale_patch");
  start_engine();
  boot_mark("after_start_engine");

  int frame = 0, boot_frames = 0;
  const u64 frame_ticks = armNsToTicks(1000000000ull / 60);
  u64 frame_deadline = armGetSystemTick() + frame_ticks;

  while (appletMainLoop() && !jni_quit_requested) {
    nxp_update();                         // touch / mouse / stick cursor / gyro
    feed_pointer();                       // -> engine nativeInput
    update_backkey();                     // B -> Android Back
    handle_keyboard();
    audio_poll();

    e_nativeUpdate(fake_env, thiz);       // update + render (this build)
    nxp_draw();                           // cursor overlay on top of the frame
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
