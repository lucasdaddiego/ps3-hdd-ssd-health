#ifndef PV_SYS_SYSTIME_H
#define PV_SYS_SYSTIME_H
#include "../ppu-types.h"
s32 sysGetCurrentTime(u64 *sec, u64 *nsec);
s32 sysUsleep(u32 us);
#endif
