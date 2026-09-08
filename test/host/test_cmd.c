/*
 * The interrupt framework is compiled in so bytes can be delivered through the
 * real receive path: the device hook fills the ring from the interrupt, and the
 * command path assembles frames out of it in the loop, exactly as in flight.
 */
#include "../../src/plat/plat_irq.c"

#include "qiran/comm/comm_bytes.h"
#include "qiran/comm/comm_cmd.h"
#include "qiran/exec/exec_core.h"
#include "qiran/mission/mission_state.h"
#include "qiran/plat/plat_cpu.h"
#include "qiran/plat/plat_isr_uart.h"
#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_crc.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_log.h"
#include "qiran/svc/svc_time.h"
#include "test_framework.h"

#include <string.h>

#define OFF_R_ID        0U
#define OFF_R_VERSION   1U
#define OFF_R_RECEIPT   2U
#define OFF_R_SEQUENCE  6U
#define OFF_R_COMMAND   8U
#define OFF_R_RESULT    9U
#define OFF_R_DETAIL   10U
#define OFF_R_CRC      14U

/* --- link out --- */

static uint8_t  s_out[256];
static uint32_t s_out_len;
static uint32_t s_out_frames;

static qiran_status_t out_tx(void *ctx, const uint8_t *data, uint32_t len)
{
    QIRAN_UNUSED(ctx);
    if (len <= sizeof(s_out)) {
        memcpy(s_out, data, len);
        s_out_len = len;
    }
    s_out_frames++;
    return QIRAN_OK;
}

/* --- link in --- */

static uint8_t  s_feed[512];
static uint32_t s_feed_len;
static uint32_t s_feed_pos;
static bool     s_feed_gap;

static uint32_t uart_drain(void *ctx, svc_ring_t *rx, uint16_t *kind)
{
    uint32_t pushed = 0U;

    QIRAN_UNUSED(ctx);

    while (s_feed_pos < s_feed_len) {
        if (!svc_ring_push(rx, s_feed[s_feed_pos])) {
            break;
        }
        s_feed_pos++;
        pushed++;
    }

    *kind = s_feed_gap ? (uint16_t)PLAT_UART_KIND_GAP
                       : (uint16_t)PLAT_UART_KIND_DATA;
    return pushed;
}

static void feed(const uint8_t *data, uint32_t len)
{
    memcpy(s_feed, data, len);
    s_feed_len = len;
    s_feed_pos = 0U;
    s_feed_gap = false;
    irq_handler(&s_slot[PLAT_IRQ_UART]);
}

static void feed_gap(void)
{
    s_feed_len = 0U;
    s_feed_pos = 0U;
    s_feed_gap = true;
    irq_handler(&s_slot[PLAT_IRQ_UART]);
}

/* --- handlers --- */

static uint32_t       s_calls[COMM_CMD_COUNT];
static uint32_t       s_param[COMM_CMD_COUNT];
static qiran_status_t s_result[COMM_CMD_COUNT];

static qiran_status_t handler(void *ctx, uint32_t parameter)
{
    comm_cmd_id_t id = (comm_cmd_id_t)(uintptr_t)ctx;

    s_calls[id]++;
    s_param[id] = parameter;
    return s_result[id];
}

static void register_handler(comm_cmd_id_t id)
{
    CHECK_TRUE(comm_cmd_register(id, handler, (void *)(uintptr_t)id) == QIRAN_OK);
    s_calls[id] = 0U;
    s_param[id] = 0U;
    s_result[id] = QIRAN_OK;
}

static void setup(void)
{
    plat_isr_uart_device_t dev;

    exec_init();
    plat_irq_init();
    svc_log_init();
    svc_fdir_init();
    svc_config_init();
    svc_time_init();
    mission_state_init();
    comm_cmd_init();

    memset(s_calls, 0, sizeof(s_calls));
    memset(s_result, 0, sizeof(s_result));
    s_out_len = 0U;
    s_out_frames = 0U;
    s_feed_len = 0U;
    s_feed_pos = 0U;
    s_feed_gap = false;

    dev.drain = uart_drain;
    dev.ctx = NULL;
    dev.irq_id = 82U;
    dev.trigger = PLAT_GIC_TRIGGER_LEVEL_HIGH;
    CHECK_TRUE(plat_isr_uart_init(&dev) == QIRAN_OK);

    (void)comm_cmd_set_transmit(out_tx, NULL);
}

static void advance_to(mission_state_id_t target)
{
    static const mission_state_id_t k_path[] = {
        SPR_PRECOND, SPR_LASER_BRINGUP, SPR_MRR_TUNE, SPR_CROW_TUNE,
        SPR_UMZI_TUNE, SPR_DLI_LOCK, SPR_SPAD_ENABLE, SPR_PVS_T1,
        SPR_EXPERIMENT
    };
    uint32_t i;

    for (i = 0U; i < QIRAN_ARRAY_LEN(k_path); i++) {
        if (mission_state_current() == target) {
            return;
        }
        if (mission_state_request(k_path[i]) != QIRAN_OK) {
            return;
        }
    }
}

static void send_cmd(comm_cmd_id_t id, uint32_t parameter, uint16_t sequence)
{
    uint8_t frame[COMM_CMD_FRAME_BYTES];

    CHECK_TRUE(comm_cmd_build(id, parameter, sequence, frame, sizeof(frame),
                              NULL) == QIRAN_OK);
    feed(frame, sizeof(frame));
    comm_cmd_service();
}

/* --- table --- */

static void test_command_table(void)
{
    uint32_t i;

    TEST_CASE("all eleven telecommands are defined and named");
    setup();
    CHECK_EQ_U64(COMM_CMD_COUNT, 11U);

    for (i = 0U; i < (uint32_t)COMM_CMD_COUNT; i++) {
        const comm_cmd_def_t *d = comm_cmd_def((comm_cmd_id_t)i);

        CHECK_TRUE(d != NULL);
        CHECK_TRUE(d->name != NULL);
        CHECK_TRUE(d->budget_ms > 0U);
    }
    CHECK_TRUE(comm_cmd_def(COMM_CMD_COUNT) == NULL);

    TEST_CASE("a command that removes energy is accepted in every state");
    for (i = 0U; i < (uint32_t)SPR_COUNT; i++) {
        CHECK_TRUE(comm_cmd_legal_in(CMD_LASER_OFF, (mission_state_id_t)i));
        CHECK_TRUE(comm_cmd_legal_in(CMD_HEATERS_OFF, (mission_state_id_t)i));
        CHECK_TRUE(comm_cmd_legal_in(CMD_PAYLOAD_OFF, (mission_state_id_t)i));
    }

    TEST_CASE("a reset is accepted in every state, since recovery must work");
    for (i = 0U; i < (uint32_t)SPR_COUNT; i++) {
        CHECK_TRUE(comm_cmd_legal_in(CMD_PROCESSOR_RESET, (mission_state_id_t)i));
        CHECK_TRUE(comm_cmd_legal_in(CMD_POWER_ON_RESET, (mission_state_id_t)i));
        CHECK_TRUE(comm_cmd_legal_in(CMD_FLIGHT_MODE_RESET,
                                     (mission_state_id_t)i));
    }

    TEST_CASE("a command that applies energy is accepted only before the "
              "sequence commits");
    CHECK_TRUE(comm_cmd_legal_in(CMD_LASER_ON, SPR_BOOT));
    CHECK_TRUE(comm_cmd_legal_in(CMD_LASER_ON, SPR_PRECOND));
    CHECK_TRUE(!comm_cmd_legal_in(CMD_LASER_ON, SPR_MRR_TUNE));
    CHECK_TRUE(!comm_cmd_legal_in(CMD_LASER_ON, SPR_EXPERIMENT));
    CHECK_TRUE(!comm_cmd_legal_in(CMD_HEATERS_ON, SPR_EXPERIMENT));
    CHECK_TRUE(!comm_cmd_legal_in(CMD_PAYLOAD_ON, SPR_EXPERIMENT));

    TEST_CASE("out-of-range identifiers are not legal anywhere");
    CHECK_TRUE(!comm_cmd_legal_in(COMM_CMD_COUNT, SPR_BOOT));
    CHECK_TRUE(!comm_cmd_legal_in(CMD_LASER_OFF, SPR_COUNT));
}

static void test_frame_build(void)
{
    uint8_t frame[COMM_CMD_FRAME_BYTES + 4U];
    uint32_t written = 0U;

    TEST_CASE("a command frame is the length declared, and checksummed");
    setup();
    memset(frame, 0xEE, sizeof(frame));

    CHECK_TRUE(comm_cmd_build(CMD_LASER_OFF, 0x11223344U, 0x5566U, frame,
                              sizeof(frame), &written) == QIRAN_OK);
    CHECK_EQ_U64(written, COMM_CMD_FRAME_BYTES);
    CHECK_EQ_U64(written, 13U);
    CHECK_EQ_U64(frame[COMM_CMD_FRAME_BYTES], 0xEEU);

    CHECK_EQ_U64(frame[0], COMM_CMD_PACKET_ID);
    CHECK_EQ_U64(frame[1], COMM_CMD_VERSION);
    CHECK_EQ_U64(frame[2], CMD_LASER_OFF);
    CHECK_EQ_U64(comm_get_u32(frame, 3U), 0x11223344U);
    CHECK_EQ_U64(comm_get_u16(frame, 7U), 0x5566U);
    CHECK_EQ_U64(comm_get_u32(frame, 9U), svc_crc32(frame, 9U));

    TEST_CASE("a short buffer is refused");
    CHECK_TRUE(comm_cmd_build(CMD_LASER_OFF, 0U, 0U, frame, 4U, &written)
               == QIRAN_ERR_PARAM);
}

/* --- the sequence --- */

static void test_accepted_command(void)
{
    TEST_CASE("a well formed, legal command reaches its handler and completes");
    setup();
    register_handler(CMD_LASER_OFF);
    advance_to(SPR_EXPERIMENT);

    send_cmd(CMD_LASER_OFF, 0xABCDU, 0x0007U);

    CHECK_EQ_U64(comm_cmd_received(), 1U);
    CHECK_EQ_U64(s_calls[CMD_LASER_OFF], 1U);
    CHECK_EQ_U64(s_param[CMD_LASER_OFF], 0xABCDU);
    CHECK_EQ_U64(comm_cmd_completed(), 1U);
    CHECK_EQ_U64(comm_cmd_rejected(), 0U);

    TEST_CASE("a result is returned naming the command and the sequence given");
    CHECK_EQ_U64(s_out_len, COMM_CMD_RESULT_BYTES);
    CHECK_EQ_U64(s_out_len, 18U);
    CHECK_EQ_U64(s_out[OFF_R_ID], COMM_CMD_RESULT_ID);
    CHECK_EQ_U64(comm_get_u16(s_out, OFF_R_SEQUENCE), 0x0007U);
    CHECK_EQ_U64(s_out[OFF_R_COMMAND], CMD_LASER_OFF);
    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_COMPLETED);
    CHECK_EQ_U64(comm_get_u32(s_out, OFF_R_CRC), svc_crc32(s_out, OFF_R_CRC));
}

static void test_time_tagging(void)
{
    uint32_t i;

    TEST_CASE("the result carries the time the command was received");
    setup();
    register_handler(CMD_LASER_OFF);

    for (i = 0U; i < 100U; i++) {
        exec_on_minor_tick();
    }

    send_cmd(CMD_LASER_OFF, 0U, 1U);
    CHECK_EQ_U64(comm_get_u32(s_out, OFF_R_RECEIPT), 2000U);
    CHECK_EQ_U64(comm_cmd_last_receipt_ms(), 2000U);

    TEST_CASE("a later command carries a later tag");
    for (i = 0U; i < 50U; i++) {
        exec_on_minor_tick();
    }
    send_cmd(CMD_LASER_OFF, 0U, 2U);
    CHECK_EQ_U64(comm_get_u32(s_out, OFF_R_RECEIPT), 3000U);
}

static void test_format_and_unknown_rejections(void)
{
    uint8_t frame[COMM_CMD_FRAME_BYTES];

    TEST_CASE("a frame of an unknown format version is rejected as format");
    setup();
    register_handler(CMD_LASER_OFF);

    CHECK_TRUE(comm_cmd_build(CMD_LASER_OFF, 0U, 3U, frame, sizeof(frame), NULL)
               == QIRAN_OK);
    frame[1] = 99U;
    frame[9] = 0U;
    comm_put_u32(frame, 9U, svc_crc32(frame, 9U));
    feed(frame, sizeof(frame));
    comm_cmd_service();

    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_REJECT_FORMAT);
    CHECK_EQ_U64(s_calls[CMD_LASER_OFF], 0U);
    CHECK_EQ_U64(comm_cmd_rejected(), 1U);

    TEST_CASE("an identifier outside the command set is rejected as unknown");
    setup();
    CHECK_TRUE(comm_cmd_build(CMD_LASER_OFF, 0U, 4U, frame, sizeof(frame), NULL)
               == QIRAN_OK);
    frame[2] = 200U;
    comm_put_u32(frame, 9U, svc_crc32(frame, 9U));
    feed(frame, sizeof(frame));
    comm_cmd_service();

    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_REJECT_UNKNOWN);
    CHECK_EQ_U64(s_out[OFF_R_COMMAND], 200U);

    TEST_CASE("a rejected command is logged as a warning, not escalated");
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_COMMAND_REJECTED));
    CHECK_EQ_U64(svc_fdir_severity_count(QIRAN_SEV_MINOR), 1U);
}

static void test_state_rejection(void)
{
    TEST_CASE("a command not legal in the current state is refused");
    setup();
    register_handler(CMD_LASER_ON);
    advance_to(SPR_EXPERIMENT);

    send_cmd(CMD_LASER_ON, 0U, 5U);

    CHECK_EQ_U64(s_calls[CMD_LASER_ON], 0U);
    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_REJECT_STATE);
    CHECK_EQ_U64(comm_get_u32(s_out, OFF_R_DETAIL), SPR_EXPERIMENT);

    TEST_CASE("the same command is accepted where the table allows it");
    setup();
    register_handler(CMD_LASER_ON);
    CHECK_EQ_U64(mission_state_current(), SPR_BOOT);

    send_cmd(CMD_LASER_ON, 0U, 6U);
    CHECK_EQ_U64(s_calls[CMD_LASER_ON], 1U);
    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_COMPLETED);
}

static void test_parameter_rejection(void)
{
    TEST_CASE("the only selectable mode is the one already running");
    setup();
    send_cmd(CMD_MODE, 0U, 7U);
    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_COMPLETED);
    CHECK_EQ_U64(comm_cmd_completed(), 1U);

    TEST_CASE("any other mode value is out of range");
    send_cmd(CMD_MODE, 1U, 8U);
    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_REJECT_PARAM);
    CHECK_EQ_U64(comm_get_u32(s_out, OFF_R_DETAIL), 1U);

    send_cmd(CMD_MODE, 0xFFFFFFFFU, 9U);
    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_REJECT_PARAM);

    TEST_CASE("a command taking no parameter ignores whatever is sent");
    register_handler(CMD_LASER_OFF);
    send_cmd(CMD_LASER_OFF, 0xDEADBEEFU, 10U);
    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_COMPLETED);
    CHECK_EQ_U64(s_param[CMD_LASER_OFF], 0xDEADBEEFU);
}

static void test_no_handler_is_a_failure_not_a_rejection(void)
{
    TEST_CASE("a legal command nothing can carry out fails rather than rejects");
    setup();
    send_cmd(CMD_LASER_OFF, 0U, 11U);

    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_FAILED);
    CHECK_EQ_U64(comm_cmd_failed(), 1U);
    CHECK_EQ_U64(comm_cmd_rejected(), 0U);

    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_COMMAND_FAILED));
}

static void test_handler_failure(void)
{
    TEST_CASE("a handler that refuses the command fails it and says why");
    setup();
    register_handler(CMD_HEATERS_OFF);
    s_result[CMD_HEATERS_OFF] = QIRAN_ERR_HARDWARE;

    send_cmd(CMD_HEATERS_OFF, 0U, 12U);
    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_FAILED);
    CHECK_EQ_U64(comm_get_u32(s_out, OFF_R_DETAIL), QIRAN_ERR_HARDWARE);
    CHECK_EQ_U64(comm_cmd_failed(), 1U);
}

/* --- execution tracking --- */

static void test_long_running_command(void)
{
    TEST_CASE("a command still running is acknowledged as accepted");
    setup();
    register_handler(CMD_PAYLOAD_OFF);
    s_result[CMD_PAYLOAD_OFF] = QIRAN_ERR_BUSY;

    send_cmd(CMD_PAYLOAD_OFF, 0U, 13U);
    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_ACCEPTED);
    CHECK_TRUE(comm_cmd_in_progress());
    CHECK_EQ_U64(comm_cmd_completed(), 0U);

    TEST_CASE("it is called again on later cycles until it finishes");
    comm_cmd_service();
    comm_cmd_service();
    CHECK_EQ_U64(s_calls[CMD_PAYLOAD_OFF], 3U);
    CHECK_TRUE(comm_cmd_in_progress());

    s_result[CMD_PAYLOAD_OFF] = QIRAN_OK;
    comm_cmd_service();
    CHECK_TRUE(!comm_cmd_in_progress());
    CHECK_EQ_U64(comm_cmd_completed(), 1U);
    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_COMPLETED);
    CHECK_EQ_U64(comm_get_u16(s_out, OFF_R_SEQUENCE), 13U);

    TEST_CASE("a second command is refused while one is still running");
    setup();
    register_handler(CMD_PAYLOAD_OFF);
    register_handler(CMD_LASER_OFF);
    s_result[CMD_PAYLOAD_OFF] = QIRAN_ERR_BUSY;
    send_cmd(CMD_PAYLOAD_OFF, 0U, 14U);
    CHECK_TRUE(comm_cmd_in_progress());

    send_cmd(CMD_LASER_OFF, 0U, 15U);
    CHECK_EQ_U64(s_calls[CMD_LASER_OFF], 0U);
    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_REJECT_BUSY);
    CHECK_EQ_U64(comm_get_u16(s_out, OFF_R_SEQUENCE), 15U);
}

static void test_command_timeout(void)
{
    uint32_t i;

    TEST_CASE("a command that never finishes is timed out at its budget");
    setup();
    register_handler(CMD_PAYLOAD_OFF);
    s_result[CMD_PAYLOAD_OFF] = QIRAN_ERR_BUSY;

    send_cmd(CMD_PAYLOAD_OFF, 0U, 16U);
    CHECK_TRUE(comm_cmd_in_progress());

    for (i = 0U; i < 200U; i++) {
        exec_on_minor_tick();
        comm_cmd_service();
        if (!comm_cmd_in_progress()) {
            break;
        }
    }

    CHECK_TRUE(!comm_cmd_in_progress());
    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_TIMEOUT);
    CHECK_EQ_U64(comm_get_u32(s_out, OFF_R_DETAIL),
                 comm_cmd_def(CMD_PAYLOAD_OFF)->budget_ms);
    CHECK_EQ_U64(comm_cmd_failed(), 1U);

    TEST_CASE("the link is free for the next command afterwards");
    s_result[CMD_PAYLOAD_OFF] = QIRAN_OK;
    send_cmd(CMD_PAYLOAD_OFF, 0U, 17U);
    CHECK_EQ_U64(s_out[OFF_R_RESULT], COMM_CMD_COMPLETED);
}

/* --- framing --- */

static void test_framing_recovers_from_noise(void)
{
    static const uint8_t k_noise[] = { 0x00U, 0xFFU, 0x12U, 0x34U };
    uint8_t frame[COMM_CMD_FRAME_BYTES];

    TEST_CASE("bytes before a frame start are discarded");
    setup();
    register_handler(CMD_LASER_OFF);

    feed(k_noise, (uint32_t)sizeof(k_noise));
    comm_cmd_service();
    CHECK_EQ_U64(comm_cmd_received(), 0U);

    send_cmd(CMD_LASER_OFF, 0U, 18U);
    CHECK_EQ_U64(comm_cmd_received(), 1U);
    CHECK_EQ_U64(comm_cmd_completed(), 1U);

    TEST_CASE("a frame that fails its checksum is dropped and resynchronised");
    setup();
    register_handler(CMD_LASER_OFF);
    CHECK_TRUE(comm_cmd_build(CMD_LASER_OFF, 0U, 19U, frame, sizeof(frame), NULL)
               == QIRAN_OK);
    frame[4] ^= 0x01U;
    feed(frame, sizeof(frame));
    comm_cmd_service();

    CHECK_EQ_U64(comm_cmd_received(), 0U);
    CHECK_EQ_U64(s_calls[CMD_LASER_OFF], 0U);
    CHECK_TRUE(comm_cmd_resyncs() > 0U);

    TEST_CASE("a good frame after a corrupt one is still parsed");
    send_cmd(CMD_LASER_OFF, 0U, 20U);
    CHECK_EQ_U64(comm_cmd_received(), 1U);
    CHECK_EQ_U64(comm_cmd_completed(), 1U);

    TEST_CASE("a truncated frame does not consume the one that follows it");
    setup();
    register_handler(CMD_LASER_OFF);
    CHECK_TRUE(comm_cmd_build(CMD_LASER_OFF, 0U, 21U, frame, sizeof(frame), NULL)
               == QIRAN_OK);
    feed(frame, 6U);           /* half a frame */
    comm_cmd_service();
    CHECK_EQ_U64(comm_cmd_received(), 0U);

    feed_gap();                /* the link goes quiet: frame boundary */
    comm_cmd_service();

    send_cmd(CMD_LASER_OFF, 0U, 22U);
    CHECK_EQ_U64(comm_cmd_received(), 1U);
    CHECK_EQ_U64(comm_get_u16(s_out, OFF_R_SEQUENCE), 22U);
}

static void test_two_frames_in_one_pass(void)
{
    uint8_t pair[COMM_CMD_FRAME_BYTES * 2U];

    TEST_CASE("two frames arriving together are both processed");
    setup();
    register_handler(CMD_LASER_OFF);
    register_handler(CMD_HEATERS_OFF);

    CHECK_TRUE(comm_cmd_build(CMD_LASER_OFF, 0U, 23U, pair,
                              COMM_CMD_FRAME_BYTES, NULL) == QIRAN_OK);
    CHECK_TRUE(comm_cmd_build(CMD_HEATERS_OFF, 0U, 24U,
                              &pair[COMM_CMD_FRAME_BYTES],
                              COMM_CMD_FRAME_BYTES, NULL) == QIRAN_OK);

    feed(pair, (uint32_t)sizeof(pair));
    comm_cmd_service();

    CHECK_EQ_U64(comm_cmd_received(), 2U);
    CHECK_EQ_U64(s_calls[CMD_LASER_OFF], 1U);
    CHECK_EQ_U64(s_calls[CMD_HEATERS_OFF], 1U);
    CHECK_EQ_U64(comm_cmd_completed(), 2U);
    CHECK_EQ_U64(comm_get_u16(s_out, OFF_R_SEQUENCE), 24U);
}

static void test_processing_without_a_return_link(void)
{
    TEST_CASE("commands are still carried out when results cannot be returned");
    setup();
    register_handler(CMD_LASER_OFF);
    (void)comm_cmd_set_transmit(NULL, NULL);

    send_cmd(CMD_LASER_OFF, 0U, 25U);
    CHECK_EQ_U64(s_calls[CMD_LASER_OFF], 1U);
    CHECK_EQ_U64(comm_cmd_completed(), 1U);
    CHECK_EQ_U64(s_out_frames, 0U);
}

static void test_registration_rules(void)
{
    TEST_CASE("handler registration is validated and refuses to replace one");
    setup();
    CHECK_TRUE(comm_cmd_register(COMM_CMD_COUNT, handler, NULL)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_cmd_register(CMD_LASER_ON, NULL, NULL) == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_cmd_register(CMD_LASER_ON, handler, NULL) == QIRAN_OK);
    CHECK_TRUE(comm_cmd_register(CMD_LASER_ON, handler, NULL)
               == QIRAN_ERR_STATE);

    TEST_CASE("the mode command's meaning is not overridable");
    CHECK_TRUE(comm_cmd_register(CMD_MODE, handler, NULL) == QIRAN_ERR_STATE);

    TEST_CASE("every result has a name");
    CHECK_TRUE(comm_cmd_result_name(COMM_CMD_COMPLETED)[0] != '?');
    CHECK_TRUE(comm_cmd_result_name(COMM_CMD_REJECT_STATE)[0] != '?');
    CHECK_TRUE(comm_cmd_result_name(COMM_CMD_TIMEOUT)[0] != '?');
    CHECK_TRUE(comm_cmd_result_name((comm_cmd_result_t)200)[0] == '?');
}

int main(void)
{
    plat_cpu_init();

    test_command_table();
    test_frame_build();
    test_accepted_command();
    test_time_tagging();
    test_format_and_unknown_rejections();
    test_state_rejection();
    test_parameter_rejection();
    test_no_handler_is_a_failure_not_a_rejection();
    test_handler_failure();
    test_long_running_command();
    test_command_timeout();
    test_framing_recovers_from_noise();
    test_two_frames_in_one_pass();
    test_processing_without_a_return_link();
    test_registration_rules();

    return TEST_REPORT();
}
