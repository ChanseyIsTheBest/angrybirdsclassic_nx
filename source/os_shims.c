/* os_shims.c -- general Bionic/NDK OS emulation for the Fusion engine. See
 * os_shims.h. Everything here is deliberately small and defensive: the goal is
 * to let libAngryBirdsClassic.so load and run on Switch even though Horizon
 * offers no Linux mmap, no dynamic loader for Android .so names, and no sockets
 * the way Android exposes them. The game is fully offline, so the network
 * surface is stubbed and mmap is emulated over aligned malloc + seek-read.
 *
 * Derived from the Burger Shop port's shim layer (MIT), with its OpenSL-specific
 * pieces removed -- Fusion mixes its own audio, so dlopen/dlsym never need to
 * resolve OpenSL here.
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
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/stat.h>
#include <switch.h>

#include "util.h"
#include "so_util.h"
#include "libc_shim.h"
#include "fusion.h"
#include "os_shims.h"

// Bionic/Linux mmap constants (independent of the host newlib values)
#define BIONIC_MAP_FIXED   0x10
#define BIONIC_MAP_ANON    0x20
#define BIONIC_MAP_FAILED  ((void *)-1)

// ---------------------------------------------------------------------------
// mmap/munmap emulation
//
// Horizon has no demand-paged file mapping, so we satisfy every request with a
// heap allocation. Anonymous maps are just zeroed memory; file-backed maps are
// filled once via seek+read at the requested offset. A small table remembers
// each base pointer so munmap can free it.
// ---------------------------------------------------------------------------

typedef struct { void *base; size_t len; } MmapRec;

static MmapRec *g_maps = NULL;
static size_t g_maps_len = 0, g_maps_cap = 0;
static Mutex g_maps_mtx;
static bool g_maps_mtx_init = false;

static void maps_lock(void) {
  if (!g_maps_mtx_init) { mutexInit(&g_maps_mtx); g_maps_mtx_init = true; }
  mutexLock(&g_maps_mtx);
}
static void maps_unlock(void) { mutexUnlock(&g_maps_mtx); }

static void maps_remember(void *base, size_t len) {
  maps_lock();
  if (g_maps_len == g_maps_cap) {
    size_t ncap = g_maps_cap ? g_maps_cap * 2 : 16;
    MmapRec *n = realloc(g_maps, ncap * sizeof(*n));
    if (n) { g_maps = n; g_maps_cap = ncap; }
  }
  if (g_maps_len < g_maps_cap) {
    g_maps[g_maps_len].base = base;
    g_maps[g_maps_len].len = len;
    g_maps_len++;
  }
  maps_unlock();
}

static int maps_forget(void *base) {
  int found = 0;
  maps_lock();
  for (size_t i = 0; i < g_maps_len; i++) {
    if (g_maps[i].base == base) {
      g_maps[i] = g_maps[g_maps_len - 1];
      g_maps_len--; found = 1; break;
    }
  }
  maps_unlock();
  return found;
}

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  (void)addr; (void)prot;
  if (length == 0) return BIONIC_MAP_FAILED;

  void *mem = NULL;
  if (posix_memalign_fake(&mem, 64, length) != 0 || !mem)
    return BIONIC_MAP_FAILED;
  memset(mem, 0, length);

  if (!(flags & BIONIC_MAP_ANON) && fd >= 0) {
    const off_t saved = lseek(fd, 0, SEEK_CUR);
    if (lseek(fd, offset, SEEK_SET) == offset) {
      ssize_t got = read(fd, mem, length);
      if (got < 0)
        debugPrintf("mmap: read(fd=%d,len=%zu,off=%lld) failed\n",
                    fd, length, (long long)offset);
    }
    if (saved >= 0) lseek(fd, saved, SEEK_SET);
  }

  maps_remember(mem, length);
  return mem;
}

int munmap_fake(void *addr, size_t length) {
  (void)length;
  if (addr && addr != BIONIC_MAP_FAILED && maps_forget(addr))
    free(addr);
  return 0;
}

int mlock_fake(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
int munlock_fake(const void *addr, size_t len) { (void)addr; (void)len; return 0; }

// ---------------------------------------------------------------------------
// dynamic linker surface
//
// Fusion has no OpenSL/codec dlopen path (it mixes audio itself), so dlopen of
// any named library returns NULL, which callers treat as "feature unavailable"
// and skip. dlopen(NULL) yields a self handle; unknown symbols fall back to a
// real export from an already-loaded module.
// ---------------------------------------------------------------------------

static int g_self_handle;

void *dlopen_fake(const char *filename, int flag) {
  (void)flag;
  if (!filename) return &g_self_handle; // dlopen(NULL) -> "this program"
  debugPrintf("dlopen(%s) -> unavailable (stubbed)\n", filename);
  return NULL;
}

void *dlsym_fake(void *handle, const char *symbol) {
  (void)handle;
  if (!symbol) return NULL;
  uintptr_t a = so_find_addr_in_loaded(symbol);
  if (a) return (void *)a;
  debugPrintf("dlsym(%s) -> NULL\n", symbol);
  return NULL;
}

int dlclose_fake(void *handle) { (void)handle; return 0; }
int dladdr_fake(const void *addr, void *info) { (void)addr; (void)info; return 0; }

// ---------------------------------------------------------------------------
// offline network stubs -- the game never needs the network on Switch
// ---------------------------------------------------------------------------

int socket_fake(int d, int t, int p) { (void)d; (void)t; (void)p; errno = EAFNOSUPPORT; return -1; }
int connect_fake(int fd, const void *a, unsigned l) { (void)fd; (void)a; (void)l; errno = ENETUNREACH; return -1; }
int setsockopt_fake(int fd, int lv, int on, const void *ov, unsigned ol) { (void)fd;(void)lv;(void)on;(void)ov;(void)ol; return 0; }
int shutdown_fake(int fd, int how) { (void)fd; (void)how; return 0; }
int poll_fake(void *fds, unsigned long nfds, int timeout) {
  (void)fds; (void)nfds;
  if (timeout > 0) svcSleepThread((int64_t)timeout * 1000000LL);
  return 0;
}

// writev: general scatter-gather write. Fusion uses it to flush save files (the
// registry, highscores, ...). It was wrongly stubbed as a network call that
// returned ENOSYS, which silently broke every save. Do the real writes.
ssize_t writev_fake(int fd, const struct iovec *iov, int iovcnt) {
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) continue;
    ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
    if (n < 0) return total > 0 ? total : -1;
    total += n;
    if ((size_t)n < iov[i].iov_len) break; // short write: stop, report progress
  }
  return total;
}

// ---------------------------------------------------------------------------
// small libc gaps
// ---------------------------------------------------------------------------

void *getenv_fake(const char *name) {
  if (name) {
    // Some engines/Lua read the UI locale from the POSIX locale env vars.
    if (!strcmp(name, "LANG") || !strcmp(name, "LANGUAGE") ||
        !strcmp(name, "LC_ALL") || !strcmp(name, "LC_MESSAGES") ||
        !strcmp(name, "LC_CTYPE")) {
      const char *loc = fusion_locale();      // e.g. "fr_FR"
      debugPrintf("getenv(%s) -> %s\n", name, loc);
      return (void *)loc;
    }
    debugPrintf("getenv(%s) -> NULL\n", name);
  }
  return NULL;
}
int   system_fake(const char *command) { (void)command; return -1; }

int fcntl_fake(int fd, int cmd, ...) {
  va_list ap; va_start(ap, cmd);
  int ret = 0;
  switch (cmd) {
    case F_DUPFD: ret = dup(fd); break;
    case F_GETFL: ret = O_RDWR; break;
    case F_GETFD:
    case F_SETFD:
    case F_SETFL: ret = 0; break;
    default:      ret = 0; break;
  }
  va_end(ap);
  return ret;
}

int getpriority_fake(int which, int who) { (void)which; (void)who; return 0; }
int setpriority_fake(int which, int who, int prio) { (void)which; (void)who; (void)prio; return 0; }

char *getcwd_fake(char *buf, size_t size) {
  // The engine gets its real paths from the asset roots; cwd only backs a few
  // relative fallbacks, so root is a safe answer.
  if (!buf || size == 0) return NULL;
  buf[0] = '/';
  if (size > 1) buf[1] = '\0'; else buf[0] = '\0';
  return buf;
}

int chdir_fake(const char *path) { (void)path; return 0; }

int mkdir_fake(const char *path, unsigned int mode) {
  if (mkdir(path, mode) == 0) return 0;
  if (errno == EEXIST) return 0; // "already exists" is success for save-dir creation
  return -1;
}

int readlink_fake(const char *path, char *buf, size_t bufsiz) {
  (void)path; (void)buf; (void)bufsiz; errno = EINVAL; return -1;
}

int utime_fake(const char *path, const void *times) {
  (void)path; (void)times; return 0; // timestamps are cosmetic here
}

char *strcasestr_fake(const char *haystack, const char *needle) {
  if (!*needle) return (char *)haystack;
  for (; *haystack; haystack++) {
    const char *h = haystack, *n = needle;
    while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
    if (!*n) return (char *)haystack;
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// symbols Angry Birds imports that this newlib doesn't provide
// ---------------------------------------------------------------------------

char *setlocale_fake(int category, const char *locale) {
  debugPrintf("setlocale(cat=%d, %s) -> C\n", category, locale ? locale : "(query)");
  (void)category; (void)locale;
  return (char *)"C"; // the engine only checks for a non-NULL locale
}

// Bionic arm64 struct statfs field layout (all uint64_t up to what matters).
// We report a large, writable volume so the game's free-space checks pass.
struct bionic_statfs {
  uint64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail;
  uint64_t f_files, f_ffree;
  uint64_t f_fsid;
  uint64_t f_namelen, f_frsize, f_flags, f_spare[4];
};

int statfs_fake(const char *path, void *buf) {
  (void)path;
  if (!buf) { errno = EFAULT; return -1; }
  struct bionic_statfs *s = (struct bionic_statfs *)buf;
  memset(s, 0, sizeof(*s));
  s->f_bsize  = 4096;
  s->f_frsize = 4096;
  s->f_blocks = 4u * 1024u * 1024u;   // ~16 GB total
  s->f_bfree  = 3u * 1024u * 1024u;   // ~12 GB free
  s->f_bavail = 3u * 1024u * 1024u;
  s->f_namelen = 255;
  return 0;
}

// minimal fnmatch supporting '*' and '?' (no bracket classes); returns 0 on
// match, 1 (FNM_NOMATCH) otherwise -- enough for the engine's asset globbing.
int fnmatch_fake(const char *pattern, const char *string, int flags) {
  (void)flags;
  const char *p = pattern, *s = string;
  const char *star = NULL, *ss = NULL;
  while (*s) {
    if (*p == '?' || *p == *s) { p++; s++; }
    else if (*p == '*') { star = p++; ss = s; }
    else if (star) { p = star + 1; s = ++ss; }
    else return 1;
  }
  while (*p == '*') p++;
  return *p ? 1 : 0;
}

void *getpwuid_fake(unsigned uid) { (void)uid; return NULL; }

int pthread_getschedparam_fake(void *thread, int *policy, void *param) {
  (void)thread;
  if (policy) *policy = SCHED_OTHER;
  if (param) ((struct sched_param *)param)->sched_priority = 0;
  return 0;
}
int pthread_setschedparam_fake(void *thread, int policy, const void *param) {
  (void)thread; (void)policy; (void)param; return 0;
}

// devkitA64's newlib declares these (so callers compile) but ships no
// implementation, so the engine's references would fail to link. Horizon has no
// users/POSIX signals/thread names, so inert stubs are the correct behaviour.
int geteuid_fake(void) { return 0; }
int pthread_setname_np_fake(void *thread, const char *name) { (void)thread; (void)name; return 0; }
int sigaction_fake(int signum, const void *act, void *oldact) {
  (void)signum; (void)act; (void)oldact; return 0; // pretend the handler installed
}
