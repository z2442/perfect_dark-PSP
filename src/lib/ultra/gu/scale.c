#include <ultra64.h>
#ifdef PLATFORM_PSP
#include <pspgu.h>
#include <pspgum.h>
#include <string.h>
#endif

void guScaleF(float mf[4][4], float x, float y, float z)
{
#ifdef PLATFORM_PSP
	ScePspFMatrix4 gumMatrix;
	const ScePspFVector3 scaleVec = { x, y, z };

	sceGumPushMatrix();
	sceGumLoadIdentity();
	sceGumScale(&scaleVec);
	sceGumStoreMatrix(&gumMatrix);
	sceGumPopMatrix();

	memcpy(mf, &gumMatrix, sizeof(gumMatrix));
#else
	guMtxIdentF(mf);

	mf[0][0] = x;
	mf[1][1] = y;
	mf[2][2] = z;
	mf[3][3] = 1;
#endif
}

void guScale(Mtx *m, float x, float y, float z)
{
	f32 mf[4][4];

	guScaleF(mf, x, y, z);

	guMtxF2L(mf, m);
}
