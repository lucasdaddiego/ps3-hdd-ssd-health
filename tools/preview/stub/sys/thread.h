#ifndef PV_SYS_THREAD_H
#define PV_SYS_THREAD_H
#include <pthread.h>
#include "../ppu-types.h"
#define THREAD_JOINABLE 1
typedef pthread_t sys_ppu_thread_t;
s32 sysThreadCreate(sys_ppu_thread_t *t, void (*entry)(void *), void *arg, s32 prio, u64 stack, u64 flags, char *name);
s32 sysThreadJoin(sys_ppu_thread_t t, u64 *ret);
void sysThreadExit(u64 v);
#endif
