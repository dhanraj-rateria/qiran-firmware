#ifndef QIRAN_COMM_SPW_LINK_H
#define QIRAN_COMM_SPW_LINK_H

#include "qiran/qiran_types.h"

/*
 * Manages the processor side of the serial data link. The link's own state
 * machine lives in the fabric; what is here is bringing it up, watching it, and
 * deciding what to do when it drops.
 */
typedef enum {
    SPW_LINK_DOWN = 0,    /* not started */
    SPW_LINK_STARTING,    /* start commanded, waiting for it to run */
    SPW_LINK_RUN,         /* usable */
    SPW_LINK_FAILED       /* start attempts exhausted */
} comm_spw_state_t;

/*
 * Supplied by whoever can reach the fabric registers. status reports whether
 * the link is running, and returns the raw status word for telemetry.
 */
typedef struct {
    qiran_status_t (*start)(void *ctx);
    qiran_status_t (*status)(void *ctx, bool *running, uint32_t *status_word);
    qiran_status_t (*send)(void *ctx, const uint8_t *data, uint32_t len);
    uint32_t       (*receive)(void *ctx, uint8_t *buf, uint32_t max);
    void          *ctx;
} comm_spw_ops_t;

void comm_spw_link_init(void);

qiran_status_t comm_spw_link_set_ops(const comm_spw_ops_t *ops);

/* Collects the errors the interrupt published. */
void comm_spw_link_service_interrupts(void);

/* Drives the bring-up and delivers received packets. */
void comm_spw_link_service(void);

comm_spw_state_t comm_spw_link_state(void);
const char      *comm_spw_link_state_name(comm_spw_state_t state);
bool             comm_spw_link_running(void);

qiran_status_t comm_spw_link_send(const uint8_t *data, uint32_t len);

uint32_t comm_spw_link_start_attempts(void);
uint32_t comm_spw_link_disconnects(void);
uint32_t comm_spw_link_errors(void);
uint32_t comm_spw_link_packets_sent(void);
uint32_t comm_spw_link_packets_received(void);
uint32_t comm_spw_link_status_word(void);

/*
 * Status as it stood when the link last raised an error, which the polled
 * status does not preserve: by the time it is read the link may have recovered,
 * and the value that explains the error would be gone.
 */
uint32_t comm_spw_link_last_error_word(void);

#endif
