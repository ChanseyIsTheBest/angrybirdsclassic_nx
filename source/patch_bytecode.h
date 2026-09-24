#ifndef PATCH_BYTECODE_H
#define PATCH_BYTECODE_H
/* patch_bytecode.h -- edits to the game's Lua 5.1 bytecode (4-byte int,
 * size_t, instruction and float number; little-endian). */
#include <stddef.h>
#include <stdint.h>

/* Inject a getCurrentLocale override forcing `locale` into gamelogic.lua. */
size_t patch_gamelogic_bytecode(const uint8_t *in, size_t in_len, const char *locale,
                                uint8_t *out, size_t out_cap);

/* One exact-match string-constant substitution. */
typedef struct { const char *from, *to; } LuaStrMap;

/* Copy a chunk, replacing string constants equal to map[i].from with map[i].to
 * in every function prototype. Returns output length (0 on malformed input or
 * out of space); *hits (optional) = number of constants rewritten. */
size_t patch_rename_string_consts(const uint8_t *in, size_t in_len,
                                  const LuaStrMap *map, int nmap,
                                  uint8_t *out, size_t out_cap, int *hits);
#endif
