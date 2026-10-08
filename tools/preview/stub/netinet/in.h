#ifndef PV_NETINET_IN_H
#define PV_NETINET_IN_H
#include "../ppu-types.h"
#include "../net/socket.h"
#define IPPROTO_TCP 6
#define INADDR_ANY 0
typedef u32 in_addr_t;
typedef u16 in_port_t;
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in { u8 sin_len; sa_family_t sin_family; in_port_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };
#endif
