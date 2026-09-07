#ifndef QIRAN_SVC_HEALTH_H
#define QIRAN_SVC_HEALTH_H

#include "qiran/qiran_types.h"

void svc_health_init(void);
void health_monitor_periodic(void);

/* Aggregate verdict consumed by the watchdog gate. */
qiran_health_t svc_health_state(void);

#endif
