#include "qiran/svc/svc_watchdog.h"

#include "qiran/qiran_config.h"
#include "qiran/svc/svc_health.h"

static svc_watchdog_backend_t s_backend;
static bool     s_armed;
static uint32_t s_suppressed;

qiran_status_t svc_watchdog_init(const svc_watchdog_backend_t *backend)
{
    s_armed = false;
    s_suppressed = 0U;

    if (backend == NULL) {
        s_backend.arm = NULL;
        s_backend.kick = NULL;
        return QIRAN_OK;
    }

    if ((backend->arm == NULL) || (backend->kick == NULL)) {
        return QIRAN_ERR_PARAM;
    }

    s_backend = *backend;

    if (s_backend.arm(QIRAN_WATCHDOG_TIMEOUT_MS) != QIRAN_OK) {
        return QIRAN_ERR_HARDWARE;
    }

    s_armed = true;
    return QIRAN_OK;
}

void watchdog_tickle_if_healthy(void)
{
    if (svc_health_state() != QIRAN_HEALTH_HEALTHY) {
        s_suppressed++;
        return;
    }

    if (s_armed) {
        s_backend.kick();
    }
}

bool svc_watchdog_armed(void)
{
    return s_armed;
}

uint32_t svc_watchdog_suppressed_cycles(void)
{
    return s_suppressed;
}
