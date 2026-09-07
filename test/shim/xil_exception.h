#ifndef SHIM_XIL_EXCEPTION_H
#define SHIM_XIL_EXCEPTION_H
#include <stdint.h>
#define XIL_EXCEPTION_ID_IRQ_INT 5U
typedef void (*Xil_ExceptionHandler)(void *);
void Xil_ExceptionRegisterHandler(uint32_t id, Xil_ExceptionHandler h, void *ref);
void Xil_ExceptionEnable(void);
#endif
