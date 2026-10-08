/* Stub: the RSX control register. The stub draws synchronously, so GET is always PUT. */
#pragma once
#include "ppu-types.h"
typedef struct { volatile u32 put, get, ref; } gcmControlRegister;
gcmControlRegister *gcmGetControlRegister(void);
