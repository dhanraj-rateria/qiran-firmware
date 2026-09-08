/*
 * The interrupt framework is compiled in so completion of an acquisition can
 * be published the way the fabric publishes it.
 */
#include "../../src/plat/plat_irq.c"

#include "qiran/comm/comm_bytes.h"
#include "qiran/comm/comm_output.h"
#include "qiran/comm/comm_spw_link.h"
#include "qiran/data/data_path.h"
#include "qiran/data/data_product.h"
#include "qiran/data/storage_ddr.h"
#include "qiran/data/storage_nand.h"
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

/* Doubles are compared against a tolerance, since exact equality of a computed
   ratio says nothing useful. */
#define CLOSE(actual, expected, tolerance)                                    \
    do {                                                                      \
        double a_ = (actual);                                                 \
        double e_ = (expected);                                               \
        double d_ = (a_ > e_) ? (a_ - e_) : (e_ - a_);                        \
        g_checks++;                                                           \
        if (d_ > (tolerance)) {                                               \
            g_failures++;                                                     \
            printf("   FAIL %s:%d [%s] %s = %f, expected %f\n",               \
                   __FILE__, __LINE__, g_case, #actual, a_, e_);              \
        }                                                                     \
    } while (0)

static uint8_t s_raw0a[256];
static uint8_t s_raw0b[256];
static uint8_t s_raw1a[256];
static uint8_t s_raw1b[256];
static uint8_t s_processed[256];
static uint8_t s_tx[256];
static uint8_t s_scratch[64];

static uint8_t  s_flash[8192];
static uint32_t s_erases;
static bool     s_write_fails;
static bool     s_read_fails;

static qiran_status_t nand_erase(void *ctx, uint32_t offset, uint32_t bytes)
{
    QIRAN_UNUSED(ctx);
    if ((offset + bytes) > sizeof(s_flash)) {
        return QIRAN_ERR_RANGE;
    }
    memset(&s_flash[offset], 0xFF, bytes);
    s_erases++;
    return QIRAN_OK;
}

static qiran_status_t nand_write(void *ctx, uint32_t offset, const void *src,
                                 uint32_t len)
{
    QIRAN_UNUSED(ctx);
    if (s_write_fails) {
        return QIRAN_ERR_HARDWARE;
    }
    if ((offset + len) > sizeof(s_flash)) {
        return QIRAN_ERR_RANGE;
    }
    memcpy(&s_flash[offset], src, len);
    return QIRAN_OK;
}

static qiran_status_t nand_read(void *ctx, uint32_t offset, void *dst,
                                uint32_t len)
{
    QIRAN_UNUSED(ctx);
    if (s_read_fails) {
        return QIRAN_ERR_HARDWARE;
    }
    if ((offset + len) > sizeof(s_flash)) {
        return QIRAN_ERR_RANGE;
    }
    memcpy(dst, &s_flash[offset], len);
    return QIRAN_OK;
}

static storage_nand_ops_t k_nand_ops;

/* A link that always accepts, so the transfer is paced by the queue alone. */
static qiran_status_t link_start(void *ctx) { QIRAN_UNUSED(ctx); return QIRAN_OK; }
static qiran_status_t link_status(void *ctx, bool *running, uint32_t *word)
{
    QIRAN_UNUSED(ctx);
    *running = true;
    *word = 0U;
    return QIRAN_OK;
}
static uint32_t s_link_sent;
static qiran_status_t link_send(void *ctx, const uint8_t *d, uint32_t n)
{
    QIRAN_UNUSED(ctx);
    QIRAN_UNUSED(d);
    QIRAN_UNUSED(n);
    s_link_sent++;
    return QIRAN_OK;
}
static uint32_t link_receive(void *ctx, uint8_t *b, uint32_t m)
{
    QIRAN_UNUSED(ctx);
    QIRAN_UNUSED(b);
    QIRAN_UNUSED(m);
    return 0U;
}
static const comm_spw_ops_t k_link_ops = {
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
    comm_ccsds_init();
    comm_spw_link_init();
    comm_output_init();
    storage_ddr_init();
    storage_nand_init();
    data_product_init();
    data_path_init();

    memset(s_flash, 0xFF, sizeof(s_flash));
    s_erases = 0U;
    s_write_fails = false;
    s_read_fails = false;
    s_link_sent = 0U;

    CHECK_TRUE(storage_ddr_set_region(DDR_REGION_RAW_CH0_A, s_raw0a,
                                      sizeof(s_raw0a)) == QIRAN_OK);
    CHECK_TRUE(storage_ddr_set_region(DDR_REGION_RAW_CH0_B, s_raw0b,
                                      sizeof(s_raw0b)) == QIRAN_OK);
    CHECK_TRUE(storage_ddr_set_region(DDR_REGION_RAW_CH1_A, s_raw1a,
                                      sizeof(s_raw1a)) == QIRAN_OK);
    CHECK_TRUE(storage_ddr_set_region(DDR_REGION_RAW_CH1_B, s_raw1b,
                                      sizeof(s_raw1b)) == QIRAN_OK);
    CHECK_TRUE(storage_ddr_set_region(DDR_REGION_PROCESSED, s_processed,
                                      sizeof(s_processed)) == QIRAN_OK);
    CHECK_TRUE(storage_ddr_set_region(DDR_REGION_TX_PACKET, s_tx,
                                      sizeof(s_tx)) == QIRAN_OK);
    CHECK_TRUE(storage_ddr_set_region(DDR_REGION_SCRATCH, s_scratch,
                                      sizeof(s_scratch)) == QIRAN_OK);

    k_nand_ops.erase = nand_erase;
    k_nand_ops.write = nand_write;
    k_nand_ops.read = nand_read;
    k_nand_ops.capacity_bytes = (uint32_t)sizeof(s_flash);
    k_nand_ops.ctx = NULL;
    CHECK_TRUE(storage_nand_set_ops(&k_nand_ops) == QIRAN_OK);
}

static void bring_up_link(void)
{
    CHECK_TRUE(comm_spw_link_set_ops(&k_link_ops) == QIRAN_OK);
    comm_spw_link_service();
    comm_spw_link_service();
    CHECK_TRUE(comm_spw_link_running());
}

/* --- buffer ownership --- */

static void test_ownership_is_stated_not_assumed(void)
{
    uint32_t i;

    TEST_CASE("every region starts owned by nobody and is named");
    setup();
    for (i = 0U; i < (uint32_t)DDR_REGION_COUNT; i++) {
        CHECK_EQ_U64(storage_ddr_owner((ddr_region_t)i), DDR_OWNER_NONE);
        CHECK_TRUE(storage_ddr_configured((ddr_region_t)i));
        CHECK_TRUE(storage_ddr_region_name((ddr_region_t)i)[0] != '?');
    }
    CHECK_TRUE(storage_ddr_region_name(DDR_REGION_COUNT)[0] == '?');
    CHECK_TRUE(storage_ddr_owner_name(DDR_OWNER_PL_DMA)[0] != '?');
    CHECK_TRUE(storage_ddr_owner_name(DDR_OWNER_COUNT)[0] == '?');

    TEST_CASE("a handoff naming the right current owner is accepted");
    CHECK_TRUE(storage_ddr_handoff(DDR_REGION_RAW_CH0_A, DDR_OWNER_NONE,
                                   DDR_OWNER_PL_DMA) == QIRAN_OK);
    CHECK_EQ_U64(storage_ddr_owner(DDR_REGION_RAW_CH0_A), DDR_OWNER_PL_DMA);
    CHECK_EQ_U64(storage_ddr_handoffs(), 1U);

    TEST_CASE("a handoff naming the wrong one is refused and reported");
    CHECK_TRUE(storage_ddr_handoff(DDR_REGION_RAW_CH0_A, DDR_OWNER_NONE,
                                   DDR_OWNER_STORAGE) == QIRAN_ERR_STATE);
    CHECK_EQ_U64(storage_ddr_owner(DDR_REGION_RAW_CH0_A), DDR_OWNER_PL_DMA);
    CHECK_EQ_U64(storage_ddr_violations(), 1U);
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_BUFFER_OWNERSHIP));

    TEST_CASE("only the current owner may write to a region");
    CHECK_TRUE(storage_ddr_append(DDR_REGION_RAW_CH0_A, DDR_OWNER_PS_PROCESSING,
                                  "x", 1U) == QIRAN_ERR_STATE);
    CHECK_TRUE(storage_ddr_append(DDR_REGION_RAW_CH0_A, DDR_OWNER_PL_DMA,
                                  "x", 1U) == QIRAN_OK);
    CHECK_EQ_U64(storage_ddr_used(DDR_REGION_RAW_CH0_A), 1U);

    TEST_CASE("only the current owner may read a region");
    {
        const uint8_t *p = NULL;
        uint32_t n = 0U;

        CHECK_TRUE(storage_ddr_read(DDR_REGION_RAW_CH0_A, DDR_OWNER_STORAGE,
                                    &p, &n) == QIRAN_ERR_STATE);
        CHECK_TRUE(storage_ddr_read(DDR_REGION_RAW_CH0_A, DDR_OWNER_PL_DMA,
                                    &p, &n) == QIRAN_OK);
        CHECK_EQ_U64(n, 1U);
        CHECK_EQ_U64(p[0], 'x');
    }

    TEST_CASE("only the current owner may release a region");
    CHECK_TRUE(storage_ddr_clear(DDR_REGION_RAW_CH0_A, DDR_OWNER_STORAGE)
               == QIRAN_ERR_STATE);
    CHECK_TRUE(storage_ddr_clear(DDR_REGION_RAW_CH0_A, DDR_OWNER_PL_DMA)
               == QIRAN_OK);
    CHECK_EQ_U64(storage_ddr_owner(DDR_REGION_RAW_CH0_A), DDR_OWNER_NONE);
    CHECK_EQ_U64(storage_ddr_used(DDR_REGION_RAW_CH0_A), 0U);

    TEST_CASE("a region cannot be moved while somebody owns it");
    CHECK_TRUE(storage_ddr_handoff(DDR_REGION_SCRATCH, DDR_OWNER_NONE,
                                   DDR_OWNER_PS_PROCESSING) == QIRAN_OK);
    CHECK_TRUE(storage_ddr_set_region(DDR_REGION_SCRATCH, s_scratch, 64U)
               == QIRAN_ERR_STATE);
}

static void test_region_capacity(void)
{
    static uint8_t big[128];

    TEST_CASE("a write that would not fit is refused rather than truncated");
    setup();
    CHECK_TRUE(storage_ddr_handoff(DDR_REGION_SCRATCH, DDR_OWNER_NONE,
                                   DDR_OWNER_PS_PROCESSING) == QIRAN_OK);
    CHECK_EQ_U64(storage_ddr_capacity(DDR_REGION_SCRATCH), 64U);
    CHECK_EQ_U64(storage_ddr_free(DDR_REGION_SCRATCH), 64U);

    memset(big, 0xAB, sizeof(big));
    CHECK_TRUE(storage_ddr_append(DDR_REGION_SCRATCH, DDR_OWNER_PS_PROCESSING,
                                  big, 64U) == QIRAN_OK);
    CHECK_EQ_U64(storage_ddr_free(DDR_REGION_SCRATCH), 0U);

    CHECK_TRUE(storage_ddr_append(DDR_REGION_SCRATCH, DDR_OWNER_PS_PROCESSING,
                                  big, 1U) == QIRAN_ERR_RANGE);
    CHECK_EQ_U64(storage_ddr_used(DDR_REGION_SCRATCH), 64U);
    CHECK_EQ_U64(storage_ddr_overruns(), 1U);
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_DDR_OVERRUN));

    TEST_CASE("what the fabric wrote is recorded, bounded by the region");
    CHECK_TRUE(storage_ddr_set_used(DDR_REGION_SCRATCH, DDR_OWNER_PS_PROCESSING,
                                    32U) == QIRAN_OK);
    CHECK_EQ_U64(storage_ddr_used(DDR_REGION_SCRATCH), 32U);
    CHECK_TRUE(storage_ddr_set_used(DDR_REGION_SCRATCH, DDR_OWNER_PS_PROCESSING,
                                    999U) == QIRAN_ERR_RANGE);
}

static void test_double_buffering(void)
{
    TEST_CASE("a channel offers its free slot, and the other when one is busy");
    setup();
    CHECK_EQ_U64(storage_ddr_free_raw_slot(0U), DDR_REGION_RAW_CH0_A);

    CHECK_TRUE(data_path_arm(0U) == QIRAN_OK);
    CHECK_EQ_U64(storage_ddr_owner(DDR_REGION_RAW_CH0_A), DDR_OWNER_PL_DMA);
    CHECK_EQ_U64(storage_ddr_free_raw_slot(0U), DDR_REGION_RAW_CH0_B);

    TEST_CASE("acquisition outrunning reduction is refused and reported");
    CHECK_TRUE(data_path_arm(0U) == QIRAN_OK);
    CHECK_TRUE(data_path_arm(0U) == QIRAN_ERR_BUSY);
    CHECK_EQ_U64(data_path_arm_failures(), 1U);
    CHECK_EQ_U64(storage_ddr_free_raw_slot(0U), DDR_REGION_COUNT);

    TEST_CASE("the two channels are independent");
    CHECK_TRUE(data_path_arm(1U) == QIRAN_OK);
    CHECK_EQ_U64(storage_ddr_owner(DDR_REGION_RAW_CH1_A), DDR_OWNER_PL_DMA);
    CHECK_TRUE(data_path_arm(2U) == QIRAN_ERR_PARAM);
    CHECK_EQ_U64(storage_ddr_free_raw_slot(9U), DDR_REGION_COUNT);
}

static void publish_dma(uint32_t datum)
{
    irq_slot_t *slot = &s_slot[PLAT_IRQ_DMA];
    uint32_t head = slot->head;

    slot->q[head & (QIRAN_IRQ_QUEUE_DEPTH - 1U)].datum = datum;
    slot->q[head & (QIRAN_IRQ_QUEUE_DEPTH - 1U)].kind = 0U;
    slot->head = head + 1U;
}

static void test_completion_hands_the_buffer_over(void)
{
    ddr_region_t region = DDR_REGION_COUNT;

    TEST_CASE("a completion names the buffer, and hands it to the processor");
    setup();
    CHECK_TRUE(data_path_arm(0U) == QIRAN_OK);

    publish_dma((uint32_t)DDR_REGION_RAW_CH0_A);
    data_path_manage();

    CHECK_EQ_U64(data_path_completions(), 1U);
    CHECK_EQ_U64(storage_ddr_owner(DDR_REGION_RAW_CH0_A),
                 DDR_OWNER_PS_PROCESSING);
    CHECK_EQ_U64(data_path_ready_slots(), 1U);

    CHECK_TRUE(data_path_take_ready(&region));
    CHECK_EQ_U64(region, DDR_REGION_RAW_CH0_A);
    CHECK_EQ_U64(data_path_ready_slots(), 0U);
    CHECK_TRUE(!data_path_take_ready(&region));
    CHECK_TRUE(!data_path_take_ready(NULL));

    TEST_CASE("a completion for a buffer the fabric did not own is refused");
    setup();
    publish_dma((uint32_t)DDR_REGION_RAW_CH1_A);
    data_path_manage();

    CHECK_EQ_U64(data_path_completions(), 0U);
    CHECK_EQ_U64(data_path_dropped(), 1U);
    CHECK_EQ_U64(storage_ddr_violations(), 1U);

    TEST_CASE("completions are taken in the order they arrived");
    setup();
    CHECK_TRUE(data_path_arm(0U) == QIRAN_OK);
    CHECK_TRUE(data_path_arm(1U) == QIRAN_OK);
    publish_dma((uint32_t)DDR_REGION_RAW_CH1_A);
    publish_dma((uint32_t)DDR_REGION_RAW_CH0_A);
    data_path_manage();

    CHECK_EQ_U64(data_path_ready_slots(), 2U);
    CHECK_TRUE(data_path_take_ready(&region));
    CHECK_EQ_U64(region, DDR_REGION_RAW_CH1_A);
    CHECK_TRUE(data_path_take_ready(&region));
    CHECK_EQ_U64(region, DDR_REGION_RAW_CH0_A);
}

/* --- derived quantities --- */

static void baseline_input(data_product_input_t *in)
{
    in->coincidence_max = 9000U;
    in->coincidence_min = 1000U;
    in->singles_signal = 1000000U;
    in->singles_idler = 1000000U;
    in->central_peak = 8000U;
    in->accidentals = 100U;
    in->integration_ms = 1000U;
    in->pump_power_uw = 500U;
    in->coincidence_window_ps = 1000U;
}

static void load_calibration(void)
{
    svc_calibration_t cal;

    memset(&cal, 0, sizeof(cal));
    cal.li_count = 0U;
    cal.branch_efficiency_ppm[0] = 100000U;   /* a tenth */
    cal.branch_efficiency_ppm[1] = 200000U;   /* a fifth */
    cal.spectral_bandwidth_pm = 2000U;        /* two nanometres */
    CHECK_TRUE(svc_config_calibration_set(&cal) == QIRAN_OK);
}

static void test_visibility_and_bound(void)
{
    data_product_input_t in;
    data_product_t out;

    TEST_CASE("visibility is the contrast of the extremes");
    setup();
    baseline_input(&in);
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_OK);

    /* (9000 - 1000) / (9000 + 1000) */
    CLOSE(out.visibility, 0.8, 1e-9);
    CHECK_TRUE(out.counts_valid);

    TEST_CASE("the correlation bound follows from it");
    CLOSE(out.s_bound, DATA_S_COEFFICIENT * 0.8, 1e-9);
    CLOSE(out.s_bound, 2.2627417, 1e-6);

    TEST_CASE("perfect contrast gives the maximum bound");
    in.coincidence_min = 0U;
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_OK);
    CLOSE(out.visibility, 1.0, 1e-9);
    CLOSE(out.s_bound, DATA_S_COEFFICIENT, 1e-9);

    TEST_CASE("no contrast gives none");
    in.coincidence_min = 9000U;
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_OK);
    CLOSE(out.visibility, 0.0, 1e-9);
    CLOSE(out.s_bound, 0.0, 1e-9);
}

static void test_ratio_guards(void)
{
    data_product_input_t in;
    data_product_t out;

    TEST_CASE("no counts at all is refused rather than reported as zero");
    setup();
    baseline_input(&in);
    in.coincidence_max = 0U;
    in.coincidence_min = 0U;
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_ERR_RANGE);
    CHECK_TRUE(!out.counts_valid);

    TEST_CASE("no interval to have counted over is refused too");
    baseline_input(&in);
    in.integration_ms = 0U;
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_ERR_RANGE);

    TEST_CASE("a minimum above the maximum is refused");
    baseline_input(&in);
    in.coincidence_min = in.coincidence_max + 1U;
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_ERR_RANGE);

    TEST_CASE("no accidentals leaves the measured ratio unclaimed");
    baseline_input(&in);
    in.accidentals = 0U;
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_OK);
    CLOSE(out.car_measured, 0.0, 1e-9);
    CHECK_TRUE(out.counts_valid);

    TEST_CASE("perfect contrast leaves the bound unclaimed, not infinite");
    baseline_input(&in);
    in.coincidence_min = 0U;
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_OK);
    CLOSE(out.car_lower_bound, 0.0, 1e-9);
    CHECK_TRUE(!out.consistent);

    TEST_CASE("arguments are validated");
    CHECK_TRUE(data_product_compute(NULL, &out) == QIRAN_ERR_PARAM);
    CHECK_TRUE(data_product_compute(&in, NULL) == QIRAN_ERR_PARAM);
}

static void test_consistency_check(void)
{
    data_product_input_t in;
    data_product_t out;

    TEST_CASE("the bound from visibility sits below the measured ratio");
    setup();
    baseline_input(&in);
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_OK);

    /* 0.8 / (1 - 0.8) = 4; measured is 8000 / 100 = 80 */
    CLOSE(out.car_lower_bound, 4.0, 1e-9);
    CLOSE(out.car_measured, 80.0, 1e-9);
    CHECK_TRUE(out.consistent);
    CHECK_EQ_U64(data_product_inconsistent(), 0U);

    TEST_CASE("a measured ratio below the bound is a finding about the data");
    baseline_input(&in);
    in.accidentals = 4000U;   /* measured ratio of two, below a bound of four */
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_OK);
    CLOSE(out.car_measured, 2.0, 1e-9);
    CHECK_TRUE(!out.consistent);
    CHECK_EQ_U64(data_product_inconsistent(), 1U);

    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_DATA_PRODUCT));
    CHECK_EQ_U64(svc_fdir_severity_count(QIRAN_SEV_MEDIUM), 1U);
}

static void test_calibrated_quantities(void)
{
    data_product_input_t in;
    data_product_t out;

    TEST_CASE("without calibration the counts-only results still stand");
    setup();
    baseline_input(&in);
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_OK);
    CHECK_TRUE(out.counts_valid);
    CHECK_TRUE(!out.calibrated);
    CLOSE(out.pair_rate_hz, 0.0, 1e-9);
    CLOSE(out.brightness, 0.0, 1e-9);
    CLOSE(out.visibility, 0.8, 1e-9);

    TEST_CASE("with it, the pair rate divides out the branch efficiencies");
    load_calibration();
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_OK);
    CHECK_TRUE(out.calibrated);
    /* 9000 per second, over a tenth times a fifth */
    CLOSE(out.pair_rate_hz, 9000.0 / (0.1 * 0.2), 1e-6);
    CLOSE(out.pair_rate_hz, 450000.0, 1e-6);

    TEST_CASE("brightness divides that by pump power and bandwidth");
    /* 450000 over half a milliwatt times two nanometres */
    CLOSE(out.brightness, 450000.0 / (0.5 * 2.0), 1e-6);
    CLOSE(out.brightness, 450000.0, 1e-6);

    TEST_CASE("the model ratio comes from the singles and the window");
    /* accidental rate 1e6 * 1e6 * 1e-9 = 1000 per second; 9000 / 1000 */
    CLOSE(out.car_predicted, 9.0, 1e-6);

    TEST_CASE("an efficiency of zero leaves those quantities unclaimed");
    {
        svc_calibration_t cal;

        memset(&cal, 0, sizeof(cal));
        cal.branch_efficiency_ppm[0] = 0U;
        cal.branch_efficiency_ppm[1] = 200000U;
        CHECK_TRUE(svc_config_calibration_set(&cal) == QIRAN_OK);
    }
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_OK);
    CHECK_TRUE(!out.calibrated);
    CLOSE(out.pair_rate_hz, 0.0, 1e-9);
}

static void test_product_framing(void)
{
    data_product_input_t in;
    data_product_t out;
    uint8_t buf[DATA_PRODUCT_FRAME_BYTES + 4U];
    uint32_t written = 0U;

    TEST_CASE("the product frames as scaled integers of the declared length");
    setup();
    load_calibration();
    baseline_input(&in);
    CHECK_TRUE(data_product_compute(&in, &out) == QIRAN_OK);

    memset(buf, 0xEE, sizeof(buf));
    CHECK_TRUE(data_product_frame(&out, buf, sizeof(buf), &written)
               == QIRAN_OK);
    CHECK_EQ_U64(written, DATA_PRODUCT_FRAME_BYTES);
    CHECK_EQ_U64(buf[DATA_PRODUCT_FRAME_BYTES], 0xEEU);

    CHECK_EQ_U64(comm_get_u32(buf, 0U), 800000U);        /* 0.8 */
    CHECK_EQ_U64(comm_get_u32(buf, 8U), 450000U);        /* pair rate */
    CHECK_EQ_U64(comm_get_u32(buf, 16U), 80000U);        /* measured ratio */
    CHECK_EQ_U64(comm_get_u32(buf, 24U), 4000U);         /* bound */
    CHECK_EQ_U64(comm_get_u32(buf, 28U), 0x07U);         /* all three flags */

    TEST_CASE("a short buffer is refused");
    CHECK_TRUE(data_product_frame(&out, buf, 4U, &written) == QIRAN_ERR_PARAM);
    CHECK_TRUE(data_product_frame(NULL, buf, sizeof(buf), &written)
               == QIRAN_ERR_PARAM);
}

/* --- non-volatile store --- */

static void test_store_and_transfer(void)
{
    static const uint8_t k_raw[] = { 1U, 2U, 3U, 4U, 5U };
    static const uint8_t k_proc[] = { 9U, 9U };

    TEST_CASE("nothing can be stored before a device is attached");
    setup();
    storage_nand_init();
    CHECK_TRUE(!storage_nand_ready());
    CHECK_TRUE(storage_nand_store(NAND_RECORD_RAW, k_raw, sizeof(k_raw))
               == QIRAN_ERR_UNSUPPORTED);

    TEST_CASE("records are stored with a header and their own checksum");
    setup();
    CHECK_TRUE(storage_nand_ready());
    CHECK_TRUE(storage_nand_store(NAND_RECORD_RAW, k_raw, sizeof(k_raw))
               == QIRAN_OK);
    CHECK_EQ_U64(storage_nand_records(), 1U);
    CHECK_EQ_U64(storage_nand_used(), NAND_HEADER_BYTES + sizeof(k_raw));

    CHECK_EQ_U64(comm_get_u32(s_flash, 0U), NAND_RECORD_MAGIC);
    CHECK_EQ_U64(comm_get_u32(s_flash, 4U), NAND_RECORD_RAW);
    CHECK_EQ_U64(comm_get_u32(s_flash, 8U), 0U);
    CHECK_EQ_U64(comm_get_u32(s_flash, 12U), sizeof(k_raw));
    CHECK_EQ_U64(comm_get_u32(s_flash, 16U),
                 svc_crc32(k_raw, (uint32_t)sizeof(k_raw)));

    TEST_CASE("each record takes the next sequence number");
    CHECK_TRUE(storage_nand_store(NAND_RECORD_PROCESSED, k_proc,
                                  sizeof(k_proc)) == QIRAN_OK);
    CHECK_EQ_U64(storage_nand_records(), 2U);
    CHECK_EQ_U64(comm_get_u32(s_flash, NAND_HEADER_BYTES + sizeof(k_raw) + 8U),
                 1U);

    TEST_CASE("the transfer sends a record per pass, by class");
    bring_up_link();
    CHECK_TRUE(storage_nand_transfer_begin() == QIRAN_OK);
    CHECK_TRUE(storage_nand_transfer_active());

    storage_nand_transfer_service();
    CHECK_EQ_U64(storage_nand_transferred(), 1U);
    storage_nand_transfer_service();
    CHECK_EQ_U64(storage_nand_transferred(), 2U);

    comm_output_service();
    CHECK_EQ_U64(comm_output_sent(), 2U);

    TEST_CASE("it finishes when the store is exhausted");
    storage_nand_transfer_service();
    CHECK_TRUE(!storage_nand_transfer_active());
    CHECK_TRUE(storage_nand_transfer_complete());

    TEST_CASE("the store may be cleared only once everything has gone");
    CHECK_TRUE(storage_nand_clear() == QIRAN_OK);
    CHECK_EQ_U64(storage_nand_used(), 0U);
    CHECK_EQ_U64(storage_nand_records(), 0U);
    CHECK_EQ_U64(s_erases, 1U);
}

static void test_store_refuses_to_lose_data(void)
{
    static const uint8_t k_raw[] = { 1U, 2U, 3U, 4U };

    TEST_CASE("clearing before everything has gone is refused");
    setup();
    CHECK_TRUE(storage_nand_store(NAND_RECORD_RAW, k_raw, sizeof(k_raw))
               == QIRAN_OK);
    CHECK_TRUE(storage_nand_store(NAND_RECORD_RAW, k_raw, sizeof(k_raw))
               == QIRAN_OK);
    CHECK_TRUE(storage_nand_clear() == QIRAN_ERR_STATE);

    bring_up_link();
    CHECK_TRUE(storage_nand_transfer_begin() == QIRAN_OK);
    storage_nand_transfer_service();
    CHECK_TRUE(storage_nand_clear() == QIRAN_ERR_BUSY);

    TEST_CASE("appending during a transfer is refused");
    CHECK_TRUE(storage_nand_store(NAND_RECORD_RAW, k_raw, sizeof(k_raw))
               == QIRAN_ERR_BUSY);

    TEST_CASE("a store that will not fit is refused and reported");
    setup();
    {
        static uint8_t big[4096];
        uint32_t i;

        memset(big, 0x5A, sizeof(big));
        for (i = 0U; i < 3U; i++) {
            (void)storage_nand_store(NAND_RECORD_RAW, big, sizeof(big));
        }
        CHECK_TRUE(storage_nand_store(NAND_RECORD_RAW, big, sizeof(big))
                   == QIRAN_ERR_RANGE);
    }
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_NAND_WRITE));

    TEST_CASE("a device that refuses a write leaves the store where it was");
    setup();
    s_write_fails = true;
    CHECK_TRUE(storage_nand_store(NAND_RECORD_RAW, k_raw, sizeof(k_raw))
               == QIRAN_ERR_HARDWARE);
    CHECK_EQ_U64(storage_nand_used(), 0U);
    CHECK_EQ_U64(storage_nand_records(), 0U);
    CHECK_EQ_U64(storage_nand_write_failures(), 1U);
}

static void test_transfer_survives_a_damaged_record(void)
{
    static const uint8_t k_a[] = { 1U, 1U, 1U, 1U };
    static const uint8_t k_b[] = { 2U, 2U, 2U, 2U };

    TEST_CASE("a record that no longer matches its checksum is passed over");
    setup();
    bring_up_link();
    CHECK_TRUE(storage_nand_store(NAND_RECORD_RAW, k_a, sizeof(k_a))
               == QIRAN_OK);
    CHECK_TRUE(storage_nand_store(NAND_RECORD_RAW, k_b, sizeof(k_b))
               == QIRAN_OK);

    s_flash[NAND_HEADER_BYTES] ^= 0xFFU;   /* damage the first payload */

    CHECK_TRUE(storage_nand_transfer_begin() == QIRAN_OK);
    storage_nand_transfer_service();
    CHECK_EQ_U64(storage_nand_transferred(), 0U);
    CHECK_EQ_U64(storage_nand_read_failures(), 1U);

    TEST_CASE("the records after it still go out");
    storage_nand_transfer_service();
    CHECK_EQ_U64(storage_nand_transferred(), 1U);

    TEST_CASE("a header that does not describe a record stops the transfer");
    setup();
    bring_up_link();
    CHECK_TRUE(storage_nand_store(NAND_RECORD_RAW, k_a, sizeof(k_a))
               == QIRAN_OK);
    s_flash[0] ^= 0xFFU;
    CHECK_TRUE(storage_nand_transfer_begin() == QIRAN_OK);
    storage_nand_transfer_service();
    CHECK_TRUE(!storage_nand_transfer_active());
    CHECK_EQ_U64(storage_nand_transferred(), 0U);

    TEST_CASE("a device that refuses a read stops it too");
    setup();
    bring_up_link();
    CHECK_TRUE(storage_nand_store(NAND_RECORD_RAW, k_a, sizeof(k_a))
               == QIRAN_OK);
    CHECK_TRUE(storage_nand_transfer_begin() == QIRAN_OK);
    s_read_fails = true;
    storage_nand_transfer_service();
    CHECK_TRUE(!storage_nand_transfer_active());
    CHECK_EQ_U64(storage_nand_read_failures(), 1U);
}

static void test_transfer_runs_only_in_its_state(void)
{
    static const uint8_t k_a[] = { 3U, 3U, 3U, 3U };

    TEST_CASE("the loop does not move data before the run reaches that state");
    setup();
    bring_up_link();
    CHECK_TRUE(storage_nand_store(NAND_RECORD_RAW, k_a, sizeof(k_a))
               == QIRAN_OK);
    CHECK_TRUE(data_path_transfer_begin() == QIRAN_OK);

    data_path_manage();
    CHECK_EQ_U64(storage_nand_transferred(), 0U);

    TEST_CASE("it does once the run is there");
    {
        static const mission_state_id_t k_path[] = {
            SPR_PRECOND, SPR_LASER_BRINGUP, SPR_MRR_TUNE, SPR_CROW_TUNE,
            SPR_UMZI_TUNE, SPR_DLI_LOCK, SPR_SPAD_ENABLE, SPR_PVS_T1,
            SPR_EXPERIMENT, SPR_PROCESS, SPR_DATA_HANDLING
        };
        uint32_t i;

        for (i = 0U; i < QIRAN_ARRAY_LEN(k_path); i++) {
            CHECK_TRUE(mission_state_request(k_path[i]) == QIRAN_OK);
        }
    }
    CHECK_EQ_U64(mission_state_current(), SPR_DATA_HANDLING);

    data_path_manage();
    CHECK_EQ_U64(storage_nand_transferred(), 1U);
    CHECK_TRUE(storage_nand_transfer_active());

    TEST_CASE("one further pass finds the store exhausted and concludes");
    data_path_manage();
    CHECK_TRUE(!storage_nand_transfer_active());
    CHECK_TRUE(data_path_transfer_done());
}

int main(void)
{
    plat_cpu_init();

    test_ownership_is_stated_not_assumed();
    test_region_capacity();
    test_double_buffering();
    test_completion_hands_the_buffer_over();

    test_visibility_and_bound();
    test_ratio_guards();
    test_consistency_check();
    test_calibrated_quantities();
    test_product_framing();

    test_store_and_transfer();
    test_store_refuses_to_lose_data();
    test_transfer_survives_a_damaged_record();
    test_transfer_runs_only_in_its_state();

    return TEST_REPORT();
}
