#ifndef PV_NET_SOCKET_H
#define PV_NET_SOCKET_H
#include "../ppu-types.h"
#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 0xFFFF
#define SO_REUSEADDR 0x0004
#define SO_SNDTIMEO 0x1005
#define SO_RCVTIMEO 0x1006
#define SO_NBIO 0x1100
typedef u32 socklen_t;
typedef u8 sa_family_t;
struct sockaddr { u8 sa_len; sa_family_t sa_family; char sa_data[14]; };
#endif
