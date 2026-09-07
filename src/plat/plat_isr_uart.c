#include "qiran/plat/plat_isr_uart.h"

#include "qiran/qiran_config.h"

static uint8_t    s_rx_storage[QIRAN_UART_RX_RING_BYTES];
static svc_ring_t s_rx;

static plat_isr_uart_device_t s_dev;

static uint32_t uart_ack(void *ctx, uint16_t *kind)
{
    QIRAN_UNUSED(ctx);
    return s_dev.drain(s_dev.ctx, &s_rx, kind);
}

qiran_status_t plat_isr_uart_init(const plat_isr_uart_device_t *dev)
{
    qiran_status_t st;
    plat_irq_source_t src;

    st = svc_ring_init(&s_rx, s_rx_storage, QIRAN_UART_RX_RING_BYTES);
    if (st != QIRAN_OK) {
        return st;
    }

    if (dev == NULL) {
        s_dev.drain = NULL;
        s_dev.ctx = NULL;
        return QIRAN_OK;
    }

    if (dev->drain == NULL) {
        return QIRAN_ERR_PARAM;
    }

    s_dev = *dev;

    src.irq_id = dev->irq_id;
    src.trigger = dev->trigger;
    src.ack = uart_ack;
    src.ctx = NULL;

    st = plat_irq_register(PLAT_IRQ_UART, &src);
    if (st != QIRAN_OK) {
        return st;
    }

    plat_irq_enable(PLAT_IRQ_UART);
    return QIRAN_OK;
}

uint32_t plat_isr_uart_read(uint8_t *out, uint32_t max)
{
    if ((out == NULL) || (max == 0U)) {
        return 0U;
    }
    return svc_ring_read(&s_rx, out, max);
}

uint32_t plat_isr_uart_available(void)
{
    return svc_ring_count(&s_rx);
}

uint32_t plat_isr_uart_overflow(void)
{
    return svc_ring_dropped(&s_rx);
}

bool plat_isr_uart_take(plat_irq_event_t *out)
{
    return plat_irq_take(PLAT_IRQ_UART, out);
}
