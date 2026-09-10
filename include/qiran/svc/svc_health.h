#ifndef QIRAN_SVC_HEALTH_H
#define QIRAN_SVC_HEALTH_H

#include "qiran/qiran_types.h"
#include "qiran/svc/svc_fault.h"

#define SVC_HEALTH_MONITOR_MAX 16U

/*
 * Whether a monitor's failure may withhold the watchdog service.
 *
 * The watchdog's remedy is a processor reset, which helps only what a restart
 * could clear: internal state that has lost integrity, or software no longer
 * meeting its deadlines. A payload condition such as an over-current has a
 * different and already specified remedy, a power-reset request followed by
 * continued execution. Letting one withhold the watchdog would add an
 * unrequested processor reset on top of the response asked for, so those
 * monitors are advisory.
 */
typedef enum {
    SVC_HEALTH_ADVISORY = 0,
    SVC_HEALTH_GATING
} svc_health_class_t;

/*
 * Called on the monitor's own period. Returns QIRAN_OK when satisfied and sets
 * *value to the reading worth reporting. Must not block.
 */
typedef qiran_status_t (*svc_health_check_t)(void *ctx, uint32_t *value);

typedef struct {
    const char    *name;
    uint32_t       runs;
    uint32_t       failures;
    uint32_t       consecutive;
    uint32_t       last_value;
    qiran_status_t last_status;
    uint32_t       period_cycles;
    uint32_t       tolerance;
    bool           gating;
    bool           contributing;
} svc_health_monitor_stat_t;

void svc_health_init(void);

/*
 * period_cycles is in minor cycles and at least one; a phase is assigned so
 * monitors registered in sequence do not all fall due on the same cycle.
 *
 * tolerance is how many consecutive failures are allowed before the monitor
 * affects the verdict.
 *
 * fault may be QIRAN_FAULT_NONE for a condition already reported by whoever
 * owns it, so it informs the verdict without being reported twice.
 */
qiran_status_t svc_health_register(const char *name,
                                   svc_health_check_t check,
                                   void *ctx,
                                   svc_health_class_t monitor_class,
                                   uint32_t period_cycles,
                                   uint32_t tolerance,
                                   qiran_fault_id_t fault);

/* Runs the monitors that are due and records what they found. */
void health_check(void);

/* Forms the verdict from what they found. */
void health_consolidate(void);

qiran_health_t svc_health_state(void);

uint32_t svc_health_count(void);
uint32_t svc_health_gating_count(void);
uint32_t svc_health_transitions(void);
bool     svc_health_monitor_stat(uint32_t index, svc_health_monitor_stat_t *out);
bool     svc_health_all_passing(void);

#endif
