#ifndef PV_SYSUTIL_H
#define PV_SYSUTIL_H
#include "../ppu-types.h"
#define SYSUTIL_EVENT_SLOT0 0
#define SYSUTIL_EXIT_GAME 0x0101
typedef void (*sysutilCallback)(u64 status, u64 param, void *usrdata);
s32 sysUtilRegisterCallback(s32 slot, sysutilCallback cb, void *usrdata);
s32 sysUtilUnregisterCallback(s32 slot);
s32 sysUtilCheckCallback(void);
#endif
