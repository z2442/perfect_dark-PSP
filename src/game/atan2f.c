#include <ultra64.h>
#include "game/acosfasinf.h"
#include "data.h"
#include "types.h"

#ifdef __PSP__
static f32 vfpu_atan2f_psp(f32 x, f32 z)
{
	const float pi = 3.14159265358979f;
	const float halfpi = 1.57079632679489f;
	const float tau = 6.28318530717958f;
	f32 result;

	if (x == 0.0f) {
		return z >= 0.0f ? 0.0f : pi;
	}

	if (z == 0.0f) {
		return x > 0.0f ? halfpi : 3.0f * halfpi;
	}

	result = vfpu_atan2f(x, z);

	if (result < 0.0f) {
		result += tau;
	}

	return result;
}
#endif

f32 atan2f(f32 x, f32 z)
{
#ifdef __PSP__
	return vfpu_atan2f_psp(x, z);
#else
	f32 result;

	if (x == 0) {
		if (z >= 0) {
			result = 0;
		} else {
			result = M_PI;
		}
	} else if (z == 0) {
		if (x > 0) {
			result = 1.5707963705063f;
		} else {
			result = 1.5707963705063f * 3;
		}
	} else {
		result = pspFpuSqrt(x * x + z * z);

		if (z < x) {
			result = acosf(z / result);

			if (x < 0) {
				result = M_TAU - result;
			}
		} else {
			result = acosf(x / result);
			result = 1.5707963705063f - result;

			if (z < 0) {
				result = M_PI - result;
			}

			if (result < 0) {
				result = result + M_TAU;
			}
		}
	}

	return result;
#endif
}
