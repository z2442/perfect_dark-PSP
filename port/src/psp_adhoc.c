#include "psp_adhoc.h"
#include "net/net.h"
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_adhoc.h>
#include <pspnet_adhocctl.h>
#include <psputility_netmodules.h>
#include <pspwlan.h>
#include <stdio.h>
#include <string.h>

static int initLevel, phase, roomCount;
static unsigned started;
static volatile int resumed;
static char status[96] = "Turn on WLAN, then host or find a game.\n";
static char address[32];
static struct SceNetAdhocctlScanInfo rooms[8];
static char roomNames[8][10];
static unsigned nowMs(void) { return (unsigned)(sceKernelGetSystemTimeWide()/1000); }
void pdAdhocShutdown(void)
{
 if (g_NetMode) netDisconnect();
 if (initLevel >= 5) { sceNetAdhocctlDisconnect(); sceNetAdhocctlTerm(); }
 if (initLevel >= 4) sceNetAdhocTerm();
 if (initLevel >= 3) sceNetTerm();
 if (initLevel >= 2) sceUtilityUnloadNetModule(PSP_NET_MODULE_ADHOC);
 if (initLevel >= 1) sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
 initLevel=phase=roomCount=0; address[0]=0;
 snprintf(status,sizeof(status),"Disconnected.\n");
}
static int failure(const char *text, int rc)
{
 pdAdhocShutdown();
 snprintf(status,sizeof(status),"%s (%08x)\n",text,(unsigned)rc);
 return -1;
}
static int initialize(void)
{
 int rc;
 struct productStruct product = {0};
 memcpy(product.product,"PDARK0001",9);
 pdAdhocShutdown(); resumed=0;
 if (!sceWlanGetSwitchState()) return failure("Turn on WLAN",0);
 if ((rc=sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON))<0) return failure("Network module failed",rc);
 initLevel=1;
 if ((rc=sceUtilityLoadNetModule(PSP_NET_MODULE_ADHOC))<0) return failure("Ad hoc module failed",rc);
 initLevel=2;
 if ((rc=sceNetInit(256*1024,42,4096,42,4096))<0) return failure("Network memory failed",rc);
 initLevel=3;
 if ((rc=sceNetAdhocInit())<0) return failure("Ad hoc init failed",rc);
 initLevel=4;
 if ((rc=sceNetAdhocctlInit(0x2000,48,&product))<0) return failure("Ad hoc control failed",rc);
 initLevel=5;
 return 0;
}
int pdAdhocHost(void)
{
 char group[9]; int rc;
 if (initialize()<0) return -1;
 snprintf(group,sizeof(group),"PD%06X",nowMs()&0xffffff);
 if ((rc=sceNetAdhocctlCreate(group))<0) return failure("Could not create room",rc);
 phase=1; started=nowMs();
 snprintf(status,sizeof(status),"Creating %s...\n",group);
 return 0;
}
int pdAdhocScan(void)
{
 int rc;
 if (initialize()<0) return -1;
 if ((rc=sceNetAdhocctlScan())<0) return failure("Scan failed",rc);
 phase=2; started=nowMs();
 snprintf(status,sizeof(status),"Searching for nearby PSPs...\n");
 return 0;
}
int pdAdhocJoin(int index)
{
 int rc;
 if (phase!=6 || index<0 || index>=roomCount) return -1;
 if ((rc=sceNetAdhocctlJoin(&rooms[index]))<0) return failure("Join failed",rc);
 phase=3; started=nowMs();
 snprintf(status,sizeof(status),"Joining %.8s...\n",rooms[index].name);
 return 0;
}
void pdAdhocNotifyResume(void) { resumed=1; }
void pdAdhocPoll(void)
{
 int state,rc;
 unsigned now=nowMs();
 if (resumed) { resumed=0; if(initLevel) failure("Disconnected after sleep",0); }
 if (!initLevel) return;
 if (!sceWlanGetSwitchState()) { failure("WLAN switched off",0); return; }
 if ((rc=sceNetAdhocctlGetState(&state))<0) { failure("Ad hoc state failed",rc); return; }
 if ((phase==1 || phase==2 || phase==3) && now-started>=20000) { failure("Connection timed out",0); return; }
 if (phase==2 && state==0 && now-started>=1000) {
  struct SceNetAdhocctlScanInfo found[32]; int length=sizeof(found);
  memset(found,0,sizeof(found));
  if ((rc=sceNetAdhocctlGetScanInfo(&length,found))<0) { failure("Reading rooms failed",rc); return; }
  roomCount=0;
  int count=length/(int)sizeof(found[0]); if(count>32)count=32;
  for(int i=0;i<count && roomCount<8;i++) if(found[i].name[0]=='P' && found[i].name[1]=='D') {
   rooms[roomCount]=found[i]; rooms[roomCount].next=NULL;
   snprintf(roomNames[roomCount],sizeof(roomNames[0]),"%.8s\n",found[i].name); roomCount++;
  }
  phase=6;
  snprintf(status,sizeof(status),roomCount?"Select a room to join.\n":"No games found. Try Find Games again.\n");
 } else if (phase==1 && state==1) {
  phase=4; snprintf(status,sizeof(status),"Room open. Waiting for another player.\n");
 } else if (phase==3 && state==1) {
  struct SceNetAdhocctlPeerInfo peers[8]; int length=sizeof(peers); unsigned char own[6];
  sceWlanGetEtherAddr(own); memset(peers,0,sizeof(peers));
  if (sceNetAdhocctlGetPeerList(&length,peers)<0) return;
  int count=length/(int)sizeof(peers[0]); if(count>8)count=8;
  for(int i=0;i<count;i++) if(memcmp(own,peers[i].mac,6)) {
   unsigned char *m=peers[i].mac;
   snprintf(address,sizeof(address),"%02x:%02x:%02x:%02x:%02x:%02x",m[0],m[1],m[2],m[3],m[4],m[5]);
   phase=5; snprintf(status,sizeof(status),"Connecting to host...\n"); break;
  }
 } else if ((phase==4 || phase==5) && state!=1) failure("Ad hoc connection lost",0);
}
int pdAdhocReady(void) { return phase==4 ? 1 : phase==5 ? 2 : 0; }
int pdAdhocRoomCount(void) { return roomCount; }
const char *pdAdhocRoomName(int i) { return i>=0 && i<roomCount ? roomNames[i] : ""; }
const char *pdAdhocPeerAddress(void) { return address; }
const char *pdAdhocStatus(void) { return status; }
