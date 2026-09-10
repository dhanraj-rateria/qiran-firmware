#include "qiran/plat/plat_irq.h"

#include "qiran/exec/exec_core.h"
#include "qiran/plat/plat_cpu.h"
#include "qiran/plat/plat_gic.h"

#define Q_MASK (QIRAN_IRQ_QUEUE_DEPTH - 1U)

typedef struct {
    plat_irq_source_t src;
    bool              registered;

    plat_irq_event_t  q[QIRAN_IRQ_QUEUE_DEPTH];
    volatile uint32_t head;
    volatile uint32_t tail;

    volatile uint32_t taken;
    volatile uint32_t dropped;
    volatile uint32_t cycles_last;
    volatile uint32_t cycles_worst;
    uint16_t          seq;
} irq_slot_t;

static irq_slot_t s_slot[PLAT_IRQ_COUNT];

static const uint8_t k_priority[PLAT_IRQ_COUNT] = {
    PLAT_GIC_PRIO_TIMER,
    PLAT_GIC_PRIO_WATCHDOG,
    PLAT_GIC_PRIO_DMA,
    PLAT_GIC_PRIO_SPW,
    PLAT_GIC_PRIO_UART,
    PLAT_GIC_PRIO_PL_ERROR
};

static const char *const k_name[PLAT_IRQ_COUNT] = {
    "timer", "watchdog", "dma", "spw", "uart", "plerr"
};

/*
 * The one handler body shared by every registered source: acknowledge at the
 * device, timestamp, publish one fixed-size event, return. It contains no loop,
 * no blocking call and no floating point, so the interrupt-context rules hold
 * structurally for the whole interrupt set rather than per hand-written handler.
 * Its own duration is measured here, which is the on-target execution time the
 * interrupt framework has to report.
 */
static void irq_handler(void *ref)
{
    irq_slot_t *s = (irq_slot_t *)ref;
    uint32_t head;
    uint16_t kind = 0U;
    uint32_t datum;
#if QIRAN_INSTRUMENT
    uint32_t start = plat_cpu_cycle_count();
    uint32_t elapsed;
#endif

    datum = s->src.ack(s->src.ctx, &kind);

    s->taken++;
    head = s->head;

    if ((head - s->tail) > Q_MASK) {
        s->dropped++;
    } else {
        plat_irq_event_t *e = &s->q[head & Q_MASK];

        e->tick = exec_raw_tick_count();
        e->datum = datum;
        e->kind = kind;
        e->seq = s->seq;
        plat_cpu_memory_barrier();
        s->head = head + 1U;
    }

    /* Advanced even on drop, so a consumer sees the discontinuity. */
    s->seq++;

#if QIRAN_INSTRUMENT
    elapsed = plat_cpu_cycle_count() - start;
    s->cycles_last = elapsed;
    if (elapsed > s->cycles_worst) {
        s->cycles_worst = elapsed;
    }
#endif
}

void plat_irq_init(void)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)PLAT_IRQ_COUNT; i++) {
        irq_slot_t *s = &s_slot[i];

        s->src.irq_id = 0U;
        s->src.trigger = 0U;
        s->src.ack = NULL;
        s->src.ctx = NULL;
        s->registered = false;
        s->head = 0U;
        s->tail = 0U;
        s->taken = 0U;
        s->dropped = 0U;
        s->cycles_last = 0U;
        s->cycles_worst = 0U;
        s->seq = 0U;
    }
}

qiran_status_t plat_irq_register(plat_irq_id_t id, const plat_irq_source_t *src)
{
    irq_slot_t *s;
    qiran_status_t st;

    if ((id >= PLAT_IRQ_COUNT) || (src == NULL) || (src->ack == NULL)) {
        return QIRAN_ERR_PARAM;
    }

    s = &s_slot[id];
    if (s->registered) {
        return QIRAN_ERR_STATE;
    }

    s->src = *src;

    st = plat_gic_connect(src->irq_id, irq_handler, s, k_priority[id],
                          src->trigger);
    if (st != QIRAN_OK) {
        return st;
    }

    s->registered = true;
    return QIRAN_OK;
}

void plat_irq_enable(plat_irq_id_t id)
{
    if ((id < PLAT_IRQ_COUNT) && s_slot[id].registered) {
        plat_gic_enable(s_slot[id].src.irq_id);
    }
}

void plat_irq_disable(plat_irq_id_t id)
{
    if ((id < PLAT_IRQ_COUNT) && s_slot[id].registered) {
        plat_gic_disable(s_slot[id].src.irq_id);
    }
}

bool plat_irq_registered(plat_irq_id_t id)
{
    return (id < PLAT_IRQ_COUNT) && s_slot[id].registered;
}

bool plat_irq_take(plat_irq_id_t id, plat_irq_event_t *out)
{
    irq_slot_t *s;
    uint32_t tail;

    if ((id >= PLAT_IRQ_COUNT) || (out == NULL)) {
        return false;
    }

    s = &s_slot[id];
    tail = s->tail;

    if (s->head == tail) {
        return false;
    }

    *out = s->q[tail & Q_MASK];
    plat_cpu_memory_barrier();
    s->tail = tail + 1U;

    return true;
}

uint32_t plat_irq_pending(plat_irq_id_t id)
{
    if (id >= PLAT_IRQ_COUNT) {
        return 0U;
    }
    return s_slot[id].head - s_slot[id].tail;
}

void plat_irq_stats_get(plat_irq_id_t id, plat_irq_stats_t *out)
{
    if ((id >= PLAT_IRQ_COUNT) || (out == NULL)) {
        return;
    }

    out->taken = s_slot[id].taken;
    out->dropped = s_slot[id].dropped;
    out->cycles_last = s_slot[id].cycles_last;
    out->cycles_worst = s_slot[id].cycles_worst;
}

const char *plat_irq_name(plat_irq_id_t id)
{
    return (id < PLAT_IRQ_COUNT) ? k_name[id] : "?";
}
