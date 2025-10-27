#include "guint.h"
#ifdef PLATFORM_PSP
#include <pspgu.h>
#include <pspgum.h>
#include <string.h>
#endif

void guTranslateF(float mf[4][4], float x, float y, float z)
{
#ifdef PLATFORM_PSP
	ScePspFMatrix4 gumMatrix;
	const ScePspFVector3 translateVec = { x, y, z };

	sceGumPushMatrix();
	sceGumLoadIdentity();
	sceGumTranslate(&translateVec);
	sceGumStoreMatrix(&gumMatrix);
	sceGumPopMatrix();

	memcpy(mf, &gumMatrix, sizeof(gumMatrix));
#else
	guMtxIdentF(mf);

	mf[3][0] = x;
	mf[3][1] = y;
	mf[3][2] = z;
#endif
}

void guTranslate(Mtx *m, float x, float y, float z)
{
	f32 mf[4][4];
	u32 stack[4];

#ifdef PLATFORM_PSP
	ScePspFMatrix4 gumMatrix;
	const ScePspFVector3 translateVec = { x, y, z };

	(void)stack; /* Avoid unused warning; PSP branch does not need it. */

	sceGumPushMatrix();
	sceGumLoadIdentity();
	sceGumTranslate(&translateVec);
	sceGumStoreMatrix(&gumMatrix);
	sceGumPopMatrix();

	memcpy(mf, &gumMatrix, sizeof(gumMatrix));
#else
	guMtxIdentF(mf);

	mf[3][0] = x;
	mf[3][1] = y;
	mf[3][2] = z;
#endif

	guMtxF2L(mf, m);
}
