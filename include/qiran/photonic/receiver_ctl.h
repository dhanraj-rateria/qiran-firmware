#ifndef QIRAN_RECEIVER_CTL_H
#define QIRAN_RECEIVER_CTL_H

#include "qiran/qiran_types.h"

/*
 * Single-photon detector enable under interlock, bias ramp and dark-count check.
 * Owner: photonic team. Functional API is supplied by the owning team; only the lifecycle hook is
 * reserved here.
 */
qiran_status_t receiver_ctl_init(void);

#endif
