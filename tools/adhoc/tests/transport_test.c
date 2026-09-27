#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "net/netenet.h"
static uint64_t clockUs=1000000;
static unsigned char localMac[6]={2,0,0,0,0,1};
static struct endpoint {int active; unsigned short port; unsigned char mac[6];} endpoints[2];
static struct datagram {int target,size; unsigned short port; unsigned char mac[6],data[4096];} queue[128];
static unsigned sent,dropped; static int loss;
uint64_t sceKernelGetSystemTimeWide(void){return clockUs;}
int sceKernelDelayThread(unsigned usec){clockUs+=usec;return 0;}
int sceWlanGetEtherAddr(unsigned char*m){memcpy(m,localMac,6);return 0;}
int sceNetAdhocPdpCreate(unsigned char*m,unsigned short p,unsigned size,int flags){
 for(int i=0;i<2;i++)if(!endpoints[i].active){endpoints[i].active=1;endpoints[i].port=p;memcpy(endpoints[i].mac,m,6);return i+1;}return -1;
}
int sceNetAdhocPdpDelete(int id,int flags){endpoints[id-1].active=0;return 0;}
int sceNetAdhocPdpSend(int id,unsigned char*m,unsigned short p,void*data,unsigned size,unsigned timeout,int nonblock){
 assert(size<=4096); ++sent;
 if(loss && sent%5==0){++dropped;return 0;}
 for(int target=0;target<2;target++)if(endpoints[target].active && endpoints[target].port==p && !memcmp(endpoints[target].mac,m,6)){
  for(int j=0;j<128;j++)if(!queue[j].size){queue[j].target=target;queue[j].size=size;queue[j].port=endpoints[id-1].port;memcpy(queue[j].mac,endpoints[id-1].mac,6);memcpy(queue[j].data,data,size);return 0;}
  assert(!"mock queue full");
 }return 0;
}
int sceNetAdhocPdpRecv(int id,unsigned char*m,unsigned short*p,void*data,int*size,unsigned timeout,int nonblock){
 for(int j=0;j<128;j++)if(queue[j].size && queue[j].target==id-1){assert(*size>=queue[j].size);*size=queue[j].size;*p=queue[j].port;memcpy(m,queue[j].mac,6);memcpy(data,queue[j].data,*size);queue[j].size=0;return 0;}
 return (int)0x80410709U;
}
static unsigned connected,received,disconnected; static char payload[12000];
static void pump(ENetHost*h){
 ENetEvent e; int rc;
 while((rc=enet_host_service(h,&e,0))>0){
  if(e.type==ENET_EVENT_TYPE_CONNECT)connected++;
  if(e.type==ENET_EVENT_TYPE_DISCONNECT)disconnected++;
  if(e.type==ENET_EVENT_TYPE_RECEIVE){assert(e.packet->dataLength==sizeof(payload));assert(!memcmp(e.packet->data,payload,sizeof(payload)));received++;enet_packet_dispose(e.packet);}
 }assert(rc==0);
}
int main(void){
 ENetAddress a={.port=27100},b={.port=27101}; assert(!enet_initialize());
 ENetHost*host=enet_host_create(&a,2,2,0,0,32768);assert(host);
 localMac[5]=2; ENetHost*guest=enet_host_create(&b,1,2,0,0,32768);assert(guest);
 assert(!enet_address_set_ip(&a,"02:00:00:00:00:01"));
 assert(enet_address_set_ip(&b,"1.2.3.4")<0);
 ENetPeer*peer=enet_host_connect(guest,&a,2,123);assert(peer);
 for(int i=0;i<1000 && connected<2;i++){clockUs+=10000;pump(guest);pump(host);}assert(connected==2);
 for(unsigned i=0;i<sizeof(payload);i++)payload[i]=(char)(i*13);
 loss=1;
 assert(!enet_peer_send(peer,0,enet_packet_create(payload,sizeof(payload),ENET_PACKET_FLAG_RELIABLE)));
 for(int i=0;i<3000 && !received;i++){clockUs+=10000;pump(guest);pump(host);}assert(received==1 && dropped);
 loss=0; enet_peer_disconnect(peer,0);
 for(int i=0;i<1000 && disconnected<2;i++){clockUs+=10000;pump(guest);pump(host);}assert(disconnected==2);
 enet_host_destroy(guest);enet_host_destroy(host);assert(!endpoints[0].active&&!endpoints[1].active);
 puts("PASS: PDP connection, 12KB reliable fragmentation with loss, disconnect and cleanup");return 0;
}
