#ifndef QIRAN_SRC_LASER_H
#define QIRAN_SRC_LASER_H

#include "qiran/qiran_types.h"

/*
 * Laser thermal stabilisation and bias-current ramp control.
 * Owner: photonic team. Functional API is supplied by the owning team; only the lifecycle hook is
 * reserved here.
 */
qiran_status_t src_laser_init(void);

#endif
