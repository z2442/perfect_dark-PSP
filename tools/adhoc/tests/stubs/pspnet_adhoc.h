int sceNetAdhocPdpCreate(unsigned char *,unsigned short,unsigned,int);
int sceNetAdhocPdpDelete(int,int);
int sceNetAdhocPdpSend(int,unsigned char *,unsigned short,void *,unsigned,unsigned,int);
int sceNetAdhocPdpRecv(int,unsigned char *,unsigned short *,void *,int *,unsigned,int);
