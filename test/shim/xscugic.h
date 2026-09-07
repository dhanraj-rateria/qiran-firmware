#ifndef SHIM_XSCUGIC_H
#define SHIM_XSCUGIC_H
#include <stdint.h>
#include "xparameters.h"
typedef struct { uint32_t DeviceId; uint32_t CpuBaseAddress; uint32_t DistBaseAddress; } XScuGic_Config;
typedef struct { XScuGic_Config *Config; uint32_t IsReady; } XScuGic;
typedef void (*Xil_InterruptHandler)(void *);
XScuGic_Config *XScuGic_LookupConfig(uint32_t id);
int  XScuGic_CfgInitialize(XScuGic *p, XScuGic_Config *c, uint32_t base);
int  XScuGic_Connect(XScuGic *p, uint32_t id, Xil_InterruptHandler h, void *ref);
void XScuGic_Enable(XScuGic *p, uint32_t id);
void XScuGic_Disable(XScuGic *p, uint32_t id);
void XScuGic_SetPriorityTriggerType(XScuGic *p, uint32_t id, uint8_t pr, uint8_t tr);
void XScuGic_InterruptHandler(void *p);
#endif
