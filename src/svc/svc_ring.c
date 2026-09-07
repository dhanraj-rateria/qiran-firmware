#include "qiran/svc/svc_ring.h"

#include "qiran/plat/plat_cpu.h"

qiran_status_t svc_ring_init(svc_ring_t *r, uint8_t *storage, uint32_t capacity)
{
    if ((r == NULL) || (storage == NULL) || (capacity == 0U)) {
        return QIRAN_ERR_PARAM;
    }
    if ((capacity & (capacity - 1U)) != 0U) {
        return QIRAN_ERR_PARAM;
    }

    r->buf = storage;
    r->mask = capacity - 1U;
    r->head = 0U;
    r->tail = 0U;
    r->dropped = 0U;

    return QIRAN_OK;
}

void svc_ring_reset(svc_ring_t *r)
{
    if (r != NULL) {
        r->head = 0U;
        r->tail = 0U;
        r->dropped = 0U;
    }
}

bool svc_ring_push(svc_ring_t *r, uint8_t byte)
{
    uint32_t head = r->head;

    if ((head - r->tail) > r->mask) {
        r->dropped++;
        return false;
    }

    r->buf[head & r->mask] = byte;
    /* The element must be visible before the index that publishes it. */
    plat_cpu_memory_barrier();
    r->head = head + 1U;

    return true;
}

bool svc_ring_pop(svc_ring_t *r, uint8_t *out)
{
    uint32_t tail = r->tail;

    if (r->head == tail) {
        return false;
    }

    *out = r->buf[tail & r->mask];
    plat_cpu_memory_barrier();
    r->tail = tail + 1U;

    return true;
}

uint32_t svc_ring_read(svc_ring_t *r, uint8_t *out, uint32_t max)
{
    uint32_t n = 0U;

    while ((n < max) && svc_ring_pop(r, &out[n])) {
        n++;
    }

    return n;
}

uint32_t svc_ring_count(const svc_ring_t *r)
{
    return r->head - r->tail;
}

uint32_t svc_ring_free(const svc_ring_t *r)
{
    return (r->mask + 1U) - (r->head - r->tail);
}

uint32_t svc_ring_dropped(const svc_ring_t *r)
{
    return r->dropped;
}
