#ifndef QIRAN_PVS_SEQ_H
#define QIRAN_PVS_SEQ_H

#include "qiran/qiran_types.h"

/*
 * Photonic verification sequence across its measurement tiers.
 * Owner: photonic team. Functional API is supplied by the owning team; only the lifecycle hook is
 * reserved here.
 */
qiran_status_t pvs_seq_init(void);

#endif
