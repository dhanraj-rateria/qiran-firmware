#ifndef QIRAN_PLAT_IRQ_H
#define QIRAN_PLAT_IRQ_H

#include "qiran/qiran_config.h"
#include "qiran/qiran_types.h"

/*
 * The complete interrupt set. Adding a source means extending this enumeration,
 * which fails the count assertion below and forces the addition to be a
 * deliberate, reviewed change rather than an incidental one.
 */
typedef enum {
    PLAT_IRQ_TIMER = 0,
    PLAT_IRQ_WATCHDOG,
    PLAT_IRQ_DMA,
    PLAT_IRQ_SPW,
    PLAT_IRQ_UART,
    PLAT_IRQ_PL_ERROR,
    PLAT_IRQ_COUNT
} plat_irq_id_t;

QIRAN_STATIC_ASSERT(PLAT_IRQ_COUNT == 6, interrupt_set_is_exactly_six);

typedef struct {
    uint32_t tick;   /* tick at which the interrupt was taken */
    uint32_t datum;  /* device status snapshot from the acknowledge hook */
    uint16_t kind;   /* source-specific classification */
    uint16_t seq;    /* per-source sequence; a gap means events were dropped */
} plat_irq_event_t;

/*
 * Acknowledges the source at the device and returns a status snapshot, setting
 * *kind to classify it. Called in interrupt context, so the implementation must
 * be bounded, must not block, and must not use floating point. Supplied by the
 * device owner because the register-level detail is theirs, not the framework's.
 */
typedef uint32_t (*plat_irq_ack_t)(void *ctx, uint16_t *kind);

typedef struct {
    uint32_t       irq_id;   /* controller interrupt number */
    uint8_t        trigger;
    plat_irq_ack_t ack;
    void          *ctx;
} plat_irq_source_t;

typedef struct {
    uint32_t taken;         /* interrupts serviced */
    uint32_t dropped;       /* events lost to a full queue */
    uint32_t cycles_last;   /* handler duration, previous entry */
    uint32_t cycles_worst;  /* handler duration, high-water mark */
} plat_irq_stats_t;

void plat_irq_init(void);

/*
 * Connects a source. Priority is chosen by the framework from the source
 * identity rather than passed in, so the relative ordering of the interrupt set
 * cannot be set inconsistently at a call site.
 */
qiran_status_t plat_irq_register(plat_irq_id_t id, const plat_irq_source_t *src);

void plat_irq_enable(plat_irq_id_t id);
void plat_irq_disable(plat_irq_id_t id);

bool     plat_irq_registered(plat_irq_id_t id);
bool     plat_irq_take(plat_irq_id_t id, plat_irq_event_t *out);
uint32_t plat_irq_pending(plat_irq_id_t id);
void     plat_irq_stats_get(plat_irq_id_t id, plat_irq_stats_t *out);

const char *plat_irq_name(plat_irq_id_t id);

#endif
