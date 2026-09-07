#ifndef QIRAN_PHOTONIC_MRR_H
#define QIRAN_PHOTONIC_MRR_H

#include "qiran/qiran_types.h"

/*
 * Micro-ring resonator dither-lock acquisition and hold.
 * Owner: photonic team. Functional API is supplied by the owning team; only the lifecycle hook is
 * reserved here.
 */
qiran_status_t photonic_mrr_init(void);

#endif
