#ifndef QIRAN_SVC_RING_H
#define QIRAN_SVC_RING_H

#include "qiran/qiran_types.h"

/*
 * Byte ring for handing data from one interrupt to one loop consumer. Indices
 * are free-running counters masked on use, so the structure is lock free for a
 * single producer and single consumer: neither side writes the other's index
 * and neither ever clears shared state. Both producer and consumer must be a
 * single context each, and both must run on the same core.
 */
typedef struct {
    uint8_t          *buf;
    uint32_t          mask;
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t dropped;
} svc_ring_t;

/* Capacity must be a non-zero power of two. */
qiran_status_t svc_ring_init(svc_ring_t *r, uint8_t *storage, uint32_t capacity);

void svc_ring_reset(svc_ring_t *r);

/* Producer side. Returns false and counts a drop when the ring is full. */
bool svc_ring_push(svc_ring_t *r, uint8_t byte);

/* Consumer side. */
bool     svc_ring_pop(svc_ring_t *r, uint8_t *out);
uint32_t svc_ring_read(svc_ring_t *r, uint8_t *out, uint32_t max);

uint32_t svc_ring_count(const svc_ring_t *r);
uint32_t svc_ring_free(const svc_ring_t *r);
uint32_t svc_ring_dropped(const svc_ring_t *r);

#endif
