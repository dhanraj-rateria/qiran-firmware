#ifndef QIRAN_SVC_LOG_H
#define QIRAN_SVC_LOG_H

#include "qiran/qiran_types.h"

/*
 * Event, fault and trace log. Every severity tier writes here.
 * Owner: Major Task 1. Not yet implemented.
 */
qiran_status_t svc_log_init(void);

#endif
