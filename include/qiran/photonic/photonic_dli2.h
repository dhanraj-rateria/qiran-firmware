#ifndef QIRAN_PHOTONIC_DLI2_H
#define QIRAN_PHOTONIC_DLI2_H

#include "qiran/qiran_types.h"

/*
 * Delay-line interferometer thermal settling and phase lock, second channel.
 * Owner: photonic team. Functional API is supplied by the owning team; only the lifecycle hook is
 * reserved here.
 */
qiran_status_t photonic_dli2_init(void);

#endif
