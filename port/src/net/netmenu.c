#include <stdio.h>
#include "platform.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include "constants.h"
#include "game/menu.h"
#include "game/mainmenu.h"
#include "game/mplayer/mplayer.h"
#include "net/net.h"
#include "psp_adhoc.h"

extern MenuItemHandlerResult menuhandlerMainMenuCombatSimulator(s32,struct menuitem*,union handlerdata*);
extern MenuItemHandlerResult menuhandlerMpAdvancedSetup(s32,struct menuitem*,union handlerdata*);
extern struct menuitem g_MpPlayerSetup234MenuItems[];
static int pending;
static char message[96];
struct menudialogdef g_NetJoiningDialog;
struct menudialogdef g_NetMenuDialog;

MenuItemHandlerResult menuhandlerHostStart(s32 op,struct menuitem *item,union handlerdata *data)
{
 (void)item;(void)data;
 if(op==MENUOP_CHECKDISABLED)return g_NetMode!=NETMODE_NONE || pending;
 if(op==MENUOP_SET && !g_NetMode) {
  if(pdAdhocReady()!=1) { if(pdAdhocHost()==0)pending=1; return 0; }
  if(netStartServer(NET_DEFAULT_PORT,2)==0) {
   pending=0;
   /* Keep the first supported mode explicit: two human players, Combat, no bots. */
   g_MpSetup.chrslots=1; g_MpSetup.scenario=MPSCENARIO_COMBAT;
   menuhandlerMainMenuCombatSimulator(MENUOP_SET,NULL,NULL);
   menuhandlerMpAdvancedSetup(MENUOP_SET,NULL,NULL);
  } else { pdAdhocShutdown(); pending=0; }
 }
 return 0;
}
MenuItemHandlerResult menuhandlerJoinStart(s32 op,struct menuitem *item,union handlerdata *data)
{
 (void)data;
 if(op==MENUOP_SET && !g_NetMode) {
  if(pdAdhocReady()!=2) { if(item && pdAdhocJoin(item->param)==0)pending=2; return 0; }
  if(netStartClient(pdAdhocPeerAddress())==0) { pending=0; menuPushDialog(&g_NetJoiningDialog); }
  else { pdAdhocShutdown();pending=0; }
 }
 return 0;
}
MenuItemHandlerResult menuhandlerHostGame(s32 op,struct menuitem *i,union handlerdata*d) { if(op==MENUOP_SET)menuPushDialog(&g_NetMenuDialog);return 0; }
MenuItemHandlerResult menuhandlerJoinGame(s32 op,struct menuitem *i,union handlerdata*d) { return menuhandlerHostGame(op,i,d); }
static MenuItemHandlerResult scan(s32 op,struct menuitem*i,union handlerdata*d)
{
 if(op==MENUOP_SET) { pending=0; pdAdhocScan(); }
 if(op==MENUOP_CHECKDISABLED)return g_NetMode!=NETMODE_NONE;
 return 0;
}
static MenuItemHandlerResult disconnect(s32 op,struct menuitem*i,union handlerdata*d)
{
 if(op==MENUOP_SET) { pending=0;pdAdhocShutdown(); }
 return 0;
}
static MenuItemHandlerResult room(s32 op,struct menuitem*i,union handlerdata*d)
{
 if(op==MENUOP_CHECKHIDDEN)return i->param>=pdAdhocRoomCount();
 if(op==MENUOP_CHECKDISABLED)return g_NetMode!=NETMODE_NONE;
 return menuhandlerJoinStart(op,i,d);
}
static char *roomName(struct menuitem*i) { return (char*)pdAdhocRoomName(i->param); }
static char *statusText(struct menuitem*i)
{
 if(g_NetMode==NETMODE_CLIENT) {
  snprintf(message,sizeof(message),g_NetLocalClient->state>=CLSTATE_LOBBY?"Joined. Waiting for host to start Combat.\n":"Connecting to host...\n");
  return message;
 }
 return (char*)pdAdhocStatus();
}
static MenuDialogHandlerResult tick(s32 op,struct menudialogdef*dialog,union handlerdata*d)
{
 if(op==MENUOP_TICK) {
  if(pending==1 && pdAdhocReady()==1)menuhandlerHostStart(MENUOP_SET,NULL,NULL);
  if(pending==2 && pdAdhocReady()==2)menuhandlerJoinStart(MENUOP_SET,NULL,NULL);
 }
 if(op==MENUOP_CLOSE && (pending || !g_NetMode)) { pending=0;pdAdhocShutdown(); }
 return 0;
}
#define ACTION(label,handler) {MENUITEMTYPE_SELECTABLE,0,MENUITEMFLAG_LITERAL_TEXT,(uintptr_t)label,0,handler}
#define ROOM(n) {MENUITEMTYPE_SELECTABLE,n,0,(uintptr_t)roomName,0,room}
static struct menuitem items[]={
 ACTION("Host Ad Hoc Game\n",menuhandlerHostStart),
 ACTION("Find Nearby Games\n",scan),
 {MENUITEMTYPE_LABEL,0,0,(uintptr_t)statusText,0,NULL},
 ROOM(0),ROOM(1),ROOM(2),ROOM(3),ROOM(4),ROOM(5),ROOM(6),ROOM(7),
 ACTION("Disconnect\n",disconnect),
 {MENUITEMTYPE_SELECTABLE,0,MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,L_OPTIONS_213,0,NULL},
 {MENUITEMTYPE_END}
};
struct menudialogdef g_NetMenuDialog={MENUDIALOGTYPE_DEFAULT,(uintptr_t)"Ad Hoc Multiplayer",items,tick,MENUDIALOGFLAG_LITERAL_TEXT,NULL};
static struct menuitem joiningItems[]={
 {MENUITEMTYPE_LABEL,0,0,(uintptr_t)statusText,0,NULL},
 {MENUITEMTYPE_SELECTABLE,0,MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,(uintptr_t)"Disconnect\n",0,disconnect},
 {MENUITEMTYPE_END}
};
struct menudialogdef g_NetJoiningDialog={MENUDIALOGTYPE_DEFAULT,(uintptr_t)"Ad Hoc Lobby",joiningItems,NULL,MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_IGNOREBACK,NULL};
struct menudialogdef g_NetJoinPlayerSetupMenuDialog={MENUDIALOGTYPE_DEFAULT,L_MPMENU_028,g_MpPlayerSetup234MenuItems,NULL,MENUDIALOGFLAG_STARTSELECTS,NULL};
