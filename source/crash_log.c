/* crash_log.c -- crash reporting for diagnosing engine crashes.
 *
 * 1. CPU exceptions (segfaults etc.): libnx calls __libnx_exception_handler on
 *    __nx_exception_stack. We write the exception type, registers and a
 *    backtrace -- every code address shown as an offset into
 *    libAngryBirdsClassic.so or the .nro, so it can be looked up in a
 *    disassembler / addr2line -- a stack hex dump, and the last assets the
 *    engine opened. It then re-raises with svcBreak, so the process still
 *    terminates exactly as before (Atmosphere's own crash report included).
 * 2. abort() / exit() called by the engine (e.g. an uncaught C++ exception or a
 *    fatal script error): logged with the caller's .so offset and a backtrace,
 *    then passed on unchanged.
 *
 * The report goes to sdmc:/switch/angrybirds/angrybirds_nx_crash.txt (always,
 * even with DEBUG_LOG 0) and is echoed into angrybirds_nx.log. The crash file
 * is written with libnx's fs calls, which don't allocate, because after a
 * crash the heap may be corrupt.
 *
 * The report is also sent to svcOutputDebugString, which emulators show in
 * their own log window. Note: emulators generally don't deliver CPU exceptions
 * to homebrew, so there part 1 may never run; the debug log and part 2 still work.
 *
 * Guarded multi-block memory probing, the stack hex dump, the debug-string
 * output and the explicit svcBreak(BreakReason_Panic) follow the battd_nx
 * port's nx_crash_handler.c.
 */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdalign.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "crash_log.h"

extern so_module game_mod;

/* libnx's default exception stack is 0x400 bytes: too small for snprintf. */
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

/* ---- recent asset opens (ring buffer; races are harmless for diagnostics) --- */
#define NOTE_N   12
#define NOTE_LEN 160
static char s_notes[NOTE_N][NOTE_LEN];
static volatile unsigned s_note_pos;

void crash_log_note(const char *what) {
  if (!what) return;
  unsigned i = s_note_pos++ % NOTE_N;
  size_t n = strlen(what);
  if (n >= NOTE_LEN) { what += n - (NOTE_LEN - 1); n = NOTE_LEN - 1; }   /* keep the tail */
  memcpy(s_notes[i], what, n);
  s_notes[i][n] = 0;
}

/* ---- report buffer (static: no malloc after a crash) ---- */
static char s_rep[48 * 1024];
static size_t s_len;

static void rep(const char *fmt, ...) {
  if (s_len >= sizeof s_rep - 1) return;
  va_list va; va_start(va, fmt);
  int n = vsnprintf(s_rep + s_len, sizeof s_rep - s_len, fmt, va);
  va_end(va);
  if (n > 0) s_len += (size_t)n < sizeof s_rep - s_len ? (size_t)n : sizeof s_rep - s_len - 1;
}

/* ---- address classification ---- */
static int query(u64 addr, MemoryInfo *mi) {
  u32 pi;
  return R_SUCCEEDED(svcQueryMemory(mi, &pi, addr));
}

/* Is addr inside the game .so's loaded image? */
static int in_so(u64 a) {
  u64 b = (u64)(uintptr_t)game_mod.load_virtbase;
  return b && a >= b && a < b + game_mod.load_size;
}

/* Is addr inside the .nro's code? (the executable region holding this function) */
static int in_nro(u64 a, u64 *base) {
  MemoryInfo self, mi;
  if (!query((u64)(uintptr_t)&crash_log_note, &self)) return 0;
  if (!query(a, &mi)) return 0;
  if (mi.addr != self.addr) return 0;
  *base = self.addr;
  return 1;
}

static void rep_addr(const char *label, u64 a) {
  u64 nb;
  if (in_so(a))
    rep("%-6s %016lx  %s+0x%lx\n", label, a, SO_NAME, a - (u64)(uintptr_t)game_mod.load_virtbase);
  else if (in_nro(a, &nb))
    rep("%-6s %016lx  angrybirds_nx.nro+0x%lx\n", label, a, a - nb);
  else
    rep("%-6s %016lx\n", label, a);
}

/* Every byte of [a, a+len) must be mapped and readable; the range may span
 * several memory blocks. Guards every read the handler makes. */
static int readable(u64 a, u64 len) {
  if (a < 0x1000) return 0;
  u64 end = a + len;
  if (end < a) return 0;                           /* overflow */
  while (a < end) {
    MemoryInfo mi;
    if (!query(a, &mi)) return 0;
    if (mi.type == MemType_Unmapped || !(mi.perm & Perm_R)) return 0;
    u64 block_end = mi.addr + mi.size;
    if (block_end <= a) return 0;                  /* no forward progress */
    a = block_end;
  }
  return 1;
}

static void rep_stack_dump(u64 sp) {
  sp &= ~7ull;
  if (!readable(sp, 0x200)) { rep("\nstack @ %016lx: unreadable\n", sp); return; }
  rep("\nstack @ %016lx:\n", sp);
  for (u64 off = 0; off < 0x200; off += 0x20) {
    const u64 *q = (const u64 *)(uintptr_t)(sp + off);
    rep("  +%03lx: %016lx %016lx %016lx %016lx\n", off, q[0], q[1], q[2], q[3]);
  }
}

/* Frame-pointer chain: each AArch64 frame record is {prev fp, return lr}. */
static void rep_backtrace(u64 fp, u64 sp) {
  rep("\nbacktrace (frame pointers):\n");
  for (int i = 0; i < 48 && fp; i++) {
    if ((fp & 7) || !readable(fp, 16)) { rep("  (chain ends at fp=%016lx)\n", fp); break; }
    u64 next = ((u64 *)(uintptr_t)fp)[0];
    u64 ret  = ((u64 *)(uintptr_t)fp)[1];
    char label[16]; snprintf(label, sizeof label, "  #%02d", i);
    rep_addr(label, ret);
    if (next <= fp) break;                         /* stacks grow down: callers are higher */
    fp = next;
  }

  /* Fallback when frame pointers are missing: words on the stack that point just
   * after a BL/BLR in the .so are very likely return addresses. */
  rep("\nlikely return addresses found on the stack:\n");
  int hits = 0;
  for (u64 p = sp & ~7ull; hits < 24 && p < (sp & ~7ull) + 0x4000; p += 8) {
    if (!readable(p, 8)) break;
    u64 v = *(u64 *)(uintptr_t)p;
    if (!in_so(v) || (v & 3) || !in_so(v - 4) || !readable(v - 4, 4)) continue;
    u32 ins = *(u32 *)(uintptr_t)(v - 4);
    int is_bl  = (ins & 0xFC000000u) == 0x94000000u;
    int is_blr = (ins & 0xFFFFFC1Fu) == 0xD63F0000u;
    if (!is_bl && !is_blr) continue;
    char label[16]; snprintf(label, sizeof label, "  sp+%03lx", (unsigned long)(p - sp));
    rep_addr(label, v);
    hits++;
  }
  if (!hits) rep("  (none)\n");
}

static void rep_notes(void) {
  rep("\nlast assets/files opened (oldest first):\n");
  unsigned end = s_note_pos;
  unsigned start = end > NOTE_N ? end - NOTE_N : 0;
  for (unsigned i = start; i < end; i++) rep("  %s\n", s_notes[i % NOTE_N]);
}

/* Write the report to the crash file without allocating, then echo to the log. */
static void flush_report(void) {
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  if (fs) {
    static const char path[] = "/switch/angrybirds/angrybirds_nx_crash.txt";
    fsFsDeleteFile(fs, path);
    if (R_SUCCEEDED(fsFsCreateFile(fs, path, 0, 0))) {
      FsFile f;
      if (R_SUCCEEDED(fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, &f))) {
        fsFileWrite(&f, 0, s_rep, s_len, FsWriteOption_Flush);
        fsFileClose(&f);
      }
    }
  }
  /* emulators print this in their log window */
  for (size_t off = 0; off < s_len; ) {
    size_t n = s_len - off > 512 ? 512 : s_len - off;
    svcOutputDebugString(s_rep + off, n);
    off += n;
  }
  debugPrintf("%s", s_rep);
}

static const char *desc_name(u32 d) {
  switch (d) {
    case ThreadExceptionDesc_InstructionAbort: return "instruction abort (jumped to bad address)";
    case ThreadExceptionDesc_MisalignedPC:     return "misaligned PC";
    case ThreadExceptionDesc_MisalignedSP:     return "misaligned SP";
    case ThreadExceptionDesc_SError:           return "SError";
    case ThreadExceptionDesc_BadSVC:           return "bad SVC";
    case ThreadExceptionDesc_Trap:             return "trap / undefined instruction";
    case ThreadExceptionDesc_Other:            return "data abort (bad memory access) or other";
    default:                                   return "unknown";
  }
}

/* ---- 1. CPU exceptions ---- */
void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  s_len = 0;
  rep("===== CRASH: %s (desc 0x%x, esr 0x%08x) =====\n", desc_name(ctx->error_desc),
      ctx->error_desc, ctx->esr);
  rep("%s loaded at %016lx, size 0x%lx\n", SO_NAME,
      (u64)(uintptr_t)game_mod.load_virtbase, (u64)game_mod.load_size);
  rep_addr("pc", ctx->pc.x);
  rep_addr("lr", ctx->lr.x);
  rep("%-6s %016lx   (fault address)\n", "far", ctx->far.x);
  rep("%-6s %016lx\n%-6s %016lx\n", "sp", ctx->sp.x, "fp", ctx->fp.x);
  for (int i = 0; i < 29; i++)
    rep("x%-2d %016lx%s", i, ctx->cpu_gprs[i].x, (i % 3 == 2) ? "\n" : "   ");
  rep("\n");
  rep_backtrace(ctx->fp.x, ctx->sp.x);
  rep_stack_dump(ctx->sp.x);
  rep_notes();
  rep("===== end of crash report =====\n");
  flush_report();
  /* re-raise so the process aborts as it would have (and Atmosphere writes its
   * own crash report); the loop only runs if svcBreak somehow returns */
  svcBreak(BreakReason_Panic, 0, 0);
  for (;;) svcSleepThread(1000000000ull);
}

/* ---- 2. abort() / exit() from the engine ---- */
void crash_log_abort(void) {
  u64 caller = (u64)(uintptr_t)__builtin_return_address(0);
  s_len = 0;
  rep("===== ENGINE CALLED abort() =====\n");
  rep_addr("caller", caller);
  rep_backtrace((u64)(uintptr_t)__builtin_frame_address(0), (u64)(uintptr_t)__builtin_frame_address(0));
  rep_notes();
  rep("===== end of crash report =====\n");
  flush_report();
  abort();
}

void crash_log_exit(int code) {
  u64 caller = (u64)(uintptr_t)__builtin_return_address(0);
  if (code == 0) {                                 /* normal quit: one log line, no crash file */
    s_len = 0;
    rep_addr("engine called exit(0) from", caller);
    debugPrintf("%s", s_rep);
    exit(0);
  }
  s_len = 0;
  rep("===== ENGINE CALLED exit(%d) =====\n", code);
  rep_addr("caller", caller);
  rep_backtrace((u64)(uintptr_t)__builtin_frame_address(0), (u64)(uintptr_t)__builtin_frame_address(0));
  rep_notes();
  rep("===== end of report =====\n");
  flush_report();
  exit(code);
}
