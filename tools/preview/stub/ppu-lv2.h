#ifndef PV_PPU_LV2_H
#define PV_PPU_LV2_H
#include "ppu-types.h"
u64 pv_syscall(int n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g);
#define lv2syscall1(n,a) u64 __r = pv_syscall(n,(u64)(a),0,0,0,0,0,0)
#define lv2syscall2(n,a,b) u64 __r = pv_syscall(n,(u64)(a),(u64)(b),0,0,0,0,0)
#define lv2syscall3(n,a,b,c) u64 __r = pv_syscall(n,(u64)(a),(u64)(b),(u64)(c),0,0,0,0)
#define lv2syscall4(n,a,b,c,d) u64 __r = pv_syscall(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d),0,0,0)
#define lv2syscall5(n,a,b,c,d,e) u64 __r = pv_syscall(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d),(u64)(e),0,0)
#define lv2syscall6(n,a,b,c,d,e,f) u64 __r = pv_syscall(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d),(u64)(e),(u64)(f),0)
#define lv2syscall7(n,a,b,c,d,e,f,g) u64 __r = pv_syscall(n,(u64)(a),(u64)(b),(u64)(c),(u64)(d),(u64)(e),(u64)(f),(u64)(g))
#define return_to_user_prog(t) return (t)__r
#endif
