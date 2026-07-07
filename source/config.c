/* config.c -- simple "name value" configuration file for the Angry Birds port.
 *
 * Format: one setting per line, "name value", '#' begins a comment. Unknown or
 * retired keys are dropped and the file is rewritten so it self-heals across
 * versions. Adapted from the Chaos Rings 3 / Burger Shop Switch ports.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "config.h"
#include "util.h"

#define CONFIG_VARS \
  CONFIG_VAR_INT(screen_width);  \
  CONFIG_VAR_INT(screen_height); \
  CONFIG_VAR_INT(docked_width);  \
  CONFIG_VAR_INT(docked_height); \
  CONFIG_VAR_STR(language);

Config config;
static int config_needs_rewrite = 0;

// One flag per config key, set when that key is found in the file. After parsing
// we compare against the full set: a missing key means the file predates a new
// option, so we rewrite it (adding the new key with its default). This is what
// makes a freshly-added option like "language" appear in an existing config.txt.
static struct { int screen_width, screen_height, docked_width, docked_height, language; } seen;

static void copy_str(char *dst, const char *src, size_t n) {
  if (n == 0) return;
  size_t i = 0;
  for (; src[i] && i < n - 1; i++) dst[i] = src[i];
  dst[i] = 0;
}

static inline void parse_var(const char *name, const char *value) {
  #define CONFIG_VAR_INT(var) if (!strcmp(name, #var)) { config.var = atoi(value); seen.var = 1; return; }
  #define CONFIG_VAR_STR(var) if (!strcmp(name, #var)) { copy_str(config.var, value, sizeof(config.var)); seen.var = 1; return; }
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_STR
  // unknown key: keep parsing but rewrite the file to prune it
  config_needs_rewrite = 1;
}

int read_config(const char *file) {
  char line[1024] = { 0 };

  // defaults
  config.screen_width  = 0;   // automatic
  config.screen_height = 0;   // automatic
  config.docked_width  = 1920;
  config.docked_height = 1080;
  copy_str(config.language, "auto", sizeof(config.language));
  config_needs_rewrite = 0;
  memset(&seen, 0, sizeof(seen));

  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;

  do {
    char *name = NULL, *value = NULL, *tmp = NULL;
    if (fgets(line, sizeof(line), f) != NULL) {
      name = line;
      while (*name && isspace((int)*name)) ++name;
      if (name[0] == '#' || name[0] == 0) continue;
      for (tmp = name; *tmp && !isspace((int)*tmp); ++tmp);
      if (*tmp != 0) {
        *tmp = 0;
        for (value = tmp + 1; *value && isspace((int)*value); ++value);
        for (tmp = value + strlen(value) - 1; tmp >= value && isspace((int)*tmp); --tmp) *tmp = 0;
        parse_var(name, value);
      }
    }
  } while (!feof(f));

  fclose(f);

  // any key missing from the file (e.g. an option added in a newer build)?
  // rewrite so it gets added with its default while keeping the user's values.
  #define CONFIG_VAR_INT(var) if (!seen.var) config_needs_rewrite = 1;
  #define CONFIG_VAR_STR(var) if (!seen.var) config_needs_rewrite = 1;
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_STR

  return config_needs_rewrite ? 1 : 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  fprintf(f, "# Angry Birds Classic (Switch) configuration\n");
  fprintf(f, "#\n");
  fprintf(f, "# screen_width / screen_height: 0 = automatic (1920x1080 docked,\n");
  fprintf(f, "#   1280x720 handheld). Set both to force a fixed resolution.\n");
  fprintf(f, "# docked_width / docked_height: resolution used when docked while\n");
  fprintf(f, "#   screen_width/height are automatic.\n");
  fprintf(f, "# language: 'auto' follows the Switch system language, or set a\n");
  fprintf(f, "#   2-letter code: en de fr es it pt ru ja ko zh nl sv da no fi.\n");
  fprintf(f, "#   Languages actually shown depend on what your game files include.\n\n");

  #define CONFIG_VAR_INT(var) fprintf(f, "%s %d\n", #var, config.var)
  #define CONFIG_VAR_STR(var) fprintf(f, "%s %s\n", #var, config.var[0] ? config.var : "auto")
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_STR

  fclose(f);
  return 0;
}
