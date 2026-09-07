#ifndef QIRAN_PLAT_ISR_UART_H
#define QIRAN_PLAT_ISR_UART_H

#include "qiran/plat/plat_irq.h"
#include "qiran/qiran_types.h"
#include "qiran/svc/svc_ring.h"

/*
 * Event classifications published by the receive interrupt. Frame assembly is
 * deliberately not done here: the command frame format is not yet fixed, and
 * parsing in interrupt context would make handler duration depend on content.
 * The interrupt reports a content-independent inter-frame idle gap instead, and
 * the communication layer assembles frames from the ring in the cyclic loop.
 */
#define PLAT_UART_KIND_DATA  0U
#define PLAT_UART_KIND_GAP   1U
#define PLAT_UART_KIND_ERROR 2U

typedef struct {
    /*
     * Moves all currently available received bytes into the ring, returns a
     * device status snapshot and sets *kind. Called in interrupt context: must
     * be bounded by the device receive depth, must not block, no floating point.
     */
    uint32_t (*drain)(void *ctx, svc_ring_t *rx, uint16_t *kind);
    void    *ctx;
    uint32_t irq_id;
    uint8_t  trigger;
} plat_isr_uart_device_t;

/* Registering with a NULL device leaves the ring usable but nothing connected. */
qiran_status_t plat_isr_uart_init(const plat_isr_uart_device_t *dev);

uint32_t plat_isr_uart_read(uint8_t *out, uint32_t max);
uint32_t plat_isr_uart_available(void);
uint32_t plat_isr_uart_overflow(void);

bool plat_isr_uart_take(plat_irq_event_t *out);

#endif
