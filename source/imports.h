/* imports.h -- .so import resolution interface
 *
 * Adapted from the FF4:AY / Burger Shop Switch ports under the MIT license.
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stdio.h>
#include <stdlib.h>
#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;

void update_imports(void);

#endif
