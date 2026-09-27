/* PSP PDP backend for the vendored ENet protocol. All calls are on the game thread. */
#ifdef __PSP__
#include "net/netenet.h"
#include <pspnet_adhoc.h>
#include <pspkernel.h>
#include <pspwlan.h>
#include <stdio.h>
#include <string.h>

static struct { int used, pdp; ENetAddress address; } sockets[2];
int enet_initialize(void) { return 0; }
void enet_deinitialize(void) {}
uint64_t enet_host_random_seed(void) { return sceKernelGetSystemTimeWide(); }
int enet_address_set_ip(ENetAddress *address, const char *name)
{
 unsigned m[6]; char tail;
 if (sscanf(name, "%2x:%2x:%2x:%2x:%2x:%2x%c", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &tail) != 6) return -1;
 memset(&address->ipv6, 0, sizeof(address->ipv6));
 for (int i = 0; i < 6; i++) address->ipv6.s6_addr[i] = m[i];
 return 0;
}
int enet_address_set_hostname(ENetAddress *a, const char *s) { return enet_address_set_ip(a, s); }
int enet_address_get_ip(const ENetAddress *a, char *s, size_t n)
{
 const uint8_t *m = a->ipv6.s6_addr;
 return snprintf(s, n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0],m[1],m[2],m[3],m[4],m[5]) < (int)n ? 0 : -1;
}
int enet_address_get_hostname(const ENetAddress *a, char *s, size_t n) { return enet_address_get_ip(a,s,n); }
ENetSocket enet_socket_create(ENetSocketType type)
{
 if (type != ENET_SOCKET_TYPE_DATAGRAM) return -1;
 for (int i = 0; i < 2; i++) if (!sockets[i].used) {
  memset(&sockets[i], 0, sizeof(sockets[i])); sockets[i].used = 1; sockets[i].pdp = -1; return i;
 }
 return -1;
}
int enet_socket_bind(ENetSocket s, const ENetAddress *address)
{
 if (s < 0 || s >= 2 || !sockets[s].used || !address || sockets[s].pdp >= 0) return -1;
 sockets[s].address = *address;
 sceWlanGetEtherAddr(sockets[s].address.ipv6.s6_addr);
 sockets[s].pdp = sceNetAdhocPdpCreate(sockets[s].address.ipv6.s6_addr, address->port, 32768, 0);
 return sockets[s].pdp < 0 ? -1 : 0;
}
int enet_socket_get_address(ENetSocket s, ENetAddress *a) { *a = sockets[s].address; return 0; }
void enet_socket_destroy(ENetSocket s)
{
 if (s >= 0 && s < 2 && sockets[s].used) {
  if (sockets[s].pdp >= 0) sceNetAdhocPdpDelete(sockets[s].pdp, 0);
  sockets[s].used = 0;
 }
}
int enet_socket_set_option(ENetSocket s, ENetSocketOption option, int value) { (void)s; (void)option; (void)value; return 0; }
int enet_socket_get_option(ENetSocket s, ENetSocketOption option, int *value) { (void)s; (void)option; *value=0; return 0; }
int enet_socket_send(ENetSocket s, const ENetAddress *a, const ENetBuffer *b, size_t count)
{
 uint8_t packet[4096]; size_t length = 0;
 if (!a || s < 0 || s >= 2 || sockets[s].pdp < 0) return -1;
 for (size_t i=0;i<count;i++) {
  if (b[i].dataLength > sizeof(packet)-length) return -1;
  memcpy(packet+length,b[i].data,b[i].dataLength); length += b[i].dataLength;
 }
 int rc = sceNetAdhocPdpSend(sockets[s].pdp, (unsigned char*)a->ipv6.s6_addr, a->port, packet, length, 0, 1);
 /* PSP reports success as zero, not a byte count. Would-block is retried by ENet. */
 return rc >= 0 ? (int)length : (uint32_t)rc == 0x80410709U ? 0 : -1;
}
int enet_socket_receive(ENetSocket s, ENetAddress *a, ENetBuffer *b, size_t count)
{
 unsigned char mac[6]; unsigned short port; int length, rc;
 if (count != 1 || s < 0 || s >= 2 || sockets[s].pdp < 0) return -1;
 length = b[0].dataLength;
 rc = sceNetAdhocPdpRecv(sockets[s].pdp, mac, &port, b[0].data, &length, 0, 1);
 if (rc < 0) return (uint32_t)rc == 0x80410709U ? 0 : -1;
 memset(a,0,sizeof(*a)); memcpy(a->ipv6.s6_addr,mac,6); a->port=port;
 return length;
}
int enet_socket_wait(ENetSocket s, uint32_t *condition, uint64_t timeout)
{
 (void)s;
 if (timeout) sceKernelDelayThread(timeout * 1000);
 *condition = ENET_SOCKET_WAIT_RECEIVE;
 return 0;
}
int enet_socket_shutdown(ENetSocket s, ENetSocketShutdown how) { (void)s; (void)how; return 0; }
#endif
