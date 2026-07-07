/* obb.h -- OBB (Android expansion file) reader interface.
 *
 * Angry Birds Classic ships its assets as loose files under assets/, not in an
 * encrypted .obb, so these are inert stubs: obb_read always reports "empty" and
 * the AAsset emulation in libc_shim.c falls through to loose-file loading.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#ifndef __OBB_H__
#define __OBB_H__

#include <stddef.h>

int   obb_open(const char *path);
void  obb_close(void);
int   obb_exists(const char *name);
void *obb_read(const char *name, size_t *out_size);

#endif
