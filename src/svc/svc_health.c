#include "qiran/svc/svc_health.h"

static qiran_health_t s_state;

void svc_health_init(void)
{
    s_state = QIRAN_HEALTH_HEALTHY;
}

void health_monitor_periodic(void)
{
}

qiran_health_t svc_health_state(void)
{
    return s_state;
}
