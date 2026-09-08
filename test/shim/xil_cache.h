#ifndef SHIM_XIL_CACHE_H
#define SHIM_XIL_CACHE_H
#include <stdint.h>
typedef uintptr_t INTPTR;
void Xil_DCacheFlushRange(INTPTR addr, uint32_t len);
void Xil_DCacheInvalidateRange(INTPTR addr, uint32_t len);
#endif
