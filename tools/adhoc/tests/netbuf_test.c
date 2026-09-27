#include <assert.h>
#include <string.h>
#include <limits.h>
#include "net/netbuf.h"
int main(void) {
 unsigned char storage[128]; struct netbuf b={.data=storage+1,.size=127};
 netbufStartWrite(&b);
 netbufWriteU8(&b,42); netbufWriteU16(&b,0xabcd); netbufWriteU32(&b,0x12345678);
 netbufWriteU64(&b,UINT64_C(0xfedcba9876543210)); netbufWriteF32(&b,1.25f); netbufWriteStr(&b,"Joanna");
 assert(!b.error); unsigned length=b.wp;
 netbufStartReadData(&b,storage+1,length);
 assert(netbufReadU8(&b)==42); assert(netbufReadU16(&b)==0xabcd);
 assert(netbufReadU32(&b)==0x12345678); assert(netbufReadU64(&b)==UINT64_C(0xfedcba9876543210));
 assert(netbufReadF32(&b)==1.25f); assert(!strcmp(netbufReadStr(&b),"Joanna"));
 assert(!b.error && netbufReadLeft(&b)==0); netbufReadU8(&b); assert(b.error);
 unsigned char truncated[]={4,0,'a',0}; netbufStartReadData(&b,truncated,sizeof(truncated));
 assert(!*netbufReadStr(&b) && b.error);
 unsigned char unterminated[]={2,0,'a','b'}; netbufStartReadData(&b,unterminated,4);
 assert(!*netbufReadStr(&b) && b.error);
 unsigned char empty[]={0,0}; netbufStartReadData(&b,empty,2); netbufReadStr(&b); assert(b.error);
 netbufStartReadData(&b,storage,8); b.rp=1; assert(!netbufReadSkip(&b,UINT_MAX) && b.error);
 b.size=8; netbufStartWrite(&b); b.wp=1; assert(!netbufWriteData(&b,storage,UINT_MAX) && b.error);
 return 0;
}
