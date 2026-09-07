/*
 * The interrupt framework is compiled in so the shared handler can be invoked
 * directly and the per-source slots inspected, which is how interrupt-context
 * behaviour is exercised without a controller.
 */
#include "../../src/plat/plat_irq.c"

#include "qiran/plat/plat_isr_uart.h"
#include "qiran/svc/svc_ring.h"
#include "test_framework.h"

static void fire(plat_irq_id_t id)
{
    irq_handler(&s_slot[id]);
}

/* --- byte ring --- */

static uint8_t  s_store[8];
static svc_ring_t s_r;

static void test_ring_rejects_bad_geometry(void)
{
    TEST_CASE("ring requires non-zero power-of-two capacity and real storage");

    CHECK_TRUE(svc_ring_init(&s_r, s_store, 0U) == QIRAN_ERR_PARAM);
    CHECK_TRUE(svc_ring_init(&s_r, s_store, 7U) == QIRAN_ERR_PARAM);
    CHECK_TRUE(svc_ring_init(&s_r, NULL, 8U) == QIRAN_ERR_PARAM);
    CHECK_TRUE(svc_ring_init(NULL, s_store, 8U) == QIRAN_ERR_PARAM);
    CHECK_TRUE(svc_ring_init(&s_r, s_store, 8U) == QIRAN_OK);
}

static void test_ring_fifo_and_overflow(void)
{
    uint8_t b;
    uint8_t out[16];
    uint32_t i;
    uint32_t n;

    TEST_CASE("ring preserves order and counts drops when full");
    CHECK_TRUE(svc_ring_init(&s_r, s_store, 8U) == QIRAN_OK);
    CHECK_EQ_U64(svc_ring_count(&s_r), 0U);
    CHECK_EQ_U64(svc_ring_free(&s_r), 8U);
    CHECK_TRUE(!svc_ring_pop(&s_r, &b));

    for (i = 0U; i < 8U; i++) {
        CHECK_TRUE(svc_ring_push(&s_r, (uint8_t)(0x10U + i)));
    }
    CHECK_EQ_U64(svc_ring_count(&s_r), 8U);
    CHECK_EQ_U64(svc_ring_free(&s_r), 0U);

    /* Full: the push fails and is counted, it does not overwrite. */
    CHECK_TRUE(!svc_ring_push(&s_r, 0xFFU));
    CHECK_TRUE(!svc_ring_push(&s_r, 0xFFU));
    CHECK_EQ_U64(svc_ring_dropped(&s_r), 2U);
    CHECK_EQ_U64(svc_ring_count(&s_r), 8U);

    n = svc_ring_read(&s_r, out, 16U);
    CHECK_EQ_U64(n, 8U);
    for (i = 0U; i < 8U; i++) {
        CHECK_EQ_U64(out[i], 0x10U + i);
    }
    CHECK_EQ_U64(svc_ring_count(&s_r), 0U);

    TEST_CASE("ring is reusable after having been filled and drained");
    CHECK_TRUE(svc_ring_push(&s_r, 0x99U));
    CHECK_TRUE(svc_ring_pop(&s_r, &b));
    CHECK_EQ_U64(b, 0x99U);
}

static void test_ring_index_wrap(void)
{
    uint8_t b;
    uint32_t i;

    TEST_CASE("ring survives 32-bit index wrap");
    CHECK_TRUE(svc_ring_init(&s_r, s_store, 8U) == QIRAN_OK);
    s_r.head = 0xFFFFFFFDU;
    s_r.tail = 0xFFFFFFFDU;

    for (i = 0U; i < 6U; i++) {
        CHECK_TRUE(svc_ring_push(&s_r, (uint8_t)(0x40U + i)));
    }
    CHECK_EQ_U64(svc_ring_count(&s_r), 6U);

    for (i = 0U; i < 6U; i++) {
        CHECK_TRUE(svc_ring_pop(&s_r, &b));
        CHECK_EQ_U64(b, 0x40U + i);
    }
    CHECK_EQ_U64(svc_ring_dropped(&s_r), 0U);
}

/* --- interrupt framework --- */

static uint32_t s_ack_calls;
static uint32_t s_ack_datum;
static uint16_t s_ack_kind;

static uint32_t stub_ack(void *ctx, uint16_t *kind)
{
    QIRAN_UNUSED(ctx);
    s_ack_calls++;
    *kind = s_ack_kind;
    return s_ack_datum;
}

static plat_irq_source_t make_source(uint32_t irq_id)
{
    plat_irq_source_t src;

    src.irq_id = irq_id;
    src.trigger = PLAT_GIC_TRIGGER_LEVEL_HIGH;
    src.ack = stub_ack;
    src.ctx = NULL;

    return src;
}

static void test_priority_ordering_invariant(void)
{
    TEST_CASE("tick outranks every other source, watchdog outranks the rest");

    CHECK_TRUE(k_priority[PLAT_IRQ_TIMER] < k_priority[PLAT_IRQ_WATCHDOG]);
    CHECK_TRUE(k_priority[PLAT_IRQ_WATCHDOG] < k_priority[PLAT_IRQ_DMA]);
    CHECK_TRUE(k_priority[PLAT_IRQ_DMA] < k_priority[PLAT_IRQ_SPW]);
    CHECK_TRUE(k_priority[PLAT_IRQ_SPW] < k_priority[PLAT_IRQ_UART]);
    CHECK_TRUE(k_priority[PLAT_IRQ_UART] < k_priority[PLAT_IRQ_PL_ERROR]);
}

static void test_registration_rules(void)
{
    plat_irq_source_t src = make_source(61U);
    plat_irq_source_t bad = make_source(62U);

    TEST_CASE("registration rejects bad arguments and refuses to double-connect");
    plat_irq_init();

    CHECK_TRUE(!plat_irq_registered(PLAT_IRQ_DMA));

    bad.ack = NULL;
    CHECK_TRUE(plat_irq_register(PLAT_IRQ_DMA, &bad) == QIRAN_ERR_PARAM);
    CHECK_TRUE(plat_irq_register(PLAT_IRQ_DMA, NULL) == QIRAN_ERR_PARAM);
    CHECK_TRUE(plat_irq_register(PLAT_IRQ_COUNT, &src) == QIRAN_ERR_PARAM);

    CHECK_TRUE(plat_irq_register(PLAT_IRQ_DMA, &src) == QIRAN_OK);
    CHECK_TRUE(plat_irq_registered(PLAT_IRQ_DMA));
    CHECK_TRUE(plat_irq_register(PLAT_IRQ_DMA, &src) == QIRAN_ERR_STATE);
}

static void test_event_publication(void)
{
    plat_irq_source_t src = make_source(61U);
    plat_irq_event_t ev;
    plat_irq_stats_t st;

    TEST_CASE("handler acknowledges, timestamps and publishes one event");
    exec_init();
    plat_irq_init();
    s_ack_calls = 0U;
    s_ack_datum = 0xDEADU;
    s_ack_kind = 7U;
    CHECK_TRUE(plat_irq_register(PLAT_IRQ_DMA, &src) == QIRAN_OK);

    exec_on_minor_tick();
    exec_on_minor_tick();
    fire(PLAT_IRQ_DMA);

    CHECK_EQ_U64(s_ack_calls, 1U);
    CHECK_EQ_U64(plat_irq_pending(PLAT_IRQ_DMA), 1U);

    CHECK_TRUE(plat_irq_take(PLAT_IRQ_DMA, &ev));
    CHECK_EQ_U64(ev.datum, 0xDEADU);
    CHECK_EQ_U64(ev.kind, 7U);
    CHECK_EQ_U64(ev.seq, 0U);
    CHECK_EQ_U64(ev.tick, 2U);

    CHECK_EQ_U64(plat_irq_pending(PLAT_IRQ_DMA), 0U);
    CHECK_TRUE(!plat_irq_take(PLAT_IRQ_DMA, &ev));

    plat_irq_stats_get(PLAT_IRQ_DMA, &st);
    CHECK_EQ_U64(st.taken, 1U);
    CHECK_EQ_U64(st.dropped, 0U);
    CHECK_TRUE(st.cycles_worst > 0U);
    CHECK_TRUE(st.cycles_worst >= st.cycles_last);
}

static void test_queue_overflow_is_visible(void)
{
    plat_irq_source_t src = make_source(61U);
    plat_irq_event_t ev;
    plat_irq_stats_t st;
    uint32_t i;

    TEST_CASE("queue overflow is counted and leaves a sequence gap");
    exec_init();
    plat_irq_init();
    s_ack_datum = 1U;
    s_ack_kind = 0U;
    CHECK_TRUE(plat_irq_register(PLAT_IRQ_SPW, &src) == QIRAN_OK);

    for (i = 0U; i < QIRAN_IRQ_QUEUE_DEPTH; i++) {
        fire(PLAT_IRQ_SPW);
    }
    CHECK_EQ_U64(plat_irq_pending(PLAT_IRQ_SPW), QIRAN_IRQ_QUEUE_DEPTH);

    fire(PLAT_IRQ_SPW);
    fire(PLAT_IRQ_SPW);
    fire(PLAT_IRQ_SPW);

    plat_irq_stats_get(PLAT_IRQ_SPW, &st);
    CHECK_EQ_U64(st.taken, QIRAN_IRQ_QUEUE_DEPTH + 3U);
    CHECK_EQ_U64(st.dropped, 3U);
    CHECK_EQ_U64(plat_irq_pending(PLAT_IRQ_SPW), QIRAN_IRQ_QUEUE_DEPTH);

    /* Buffered events are the oldest, in order, and unmodified by the drops. */
    for (i = 0U; i < QIRAN_IRQ_QUEUE_DEPTH; i++) {
        CHECK_TRUE(plat_irq_take(PLAT_IRQ_SPW, &ev));
        CHECK_EQ_U64(ev.seq, i);
    }

    /* The next event's sequence skips the dropped ones. */
    fire(PLAT_IRQ_SPW);
    CHECK_TRUE(plat_irq_take(PLAT_IRQ_SPW, &ev));
    CHECK_EQ_U64(ev.seq, QIRAN_IRQ_QUEUE_DEPTH + 3U);
}

static void test_sources_are_independent(void)
{
    plat_irq_source_t a = make_source(61U);
    plat_irq_source_t b = make_source(62U);
    plat_irq_event_t ev;

    TEST_CASE("each source has its own queue and cannot starve another");
    exec_init();
    plat_irq_init();
    CHECK_TRUE(plat_irq_register(PLAT_IRQ_DMA, &a) == QIRAN_OK);
    CHECK_TRUE(plat_irq_register(PLAT_IRQ_PL_ERROR, &b) == QIRAN_OK);

    s_ack_datum = 0xAAU;
    fire(PLAT_IRQ_DMA);
    s_ack_datum = 0xBBU;
    fire(PLAT_IRQ_PL_ERROR);

    CHECK_EQ_U64(plat_irq_pending(PLAT_IRQ_DMA), 1U);
    CHECK_EQ_U64(plat_irq_pending(PLAT_IRQ_PL_ERROR), 1U);

    CHECK_TRUE(plat_irq_take(PLAT_IRQ_DMA, &ev));
    CHECK_EQ_U64(ev.datum, 0xAAU);
    CHECK_TRUE(plat_irq_take(PLAT_IRQ_PL_ERROR, &ev));
    CHECK_EQ_U64(ev.datum, 0xBBU);

    CHECK_TRUE(!plat_irq_registered(PLAT_IRQ_UART));
    CHECK_EQ_U64(plat_irq_pending(PLAT_IRQ_UART), 0U);
}

/* --- serial receive path --- */

static uint32_t s_drain_bytes;
static uint8_t  s_drain_next;

static uint32_t fake_drain(void *ctx, svc_ring_t *rx, uint16_t *kind)
{
    uint32_t i;

    QIRAN_UNUSED(ctx);
    for (i = 0U; i < s_drain_bytes; i++) {
        (void)svc_ring_push(rx, s_drain_next);
        s_drain_next++;
    }
    *kind = (uint16_t)((s_drain_bytes == 0U) ? PLAT_UART_KIND_GAP
                                             : PLAT_UART_KIND_DATA);
    return s_drain_bytes;
}

static void test_uart_receive_path(void)
{
    plat_isr_uart_device_t dev;
    plat_irq_event_t ev;
    uint8_t out[64];
    uint32_t n;
    uint32_t i;

    TEST_CASE("serial receive ring is usable before a device is attached");
    exec_init();
    plat_irq_init();
    CHECK_TRUE(plat_isr_uart_init(NULL) == QIRAN_OK);
    CHECK_EQ_U64(plat_isr_uart_available(), 0U);
    CHECK_TRUE(!plat_irq_registered(PLAT_IRQ_UART));

    TEST_CASE("receive interrupt buffers bytes and classifies the event");
    dev.drain = fake_drain;
    dev.ctx = NULL;
    dev.irq_id = 82U;
    dev.trigger = PLAT_GIC_TRIGGER_LEVEL_HIGH;
    CHECK_TRUE(plat_isr_uart_init(&dev) == QIRAN_OK);
    CHECK_TRUE(plat_irq_registered(PLAT_IRQ_UART));

    s_drain_bytes = 4U;
    s_drain_next = 0x50U;
    fire(PLAT_IRQ_UART);

    CHECK_EQ_U64(plat_isr_uart_available(), 4U);
    CHECK_TRUE(plat_isr_uart_take(&ev));
    CHECK_EQ_U64(ev.kind, PLAT_UART_KIND_DATA);
    CHECK_EQ_U64(ev.datum, 4U);

    n = plat_isr_uart_read(out, sizeof(out));
    CHECK_EQ_U64(n, 4U);
    for (i = 0U; i < 4U; i++) {
        CHECK_EQ_U64(out[i], 0x50U + i);
    }

    TEST_CASE("an idle gap is reported without any byte having arrived");
    s_drain_bytes = 0U;
    fire(PLAT_IRQ_UART);
    CHECK_TRUE(plat_isr_uart_take(&ev));
    CHECK_EQ_U64(ev.kind, PLAT_UART_KIND_GAP);
    CHECK_EQ_U64(plat_isr_uart_available(), 0U);

    TEST_CASE("receive ring overflow is counted, not silent");
    CHECK_EQ_U64(plat_isr_uart_overflow(), 0U);
    s_drain_bytes = QIRAN_UART_RX_RING_BYTES + 16U;
    fire(PLAT_IRQ_UART);
    CHECK_EQ_U64(plat_isr_uart_available(), QIRAN_UART_RX_RING_BYTES);
    CHECK_EQ_U64(plat_isr_uart_overflow(), 16U);
}

int main(void)
{
    plat_cpu_init();

    test_ring_rejects_bad_geometry();
    test_ring_fifo_and_overflow();
    test_ring_index_wrap();

    test_priority_ordering_invariant();
    test_registration_rules();
    test_event_publication();
    test_queue_overflow_is_visible();
    test_sources_are_independent();

    test_uart_receive_path();

    return TEST_REPORT();
}
