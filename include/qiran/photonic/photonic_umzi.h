#ifndef QIRAN_PHOTONIC_UMZI_H
#define QIRAN_PHOTONIC_UMZI_H

#include "qiran/qiran_types.h"

/*
 * Unbalanced Mach-Zehnder arm tuning and splitting-ratio optimisation.
 * Owner: photonic team. Functional API is supplied by the owning team; only the lifecycle hook is
 * reserved here.
 */
qiran_status_t photonic_umzi_init(void);

#endif
