#ifndef CRASH_LOG_H
#define CRASH_LOG_H
// Crash reporting (see crash_log.c). The CPU exception handler installs itself
// (libnx __libnx_exception_handler); these are the other entry points.

// Remember a recently opened asset/file; the last few appear in crash reports.
void crash_log_note(const char *what);

// Replacements for the engine's abort()/exit() imports: log the caller (as a
// .so offset) and a backtrace, then abort()/exit() as normal.
void crash_log_abort(void);
void crash_log_exit(int code);
#endif
