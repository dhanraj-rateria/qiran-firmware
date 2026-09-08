#ifndef QIRAN_SVC_FDIR_H
#define QIRAN_SVC_FDIR_H

#include "qiran/qiran_types.h"
#include "qiran/svc/svc_fault.h"

typedef struct {
    qiran_severity_t   severity;
    qiran_escalation_t escalation;
    /*
     * Occurrences of a Critical fault tolerated before it is elevated to
     * Severe. This is also the per-stage retry limit: a stage's retry counter
     * is this counter, not a second mechanism beside it.
     */
    uint16_t           limit;
} svc_fdir_policy_t;

typedef struct {
    uint32_t occurrences;
    uint32_t escalations;
    uint32_t last_tick;
    uint32_t last_detail;
    bool     flag;
    bool     elevated;
} svc_fdir_record_t;

/*
 * Delivery of a fault to the observer. Registered rather than assumed, because
 * the housekeeping link is not yet implemented. Undelivered escalations are
 * counted, so an unregistered uplink is visible instead of silent.
 */
typedef struct {
    qiran_status_t (*report_flag)(qiran_fault_id_t id,
                                  qiran_severity_t severity,
                                  uint32_t detail);
    qiran_status_t (*request_power_reset)(qiran_fault_id_t id);
} svc_fdir_uplink_t;

void           svc_fdir_init(void);
qiran_status_t svc_fdir_set_uplink(const svc_fdir_uplink_t *uplink);

/*
 * The single reporting path for every fault in the system. Safe from any
 * context including interrupt context, never blocks, and performs no delivery
 * itself: classification and escalation happen in the cyclic loop, so no
 * interrupt ever waits on a link transaction.
 */
void svc_fdir_report(qiran_fault_id_t id, uint32_t detail);

/*
 * Classifies everything reported since the previous call, escalates what has
 * reached its limit, and collects the fault counters owned by the platform.
 */
void error_handling_service(void);

/* Stage-local retry support. The occurrence count is the retry count. */
uint32_t svc_fdir_occurrences(qiran_fault_id_t id);
bool     svc_fdir_retries_exhausted(qiran_fault_id_t id);

/* Called on success, so a later failure of the same class starts from zero. */
void     svc_fdir_clear(qiran_fault_id_t id);

bool     svc_fdir_flag(qiran_fault_id_t id);
void     svc_fdir_record_get(qiran_fault_id_t id, svc_fdir_record_t *out);
const svc_fdir_policy_t *svc_fdir_policy(qiran_fault_id_t id);

uint32_t svc_fdir_severity_count(qiran_severity_t severity);
uint32_t svc_fdir_undelivered(void);

/* Re-entry accounting, shared by every stage so the limit is global. */
void     svc_fdir_reentry_note(qiran_fault_id_t cause);
uint32_t svc_fdir_reentry_count(void);
bool     svc_fdir_reentry_exceeded(void);

#endif
