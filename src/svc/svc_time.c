#include "qiran/svc/svc_time.h"

#include "qiran/exec/exec_core.h"
#include "qiran/qiran_config.h"

static uint32_t s_last_raw;
static uint64_t s_ticks;
static uint64_t s_epoch_ms;
static bool     s_epoch_valid;

void svc_time_init(void)
{
    s_last_raw = exec_raw_tick_count();
    s_ticks = 0U;
    s_epoch_ms = 0U;
    s_epoch_valid = false;
}

uint64_t svc_time_now_ms(void)
{
    uint32_t raw = exec_raw_tick_count();

    s_ticks += (uint64_t)(raw - s_last_raw);
    s_last_raw = raw;

    return s_ticks * QIRAN_MINOR_CYCLE_MS;
}

uint32_t svc_time_tick(void)
{
    return exec_raw_tick_count();
}

void svc_time_epoch_set(uint64_t obc_time_ms)
{
    s_epoch_ms = obc_time_ms - svc_time_now_ms();
    s_epoch_valid = true;
}

bool svc_time_epoch_valid(void)
{
    return s_epoch_valid;
}

uint64_t svc_time_epoch_ms(void)
{
    return s_epoch_ms + svc_time_now_ms();
}

void svc_timeout_arm(svc_timeout_t *t, uint32_t duration_ms)
{
    if (t == NULL) {
        return;
    }
    t->deadline_ms = svc_time_now_ms() + (uint64_t)duration_ms;
    t->armed = true;
}

void svc_timeout_disarm(svc_timeout_t *t)
{
    if (t != NULL) {
        t->armed = false;
    }
}

bool svc_timeout_expired(const svc_timeout_t *t)
{
    if ((t == NULL) || !t->armed) {
        return false;
    }
    return svc_time_now_ms() >= t->deadline_ms;
}

uint32_t svc_timeout_remaining_ms(const svc_timeout_t *t)
{
    uint64_t now;

    if ((t == NULL) || !t->armed) {
        return 0U;
    }

    now = svc_time_now_ms();
    return (now >= t->deadline_ms) ? 0U : (uint32_t)(t->deadline_ms - now);
}

uint32_t svc_timeout_elapsed_ms(const svc_timeout_t *t, uint32_t duration_ms)
{
    return duration_ms - svc_timeout_remaining_ms(t);
}
