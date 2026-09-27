#ifndef PD_PSP_ADHOC_H
#define PD_PSP_ADHOC_H
#ifdef __cplusplus
extern "C" {
#endif
void pdAdhocPoll(void);
void pdAdhocShutdown(void);
void pdAdhocNotifyResume(void);
int pdAdhocHost(void);
int pdAdhocScan(void);
int pdAdhocJoin(int index);
int pdAdhocReady(void); /* 1 host, 2 client, 0 pending/off */
int pdAdhocRoomCount(void);
const char *pdAdhocRoomName(int index);
const char *pdAdhocPeerAddress(void);
const char *pdAdhocStatus(void);
#ifdef __cplusplus
}
#endif
#endif
