/* powerup_patch.c -- optional "powerups" cheat from config.txt.
 *
 * At boot, before the engine starts, top up every power-up in the player's save
 * (<game dir>/settings.lua) so each one has at least N available, where N is
 * config "powerups": "max" = 9999 (the in-game counter shows min(count, 9999)),
 * or a number 1-9999. "off" (default) never touches the save.
 *
 * The save is Lua source, AES-256-CBC encrypted (zero IV, PKCS#7 padding) with
 * Rovio's classic save key. Power-ups live in
 *
 *   installationSpecificInventory = {
 *       kingsling = {
 *           totalCount = 19,
 *           usedCount = 0,
 *       },
 *       ...
 *   }
 *
 * and the game shows totalCount - usedCount. We parse the file with a small
 * parser for the Lua table syntax the game writes, then change only the
 * totalCount numbers (usedCount is kept, so the game's own stats stay right)
 * and, for the four core power-ups the game hands out from the start, add a
 * missing entry in the game's own format. Anything the parser doesn't
 * recognise leaves the save untouched. Counts already above the target are
 * never lowered. The original save is backed up once to settings.lua.bak.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>

#include "config.h"
#include "util.h"
#include "libc_shim.h"
#include "powerup_patch.h"
#include "aes.h"

#define POWERUP_MAX 9999        /* GameHud draws math.min(count, 9999) */
#define PATHMAX 512

/* ========================================================================= *
 * Save text editing (pure; no I/O)
 * ========================================================================= */

enum { T_EOF, T_NAME, T_NUMBER, T_STRING, T_PUNCT, T_ERROR };

typedef struct { int type; size_t a, b; char ch; } Tok;      /* span [a,b) */

typedef struct {
  const char *s; size_t n, pos;
  Tok tok;                                                  /* current token */
  int err;
} Lex;

static int is_name0(int c) { return isalpha(c) || c == '_'; }
static int is_name1(int c) { return isalnum(c) || c == '_'; }

/* Long bracket [[...]] / [==[...]==] starting at p (at '['); returns index
 * just past the closing bracket, or 0 if p isn't a long bracket / unterminated. */
static size_t long_bracket(const char *s, size_t n, size_t p) {
  size_t q = p + 1, lvl = 0;
  while (q < n && s[q] == '=') { lvl++; q++; }
  if (q >= n || s[q] != '[') return 0;
  for (q++; q < n; q++) {
    if (s[q] != ']') continue;
    size_t r = q + 1, l = 0;
    while (r < n && s[r] == '=') { l++; r++; }
    if (l == lvl && r < n && s[r] == ']') return r + 1;
  }
  return 0;
}

static void lex_next(Lex *L) {
  const char *s = L->s; size_t n = L->n, p = L->pos;
  for (;;) {                                                /* whitespace, comments */
    while (p < n && isspace((unsigned char)s[p])) p++;
    if (p + 1 < n && s[p] == '-' && s[p + 1] == '-') {
      if (p + 2 < n && s[p + 2] == '[') {
        size_t e = long_bracket(s, n, p + 2);
        if (e) { p = e; continue; }
      }
      while (p < n && s[p] != '\n') p++;
      continue;
    }
    break;
  }
  Tok t = { T_EOF, p, p, 0 };
  if (p >= n) { L->tok = t; L->pos = p; return; }
  unsigned char c = (unsigned char)s[p];
  if (is_name0(c)) {
    size_t q = p; while (q < n && is_name1((unsigned char)s[q])) q++;
    t.type = T_NAME; t.b = q;
  } else if (isdigit(c) || (c == '.' && p + 1 < n && isdigit((unsigned char)s[p + 1])) ||
             (c == '-' && p + 1 < n && (isdigit((unsigned char)s[p + 1]) || s[p + 1] == '.'))) {
    size_t q = p + 1;
    while (q < n && (isalnum((unsigned char)s[q]) || s[q] == '.' ||
                     ((s[q] == '+' || s[q] == '-') && (s[q - 1] == 'e' || s[q - 1] == 'E'))))
      q++;
    t.type = T_NUMBER; t.b = q;
  } else if (c == '"' || c == '\'') {
    size_t q = p + 1;
    while (q < n && s[q] != (char)c && s[q] != '\n') q += (s[q] == '\\' && q + 1 < n) ? 2 : 1;
    if (q >= n || s[q] != (char)c) { t.type = T_ERROR; }
    else { t.type = T_STRING; t.b = q + 1; }
  } else if (c == '[' && p + 1 < n && (s[p + 1] == '[' || s[p + 1] == '=')) {
    size_t e = long_bracket(s, n, p);
    if (e) { t.type = T_STRING; t.b = e; } else t.type = T_ERROR;
  } else if (strchr("{}[]=,;", c)) {
    t.type = T_PUNCT; t.ch = (char)c; t.b = p + 1;
  } else {
    t.type = T_ERROR;
  }
  if (t.type == T_ERROR) L->err = 1;
  L->tok = t; L->pos = t.b;
}

static int tok_is(const Lex *L, char ch) { return L->tok.type == T_PUNCT && L->tok.ch == ch; }
static int tok_name_eq(const Lex *L, const char *w) {
  size_t l = L->tok.b - L->tok.a;
  return L->tok.type == T_NAME && strlen(w) == l && memcmp(L->s + L->tok.a, w, l) == 0;
}

/* ---- what we collect from installationSpecificInventory ---- */
#define MAX_ENTRIES 64
typedef struct {
  char key[48];
  size_t open;                        /* index of the entry's '{' */
  int has_total, has_used;
  double total, used;
  size_t total_a, total_b;            /* span of the totalCount number */
} Entry;

typedef struct {
  int found;                          /* saw a top-level installationSpecificInventory table */
  size_t close;                       /* index of its closing '}' */
  int needs_sep;                      /* last field lacks a trailing ',' / ';' */
  int n; Entry e[MAX_ENTRIES];
  int overflow;
} Inv;

enum { ROLE_NONE, ROLE_INV, ROLE_ENTRY };

static void parse_value(Lex *L, int depth, int role, Inv *inv, Entry *ent,
                        const char *key, size_t keylen);

/* Current token is '{'. Parses to the matching '}' (consumed). */
static void parse_table(Lex *L, int depth, int role, Inv *inv, Entry *ent) {
  if (depth > 64) { L->err = 1; return; }
  lex_next(L);                                              /* past '{' */
  int fields = 0, last_sep = 1;
  while (!L->err && !tok_is(L, '}')) {
    if (L->tok.type == T_EOF) { L->err = 1; return; }
    const char *key = NULL; size_t keylen = 0;
    if (tok_is(L, '[')) {                                   /* [expr] = value */
      lex_next(L);
      if (L->tok.type == T_STRING && L->s[L->tok.a] != '[') {
        key = L->s + L->tok.a + 1; keylen = L->tok.b - L->tok.a - 2;   /* unquoted */
      }
      parse_value(L, depth + 1, ROLE_NONE, NULL, NULL, NULL, 0);
      if (L->err || !tok_is(L, ']')) { L->err = 1; return; }
      lex_next(L);
      if (!tok_is(L, '=')) { L->err = 1; return; }
      lex_next(L);
    } else if (L->tok.type == T_NAME) {                     /* name = value, or a bare value */
      Lex save = *L;
      size_t na = L->tok.a, nb = L->tok.b;
      lex_next(L);
      if (tok_is(L, '=')) { key = L->s + na; keylen = nb - na; lex_next(L); }
      else *L = save;                                       /* positional true/false/nil */
    }
    parse_value(L, depth + 1, role, inv, ent, key, keylen);
    if (L->err) return;
    fields++;
    last_sep = 0;
    if (tok_is(L, ',') || tok_is(L, ';')) { last_sep = 1; lex_next(L); }
    else if (!tok_is(L, '}')) { L->err = 1; return; }
  }
  if (L->err) return;
  if (role == ROLE_INV && inv) { inv->close = L->tok.a; inv->needs_sep = fields > 0 && !last_sep; }
  lex_next(L);                                              /* past '}' */
}

/* Parses one value at the current token. `role` is the role of the table this
 * value sits in; `key` is its field name (if any). */
static void parse_value(Lex *L, int depth, int role, Inv *inv, Entry *ent,
                        const char *key, size_t keylen) {
  if (tok_is(L, '{')) {
    if (role == ROLE_INV && inv) {                          /* one power-up entry */
      Entry *e = NULL;
      if (key && keylen < sizeof inv->e[0].key) {
        if (inv->n < MAX_ENTRIES) {
          e = &inv->e[inv->n++];
          memset(e, 0, sizeof *e);
          memcpy(e->key, key, keylen); e->key[keylen] = 0;
          e->open = L->tok.a;
        } else inv->overflow = 1;
      }
      parse_table(L, depth, e ? ROLE_ENTRY : ROLE_NONE, inv, e);
    } else {
      parse_table(L, depth, ROLE_NONE, NULL, NULL);
    }
    return;
  }
  if (L->tok.type == T_NUMBER) {
    if (role == ROLE_ENTRY && ent && key) {
      char buf[64]; size_t l = L->tok.b - L->tok.a;
      if (l < sizeof buf) {
        memcpy(buf, L->s + L->tok.a, l); buf[l] = 0;
        char *end; double v = strtod(buf, &end);
        if (*end == 0 && isfinite(v)) {
          if (keylen == 10 && !memcmp(key, "totalCount", 10)) {
            ent->has_total = 1; ent->total = v; ent->total_a = L->tok.a; ent->total_b = L->tok.b;
          } else if (keylen == 9 && !memcmp(key, "usedCount", 9)) {
            ent->has_used = 1; ent->used = v;
          }
        }
      }
    }
    lex_next(L); return;
  }
  if (L->tok.type == T_STRING || L->tok.type == T_NAME) { lex_next(L); return; }
  L->err = 1;
}

/* Parse the whole save: a sequence of top-level `name = value` statements. */
static int parse_save(const char *s, size_t n, Inv *inv) {
  Lex L = { s, n, 0, {0}, 0 };
  memset(inv, 0, sizeof *inv);
  lex_next(&L);
  while (!L.err && L.tok.type != T_EOF) {
    if (L.tok.type != T_NAME) return 0;
    int is_inv = tok_name_eq(&L, "installationSpecificInventory");
    lex_next(&L);
    if (!tok_is(&L, '=')) return 0;
    lex_next(&L);
    if (is_inv && tok_is(&L, '{')) {
      Inv fresh; memset(&fresh, 0, sizeof fresh);           /* a later assignment wins, as in Lua */
      fresh.found = 1;
      parse_table(&L, 1, ROLE_INV, &fresh, NULL);
      if (!L.err) *inv = fresh;
    } else {
      parse_value(&L, 1, ROLE_NONE, NULL, NULL, NULL, 0);
    }
  }
  return !L.err && !inv->overflow;
}

/* ---- edits ---- */
typedef struct { size_t pos, del; char text[1024]; } Edit;   /* > prefix + add[512] */

static int cmp_edit(const void *a, const void *b) {
  const Edit *x = a, *y = b;
  return (x->pos > y->pos) - (x->pos < y->pos);
}

/* The four power-ups the game gives from the start; added if a save lacks them.
 * (Shockwave and Red's Mighty Feathers are unlocked later; they're topped up
 * once the game has created their entries.) */
static const char *const CORE_POWERUPS[] = { "slingscope", "kingsling", "powerpotion", "birdquake" };

char *powerups_edit_save_text(const char *in, size_t len, int target, size_t *out_len, int *changed) {
  if (changed) *changed = 0;
  if (out_len) *out_len = 0;
  if (!in || target < 1) return NULL;
  if (target > POWERUP_MAX) target = POWERUP_MAX;

  Inv *inv = calloc(1, sizeof *inv);
  if (!inv) return NULL;
  if (!parse_save(in, len, inv)) { free(inv); if (changed) *changed = -1; return NULL; }

  Edit *ed = calloc(MAX_ENTRIES + 8, sizeof *ed);
  if (!ed) { free(inv); return NULL; }
  int ne = 0, nchanged = 0;

  for (int i = 0; i < inv->n; i++) {                        /* raise existing entries */
    Entry *e = &inv->e[i];
    if (!e->has_total) continue;                            /* not a shape we know: leave it */
    double used = e->has_used ? ceil(e->used) : 0.0;
    if (used < 0) used = 0;
    if (e->total - used >= target) continue;                /* already has enough */
    double want = used + target;
    if (want > 16000000.0) continue;                        /* keep within float precision */
    Edit *x = &ed[ne++];
    x->pos = e->total_a; x->del = e->total_b - e->total_a;
    snprintf(x->text, sizeof x->text, "%ld", (long)want);
    nchanged++;
  }

  char add[4 * 128] = "";                                   /* missing core entries */
  for (size_t c = 0; c < sizeof CORE_POWERUPS / sizeof *CORE_POWERUPS; c++) {
    int have = 0;
    for (int i = 0; i < inv->n; i++) if (!strcmp(inv->e[i].key, CORE_POWERUPS[c])) have = 1;
    if (have) continue;
    char one[128];
    snprintf(one, sizeof one, "    %s = {\n        totalCount = %d,\n        usedCount = 0,\n    },\n",
             CORE_POWERUPS[c], target);
    strncat(add, one, sizeof add - strlen(add) - 1);
    nchanged++;
  }
  if (add[0]) {
    Edit *x = &ed[ne++];
    if (inv->found) {                                       /* insert before the table's '}' */
      int at_line_start = inv->close == 0 || in[inv->close - 1] == '\n';
      x->pos = inv->close; x->del = 0;
      snprintf(x->text, sizeof x->text, "%s%s%s",
               inv->needs_sep ? "," : "", at_line_start ? "" : "\n", add);
    } else {                                                /* no table yet: append one */
      x->pos = len; x->del = 0;
      snprintf(x->text, sizeof x->text, "%sinstallationSpecificInventory = {\n%s}\n",
               (len && in[len - 1] != '\n') ? "\n" : "", add);
    }
  }
  free(inv);

  if (!nchanged) { free(ed); return NULL; }
  qsort(ed, (size_t)ne, sizeof *ed, cmp_edit);

  size_t extra = 0;
  for (int i = 0; i < ne; i++) extra += strlen(ed[i].text);
  char *out = malloc(len + extra + 1);
  if (!out) { free(ed); return NULL; }
  size_t o = 0, p = 0;
  for (int i = 0; i < ne; i++) {
    memcpy(out + o, in + p, ed[i].pos - p); o += ed[i].pos - p;
    size_t tl = strlen(ed[i].text);
    memcpy(out + o, ed[i].text, tl); o += tl;
    p = ed[i].pos + ed[i].del;
  }
  memcpy(out + o, in + p, len - p); o += len - p;
  out[o] = 0;
  free(ed);
  if (out_len) *out_len = o;
  if (changed) *changed = nchanged;
  return out;
}

/* ========================================================================= *
 * Save file I/O + crypto
 * ========================================================================= */

/* Rovio's save-file key (settings.lua / highscores.lua), AES-256-CBC, zero IV. */
static const uint8_t SAVE_KEY[32] = "44iUY5aTrlaYoet9lapRlaK1Ehlec5i0";

int powerups_target_from_config(const char *v) {
  if (!v || !v[0]) return 0;
  if (!strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcasecmp(v, "false")) return 0;
  if (!strcasecmp(v, "max") || !strcasecmp(v, "on") || !strcasecmp(v, "yes") ||
      !strcasecmp(v, "true")) return POWERUP_MAX;
  char *end; long n = strtol(v, &end, 10);
  if (*end || n <= 0) return 0;
  return n > POWERUP_MAX ? POWERUP_MAX : (int)n;
}

static unsigned char *read_all(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 16 * 1024 * 1024) { fclose(f); return NULL; }
  unsigned char *b = malloc((size_t)sz + 1);
  if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
  fclose(f);
  if (b) *len = (size_t)sz;
  return b;
}

static int write_all(const char *path, const void *buf, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  size_t w = fwrite(buf, 1, len, f);
  int ok = (fclose(f) == 0) && w == len;
  return ok;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void patch_one_save(const char path[PATHMAX], int target) {
  size_t elen = 0;
  unsigned char *enc = read_all(path, &elen);
  if (!enc) { debugPrintf("powerups: no save at %s yet (play once, then relaunch)\n", path); return; }
  if (elen < 16 || elen % 16) { debugPrintf("powerups: %s: unexpected size, left alone\n", path); free(enc); return; }

  unsigned char *pt = malloc(elen + 1);
  if (!pt) { free(enc); return; }
  memcpy(pt, enc, elen);
  struct AES_ctx aes; uint8_t iv[16] = {0};
  AES_init_ctx_iv(&aes, SAVE_KEY, iv);
  AES_CBC_decrypt_buffer(&aes, pt, elen);

  /* PKCS#7 padding + plain text, or this isn't a save we understand */
  unsigned pad = pt[elen - 1];
  int ok = pad >= 1 && pad <= 16;
  for (unsigned i = 1; ok && i <= pad; i++) ok = pt[elen - i] == pad;
  size_t tlen = ok ? elen - pad : 0;
  for (size_t i = 0; ok && i < tlen; i++) ok = pt[i] != 0;
  if (!ok) { debugPrintf("powerups: %s: could not decrypt (wrong key/format), left alone\n", path);
             free(pt); free(enc); return; }

  int changed = 0; size_t nlen = 0;
  char *txt = powerups_edit_save_text((const char *)pt, tlen, target, &nlen, &changed);
  free(pt);
  if (!txt) {
    if (changed < 0) debugPrintf("powerups: %s: unrecognised save layout, left alone\n", path);
    else debugPrintf("powerups: %s: every power-up already has %d or more\n", path, target);
    free(enc); return;
  }

  /* re-encrypt with PKCS#7 */
  size_t npad = 16 - (nlen % 16), clen = nlen + npad;
  unsigned char *ct = malloc(clen);
  if (!ct) { free(txt); free(enc); return; }
  memcpy(ct, txt, nlen);
  memset(ct + nlen, (int)npad, npad);
  free(txt);
  uint8_t iv2[16] = {0};
  AES_init_ctx_iv(&aes, SAVE_KEY, iv2);
  AES_CBC_encrypt_buffer(&aes, ct, clen);

  char bak[PATHMAX + 8], tmp[PATHMAX + 8];                  /* path + ".bak"/".tmp" always fits */
  snprintf(bak, sizeof bak, "%s.bak", path);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  if (!exists(bak) && !write_all(bak, enc, elen)) {         /* one-time backup of the original */
    debugPrintf("powerups: cannot back up %s, left alone\n", path);
    free(ct); free(enc); return;
  }
  /* write-then-rename; Switch FAT won't rename over an existing file */
  if (write_all(tmp, ct, clen)) {
    remove(path);
    if (rename(tmp, path) == 0)
      debugPrintf("powerups: %s: %d power-up(s) topped up to %d\n", path, changed, target);
    else
      debugPrintf("powerups: rename %s -> %s failed (backup kept at %s)\n", tmp, path, bak);
  } else {
    remove(tmp);
    debugPrintf("powerups: cannot write %s, left alone\n", tmp);
  }
  free(ct); free(enc);
}

void powerup_patch_init(void) {
  int target = powerups_target_from_config(config.powerups);
  if (!target) {
    if (strcasecmp(config.powerups, "off") && strcasecmp(config.powerups, "no") &&
        strcasecmp(config.powerups, "false") && strcmp(config.powerups, "0"))
      debugPrintf("powerups: unrecognised value '%s' (use off, max or 1-9999); treating as off\n",
                  config.powerups);
    else
      debugPrintf("powerups: off\n");
    return;
  }

  /* The engine opens its save as "/settings.lua"; data_file_path() applies the
   * same remap, so this is exactly the file the game will read. Also cover
   * DATA_DIR in case the .nro was launched from another folder. */
  char a[PATHMAX], b[PATHMAX];
  if (data_file_path("settings.lua", a, sizeof a)) patch_one_save(a, target);
  snprintf(b, sizeof b, "%s/settings.lua", DATA_DIR);
  if (strcmp(a, b) != 0) {
    struct stat sa, sb;
    int same = stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_ino == sb.st_ino && sa.st_ino != 0;
    if (!same && exists(b)) patch_one_save(b, target);
  }
}
