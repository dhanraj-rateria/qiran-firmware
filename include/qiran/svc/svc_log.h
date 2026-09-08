#ifndef QIRAN_SVC_LOG_H
#define QIRAN_SVC_LOG_H

#include "qiran/qiran_types.h"
#include "qiran/svc/svc_fault.h"

typedef enum {
    SVC_LOG_EVENT = 0,
    SVC_LOG_FAULT,
    SVC_LOG_TRACE
} svc_log_category_t;

typedef struct {
    uint32_t tick;
    uint32_t detail;
    uint16_t code;
    uint8_t  severity;
    uint8_t  category;
} svc_log_record_t;

void svc_log_init(void);

/*
 * All three are safe from any context, including interrupt context, and never
 * block. Every severity tier logs, not only the lower two.
 */
void svc_log_event(uint16_t code, uint32_t detail);
void svc_log_fault(qiran_fault_id_t id, qiran_severity_t severity, uint32_t detail);
void svc_log_trace(uint16_t code, uint32_t detail);

/* Drains records for flush to storage or downlink. */
uint32_t svc_log_read(svc_log_record_t *out, uint32_t max);

uint32_t svc_log_count(void);
uint32_t svc_log_dropped(void);
uint32_t svc_log_total(void);

/* Cumulative fault entries, reported as a housekeeping field. */
uint32_t svc_log_fault_total(void);

#endif
