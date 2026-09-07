#ifndef QIRAN_SVC_TIME_H
#define QIRAN_SVC_TIME_H

#include "qiran/qiran_types.h"

typedef struct {
    uint64_t deadline_ms;
    bool     armed;
} svc_timeout_t;

void svc_time_init(void);

/*
 * Milliseconds since svc_time_init(), at minor-cycle granularity. Main-loop
 * and boot context only: it extends the 32-bit tick counter to 64 bits and is
 * therefore not re-entrant.
 */
uint64_t svc_time_now_ms(void);

/* Raw minor-cycle tick count. Interrupt-safe; use this to timestamp in an ISR. */
uint32_t svc_time_tick(void);

/*
 * Correlates payload uptime with the timebase supplied by the OBC, so logged
 * and telemetered timestamps can be placed on a common scale.
 */
void     svc_time_epoch_set(uint64_t obc_time_ms);
bool     svc_time_epoch_valid(void);
uint64_t svc_time_epoch_ms(void);

void     svc_timeout_arm(svc_timeout_t *t, uint32_t duration_ms);
void     svc_timeout_disarm(svc_timeout_t *t);
bool     svc_timeout_expired(const svc_timeout_t *t);
uint32_t svc_timeout_remaining_ms(const svc_timeout_t *t);
uint32_t svc_timeout_elapsed_ms(const svc_timeout_t *t, uint32_t duration_ms);

#endif
