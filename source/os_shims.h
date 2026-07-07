/* os_shims.h -- general Bionic/NDK OS emulation for the Fusion engine
 *
 * These are the engine-agnostic OS gaps (mmap-over-malloc, a dynamic-loader
 * stub, Bionic->newlib flag conversion, and small libc holes) that Horizon
 * doesn't provide. Derived from the Burger Shop port's shim layer with its
 * OpenSL-specific pieces removed (Fusion mixes its own audio).
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#ifndef __OS_SHIMS_H__
#define __OS_SHIMS_H__

#include <stddef.h>
#include <sys/types.h>

// devkitA64's newlib ships no <sys/uio.h>; define the (ABI-stable) iovec here.
// Matches Bionic's layout, which is what the game's writev passes.
#ifndef IOVEC_DEFINED
#define IOVEC_DEFINED
struct iovec { void *iov_base; size_t iov_len; };
#endif

// mmap emulation
void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap_fake(void *addr, size_t length);
int   mlock_fake(const void *addr, size_t len);
int   munlock_fake(const void *addr, size_t len);

// dynamic linker surface (no real dlopen on Horizon)
void *dlopen_fake(const char *filename, int flag);
void *dlsym_fake(void *handle, const char *symbol);
int   dlclose_fake(void *handle);
int   dladdr_fake(const void *addr, void *info);

// offline network stubs
int  socket_fake(int domain, int type, int protocol);
int  connect_fake(int fd, const void *addr, unsigned len);
int  setsockopt_fake(int fd, int level, int optname, const void *optval, unsigned optlen);
int  shutdown_fake(int fd, int how);
int  poll_fake(void *fds, unsigned long nfds, int timeout);
ssize_t writev_fake(int fd, const struct iovec *iov, int iovcnt);

// small libc gaps
void *getenv_fake(const char *name);
int   system_fake(const char *command);
int   fcntl_fake(int fd, int cmd, ...);
int   getpriority_fake(int which, int who);
int   setpriority_fake(int which, int who, int prio);
char *getcwd_fake(char *buf, size_t size);
int   chdir_fake(const char *path);
int   mkdir_fake(const char *path, unsigned int mode);
int   readlink_fake(const char *path, char *buf, size_t bufsiz);
int   utime_fake(const char *path, const void *times);
char *strcasestr_fake(const char *haystack, const char *needle);

// symbols Angry Birds imports that this newlib doesn't provide
char *setlocale_fake(int category, const char *locale);
int   statfs_fake(const char *path, void *buf);
int   fnmatch_fake(const char *pattern, const char *string, int flags);
void *getpwuid_fake(unsigned uid);
int   pthread_getschedparam_fake(void *thread, int *policy, void *param);
int   pthread_setschedparam_fake(void *thread, int policy, const void *param);

// symbols devkitA64's newlib declares but does not provide
int   geteuid_fake(void);
int   pthread_setname_np_fake(void *thread, const char *name);
int   sigaction_fake(int signum, const void *act, void *oldact);

#endif
