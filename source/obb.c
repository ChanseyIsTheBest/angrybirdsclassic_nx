/* obb.c -- inert OBB reader (Angry Birds Classic has no expansion file).
 * See obb.h. Every entry point reports "no archive / empty" so the AAsset
 * emulation falls through to loose files.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#include <stddef.h>
#include "obb.h"

int obb_open(const char *path) { (void)path; return -1; }

void obb_close(void) {}

int obb_exists(const char *name) { (void)name; return 0; }

void *obb_read(const char *name, size_t *out_size) {
  (void)name;
  if (out_size) *out_size = 0;
  return NULL; // fall through to loose-file lookups
}
