#ifndef QIRAN_COMM_OUTPUT_H
#define QIRAN_COMM_OUTPUT_H

#include "qiran/comm/comm_ccsds.h"
#include "qiran/qiran_types.h"

/*
 * Outbound queue, ordered by what a packet carries rather than by when it was
 * queued.
 *
 * Ordering matters because the bulk of what leaves the payload is raw science,
 * measured in hundreds of megabits. Sent in arrival order it would sit ahead of
 * every fault report and every status frame behind it for as long as the
 * transfer took, which is exactly when those are most worth having. So a fault
 * report goes first, then status, then processed results, and raw science last.
 *
 * The queue holds descriptors, not copies. The buffer a descriptor points at
 * belongs to the queue until it has been sent, and the producer must not touch
 * it in the meantime; that is the same single-owner rule the data path uses.
 */
#define COMM_OUTPUT_DEPTH 16U

typedef struct {
    const uint8_t *data;
    uint32_t       len;
    ccsds_class_t  cls;
} comm_output_item_t;

void comm_output_init(void);

/*
 * Queues application data to be packetised and sent. Returns busy when the
 * queue is full, which is counted and reported rather than silently dropping
 * the oldest item: losing science already computed is worse than refusing to
 * accept more.
 */
qiran_status_t comm_output_submit(ccsds_class_t cls, const uint8_t *data,
                                  uint32_t len);

/* Sends as much as the link will take, highest priority first. */
void comm_output_service(void);

uint32_t comm_output_pending(void);
uint32_t comm_output_high_water(void);
uint32_t comm_output_submitted(void);
uint32_t comm_output_sent(void);
uint32_t comm_output_rejected(void);
uint32_t comm_output_failed(void);
uint8_t  comm_output_priority_of(ccsds_class_t cls);

#endif
