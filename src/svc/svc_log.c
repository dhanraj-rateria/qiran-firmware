#include "qiran/svc/svc_log.h"

#include "qiran/exec/exec_core.h"
#include "qiran/plat/plat_cpu.h"
#include "qiran/qiran_config.h"

#define LOG_MASK (QIRAN_LOG_DEPTH - 1U)

static svc_log_record_t  s_buf[QIRAN_LOG_DEPTH];
static volatile uint32_t s_head;
static volatile uint32_t s_tail;
static volatile uint32_t s_dropped;
static volatile uint32_t s_total;
static volatile uint32_t s_fault_total;

/*
 * Writers may be several different interrupts as well as the cyclic loop, which
 * is more than one producer, so the index update is briefly guarded rather than
 * relying on the single-producer discipline used elsewhere. The guarded region
 * is a compare, a copy and two increments.
 */
static void push(uint16_t code, uint8_t category, uint8_t severity, uint32_t detail)
{
    uint32_t prior = plat_cpu_irq_disable();
    uint32_t head = s_head;

    if ((head - s_tail) > LOG_MASK) {
        s_dropped++;
    } else {
        svc_log_record_t *r = &s_buf[head & LOG_MASK];

        r->tick = exec_raw_tick_count();
        r->detail = detail;
        r->code = code;
        r->severity = severity;
        r->category = category;
        s_head = head + 1U;
    }

    s_total++;
    if (category == (uint8_t)SVC_LOG_FAULT) {
        s_fault_total++;
    }

    plat_cpu_irq_restore(prior);
}

void svc_log_init(void)
{
    s_head = 0U;
    s_tail = 0U;
    s_dropped = 0U;
    s_total = 0U;
    s_fault_total = 0U;
}

void svc_log_event(uint16_t code, uint32_t detail)
{
    push(code, (uint8_t)SVC_LOG_EVENT, 0U, detail);
}

void svc_log_fault(qiran_fault_id_t id, qiran_severity_t severity, uint32_t detail)
{
    push((uint16_t)id, (uint8_t)SVC_LOG_FAULT, (uint8_t)severity, detail);
}

void svc_log_trace(uint16_t code, uint32_t detail)
{
    push(code, (uint8_t)SVC_LOG_TRACE, 0U, detail);
}

/*
 * Consumer side. Only the cyclic loop drains, so the tail needs no guard: a
 * writer that reads a stale tail can only conclude the log is fuller than it
 * is, which drops a record rather than corrupting one.
 */
uint32_t svc_log_read(svc_log_record_t *out, uint32_t max)
{
    uint32_t n = 0U;

    if (out == NULL) {
        return 0U;
    }

    while (n < max) {
        uint32_t tail = s_tail;

        if (s_head == tail) {
            break;
        }

        out[n] = s_buf[tail & LOG_MASK];
        plat_cpu_memory_barrier();
        s_tail = tail + 1U;
        n++;
    }

    return n;
}

uint32_t svc_log_count(void)       { return s_head - s_tail; }
uint32_t svc_log_dropped(void)     { return s_dropped; }
uint32_t svc_log_total(void)       { return s_total; }
uint32_t svc_log_fault_total(void) { return s_fault_total; }
