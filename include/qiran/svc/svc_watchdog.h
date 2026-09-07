#ifndef QIRAN_SVC_WATCHDOG_H
#define QIRAN_SVC_WATCHDOG_H

#include "qiran/qiran_types.h"

/*
 * The watchdog device is supplied by the platform rather than assumed here,
 * because which timer issues the processor reset is still a hardware-design
 * decision. Registering a backend is the only change required once it is fixed;
 * with no backend registered the gate logic still runs and is observable, but
 * nothing is armed. That is the intended state during executive bring-up.
 */
typedef struct {
    qiran_status_t (*arm)(uint32_t timeout_ms);
    void           (*kick)(void);
} svc_watchdog_backend_t;

qiran_status_t svc_watchdog_init(const svc_watchdog_backend_t *backend);

/*
 * Services the watchdog only while the aggregate health verdict is healthy, so
 * a detected-unhealthy system is allowed to time out rather than being held
 * alive by an unconditional tickle.
 */
void watchdog_tickle_if_healthy(void);

bool     svc_watchdog_armed(void);
uint32_t svc_watchdog_suppressed_cycles(void);

#endif
