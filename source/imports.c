/* imports.c -- .so import resolution for Angry Birds Classic
 *              (libAngryBirdsClassic.so = Rovio "Fusion" engine, single module)
 *
 * Pthread wrappers, the lazy-init sync-object handling and the general shape of
 * this table are adapted from the FF4:AY / Burger Shop Switch ports (fgsfds,
 * Andy Nguyen, ChanseyIsTheBest) under the MIT license.
 *
 * Angry Birds' native lib imports 318 symbols: a libc subset (shimmed where
 * bionic and newlib disagree, otherwise straight to newlib), the GLES2 shader
 * pipeline over mesa, the Android AAsset NDK surface (emulated over loose files
 * in the game dir), __android_log_*, and a networking subset that is stubbed to
 * fail cleanly so the engine's ad/analytics paths disable themselves. The Java
 * side it calls back into is recreated in fusion.c / jni_fake.c, not here.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <wctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <wchar.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "os_shims.h"

/* newlib provides these as globals; the engine imports them by name. */
extern uintptr_t __cxa_atexit;
// Stack Smashing Protection symbols the engine imports; provided by the
// toolchain's SSP runtime. We hand the library the host's own guard + handler.
extern uintptr_t __stack_chk_fail;
extern uintptr_t __stack_chk_guard;

/* ------------------------------------------------------------------------- */
/* stubs for imports we deliberately neutralise                              */
/* ------------------------------------------------------------------------- */

/* networking: no sockets in a homebrew port. Fail so the engine's optional
 * ad/telemetry/cloud paths detect "offline" and skip themselves. errno=ENOSYS
 * keeps any error-string logging sane. */
static long net_fail_stub(void) { errno = ENOSYS; return -1; }

/* Google/bionic scheduler hints used to bracket blocking regions for the ART
 * runtime. There is no ART here; they are pure no-ops. */
static void noop_stub(void) {}

/* Android logging -> our debug log. Keep the tag+message, drop the priority. */
static int __android_log_print_stub(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  char buf[1024];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return debugPrintf("[AB/%s] %s\n", tag ? tag : "?", buf);
}
static void __android_log_assert_stub(const char *cond, const char *tag, const char *fmt, ...) {
  char buf[1024] = {0};
  if (fmt) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
  }
  debugPrintf("[AB/%s] ASSERT(%s): %s\n", tag ? tag : "?", cond ? cond : "", buf);
}


// ---------------------------------------------------------------------------
// pthread wrappers: bionic allocates the opaque sync types inline and zero-
// inits them, so we lazily back them with heap-allocated newlib objects
// stashed through the caller's pointer slot. (Carried over from the FF4 port.)
// ---------------------------------------------------------------------------

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (mutexattr && *mutexattr == 1);
  int ret;
  if (recursive) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return -1; }
  *uid = m;
  return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) ret = pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) { int attr = 1; ret = pthread_mutex_init_fake(uid, &attr); }
  if (ret < 0) return ret;
  return pthread_mutex_lock(*uid);
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) ret = pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) { int attr = 1; ret = pthread_mutex_init_fake(uid, &attr); }
  if (ret < 0) return ret;
  return pthread_mutex_trylock(*uid);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) ret = pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) { int attr = 1; ret = pthread_mutex_init_fake(uid, &attr); }
  if (ret < 0) return ret;
  return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  (void)condattr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) != 0) { free(c); return -1; }
  *cnd = c;
  return 0;
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_broadcast(*cnd);
}

int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_signal(*cnd);
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd) {
    pthread_cond_destroy(*cnd);
    free(*cnd);
    *cnd = NULL;
  }
  return 0;
}

int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  if (!*mtx && pthread_mutex_init_fake(mtx, NULL) < 0) return -1;
  return pthread_cond_wait(*cnd, *mtx);
}

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  if (!*mtx && pthread_mutex_init_fake(mtx, NULL) < 0) return -1;
  // Some engine threads set their condvar clock to CLOCK_MONOTONIC and compute
  // the deadline from clock_gettime(CLOCK_MONOTONIC); newlib's condvar waits on
  // CLOCK_REALTIME. Reduce the caller's absolute deadline to a relative delay
  // against whichever clock it matches, then rebuild it as a REALTIME deadline
  // newlib understands.
  struct timespec mono, real;
  clock_gettime(CLOCK_MONOTONIC, &mono);
  clock_gettime(CLOCK_REALTIME, &real);
  long long t_ns    = (long long)t->tv_sec * 1000000000LL + t->tv_nsec;
  long long mono_ns = (long long)mono.tv_sec * 1000000000LL + mono.tv_nsec;
  long long real_ns = (long long)real.tv_sec * 1000000000LL + real.tv_nsec;
  long long rel_m = t_ns - mono_ns;
  long long rel_r = t_ns - real_ns;
  long long rel = (llabs(rel_m) <= llabs(rel_r)) ? rel_m : rel_r;
  if (rel < 0) rel = 0;
  if (rel > 1000000000LL) rel = 1000000000LL; // clamp to 1s, never hang
  long long deadline = real_ns + rel;
  struct timespec abs;
  abs.tv_sec  = (time_t)(deadline / 1000000000LL);
  abs.tv_nsec = (long)(deadline % 1000000000LL);
  return pthread_cond_timedwait(*cnd, *mtx, &abs);
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

// Engine worker threads must get tpidr_el0 pointed at a stack-guard block before
// they run any guarded code (see tls_setup_guard in util.c)
typedef struct { void *(*entry)(void *); void *arg; } ThreadStart;

static void *thread_trampoline(void *p) {
  ThreadStart ts = *(ThreadStart *)p;
  free(p);
  tls_setup_guard();
  return ts.entry(ts.arg);
}

int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
  (void)unused;
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  return pthread_create(thread, NULL, thread_trampoline, ts);
}

// bionic pthread_mutexattr_t is an int; we store the type plainly and read it
// back in pthread_mutex_init_fake (PTHREAD_MUTEX_RECURSIVE == 1 in bionic).
int pthread_mutexattr_init_fake(int *attr) { if (attr) *attr = 0; return 0; }
int pthread_mutexattr_settype_fake(int *attr, int type) { if (attr) *attr = type; return 0; }

// rwlock slots are also lazy void* (the rdlock/wrlock/unlock fakes in
// libc_shim.c create the libnx primitive on first use). init just clears the
// slot; destroy frees the FakeRwLock if one was created.
int pthread_rwlock_init_fake(void **rw, const void *attr) {
  (void)attr;
  if (rw) *rw = NULL;
  return 0;
}
int pthread_rwlock_destroy_fake(void **rw) {
  if (rw && *rw) { free(*rw); *rw = NULL; }
  return 0;
}

int pthread_condattr_setclock_fake(void *attr, int clock_id) { (void)attr; (void)clock_id; return 0; }

int pthread_detach_fake(pthread_t t) { (void)t; return 0; } // libnx reaps on exit
int pthread_equal_fake(unsigned long a, unsigned long b) { return a == b; }
// a stable, unique per-thread token (the thread's TLS base); good enough for
// the engine's "am I on the main thread" comparisons.
unsigned long pthread_self_fake(void) { return (unsigned long)(uintptr_t)armGetTls(); }


/* ------------------------------------------------------------------------- */
/* import table (generated from libAngryBirdsClassic.so's 318 undefined syms) */
/* ------------------------------------------------------------------------- */

DynLibFunction dynlib_functions[] = {
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },
  { "AAsset_getBuffer", (uintptr_t)&AAsset_getBuffer_fake },
  { "AAsset_getLength64", (uintptr_t)&AAsset_getLength64_fake },
  { "__android_log_assert", (uintptr_t)&__android_log_assert_stub },
  { "__android_log_print", (uintptr_t)&__android_log_print_stub },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno },
  { "__google_potentially_blocking_region_begin", (uintptr_t)&noop_stub },
  { "__google_potentially_blocking_region_end", (uintptr_t)&noop_stub },
  { "__sF", (uintptr_t)&fake_sF },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard },
  { "_ctype_", (uintptr_t)&_ctype_ },
  { "abort", (uintptr_t)&abort },
  { "access", (uintptr_t)&access_fake },
  { "acos", (uintptr_t)&acos },
  { "acosf", (uintptr_t)&acosf },
  { "asin", (uintptr_t)&asin },
  { "asinf", (uintptr_t)&asinf },
  { "atan", (uintptr_t)&atan },
  { "atan2", (uintptr_t)&atan2 },
  { "atan2f", (uintptr_t)&atan2f },
  { "atof", (uintptr_t)&atof },
  { "basename", (uintptr_t)&basename },
  { "bind", (uintptr_t)&net_fail_stub },
  { "bsearch", (uintptr_t)&bsearch },
  { "btowc", (uintptr_t)&btowc },
  { "calloc", (uintptr_t)&calloc },
  { "chmod", (uintptr_t)&chmod },
  { "clock", (uintptr_t)&clock },
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "close", (uintptr_t)&close },
  { "closedir", (uintptr_t)&closedir },
  { "connect", (uintptr_t)&net_fail_stub },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "cosh", (uintptr_t)&cosh },
  { "difftime", (uintptr_t)&difftime },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dup", (uintptr_t)&dup },
  { "exit", (uintptr_t)&exit },
  { "exp", (uintptr_t)&exp },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fcntl", (uintptr_t)&fcntl_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "feof", (uintptr_t)&feof_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fgets", (uintptr_t)&fgets_fake },
  { "fileno", (uintptr_t)&fileno },
  { "fmod", (uintptr_t)&fmod },
  { "fmodf", (uintptr_t)&fmodf },
  { "fnmatch", (uintptr_t)&fnmatch_fake },
  { "fopen", (uintptr_t)&fopen_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "free", (uintptr_t)&free },
  { "freopen", (uintptr_t)&freopen },
  { "frexp", (uintptr_t)&frexp },
  { "fseek", (uintptr_t)&fseek_fake },
  { "fseeko", (uintptr_t)&fseeko },
  { "fstat", (uintptr_t)&fstat_fake },
  { "fsync", (uintptr_t)&fsync },
  { "ftell", (uintptr_t)&ftell_fake },
  { "ftello", (uintptr_t)&ftello },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "getc", (uintptr_t)&getc },
  { "getcwd", (uintptr_t)&getcwd_fake },
  { "getenv", (uintptr_t)&getenv_fake },
  { "geteuid", (uintptr_t)&geteuid_fake },
  { "gethostname", (uintptr_t)&net_fail_stub },
  { "getpeername", (uintptr_t)&net_fail_stub },
  { "getpid", (uintptr_t)&getpid },
  { "getpwuid", (uintptr_t)&getpwuid_fake },
  { "getsockname", (uintptr_t)&net_fail_stub },
  { "getsockopt", (uintptr_t)&net_fail_stub },
  { "gettimeofday", (uintptr_t)&gettimeofday_fake },
  { "getwc", (uintptr_t)&getwc },
  { "glActiveTexture", (uintptr_t)&glActiveTexture },
  { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
  { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendEquation", (uintptr_t)&glBlendEquation },
  { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBufferData", (uintptr_t)&glBufferData },
  { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor },
  { "glCompileShader", (uintptr_t)&glCompileShader },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D },
  { "glCreateProgram", (uintptr_t)&glCreateProgram },
  { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glCullFace", (uintptr_t)&glCullFace },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
  { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
  { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask },
  { "glDetachShader", (uintptr_t)&glDetachShader },
  { "glDisable", (uintptr_t)&glDisable },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glDrawElements", (uintptr_t)&glDrawElements },
  { "glEnable", (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFinish", (uintptr_t)&glFinish },
  { "glFlush", (uintptr_t)&glFlush },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
  { "glFrontFace", (uintptr_t)&glFrontFace },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGetActiveUniform", (uintptr_t)&glGetActiveUniform },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
  { "glGetString", (uintptr_t)&glGetString },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glGetUniformfv", (uintptr_t)&glGetUniformfv },
  { "glLinkProgram", (uintptr_t)&glLinkProgram },
  { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
  { "glScissor", (uintptr_t)&glScissor },
  { "glShaderSource", (uintptr_t)&glShaderSource },
  { "glTexImage2D", (uintptr_t)&glTexImage2D },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
  { "glUniform1f", (uintptr_t)&glUniform1f },
  { "glUniform1i", (uintptr_t)&glUniform1i },
  { "glUniform4f", (uintptr_t)&glUniform4f },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
  { "glUseProgram", (uintptr_t)&glUseProgram },
  { "glValidateProgram", (uintptr_t)&glValidateProgram },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
  { "glViewport", (uintptr_t)&glViewport },
  { "gmtime", (uintptr_t)&gmtime },
  { "gmtime_r", (uintptr_t)&gmtime_r },
  { "inet_addr", (uintptr_t)&net_fail_stub },
  { "inet_ntop", (uintptr_t)&net_fail_stub },
  { "inet_pton", (uintptr_t)&net_fail_stub },
  { "ioctl", (uintptr_t)&net_fail_stub },
  { "isalnum", (uintptr_t)&isalnum },
  { "isalpha", (uintptr_t)&isalpha },
  { "iscntrl", (uintptr_t)&iscntrl },
  { "islower", (uintptr_t)&islower },
  { "ispunct", (uintptr_t)&ispunct },
  { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper },
  { "iswctype", (uintptr_t)&iswctype },
  { "isxdigit", (uintptr_t)&isxdigit },
  { "ldexp", (uintptr_t)&ldexp },
  { "localtime", (uintptr_t)&localtime },
  { "log", (uintptr_t)&log },
  { "log10", (uintptr_t)&log10 },
  { "longjmp", (uintptr_t)&longjmp },
  { "lrint", (uintptr_t)&lrint },
  { "lrintf", (uintptr_t)&lrintf },
  { "lseek", (uintptr_t)&lseek },
  { "malloc", (uintptr_t)&malloc },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memrchr", (uintptr_t)&memrchr },
  { "memset", (uintptr_t)&memset },
  { "mkdir", (uintptr_t)&mkdir_fake },
  { "mktime", (uintptr_t)&mktime },
  { "mmap", (uintptr_t)&mmap_fake },
  { "modf", (uintptr_t)&modf },
  { "modff", (uintptr_t)&modff },
  { "munmap", (uintptr_t)&munmap_fake },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "open", (uintptr_t)&open_fake },
  { "opendir", (uintptr_t)&opendir },
  { "pathconf", (uintptr_t)&pathconf },
  { "pipe", (uintptr_t)&net_fail_stub },
  { "poll", (uintptr_t)&net_fail_stub },
  { "pow", (uintptr_t)&pow },
  { "printf", (uintptr_t)&printf },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_detach", (uintptr_t)&pthread_detach_fake },
  { "pthread_equal", (uintptr_t)&pthread_equal_fake },
  { "pthread_getschedparam", (uintptr_t)&pthread_getschedparam_fake },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy_fake },
  { "pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_self", (uintptr_t)&pthread_self_fake },
  { "pthread_setname_np", (uintptr_t)&pthread_setname_np_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "putc", (uintptr_t)&putc },
  { "puts", (uintptr_t)&puts },
  { "putwc", (uintptr_t)&putwc },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "read", (uintptr_t)&read },
  { "readdir_r", (uintptr_t)&readdir_r },
  { "realloc", (uintptr_t)&realloc },
  { "recv", (uintptr_t)&net_fail_stub },
  { "recvfrom", (uintptr_t)&net_fail_stub },
  { "remove", (uintptr_t)&remove_fake },
  { "rename", (uintptr_t)&rename_fake },
  { "rmdir", (uintptr_t)&rmdir },
  { "sched_yield", (uintptr_t)&sched_yield },
  { "send", (uintptr_t)&net_fail_stub },
  { "setjmp", (uintptr_t)&setjmp },
  { "setlocale", (uintptr_t)&setlocale_fake },
  { "setsockopt", (uintptr_t)&net_fail_stub },
  { "setvbuf", (uintptr_t)&setvbuf },
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "sin", (uintptr_t)&sin },
  { "sinf", (uintptr_t)&sinf },
  { "sinh", (uintptr_t)&sinh },
  { "snprintf", (uintptr_t)&snprintf },
  { "socket", (uintptr_t)&net_fail_stub },
  { "sprintf", (uintptr_t)&sprintf },
  { "sqrt", (uintptr_t)&sqrt },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "srand", (uintptr_t)&srand },
  { "sscanf", (uintptr_t)&sscanf },
  { "stat", (uintptr_t)&stat_fake },
  { "statfs", (uintptr_t)&statfs_fake },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcoll", (uintptr_t)&strcoll },
  { "strcpy", (uintptr_t)&strcpy },
  { "strcspn", (uintptr_t)&strcspn },
  { "strdup", (uintptr_t)&strdup },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r },
  { "strftime", (uintptr_t)&strftime },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncat", (uintptr_t)&strncat },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtok_r", (uintptr_t)&strtok_r },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "syscall", (uintptr_t)&syscall_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "tan", (uintptr_t)&tan },
  { "tanf", (uintptr_t)&tanf },
  { "tanh", (uintptr_t)&tanh },
  { "time", (uintptr_t)&time },
  { "tmpfile", (uintptr_t)&tmpfile },
  { "tolower", (uintptr_t)&tolower },
  { "toupper", (uintptr_t)&toupper },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "ungetwc", (uintptr_t)&ungetwc },
  { "unlink", (uintptr_t)&unlink_fake },
  { "utime", (uintptr_t)&utime_fake },
  { "vprintf", (uintptr_t)&vprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcsftime", (uintptr_t)&wcsftime },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wctob", (uintptr_t)&wctob },
  { "wctype", (uintptr_t)&wctype },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
  { "write", (uintptr_t)&write },
  { "writev", (uintptr_t)&writev_fake },};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void update_imports(void) {
  /* no runtime hook swaps needed for the GLES2 path */
}
