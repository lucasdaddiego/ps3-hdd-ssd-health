#ifndef PV_NET_H
#define PV_NET_H
#include <sys/types.h>
#include <sys/time.h>
#include "../ppu-types.h"
#include "socket.h"
struct net_hostent { u32 h_name, h_aliases; s32 h_addrtype, h_length; u32 h_addr_list; };
s32 netInitialize(void); s32 netDeinitialize(void);
s32 netSocket(s32 d, s32 t, s32 p); s32 netConnect(s32 s, const struct sockaddr *a, socklen_t l); s32 netClose(s32 s);
s32 netBind(s32 s, const struct sockaddr *a, socklen_t l); s32 netListen(s32 s, s32 b); s32 netAccept(s32 s, const struct sockaddr *a, socklen_t *l);
s32 netSetSockOpt(s32 s, s32 level, s32 opt, const void *v, socklen_t l);
ssize_t netSend(s32 s, const void *b, size_t l, s32 f); ssize_t netRecv(s32 s, void *b, size_t l, s32 f);
struct net_hostent *netGetHostByName(const char *name);
#endif
