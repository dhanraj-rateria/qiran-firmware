/*
 * The interrupt framework is compiled in so link errors can be published the
 * way the fabric publishes them, through the interrupt queue.
 */
#include "../../src/plat/plat_irq.c"

#include "qiran/comm/comm_bytes.h"
#include "qiran/comm/comm_ccsds.h"
#include "qiran/comm/comm_cmd.h"
#include "qiran/comm/comm_output.h"
#include "qiran/comm/comm_spw_link.h"
#include "qiran/exec/exec_core.h"
#include "qiran/mission/mission_state.h"
#include "qiran/plat/plat_cpu.h"
#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_crc.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_log.h"
#include "qiran/svc/svc_time.h"
#include "test_framework.h"

#include <string.h>

static uint32_t       s_consumed[CCSDS_CLASS_COUNT];
static uint32_t       s_consumed_len;
static uint8_t        s_consumed_data[64];
static qiran_status_t s_consume_result;

/* --- a link that can be made to behave badly --- */

static bool     s_link_running;
static bool     s_start_fails;
static bool     s_status_fails;
static qiran_status_t s_send_result;
static uint32_t s_start_calls;
static uint32_t s_send_calls;
static uint8_t  s_wire[1024];
static uint32_t s_wire_len;

static uint8_t  s_inject[1024];
static uint32_t s_inject_len;

static qiran_status_t link_start(void *ctx)
{
    QIRAN_UNUSED(ctx);
    s_start_calls++;
    return s_start_fails ? QIRAN_ERR_HARDWARE : QIRAN_OK;
}

static qiran_status_t link_status(void *ctx, bool *running, uint32_t *word)
{
    QIRAN_UNUSED(ctx);
    if (s_status_fails) {
        return QIRAN_ERR_HARDWARE;
    }
    *running = s_link_running;
    *word = 0x1234U;
    return QIRAN_OK;
}

static qiran_status_t link_send(void *ctx, const uint8_t *data, uint32_t len)
{
    QIRAN_UNUSED(ctx);
    s_send_calls++;
    if (s_send_result != QIRAN_OK) {
        return s_send_result;
    }
    if (len <= sizeof(s_wire)) {
        memcpy(s_wire, data, len);
        s_wire_len = len;
    }
    return QIRAN_OK;
}

static uint32_t link_receive(void *ctx, uint8_t *buf, uint32_t max)
{
    uint32_t n = (s_inject_len < max) ? s_inject_len : max;

    QIRAN_UNUSED(ctx);
    if (n != 0U) {
        memcpy(buf, s_inject, n);
        s_inject_len = 0U;
    }
    return n;
}

static const comm_spw_ops_t k_ops = {
    link_start, link_status, link_send, link_receive, NULL
};

static void setup(void)
{
    exec_init();
    plat_irq_init();
    svc_log_init();
    svc_fdir_init();
    svc_config_init();
    svc_time_init();
    mission_state_init();
    comm_cmd_init();
    comm_ccsds_init();
    comm_spw_link_init();
    comm_output_init();

    s_link_running = false;
    s_start_fails = false;
    s_status_fails = false;
    s_send_result = QIRAN_OK;
    s_start_calls = 0U;
    s_send_calls = 0U;
    s_wire_len = 0U;
    s_inject_len = 0U;
    memset(s_consumed, 0, sizeof(s_consumed));
    s_consumed_len = 0U;
    s_consume_result = QIRAN_OK;
}

static void bring_up(void)
{
    CHECK_TRUE(comm_spw_link_set_ops(&k_ops) == QIRAN_OK);
    comm_spw_link_service();               /* down -> starting */
    s_link_running = true;
    comm_spw_link_service();               /* starting -> run */
    CHECK_TRUE(comm_spw_link_running());
}

/* --- primary header, against the standard --- */

static void test_header_bit_layout(void)
{
    ccsds_header_t h;
    ccsds_header_t back;
    uint8_t buf[CCSDS_PRIMARY_HEADER_BYTES + 2U];

    TEST_CASE("the primary header is six octets with the standard bit layout");
    setup();
    memset(buf, 0xEE, sizeof(buf));

    h.version = CCSDS_VERSION_1;
    h.type = CCSDS_TC;
    h.secondary_header = true;
    h.apid = 0x2ABU;
    h.seq_flags = CCSDS_SEQ_LAST;
    h.seq_count = 0x1234U;
    h.data_length = 0x0100U;

    CHECK_TRUE(comm_ccsds_build_header(&h, buf, sizeof(buf)) == QIRAN_OK);
    CHECK_EQ_U64(buf[CCSDS_PRIMARY_HEADER_BYTES], 0xEEU);

    /* version 000, type 1, secondary 1, apid 010 1010 1011 */
    CHECK_EQ_U64(comm_get_u16(buf, 0U), 0x1AABU);
    /* flags 10, count 01 0010 0011 0100 */
    CHECK_EQ_U64(comm_get_u16(buf, 2U), 0x9234U);
    CHECK_EQ_U64(comm_get_u16(buf, 4U), 0x0100U);

    TEST_CASE("parsing gives back exactly what was built");
    CHECK_TRUE(comm_ccsds_parse_header(buf, sizeof(buf), &back) == QIRAN_OK);
    CHECK_EQ_U64(back.version, h.version);
    CHECK_EQ_U64(back.type, CCSDS_TC);
    CHECK_TRUE(back.secondary_header);
    CHECK_EQ_U64(back.apid, 0x2ABU);
    CHECK_EQ_U64(back.seq_flags, CCSDS_SEQ_LAST);
    CHECK_EQ_U64(back.seq_count, 0x1234U);
    CHECK_EQ_U64(back.data_length, 0x0100U);

    TEST_CASE("telemetry with no secondary header packs its flags as zero");
    h.type = CCSDS_TM;
    h.secondary_header = false;
    h.apid = 0x001U;
    h.seq_flags = CCSDS_SEQ_UNSEGMENTED;
    h.seq_count = 0U;
    CHECK_TRUE(comm_ccsds_build_header(&h, buf, sizeof(buf)) == QIRAN_OK);
    CHECK_EQ_U64(comm_get_u16(buf, 0U), 0x0001U);
    CHECK_EQ_U64(comm_get_u16(buf, 2U), 0xC000U);

    TEST_CASE("the length field is one less than the octets it describes");
    h.data_length = 99U;
    CHECK_TRUE(comm_ccsds_build_header(&h, buf, sizeof(buf)) == QIRAN_OK);
    CHECK_TRUE(comm_ccsds_parse_header(buf, sizeof(buf), &back) == QIRAN_OK);
    CHECK_EQ_U64(comm_ccsds_packet_bytes(&back),
                 CCSDS_PRIMARY_HEADER_BYTES + 100U);

    TEST_CASE("out of range fields and short buffers are refused");
    h.apid = 0x800U;
    CHECK_TRUE(comm_ccsds_build_header(&h, buf, sizeof(buf)) == QIRAN_ERR_RANGE);
    h.apid = 1U;
    h.seq_count = 0x4000U;
    CHECK_TRUE(comm_ccsds_build_header(&h, buf, sizeof(buf)) == QIRAN_ERR_RANGE);
    h.seq_count = 0U;
    CHECK_TRUE(comm_ccsds_build_header(&h, buf, 5U) == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_ccsds_build_header(NULL, buf, sizeof(buf)) == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_ccsds_parse_header(buf, 5U, &back) == QIRAN_ERR_PARAM);
    CHECK_EQ_U64(comm_ccsds_packet_bytes(NULL), 0U);
}

/* --- identifiers and routing --- */



static qiran_status_t consume(void *ctx, const ccsds_header_t *header,
                              const uint8_t *app, uint32_t len)
{
    ccsds_class_t cls = (ccsds_class_t)(uintptr_t)ctx;

    QIRAN_UNUSED(header);
    s_consumed[cls]++;
    s_consumed_len = len;
    if (len <= sizeof(s_consumed_data)) {
        memcpy(s_consumed_data, app, len);
    }
    return s_consume_result;
}

static void register_consumer(ccsds_class_t cls)
{
    CHECK_TRUE(comm_ccsds_register(cls, consume, (void *)(uintptr_t)cls)
               == QIRAN_OK);
}

static void test_apid_map(void)
{
    uint32_t i;

    TEST_CASE("every class has a distinct identifier");
    setup();
    for (i = 0U; i < (uint32_t)CCSDS_CLASS_COUNT; i++) {
        uint32_t j;

        CHECK_TRUE(comm_ccsds_apid((ccsds_class_t)i) <= CCSDS_APID_MAX);
        CHECK_TRUE(comm_ccsds_apid((ccsds_class_t)i) != CCSDS_APID_IDLE);
        CHECK_EQ_U64(comm_ccsds_class_of(comm_ccsds_apid((ccsds_class_t)i)), i);

        for (j = i + 1U; j < (uint32_t)CCSDS_CLASS_COUNT; j++) {
            CHECK_TRUE(comm_ccsds_apid((ccsds_class_t)i) !=
                       comm_ccsds_apid((ccsds_class_t)j));
        }
        CHECK_TRUE(comm_ccsds_class_name((ccsds_class_t)i)[0] != '?');
    }
    CHECK_TRUE(comm_ccsds_class_name(CCSDS_CLASS_COUNT)[0] == '?');

    TEST_CASE("an identifier can be reassigned to match the interface");
    CHECK_TRUE(comm_ccsds_set_apid(CCSDS_CLASS_HEALTH, 0x123U) == QIRAN_OK);
    CHECK_EQ_U64(comm_ccsds_apid(CCSDS_CLASS_HEALTH), 0x123U);
    CHECK_EQ_U64(comm_ccsds_class_of(0x123U), CCSDS_CLASS_HEALTH);
    CHECK_EQ_U64(comm_ccsds_class_of(0x020U), CCSDS_CLASS_COUNT);

    TEST_CASE("two classes cannot share one identifier");
    CHECK_TRUE(comm_ccsds_set_apid(CCSDS_CLASS_EVENT, 0x123U) == QIRAN_ERR_STATE);
    CHECK_TRUE(comm_ccsds_set_apid(CCSDS_CLASS_EVENT,
                                   comm_ccsds_apid(CCSDS_CLASS_EVENT))
               == QIRAN_OK);

    TEST_CASE("the reserved idle identifier and out of range values are refused");
    CHECK_TRUE(comm_ccsds_set_apid(CCSDS_CLASS_EVENT, CCSDS_APID_IDLE)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_ccsds_set_apid(CCSDS_CLASS_EVENT, 0x800U) == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_ccsds_set_apid(CCSDS_CLASS_COUNT, 1U) == QIRAN_ERR_PARAM);
}

static void test_build_and_route(void)
{
    static const uint8_t k_app[] = { 0xDEU, 0xADU, 0xBEU, 0xEFU, 0x01U };
    uint8_t packet[64];
    uint32_t written = 0U;
    ccsds_header_t h;

    TEST_CASE("a built packet carries its class identifier and its data");
    setup();
    register_consumer(CCSDS_CLASS_HEALTH);

    CHECK_TRUE(comm_ccsds_build(CCSDS_CLASS_HEALTH, k_app, sizeof(k_app),
                                packet, sizeof(packet), &written) == QIRAN_OK);
    CHECK_EQ_U64(written, CCSDS_PRIMARY_HEADER_BYTES + sizeof(k_app) +
                          CCSDS_INTEGRITY_BYTES);

    CHECK_TRUE(comm_ccsds_parse_header(packet, written, &h) == QIRAN_OK);
    CHECK_EQ_U64(h.apid, comm_ccsds_apid(CCSDS_CLASS_HEALTH));
    CHECK_EQ_U64(h.type, CCSDS_TM);
    CHECK_EQ_U64(h.seq_flags, CCSDS_SEQ_UNSEGMENTED);
    CHECK_EQ_U64(comm_ccsds_packet_bytes(&h), written);

    TEST_CASE("the checksum covers the application data");
    CHECK_EQ_U64(comm_get_u32(packet, CCSDS_PRIMARY_HEADER_BYTES +
                              sizeof(k_app)),
                 svc_crc32(k_app, (uint32_t)sizeof(k_app)));

    TEST_CASE("receiving it routes the data to that class's consumer");
    CHECK_TRUE(comm_ccsds_receive(packet, written) == QIRAN_OK);
    CHECK_EQ_U64(s_consumed[CCSDS_CLASS_HEALTH], 1U);
    CHECK_EQ_U64(s_consumed_len, sizeof(k_app));
    CHECK_EQ_U64(s_consumed_data[0], 0xDEU);
    CHECK_EQ_U64(s_consumed_data[4], 0x01U);
    CHECK_EQ_U64(comm_ccsds_received(CCSDS_CLASS_HEALTH), 1U);
    CHECK_EQ_U64(comm_ccsds_sent(CCSDS_CLASS_HEALTH), 1U);

    TEST_CASE("a packet for a class with no consumer is not routed");
    CHECK_TRUE(comm_ccsds_build(CCSDS_CLASS_EVENT, k_app, sizeof(k_app),
                                packet, sizeof(packet), &written) == QIRAN_OK);
    CHECK_TRUE(comm_ccsds_receive(packet, written) == QIRAN_ERR_UNSUPPORTED);
    CHECK_EQ_U64(comm_ccsds_unroutable(), 1U);

    TEST_CASE("space reserved for file delivery routes but has no consumer");
    CHECK_EQ_U64(comm_ccsds_class_of(comm_ccsds_apid(CCSDS_CLASS_FILE)),
                 CCSDS_CLASS_FILE);
    register_consumer(CCSDS_CLASS_FILE);
    CHECK_TRUE(comm_ccsds_build(CCSDS_CLASS_FILE, k_app, sizeof(k_app), packet,
                                sizeof(packet), &written) == QIRAN_OK);
    CHECK_TRUE(comm_ccsds_receive(packet, written) == QIRAN_OK);
    CHECK_EQ_U64(s_consumed[CCSDS_CLASS_FILE], 1U);

    TEST_CASE("build arguments are validated");
    CHECK_TRUE(comm_ccsds_build(CCSDS_CLASS_COUNT, k_app, 1U, packet,
                                sizeof(packet), NULL) == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_ccsds_build(CCSDS_CLASS_HEALTH, NULL, 1U, packet,
                                sizeof(packet), NULL) == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_ccsds_build(CCSDS_CLASS_HEALTH, k_app, sizeof(k_app),
                                packet, 4U, NULL) == QIRAN_ERR_RANGE);
}

static void test_receive_checks(void)
{
    static const uint8_t k_app[] = { 1U, 2U, 3U, 4U };
    uint8_t packet[64];
    uint32_t written = 0U;

    TEST_CASE("a packet shorter than it declares is rejected");
    setup();
    register_consumer(CCSDS_CLASS_HEALTH);
    CHECK_TRUE(comm_ccsds_build(CCSDS_CLASS_HEALTH, k_app, sizeof(k_app),
                                packet, sizeof(packet), &written) == QIRAN_OK);

    CHECK_TRUE(comm_ccsds_receive(packet, written - 1U) == QIRAN_ERR_INTEGRITY);
    CHECK_EQ_U64(comm_ccsds_length_errors(), 1U);

    TEST_CASE("a packet longer than it declares is rejected too");
    CHECK_TRUE(comm_ccsds_receive(packet, written + 1U) == QIRAN_ERR_INTEGRITY);
    CHECK_EQ_U64(comm_ccsds_length_errors(), 2U);

    TEST_CASE("a corrupted application field is rejected by its checksum");
    packet[CCSDS_PRIMARY_HEADER_BYTES + 1U] ^= 0x01U;
    CHECK_TRUE(comm_ccsds_receive(packet, written) == QIRAN_ERR_INTEGRITY);
    CHECK_EQ_U64(comm_ccsds_integrity_errors(), 1U);
    CHECK_EQ_U64(s_consumed[CCSDS_CLASS_HEALTH], 0U);

    TEST_CASE("a data field too small to hold a checksum is rejected");
    {
        ccsds_header_t h;

        h.version = CCSDS_VERSION_1;
        h.type = CCSDS_TM;
        h.secondary_header = false;
        h.apid = comm_ccsds_apid(CCSDS_CLASS_HEALTH);
        h.seq_flags = CCSDS_SEQ_UNSEGMENTED;
        h.seq_count = 0U;
        h.data_length = 1U;   /* two octets, less than a checksum */
        CHECK_TRUE(comm_ccsds_build_header(&h, packet, sizeof(packet))
                   == QIRAN_OK);
        CHECK_TRUE(comm_ccsds_receive(packet, CCSDS_PRIMARY_HEADER_BYTES + 2U)
                   == QIRAN_ERR_INTEGRITY);
    }

    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_PACKET_INTEGRITY));
}

static void test_sequence_counting(void)
{
    static const uint8_t k_app[] = { 7U, 7U, 7U, 7U };
    uint8_t first[64];
    uint8_t second[64];
    uint8_t third[64];
    uint8_t fourth[64];
    uint32_t n1 = 0U;
    uint32_t n2 = 0U;
    uint32_t n3 = 0U;
    uint32_t n4 = 0U;
    ccsds_header_t h;

    TEST_CASE("the count advances by one for each packet of a class");
    setup();
    register_consumer(CCSDS_CLASS_SCIENCE_RAW);

    CHECK_TRUE(comm_ccsds_build(CCSDS_CLASS_SCIENCE_RAW, k_app, sizeof(k_app),
                                first, sizeof(first), &n1) == QIRAN_OK);
    CHECK_TRUE(comm_ccsds_build(CCSDS_CLASS_SCIENCE_RAW, k_app, sizeof(k_app),
                                second, sizeof(second), &n2) == QIRAN_OK);
    CHECK_TRUE(comm_ccsds_build(CCSDS_CLASS_SCIENCE_RAW, k_app, sizeof(k_app),
                                third, sizeof(third), &n3) == QIRAN_OK);
    CHECK_TRUE(comm_ccsds_build(CCSDS_CLASS_SCIENCE_RAW, k_app, sizeof(k_app),
                                fourth, sizeof(fourth), &n4) == QIRAN_OK);

    CHECK_TRUE(comm_ccsds_parse_header(first, n1, &h) == QIRAN_OK);
    CHECK_EQ_U64(h.seq_count, 0U);
    CHECK_TRUE(comm_ccsds_parse_header(second, n2, &h) == QIRAN_OK);
    CHECK_EQ_U64(h.seq_count, 1U);
    CHECK_TRUE(comm_ccsds_parse_header(third, n3, &h) == QIRAN_OK);
    CHECK_EQ_U64(h.seq_count, 2U);
    CHECK_TRUE(comm_ccsds_parse_header(fourth, n4, &h) == QIRAN_OK);
    CHECK_EQ_U64(h.seq_count, 3U);

    TEST_CASE("counts arriving in order raise nothing");
    CHECK_TRUE(comm_ccsds_receive(first, n1) == QIRAN_OK);
    CHECK_TRUE(comm_ccsds_receive(second, n2) == QIRAN_OK);
    CHECK_EQ_U64(comm_ccsds_sequence_gaps(), 0U);

    TEST_CASE("a missing packet shows up as a gap, reported once");
    setup();
    register_consumer(CCSDS_CLASS_SCIENCE_RAW);
    CHECK_TRUE(comm_ccsds_receive(first, n1) == QIRAN_OK);
    CHECK_TRUE(comm_ccsds_receive(third, n3) == QIRAN_OK);
    CHECK_EQ_U64(comm_ccsds_sequence_gaps(), 1U);

    TEST_CASE("the gap is adopted, so later packets are not all reported");
    CHECK_TRUE(comm_ccsds_receive(fourth, n4) == QIRAN_OK);
    CHECK_EQ_U64(comm_ccsds_sequence_gaps(), 1U);

    TEST_CASE("a corrupted packet is not mistaken for a lost one");
    setup();
    register_consumer(CCSDS_CLASS_SCIENCE_RAW);
    CHECK_TRUE(comm_ccsds_receive(first, n1) == QIRAN_OK);
    third[CCSDS_PRIMARY_HEADER_BYTES] ^= 0xFFU;
    CHECK_TRUE(comm_ccsds_receive(third, n3) == QIRAN_ERR_INTEGRITY);
    CHECK_EQ_U64(comm_ccsds_sequence_gaps(), 0U);
    CHECK_EQ_U64(comm_ccsds_integrity_errors(), 1U);
}

/* --- link bring-up --- */

static void test_link_bring_up(void)
{
    TEST_CASE("nothing happens until a link device is attached");
    setup();
    comm_spw_link_service();
    CHECK_EQ_U64(comm_spw_link_state(), SPW_LINK_DOWN);
    CHECK_EQ_U64(s_start_calls, 0U);
    CHECK_TRUE(!comm_spw_link_running());

    TEST_CASE("half-supplied link operations are refused");
    {
        comm_spw_ops_t bad = k_ops;

        bad.send = NULL;
        CHECK_TRUE(comm_spw_link_set_ops(&bad) == QIRAN_ERR_PARAM);
    }

    TEST_CASE("the link is started, then confirmed running");
    CHECK_TRUE(comm_spw_link_set_ops(&k_ops) == QIRAN_OK);
    comm_spw_link_service();
    CHECK_EQ_U64(comm_spw_link_state(), SPW_LINK_STARTING);
    CHECK_EQ_U64(s_start_calls, 1U);

    comm_spw_link_service();
    CHECK_EQ_U64(comm_spw_link_state(), SPW_LINK_STARTING);

    s_link_running = true;
    comm_spw_link_service();
    CHECK_EQ_U64(comm_spw_link_state(), SPW_LINK_RUN);
    CHECK_TRUE(comm_spw_link_running());
    CHECK_EQ_U64(comm_spw_link_status_word(), 0x1234U);

    TEST_CASE("every state has a name");
    CHECK_TRUE(comm_spw_link_state_name(SPW_LINK_DOWN)[0] != '?');
    CHECK_TRUE(comm_spw_link_state_name(SPW_LINK_RUN)[0] != '?');
    CHECK_TRUE(comm_spw_link_state_name((comm_spw_state_t)9)[0] == '?');
}

static void test_link_start_gives_up_at_its_limit(void)
{
    uint32_t i;

    TEST_CASE("a link that will not start is retried, then declared failed");
    setup();
    s_start_fails = true;
    CHECK_TRUE(comm_spw_link_set_ops(&k_ops) == QIRAN_OK);

    for (i = 0U; i < 20U; i++) {
        comm_spw_link_service();
        error_handling_service();
        if (comm_spw_link_state() == SPW_LINK_FAILED) {
            break;
        }
    }

    CHECK_EQ_U64(comm_spw_link_state(), SPW_LINK_FAILED);
    CHECK_EQ_U64(s_start_calls, 3U);
    CHECK_TRUE(!comm_spw_link_running());

    TEST_CASE("a failed link is not retried further");
    comm_spw_link_service();
    CHECK_EQ_U64(s_start_calls, 3U);
}

static void test_link_start_times_out(void)
{
    uint32_t i;

    TEST_CASE("a link that starts but never runs times out and is retried");
    setup();
    CHECK_TRUE(comm_spw_link_set_ops(&k_ops) == QIRAN_OK);
    comm_spw_link_service();
    CHECK_EQ_U64(comm_spw_link_state(), SPW_LINK_STARTING);

    for (i = 0U; i < 100U; i++) {
        exec_on_minor_tick();
        comm_spw_link_service();
        if (comm_spw_link_state() != SPW_LINK_STARTING) {
            break;
        }
    }

    CHECK_EQ_U64(comm_spw_link_state(), SPW_LINK_DOWN);
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_SPW_LINK));
}

static void test_link_disconnect(void)
{
    TEST_CASE("a link that was running and stops is counted as a disconnect");
    setup();
    bring_up();
    CHECK_EQ_U64(comm_spw_link_disconnects(), 0U);

    s_link_running = false;
    comm_spw_link_service();

    CHECK_EQ_U64(comm_spw_link_state(), SPW_LINK_DOWN);
    CHECK_EQ_U64(comm_spw_link_disconnects(), 1U);
    CHECK_EQ_U64(comm_spw_link_start_attempts(), 1U);

    TEST_CASE("it comes back up on its own");
    s_link_running = true;
    comm_spw_link_service();
    comm_spw_link_service();
    CHECK_TRUE(comm_spw_link_running());

    TEST_CASE("a status read that fails drops the link rather than trusting it");
    s_status_fails = true;
    comm_spw_link_service();
    CHECK_EQ_U64(comm_spw_link_state(), SPW_LINK_DOWN);
}

static void test_link_errors_from_interrupt(void)
{
    plat_irq_source_t src;

    TEST_CASE("errors published by the interrupt are collected and reported");
    setup();
    bring_up();

    src.irq_id = 63U;
    src.trigger = PLAT_GIC_TRIGGER_LEVEL_HIGH;
    src.ack = NULL;
    CHECK_TRUE(plat_irq_register(PLAT_IRQ_SPW, &src) == QIRAN_ERR_PARAM);

    /* Published directly, as the handler would. */
    {
        irq_slot_t *slot = &s_slot[PLAT_IRQ_SPW];

        slot->q[0].datum = 0xBADU;
        slot->q[0].kind = 1U;
        slot->head = 1U;
    }

    comm_spw_link_service();
    error_handling_service();

    CHECK_EQ_U64(comm_spw_link_errors(), 1U);
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_SPW_LINK));

    TEST_CASE("the status at the error is kept, not overwritten by the current one");
    CHECK_EQ_U64(comm_spw_link_last_error_word(), 0xBADU);
    CHECK_EQ_U64(comm_spw_link_status_word(), 0x1234U);
}

static void test_send_requires_a_running_link(void)
{
    static const uint8_t k_data[] = { 1U, 2U, 3U };

    TEST_CASE("sending on a link that is not running is refused");
    setup();
    CHECK_TRUE(comm_spw_link_send(k_data, sizeof(k_data)) == QIRAN_ERR_STATE);

    TEST_CASE("sending on a running link reaches the device");
    bring_up();
    CHECK_TRUE(comm_spw_link_send(k_data, sizeof(k_data)) == QIRAN_OK);
    CHECK_EQ_U64(comm_spw_link_packets_sent(), 1U);
    CHECK_EQ_U64(s_wire_len, sizeof(k_data));

    TEST_CASE("arguments are validated");
    CHECK_TRUE(comm_spw_link_send(NULL, 4U) == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_spw_link_send(k_data, 0U) == QIRAN_ERR_PARAM);

    TEST_CASE("a device that refuses the send reports it");
    s_send_result = QIRAN_ERR_HARDWARE;
    CHECK_TRUE(comm_spw_link_send(k_data, sizeof(k_data)) == QIRAN_ERR_HARDWARE);
    CHECK_EQ_U64(comm_spw_link_errors(), 1U);
}

static void test_received_packet_reaches_the_router(void)
{
    static const uint8_t k_app[] = { 9U, 8U, 7U, 6U };
    uint32_t written = 0U;

    TEST_CASE("a packet arriving on the link is routed by its identifier");
    setup();
    register_consumer(CCSDS_CLASS_HEALTH);
    bring_up();

    CHECK_TRUE(comm_ccsds_build(CCSDS_CLASS_HEALTH, k_app, sizeof(k_app),
                                s_inject, (uint32_t)sizeof(s_inject), &written)
               == QIRAN_OK);
    s_inject_len = written;

    comm_spw_link_service();

    CHECK_EQ_U64(comm_spw_link_packets_received(), 1U);
    CHECK_EQ_U64(s_consumed[CCSDS_CLASS_HEALTH], 1U);
    CHECK_EQ_U64(s_consumed_len, sizeof(k_app));
}

static void test_command_arrives_over_the_data_link(void)
{
    uint8_t frame[COMM_CMD_FRAME_BYTES];
    uint32_t written = 0U;

    TEST_CASE("a command inside a packet joins the same command sequence");
    setup();
    CHECK_TRUE(comm_ccsds_register(CCSDS_CLASS_COMMAND, consume,
                                   (void *)(uintptr_t)CCSDS_CLASS_COMMAND)
               == QIRAN_OK);

    CHECK_TRUE(comm_cmd_build(CMD_LASER_OFF, 0U, 42U, frame, sizeof(frame),
                              NULL) == QIRAN_OK);
    CHECK_TRUE(comm_ccsds_build(CCSDS_CLASS_COMMAND, frame, sizeof(frame),
                                s_inject, (uint32_t)sizeof(s_inject), &written)
               == QIRAN_OK);

    CHECK_TRUE(comm_ccsds_receive(s_inject, written) == QIRAN_OK);
    CHECK_EQ_U64(s_consumed[CCSDS_CLASS_COMMAND], 1U);
    CHECK_EQ_U64(s_consumed_len, COMM_CMD_FRAME_BYTES);

    TEST_CASE("an already framed command runs the sequence directly");
    CHECK_TRUE(comm_cmd_submit(frame, sizeof(frame)) == QIRAN_OK);
    CHECK_EQ_U64(comm_cmd_received(), 1U);

    TEST_CASE("a framed command of the wrong length or checksum is refused");
    CHECK_TRUE(comm_cmd_submit(frame, 4U) == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_cmd_submit(NULL, sizeof(frame)) == QIRAN_ERR_PARAM);
    frame[3] ^= 0x01U;
    CHECK_TRUE(comm_cmd_submit(frame, sizeof(frame)) == QIRAN_ERR_INTEGRITY);
    CHECK_EQ_U64(comm_cmd_received(), 1U);
}

/* --- output ordering --- */

static uint8_t s_raw_buf[8];
static uint8_t s_proc_buf[8];
static uint8_t s_health_buf[8];
static uint8_t s_event_buf[8];

static ccsds_class_t class_on_wire(void)
{
    ccsds_header_t h;

    if (comm_ccsds_parse_header(s_wire, s_wire_len, &h) != QIRAN_OK) {
        return CCSDS_CLASS_COUNT;
    }
    return comm_ccsds_class_of(h.apid);
}

static void test_output_priority_order(void)
{
    TEST_CASE("fault data outranks status, which outranks results, then raw");
    setup();
    CHECK_TRUE(comm_output_priority_of(CCSDS_CLASS_EVENT) <
               comm_output_priority_of(CCSDS_CLASS_HEALTH));
    CHECK_TRUE(comm_output_priority_of(CCSDS_CLASS_HEALTH) <
               comm_output_priority_of(CCSDS_CLASS_SCIENCE_PROCESSED));
    CHECK_TRUE(comm_output_priority_of(CCSDS_CLASS_SCIENCE_PROCESSED) <
               comm_output_priority_of(CCSDS_CLASS_SCIENCE_RAW));
    CHECK_EQ_U64(comm_output_priority_of(CCSDS_CLASS_COUNT), 0xFFU);

    TEST_CASE("queued in the worst order, they leave in the right one");
    memset(s_raw_buf, 0x11, sizeof(s_raw_buf));
    memset(s_proc_buf, 0x22, sizeof(s_proc_buf));
    memset(s_health_buf, 0x33, sizeof(s_health_buf));
    memset(s_event_buf, 0x44, sizeof(s_event_buf));

    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_SCIENCE_RAW, s_raw_buf,
                                  sizeof(s_raw_buf)) == QIRAN_OK);
    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_SCIENCE_PROCESSED, s_proc_buf,
                                  sizeof(s_proc_buf)) == QIRAN_OK);
    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_HEALTH, s_health_buf,
                                  sizeof(s_health_buf)) == QIRAN_OK);
    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_EVENT, s_event_buf,
                                  sizeof(s_event_buf)) == QIRAN_OK);
    CHECK_EQ_U64(comm_output_pending(), 4U);
    CHECK_EQ_U64(comm_output_high_water(), 4U);

    bring_up();

    /* Nothing leaves while the link is down, so drain one at a time by
       refusing the send after each. */
    s_send_result = QIRAN_OK;
    comm_output_service();
    CHECK_EQ_U64(comm_output_pending(), 0U);
    CHECK_EQ_U64(comm_output_sent(), 4U);

    /* The last one on the wire is the lowest priority, sent last. */
    CHECK_EQ_U64(class_on_wire(), CCSDS_CLASS_SCIENCE_RAW);
}

static void test_output_sends_one_at_a_time_in_order(void)
{
    TEST_CASE("with the link taking one packet per pass, order is observable");
    setup();
    register_consumer(CCSDS_CLASS_HEALTH);
    bring_up();

    memset(s_raw_buf, 0x11, sizeof(s_raw_buf));
    memset(s_health_buf, 0x33, sizeof(s_health_buf));
    memset(s_event_buf, 0x44, sizeof(s_event_buf));

    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_SCIENCE_RAW, s_raw_buf, 8U)
               == QIRAN_OK);
    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_HEALTH, s_health_buf, 8U)
               == QIRAN_OK);
    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_EVENT, s_event_buf, 8U)
               == QIRAN_OK);

    /* Refuse after the first so the pass stops, then inspect what went. */
    s_send_result = QIRAN_OK;
    s_send_calls = 0U;
    comm_output_service();
    CHECK_EQ_U64(comm_output_sent(), 3U);
    CHECK_EQ_U64(s_send_calls, 3U);

    TEST_CASE("each class's sequence count advanced independently");
    CHECK_EQ_U64(comm_ccsds_sent(CCSDS_CLASS_EVENT), 1U);
    CHECK_EQ_U64(comm_ccsds_sent(CCSDS_CLASS_HEALTH), 1U);
    CHECK_EQ_U64(comm_ccsds_sent(CCSDS_CLASS_SCIENCE_RAW), 1U);
}

static void test_output_holds_when_the_link_will_not_take_it(void)
{
    TEST_CASE("nothing is sent while the link is not running");
    setup();
    memset(s_event_buf, 0x44, sizeof(s_event_buf));
    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_EVENT, s_event_buf, 8U)
               == QIRAN_OK);

    comm_output_service();
    CHECK_EQ_U64(comm_output_pending(), 1U);
    CHECK_EQ_U64(comm_output_sent(), 0U);

    TEST_CASE("a refused send leaves the item queued for a later pass");
    bring_up();
    s_send_result = QIRAN_ERR_HARDWARE;
    comm_output_service();
    CHECK_EQ_U64(comm_output_pending(), 1U);
    CHECK_EQ_U64(comm_output_sent(), 0U);

    TEST_CASE("it goes out once the link accepts it");
    s_send_result = QIRAN_OK;
    comm_output_service();
    CHECK_EQ_U64(comm_output_pending(), 0U);
    CHECK_EQ_U64(comm_output_sent(), 1U);
}

static void test_output_full_queue(void)
{
    static uint8_t buf[8];
    uint32_t i;

    TEST_CASE("a full queue refuses new work rather than discarding old");
    setup();
    memset(buf, 0x55, sizeof(buf));

    for (i = 0U; i < COMM_OUTPUT_DEPTH; i++) {
        CHECK_TRUE(comm_output_submit(CCSDS_CLASS_SCIENCE_RAW, buf,
                                      sizeof(buf)) == QIRAN_OK);
    }
    CHECK_EQ_U64(comm_output_pending(), COMM_OUTPUT_DEPTH);

    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_SCIENCE_RAW, buf, sizeof(buf))
               == QIRAN_ERR_BUSY);
    CHECK_EQ_U64(comm_output_pending(), COMM_OUTPUT_DEPTH);
    CHECK_EQ_U64(comm_output_rejected(), 1U);

    TEST_CASE("the refusal is reported, since computed data was turned away");
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_OUTPUT_QUEUE));

    TEST_CASE("draining it makes room again");
    bring_up();
    comm_output_service();
    CHECK_EQ_U64(comm_output_pending(), 0U);
    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_SCIENCE_RAW, buf, sizeof(buf))
               == QIRAN_OK);

    TEST_CASE("submission arguments are validated");
    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_COUNT, buf, 8U)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_EVENT, NULL, 8U)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_EVENT, buf, 0U)
               == QIRAN_ERR_PARAM);

    TEST_CASE("data too large to packetise is refused at submission");
    CHECK_TRUE(comm_output_submit(CCSDS_CLASS_EVENT, buf, 100000U)
               == QIRAN_ERR_RANGE);
}

int main(void)
{
    plat_cpu_init();

    test_header_bit_layout();
    test_apid_map();
    test_build_and_route();
    test_receive_checks();
    test_sequence_counting();

    test_link_bring_up();
    test_link_start_gives_up_at_its_limit();
    test_link_start_times_out();
    test_link_disconnect();
    test_link_errors_from_interrupt();
    test_send_requires_a_running_link();
    test_received_packet_reaches_the_router();
    test_command_arrives_over_the_data_link();

    test_output_priority_order();
    test_output_sends_one_at_a_time_in_order();
    test_output_holds_when_the_link_will_not_take_it();
    test_output_full_queue();

    return TEST_REPORT();
}
