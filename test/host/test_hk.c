#include "qiran/comm/comm_bytes.h"
#include "qiran/comm/comm_rs485_hk.h"
#include "qiran/exec/exec_core.h"
#include "qiran/mission/mission_state.h"
#include "qiran/plat/plat_cpu.h"
#include "qiran/plat/plat_irq.h"
#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_crc.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_health.h"
#include "qiran/svc/svc_log.h"
#include "qiran/svc/svc_watchdog.h"
#include "qiran/svc/svc_time.h"
#include "test_framework.h"

#include <string.h>

/* Offsets read straight off the frame definition table. */
#define OFF_ID          0U
#define OFF_VERSION     1U
#define OFF_TIMESTAMP   2U
#define OFF_RAIL_1V0    6U
#define OFF_RAIL_1V5    8U
#define OFF_RAIL_1V8   10U
#define OFF_RAIL_3V3   12U
#define OFF_RAIL_5V    14U
#define OFF_PS_TEMP    16U
#define OFF_BOARD_T0   18U
#define OFF_BOARD_T1   20U
#define OFF_DDR_TEMP   22U
#define OFF_PS_CLOCK   24U
#define OFF_PL_CLOCK   28U
#define OFF_POR        32U
#define OFF_PL_CFG     33U
#define OFF_WATCHDOG   34U
#define OFF_OVERCURR   35U
#define OFF_BROWNOUT   36U
#define OFF_PL_ERROR   37U
#define OFF_BOOT_MODE  45U
#define OFF_UPTIME     46U
#define OFF_FAULT_LOG  50U
#define OFF_HK_CRC     52U

#define OFF_ST_PAYLOAD   6U
#define OFF_ST_HEALTH    7U
#define OFF_ST_INTERLOCK 8U
#define OFF_ST_FAULT    13U
#define OFF_ST_SEVERITY 15U
#define OFF_ST_CRC      16U

static uint8_t  s_link[512];
static uint32_t s_link_len;
static uint32_t s_tx_calls;
static qiran_status_t s_tx_result;

static qiran_status_t fake_tx(void *ctx, const uint8_t *data, uint32_t len)
{
    QIRAN_UNUSED(ctx);
    s_tx_calls++;
    if (s_tx_result != QIRAN_OK) {
        return s_tx_result;
    }
    if (len <= sizeof(s_link)) {
        memcpy(s_link, data, len);
        s_link_len = len;
    }
    return QIRAN_OK;
}

static comm_hk_sample_t s_sample;
static qiran_status_t   s_sample_result;
static uint32_t         s_sample_calls;

static qiran_status_t fake_sample(void *ctx, comm_hk_sample_t *out)
{
    QIRAN_UNUSED(ctx);
    s_sample_calls++;
    if (s_sample_result != QIRAN_OK) {
        return s_sample_result;
    }
    *out = s_sample;
    return QIRAN_OK;
}

static void build_sample(void)
{
    memset(&s_sample, 0, sizeof(s_sample));
    s_sample.rail_mv[0] = 1000U;
    s_sample.rail_mv[1] = 1500U;
    s_sample.rail_mv[2] = 1800U;
    s_sample.rail_mv[3] = 3300U;
    s_sample.rail_mv[4] = 5000U;
    s_sample.ps_temp_c100 = 4250;
    s_sample.board_temp_c100[0] = 2710;
    s_sample.board_temp_c100[1] = -1250;
    s_sample.ddr_temp_c100 = 3900;
    s_sample.ps_clock_hz = 666666687U;
    s_sample.pl_clock_hz = 100000000U;
    s_sample.por_status = 0x03U;
    s_sample.pl_config_done = 1U;
    s_sample.overcurrent_flags = 0x12U;
    s_sample.brownout_flags = 0x34U;
    s_sample.pl_error[0] = 0xDEU;
    s_sample.pl_error[7] = 0xADU;
    s_sample.boot_mode = 0x0AU;
}

static void setup(bool with_link)
{
    exec_init();
    plat_irq_init();
    svc_log_init();
    svc_fdir_init();
    svc_config_init();
    svc_time_init();
    mission_state_init();
    svc_health_init();
    (void)svc_watchdog_init(NULL);
    comm_rs485_hk_init();

    s_link_len = 0U;
    s_tx_calls = 0U;
    s_tx_result = QIRAN_OK;
    s_sample_calls = 0U;
    s_sample_result = QIRAN_OK;
    build_sample();

    if (with_link) {
        (void)comm_rs485_hk_set_transmit(fake_tx, NULL);
        (void)comm_rs485_hk_set_sampler(fake_sample, NULL);
    }
}

static void tick(uint32_t n)
{
    uint32_t i;
    for (i = 0U; i < n; i++) {
        exec_on_minor_tick();
    }
}

/* --- housekeeping frame --- */

static void test_frame_layout(void)
{
    uint8_t buf[COMM_HK_FRAME_BYTES + 8U];
    uint32_t written = 0U;
    uint32_t i;

    TEST_CASE("the frame is exactly the length the definition gives");
    setup(true);
    memset(buf, 0xEE, sizeof(buf));

    CHECK_TRUE(comm_rs485_hk_build(buf, sizeof(buf), &written) == QIRAN_OK);
    CHECK_EQ_U64(written, COMM_HK_FRAME_BYTES);
    CHECK_EQ_U64(written, 56U);
    CHECK_EQ_U64(buf[COMM_HK_FRAME_BYTES], 0xEEU);

    TEST_CASE("the header carries the packet identifier and format version");
    CHECK_EQ_U64(buf[OFF_ID], COMM_HK_PACKET_ID);
    CHECK_EQ_U64(buf[OFF_ID], 0xA5U);
    CHECK_EQ_U64(buf[OFF_VERSION], COMM_HK_PACKET_VERSION);

    TEST_CASE("every rail voltage sits at its documented offset");
    CHECK_EQ_U64(comm_get_u16(buf, OFF_RAIL_1V0), 1000U);
    CHECK_EQ_U64(comm_get_u16(buf, OFF_RAIL_1V5), 1500U);
    CHECK_EQ_U64(comm_get_u16(buf, OFF_RAIL_1V8), 1800U);
    CHECK_EQ_U64(comm_get_u16(buf, OFF_RAIL_3V3), 3300U);
    CHECK_EQ_U64(comm_get_u16(buf, OFF_RAIL_5V), 5000U);

    TEST_CASE("temperatures sit at theirs, negative values included");
    CHECK_EQ_U64(comm_get_u16(buf, OFF_PS_TEMP), 4250U);
    CHECK_EQ_U64(comm_get_u16(buf, OFF_BOARD_T0), 2710U);
    /* Minus twelve and a half degrees, in two's complement. */
    CHECK_EQ_U64(comm_get_u16(buf, OFF_BOARD_T1), 64286U);
    CHECK_EQ_U64((int16_t)comm_get_u16(buf, OFF_BOARD_T1) / 100, -12);
    CHECK_EQ_U64(comm_get_u16(buf, OFF_DDR_TEMP), 3900U);

    TEST_CASE("measured clocks sit at theirs");
    CHECK_EQ_U64(comm_get_u32(buf, OFF_PS_CLOCK), 666666687U);
    CHECK_EQ_U64(comm_get_u32(buf, OFF_PL_CLOCK), 100000000U);

    TEST_CASE("the raw status bytes are passed through unaltered");
    CHECK_EQ_U64(buf[OFF_POR], 0x03U);
    CHECK_EQ_U64(buf[OFF_PL_CFG], 1U);
    CHECK_EQ_U64(buf[OFF_OVERCURR], 0x12U);
    CHECK_EQ_U64(buf[OFF_BROWNOUT], 0x34U);
    CHECK_EQ_U64(buf[OFF_BOOT_MODE], 0x0AU);
    CHECK_EQ_U64(buf[OFF_PL_ERROR], 0xDEU);
    CHECK_EQ_U64(buf[OFF_PL_ERROR + 7U], 0xADU);

    TEST_CASE("a buffer shorter than the frame is refused");
    CHECK_TRUE(comm_rs485_hk_build(buf, COMM_HK_FRAME_BYTES - 1U, &written)
               == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_rs485_hk_build(NULL, sizeof(buf), &written)
               == QIRAN_ERR_PARAM);

    TEST_CASE("a written length is optional");
    CHECK_TRUE(comm_rs485_hk_build(buf, sizeof(buf), NULL) == QIRAN_OK);

    for (i = 0U; i < COMM_HK_RAILS; i++) {
        CHECK_TRUE(s_sample.rail_mv[i] != 0U);
    }
}

static void test_big_endian_packing(void)
{
    uint8_t buf[COMM_HK_FRAME_BYTES];

    TEST_CASE("multi-byte fields are packed most significant byte first");
    setup(true);
    s_sample.ps_clock_hz = 0x01020304U;
    s_sample.rail_mv[0] = 0xABCDU;

    CHECK_TRUE(comm_rs485_hk_build(buf, sizeof(buf), NULL) == QIRAN_OK);

    CHECK_EQ_U64(buf[OFF_PS_CLOCK], 0x01U);
    CHECK_EQ_U64(buf[OFF_PS_CLOCK + 1U], 0x02U);
    CHECK_EQ_U64(buf[OFF_PS_CLOCK + 2U], 0x03U);
    CHECK_EQ_U64(buf[OFF_PS_CLOCK + 3U], 0x04U);

    CHECK_EQ_U64(buf[OFF_RAIL_1V0], 0xABU);
    CHECK_EQ_U64(buf[OFF_RAIL_1V0 + 1U], 0xCDU);
}

static void test_frame_integrity(void)
{
    uint8_t buf[COMM_HK_FRAME_BYTES];
    uint32_t stored;

    TEST_CASE("the checksum covers everything before it");
    setup(true);
    CHECK_TRUE(comm_rs485_hk_build(buf, sizeof(buf), NULL) == QIRAN_OK);

    stored = comm_get_u32(buf, OFF_HK_CRC);
    CHECK_EQ_U64(stored, svc_crc32(buf, OFF_HK_CRC));

    TEST_CASE("a single changed byte anywhere invalidates it");
    buf[OFF_RAIL_3V3] ^= 0x01U;
    CHECK_TRUE(svc_crc32(buf, OFF_HK_CRC) != stored);
    buf[OFF_RAIL_3V3] ^= 0x01U;
    buf[OFF_ID] ^= 0x80U;
    CHECK_TRUE(svc_crc32(buf, OFF_HK_CRC) != stored);
}

static void test_fields_the_firmware_owns(void)
{
    uint8_t buf[COMM_HK_FRAME_BYTES];

    TEST_CASE("timestamp and uptime come from the flight software itself");
    setup(true);
    tick(150U);   /* three seconds */

    CHECK_TRUE(comm_rs485_hk_build(buf, sizeof(buf), NULL) == QIRAN_OK);
    CHECK_EQ_U64(comm_get_u32(buf, OFF_TIMESTAMP), 3000U);
    CHECK_EQ_U64(comm_get_u32(buf, OFF_UPTIME), 3U);

    TEST_CASE("watchdog status reflects whether one is actually armed");
    CHECK_EQ_U64(buf[OFF_WATCHDOG], 0U);

    TEST_CASE("the fault counter comes from the log");
    svc_log_fault(QIRAN_FAULT_OVERCURRENT, QIRAN_SEV_SEVERE, 0U);
    svc_log_fault(QIRAN_FAULT_CONFIG, QIRAN_SEV_CRITICAL, 0U);
    CHECK_TRUE(comm_rs485_hk_build(buf, sizeof(buf), NULL) == QIRAN_OK);
    CHECK_EQ_U64(comm_get_u16(buf, OFF_FAULT_LOG), 2U);
}

static void test_frame_is_sent_even_without_board_data(void)
{
    uint8_t buf[COMM_HK_FRAME_BYTES];

    TEST_CASE("with no sampler the frame is still built, with those fields zero");
    setup(false);
    CHECK_TRUE(comm_rs485_hk_build(buf, sizeof(buf), NULL) == QIRAN_OK);
    CHECK_EQ_U64(comm_get_u16(buf, OFF_RAIL_3V3), 0U);
    CHECK_EQ_U64(comm_get_u32(buf, OFF_PS_CLOCK), 0U);
    CHECK_EQ_U64(buf[OFF_ID], COMM_HK_PACKET_ID);

    TEST_CASE("the missing sampler is counted and reported, not hidden");
    CHECK_EQ_U64(comm_rs485_hk_sample_failures(), 1U);
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_HK_SAMPLE));

    TEST_CASE("a sampler that fails leaves no stale readings behind");
    setup(true);
    CHECK_TRUE(comm_rs485_hk_build(buf, sizeof(buf), NULL) == QIRAN_OK);
    CHECK_EQ_U64(comm_get_u16(buf, OFF_RAIL_3V3), 3300U);

    s_sample_result = QIRAN_ERR_HARDWARE;
    CHECK_TRUE(comm_rs485_hk_build(buf, sizeof(buf), NULL) == QIRAN_OK);
    CHECK_EQ_U64(comm_get_u16(buf, OFF_RAIL_3V3), 0U);
    CHECK_EQ_U64(comm_rs485_hk_sample_failures(), 1U);
}

/* --- status report --- */

static void test_status_frame(void)
{
    comm_status_report_t r;
    uint8_t buf[COMM_HK_STATUS_BYTES + 4U];
    uint32_t written = 0U;

    TEST_CASE("status is reported as formal parameter values, not a bit pattern");
    setup(true);
    memset(buf, 0xEE, sizeof(buf));

    r.payload = QIRAN_PAYLOAD_OPERATIONAL;
    r.health = QIRAN_HEALTH_FAILED;
    r.interlock[0] = COMM_INTERLOCK_PASS;
    r.interlock[1] = COMM_INTERLOCK_FAIL;
    r.interlock[2] = COMM_INTERLOCK_UNEVALUATED;
    r.interlock[3] = COMM_INTERLOCK_PASS;
    r.interlock[4] = COMM_INTERLOCK_PASS;
    r.fault = QIRAN_FAULT_DLI_LOCK;
    r.severity = QIRAN_SEV_CRITICAL;

    CHECK_TRUE(comm_rs485_status_build(&r, buf, sizeof(buf), &written) == QIRAN_OK);
    CHECK_EQ_U64(written, COMM_HK_STATUS_BYTES);
    CHECK_EQ_U64(written, 20U);
    CHECK_EQ_U64(buf[COMM_HK_STATUS_BYTES], 0xEEU);

    CHECK_EQ_U64(buf[OFF_ID], COMM_HK_STATUS_ID);
    CHECK_EQ_U64(buf[OFF_ST_PAYLOAD], QIRAN_PAYLOAD_OPERATIONAL);
    CHECK_EQ_U64(buf[OFF_ST_HEALTH], QIRAN_HEALTH_FAILED);
    CHECK_EQ_U64(buf[OFF_ST_INTERLOCK + 0U], COMM_INTERLOCK_PASS);
    CHECK_EQ_U64(buf[OFF_ST_INTERLOCK + 1U], COMM_INTERLOCK_FAIL);
    CHECK_EQ_U64(buf[OFF_ST_INTERLOCK + 2U], COMM_INTERLOCK_UNEVALUATED);
    CHECK_EQ_U64(comm_get_u16(buf, OFF_ST_FAULT), QIRAN_FAULT_DLI_LOCK);
    CHECK_EQ_U64(buf[OFF_ST_SEVERITY], QIRAN_SEV_CRITICAL);
    CHECK_EQ_U64(comm_get_u32(buf, OFF_ST_CRC), svc_crc32(buf, OFF_ST_CRC));

    TEST_CASE("a short buffer or absent report is refused");
    CHECK_TRUE(comm_rs485_status_build(&r, buf, 4U, &written) == QIRAN_ERR_PARAM);
    CHECK_TRUE(comm_rs485_status_build(NULL, buf, sizeof(buf), &written)
               == QIRAN_ERR_PARAM);

    TEST_CASE("an interlock not yet evaluated says so rather than passing");
    setup(true);
    CHECK_TRUE(comm_rs485_status_send(&r) == QIRAN_OK);
    comm_rs485_hk_set_interlock(0U, COMM_INTERLOCK_PASS);
    comm_rs485_hk_set_interlock(9U, COMM_INTERLOCK_PASS);
}

/* --- periodic sending --- */

static void test_period_is_honoured(void)
{
    TEST_CASE("the first service sends immediately");
    setup(true);
    comm_rs485_hk_service();
    CHECK_EQ_U64(comm_rs485_hk_frames_sent(), 1U);
    CHECK_EQ_U64(s_link_len, COMM_HK_FRAME_BYTES);
    CHECK_EQ_U64(s_link[OFF_ID], COMM_HK_PACKET_ID);

    TEST_CASE("it does not send again before its interval has elapsed");
    comm_rs485_hk_service();
    comm_rs485_hk_service();
    CHECK_EQ_U64(comm_rs485_hk_frames_sent(), 1U);

    tick(49U);   /* 980 ms of a 1000 ms interval */
    comm_rs485_hk_service();
    CHECK_EQ_U64(comm_rs485_hk_frames_sent(), 1U);

    TEST_CASE("it sends once the interval has elapsed");
    tick(1U);
    comm_rs485_hk_service();
    CHECK_EQ_U64(comm_rs485_hk_frames_sent(), 2U);

    TEST_CASE("shortening the interval is picked up");
    CHECK_TRUE(svc_config_set(CFG_HK_PERIOD_MS, 100U) == QIRAN_OK);
    tick(5U);
    comm_rs485_hk_service();
    CHECK_EQ_U64(comm_rs485_hk_frames_sent(), 3U);
}

static void test_transmit_failures(void)
{
    TEST_CASE("with no link the frame is not counted as sent, and is reported");
    setup(false);
    CHECK_TRUE(!comm_rs485_hk_ready());
    comm_rs485_hk_service();
    CHECK_EQ_U64(comm_rs485_hk_frames_sent(), 0U);
    CHECK_EQ_U64(comm_rs485_hk_transmit_failures(), 1U);
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_HK_TRANSMIT));

    TEST_CASE("a link that refuses the frame is counted and reported");
    setup(true);
    CHECK_TRUE(comm_rs485_hk_ready());
    s_tx_result = QIRAN_ERR_HARDWARE;
    comm_rs485_hk_service();
    CHECK_EQ_U64(s_tx_calls, 1U);
    CHECK_EQ_U64(comm_rs485_hk_frames_sent(), 0U);
    CHECK_EQ_U64(comm_rs485_hk_transmit_failures(), 1U);

    TEST_CASE("persistent link failure escalates through its fault class");
    {
        uint32_t i;
        for (i = 0U; i < 10U; i++) {
            tick(51U);
            comm_rs485_hk_service();
            error_handling_service();
        }
    }
    CHECK_TRUE(svc_fdir_occurrences(QIRAN_FAULT_HK_TRANSMIT) >= 5U);
}

/* --- the fault path actually reaching the link --- */

static void test_escalations_are_transmitted(void)
{
    TEST_CASE("registering this link makes escalations leave the payload");
    setup(true);
    CHECK_TRUE(svc_fdir_set_uplink(comm_rs485_hk_uplink()) == QIRAN_OK);
    CHECK_EQ_U64(svc_fdir_undelivered(), 0U);

    svc_fdir_report(QIRAN_FAULT_OVERCURRENT, 0x99U);
    error_handling_service();

    CHECK_EQ_U64(comm_rs485_hk_status_sent(), 1U);
    CHECK_EQ_U64(svc_fdir_undelivered(), 0U);

    TEST_CASE("the frame that leaves names the fault and its tier");
    CHECK_EQ_U64(s_link_len, COMM_HK_STATUS_BYTES);
    CHECK_EQ_U64(s_link[OFF_ID], COMM_HK_STATUS_ID);
    CHECK_EQ_U64(comm_get_u16(s_link, OFF_ST_FAULT), QIRAN_FAULT_OVERCURRENT);
    CHECK_EQ_U64(s_link[OFF_ST_SEVERITY], QIRAN_SEV_SEVERE);

    TEST_CASE("it carries the payload status current at the time");
    CHECK_EQ_U64(s_link[OFF_ST_PAYLOAD], QIRAN_PAYLOAD_NON_OPERATIONAL);

    TEST_CASE("a link failure during escalation is counted as undelivered");
    setup(true);
    CHECK_TRUE(svc_fdir_set_uplink(comm_rs485_hk_uplink()) == QIRAN_OK);
    s_tx_result = QIRAN_ERR_HARDWARE;
    svc_fdir_report(QIRAN_FAULT_TEMPERATURE, 0U);
    error_handling_service();
    CHECK_EQ_U64(svc_fdir_undelivered(), 1U);
}

int main(void)
{
    plat_cpu_init();

    test_frame_layout();
    test_big_endian_packing();
    test_frame_integrity();
    test_fields_the_firmware_owns();
    test_frame_is_sent_even_without_board_data();
    test_status_frame();
    test_period_is_honoured();
    test_transmit_failures();
    test_escalations_are_transmitted();

    return TEST_REPORT();
}
