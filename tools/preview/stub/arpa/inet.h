#ifndef PV_ARPA_INET_H
#define PV_ARPA_INET_H
#include "../netinet/in.h"
#define htonl(x) (x)
#define htons(x) (x)
in_addr_t inet_addr(const char *cp);
#endif
