#include <stdint.h>
struct in_addr {uint32_t s_addr;};
#define INADDR_BROADCAST 0xffffffffU
#undef htons
#undef ntohs
#undef htonl
#undef ntohl
