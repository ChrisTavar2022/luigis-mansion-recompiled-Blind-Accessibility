#ifndef VERSION_H
#define VERSION_H

#include <dolphin/types.h>

#define VERSION_USA 0
#define VERSION_JPN 1
#define VERSION_PAL_0 2
#define VERSION_PAL_1 3

#define VERSION_PAL (VERSION == VERSION_PAL_0 || VERSION == VERSION_PAL_1)

#endif
