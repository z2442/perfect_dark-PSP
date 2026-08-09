#ifndef PD_PSP_VFPU_H
#define PD_PSP_VFPU_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *pdPspMemcpyVfpu(void *dst, const void *src, size_t size);

#ifdef __cplusplus
}
#endif

#endif
