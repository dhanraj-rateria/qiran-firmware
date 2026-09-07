#ifndef SHIM_XSCUTIMER_H
#define SHIM_XSCUTIMER_H
#include <stdint.h>
#include "xparameters.h"
typedef struct { uint32_t DeviceId; uint32_t BaseAddr; } XScuTimer_Config;
typedef struct { XScuTimer_Config Config; uint32_t IsReady; uint32_t IsStarted; } XScuTimer;
XScuTimer_Config *XScuTimer_LookupConfig(uint32_t id);
int  XScuTimer_CfgInitialize(XScuTimer *p, XScuTimer_Config *c, uint32_t base);
void XScuTimer_SetPrescaler(XScuTimer *p, uint8_t v);
void XScuTimer_LoadTimer(XScuTimer *p, uint32_t v);
void XScuTimer_EnableAutoReload(XScuTimer *p);
void XScuTimer_EnableInterrupt(XScuTimer *p);
void XScuTimer_Start(XScuTimer *p);
void XScuTimer_Stop(XScuTimer *p);
void XScuTimer_ClearInterruptStatus(XScuTimer *p);
#endif
