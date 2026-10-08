#ifndef PV_NETCTL_H
#define PV_NETCTL_H
#include "../ppu-types.h"
#define NET_CTL_INFO_IP_ADDRESS 16
union net_ctl_info { char ip_address[16]; u32 pad[64]; };
s32 netCtlInit(void); void netCtlTerm(void); s32 netCtlGetInfo(s32 code, union net_ctl_info *info);
#endif
