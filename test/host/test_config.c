/*
 * Compiled in so the live parameter block can be corrupted directly, which is
 * how the integrity check is exercised against something that did not go
 * through the setter.
 */
#include "../../src/svc/svc_config.c"

#include "qiran/plat/plat_cpu.h"
#include "qiran/svc/svc_crc.h"
#include "test_framework.h"

#include <string.h>

/* --- checksum, against the published check values --- */

static void test_crc_known_vectors(void)
{
    uint32_t crc;

    TEST_CASE("checksum matches the published check value for the algorithm");
    CHECK_EQ_U64(svc_crc32("123456789", 9U), 0xCBF43926U);
    CHECK_EQ_U64(svc_crc32("A", 1U), 0xD3D99E8BU);
    CHECK_EQ_U64(svc_crc32("", 0U), 0x00000000U);

    TEST_CASE("the incremental form agrees with the single-shot form");
    crc = svc_crc32_update(SVC_CRC32_INIT, "1234", 4U);
    crc = svc_crc32_update(crc, "56789", 5U);
    CHECK_EQ_U64(svc_crc32_finish(crc), 0xCBF43926U);

    TEST_CASE("a single changed bit changes the checksum");
    CHECK_TRUE(svc_crc32("123456789", 9U) != svc_crc32("123456788", 9U));

    TEST_CASE("a null buffer leaves the running value untouched");
    CHECK_EQ_U64(svc_crc32_update(0x1234U, NULL, 10U), 0x1234U);
}

/* --- fake configuration partition --- */

static uint8_t s_flash[1024];
static bool    s_write_fails;
static uint32_t s_writes;

static qiran_status_t flash_read(uint32_t offset, void *dst, uint32_t len)
{
    if ((offset + len) > sizeof(s_flash)) {
        return QIRAN_ERR_RANGE;
    }
    memcpy(dst, &s_flash[offset], len);
    return QIRAN_OK;
}

static qiran_status_t flash_write(uint32_t offset, const void *src, uint32_t len)
{
    if (s_write_fails) {
        return QIRAN_ERR_HARDWARE;
    }
    if ((offset + len) > sizeof(s_flash)) {
        return QIRAN_ERR_RANGE;
    }
    memcpy(&s_flash[offset], src, len);
    s_writes++;
    return QIRAN_OK;
}

static const svc_config_storage_t k_storage = { flash_read, flash_write };

static void setup(bool with_storage)
{
    memset(s_flash, 0, sizeof(s_flash));
    s_write_fails = false;
    s_writes = 0U;
    svc_log_init();
    svc_fdir_init();
    svc_config_init();
    if (with_storage) {
        (void)svc_config_set_storage(&k_storage);
    }
}

/* --- parameter table --- */

static void test_defaults_are_inside_their_own_limits(void)
{
    uint32_t i;

    TEST_CASE("every parameter's default lies within its own limits");
    setup(false);

    for (i = 0U; i < (uint32_t)SVC_CONFIG_PARAM_COUNT; i++) {
        const svc_config_def_t *d = svc_config_def((svc_config_param_t)i);

        CHECK_TRUE(d != NULL);
        CHECK_TRUE(d->name != NULL);
        CHECK_TRUE(d->unit != NULL);
        CHECK_TRUE(d->min <= d->max);
        CHECK_TRUE(d->def >= d->min);
        CHECK_TRUE(d->def <= d->max);
        CHECK_EQ_U64(svc_config_get((svc_config_param_t)i), d->def);
    }

    TEST_CASE("an out-of-range identifier reads zero and has no definition");
    CHECK_EQ_U64(svc_config_get(SVC_CONFIG_PARAM_COUNT), 0U);
    CHECK_TRUE(svc_config_def(SVC_CONFIG_PARAM_COUNT) == NULL);
}

static void test_documented_values_are_the_ones_in_the_table(void)
{
    TEST_CASE("the commanded temperature cannot be set outside its absolute limits");
    setup(false);

    CHECK_EQ_U64(svc_config_get(CFG_LASER_TEC_SETPOINT_MDEGC), 27000);
    CHECK_EQ_U64(svc_config_def(CFG_LASER_TEC_SETPOINT_MDEGC)->min, 0);
    CHECK_EQ_U64(svc_config_def(CFG_LASER_TEC_SETPOINT_MDEGC)->max, 50000);

    CHECK_TRUE(svc_config_set(CFG_LASER_TEC_SETPOINT_MDEGC, 50001)
               == QIRAN_ERR_RANGE);
    CHECK_TRUE(svc_config_set(CFG_LASER_TEC_SETPOINT_MDEGC, -1)
               == QIRAN_ERR_RANGE);
    CHECK_EQ_U64(svc_config_get(CFG_LASER_TEC_SETPOINT_MDEGC), 27000);
    CHECK_EQ_U64(svc_config_rejections(), 2U);

    TEST_CASE("a rejected write is reported as a fault");
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_CONFIG));

    TEST_CASE("the bias operating point is held inside its documented window");
    CHECK_EQ_U64(svc_config_def(CFG_LASER_BIAS_TARGET_UA)->min, 55000);
    CHECK_EQ_U64(svc_config_def(CFG_LASER_BIAS_TARGET_UA)->max, 60000);
    CHECK_TRUE(svc_config_set(CFG_LASER_BIAS_TARGET_UA, 54999) == QIRAN_ERR_RANGE);

    TEST_CASE("the ramp rate cannot be raised above its documented ceiling");
    CHECK_EQ_U64(svc_config_def(CFG_LASER_BIAS_RAMP_UA_PER_MS)->max, 1000);
    CHECK_TRUE(svc_config_set(CFG_LASER_BIAS_RAMP_UA_PER_MS, 1001)
               == QIRAN_ERR_RANGE);
    CHECK_TRUE(svc_config_set(CFG_LASER_BIAS_RAMP_UA_PER_MS, 500) == QIRAN_OK);
}

static void test_write_protection(void)
{
    TEST_CASE("both kinds of parameter are settable while unlocked");
    setup(false);
    CHECK_TRUE(!svc_config_locked());
    CHECK_TRUE(svc_config_set(CFG_LASER_TEC_SETPOINT_MDEGC, 26000) == QIRAN_OK);
    CHECK_TRUE(svc_config_set(CFG_MRR_BUDGET_MS, 12000) == QIRAN_OK);

    TEST_CASE("once locked, only what the ground may adjust stays settable");
    svc_config_lock();
    CHECK_TRUE(svc_config_locked());
    CHECK_TRUE(svc_config_set(CFG_LASER_TEC_SETPOINT_MDEGC, 28000) == QIRAN_OK);
    CHECK_EQ_U64(svc_config_get(CFG_LASER_TEC_SETPOINT_MDEGC), 28000);

    CHECK_TRUE(svc_config_set(CFG_MRR_BUDGET_MS, 15000) == QIRAN_ERR_STATE);
    CHECK_EQ_U64(svc_config_get(CFG_MRR_BUDGET_MS), 12000);

    TEST_CASE("a locked parameter is still range checked, not merely refused");
    CHECK_TRUE(svc_config_set(CFG_MRR_BUDGET_MS, 999999) == QIRAN_ERR_RANGE);

    TEST_CASE("unlocking restores full access");
    svc_config_unlock();
    CHECK_TRUE(svc_config_set(CFG_MRR_BUDGET_MS, 15000) == QIRAN_OK);
}

/* --- persistence --- */

static void test_save_and_load_round_trip(void)
{
    TEST_CASE("saved parameters come back after a reinitialise");
    setup(true);
    CHECK_TRUE(svc_config_set(CFG_LASER_TEC_SETPOINT_MDEGC, 26500) == QIRAN_OK);
    CHECK_TRUE(svc_config_set(CFG_DLI_LOCK_RMS_MRAD, 120) == QIRAN_OK);
    CHECK_TRUE(svc_config_save() == QIRAN_OK);

    svc_config_init();
    (void)svc_config_set_storage(&k_storage);
    CHECK_EQ_U64(svc_config_get(CFG_LASER_TEC_SETPOINT_MDEGC), 27000);

    CHECK_TRUE(svc_config_load() == QIRAN_OK);
    CHECK_EQ_U64(svc_config_get(CFG_LASER_TEC_SETPOINT_MDEGC), 26500);
    CHECK_EQ_U64(svc_config_get(CFG_DLI_LOCK_RMS_MRAD), 120);
    CHECK_EQ_U64(svc_config_corrections(), 0U);
}

static void test_load_without_storage_or_content(void)
{
    TEST_CASE("with no storage attached the defaults stand and it is reported");
    setup(false);
    CHECK_TRUE(svc_config_load() == QIRAN_ERR_UNSUPPORTED);
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_CONFIG));
    CHECK_EQ_U64(svc_config_get(CFG_LASER_TEC_SETPOINT_MDEGC), 27000);

    TEST_CASE("an erased partition leaves the defaults and is reported");
    setup(true);
    CHECK_TRUE(svc_config_load() == QIRAN_ERR_INTEGRITY);
    CHECK_EQ_U64(svc_config_get(CFG_LASER_TEC_SETPOINT_MDEGC), 27000);
    CHECK_TRUE(!svc_config_calibration_valid());

    TEST_CASE("sound parameters with no curve is not a parameter failure");
    setup(true);
    CHECK_TRUE(svc_config_save() == QIRAN_OK);
    CHECK_TRUE(svc_config_load() == QIRAN_OK);
    CHECK_TRUE(!svc_config_calibration_valid());
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_CONFIG));
}

static void test_load_rejects_a_corrupt_block(void)
{
    TEST_CASE("a single flipped bit in the stored block is caught");
    setup(true);
    CHECK_TRUE(svc_config_set(CFG_LASER_TEC_SETPOINT_MDEGC, 26500) == QIRAN_OK);
    CHECK_TRUE(svc_config_save() == QIRAN_OK);

    s_flash[20] ^= 0x01U;

    CHECK_TRUE(svc_config_load() == QIRAN_ERR_INTEGRITY);
    CHECK_EQ_U64(svc_config_get(CFG_LASER_TEC_SETPOINT_MDEGC), 27000);

    TEST_CASE("a block from an unknown format version is not used");
    setup(true);
    CHECK_TRUE(svc_config_save() == QIRAN_OK);
    {
        config_image_t image;

        memcpy(&image, s_flash, sizeof(image));
        image.version = 99U;
        image.crc = svc_crc32(&image, (uint32_t)(sizeof(image) - sizeof(uint32_t)));
        memcpy(s_flash, &image, sizeof(image));
    }
    CHECK_TRUE(svc_config_load() == QIRAN_ERR_INTEGRITY);
}

static void test_load_handles_a_shorter_stored_block(void)
{
    config_image_t image;

    TEST_CASE("a block holding fewer parameters loads those it has");
    setup(true);

    memset(&image, 0, sizeof(image));
    image.magic = CONFIG_MAGIC;
    image.version = CONFIG_VERSION;
    image.count = 3U;
    image.value[0] = 26000;
    image.value[1] = 200;
    image.value[2] = 16000;
    image.crc = svc_crc32(&image, (uint32_t)(sizeof(image) - sizeof(uint32_t)));
    memcpy(s_flash, &image, sizeof(image));

    CHECK_TRUE(svc_config_load() == QIRAN_OK);
    CHECK_EQ_U64(svc_config_get(CFG_LASER_TEC_SETPOINT_MDEGC), 26000);
    CHECK_EQ_U64(svc_config_get(CFG_LASER_TEC_BAND_MDEGC), 200);
    CHECK_EQ_U64(svc_config_get(CFG_LASER_TEC_NOMINAL_MS), 16000);

    TEST_CASE("the parameters it does not carry keep their defaults");
    CHECK_EQ_U64(svc_config_get(CFG_LASER_BIAS_TARGET_UA),
                 svc_config_def(CFG_LASER_BIAS_TARGET_UA)->def);
}

static void test_load_refuses_an_out_of_range_stored_value(void)
{
    config_image_t image;

    TEST_CASE("an intact block holding an illegal value falls back per parameter");
    setup(true);

    memset(&image, 0, sizeof(image));
    image.magic = CONFIG_MAGIC;
    image.version = CONFIG_VERSION;
    image.count = (uint32_t)SVC_CONFIG_PARAM_COUNT;
    {
        uint32_t i;
        for (i = 0U; i < (uint32_t)SVC_CONFIG_PARAM_COUNT; i++) {
            image.value[i] = svc_config_def((svc_config_param_t)i)->def;
        }
    }
    /* Beyond the absolute maximum commanded temperature. */
    image.value[CFG_LASER_TEC_SETPOINT_MDEGC] = 90000;
    image.crc = svc_crc32(&image, (uint32_t)(sizeof(image) - sizeof(uint32_t)));
    memcpy(s_flash, &image, sizeof(image));

    CHECK_TRUE(svc_config_load() == QIRAN_ERR_RANGE);
    CHECK_EQ_U64(svc_config_get(CFG_LASER_TEC_SETPOINT_MDEGC), 27000);
    CHECK_EQ_U64(svc_config_corrections(), 1U);

    TEST_CASE("the rest of an otherwise sound block is still taken");
    CHECK_EQ_U64(svc_config_get(CFG_DLI_LOCK_RMS_MRAD),
                 svc_config_def(CFG_DLI_LOCK_RMS_MRAD)->def);
}

/* --- integrity of the live block --- */

static void test_verify_detects_corruption_in_place(void)
{
    TEST_CASE("a sound block verifies clean");
    setup(false);
    CHECK_TRUE(svc_config_verify() == QIRAN_OK);
    CHECK_EQ_U64(svc_config_corrections(), 0U);

    TEST_CASE("a value changed without the setter is detected and put back");
    s_value[CFG_LASER_TEC_SETPOINT_MDEGC] = 99999;
    CHECK_TRUE(svc_config_verify() == QIRAN_ERR_INTEGRITY);
    CHECK_EQ_U64(svc_config_get(CFG_LASER_TEC_SETPOINT_MDEGC), 27000);
    CHECK_EQ_U64(svc_config_corrections(), 1U);
    error_handling_service();
    CHECK_TRUE(svc_fdir_flag(QIRAN_FAULT_CONFIG));

    TEST_CASE("a bit flip that leaves the value legal is still detected");
    setup(false);
    s_value[CFG_DLI_LOCK_RMS_MRAD] = 141;
    CHECK_TRUE(svc_config_verify() == QIRAN_ERR_INTEGRITY);
    CHECK_EQ_U64(svc_config_get(CFG_DLI_LOCK_RMS_MRAD), 140);

    TEST_CASE("with storage attached the stored block is what is restored");
    setup(true);
    CHECK_TRUE(svc_config_set(CFG_DLI_LOCK_RMS_MRAD, 130) == QIRAN_OK);
    CHECK_TRUE(svc_config_save() == QIRAN_OK);
    s_value[CFG_DLI_LOCK_RMS_MRAD] = 999;
    CHECK_TRUE(svc_config_verify() == QIRAN_ERR_INTEGRITY);
    CHECK_EQ_U64(svc_config_get(CFG_DLI_LOCK_RMS_MRAD), 130);

    TEST_CASE("verifying again after a correction is clean");
    CHECK_TRUE(svc_config_verify() == QIRAN_OK);
}

/* --- calibration --- */

static void build_curve(svc_calibration_t *cal)
{
    memset(cal, 0, sizeof(*cal));
    cal->li_count = 2U;
    /* Threshold current and slope efficiency as documented: zero output at
       40 mA rising to 0.6 mW at 60 mA, which is 0.03 mW per mA. */
    cal->li[0].current_ua = 40000U;
    cal->li[0].power_nw = 0U;
    cal->li[1].current_ua = 60000U;
    cal->li[1].power_nw = 600000U;
    cal->dli_setpoint_uk[0] = 300000000;
    cal->dli_setpoint_uk[1] = 300000000;
    cal->dark_count_baseline_cps[0] = 100U;
    cal->dark_count_baseline_cps[1] = 120U;
}

static void test_calibration_block(void)
{
    svc_calibration_t cal;
    const svc_calibration_t *stored;
    uint32_t power = 0U;

    TEST_CASE("calibration is unavailable until it has been supplied");
    setup(true);
    CHECK_TRUE(!svc_config_calibration_valid());
    CHECK_TRUE(svc_config_calibration() == NULL);
    CHECK_TRUE(svc_config_li_expected_nw(50000U, &power) == QIRAN_ERR_UNSUPPORTED);

    TEST_CASE("supplying it checksums it, stores it and makes it readable");
    build_curve(&cal);
    CHECK_TRUE(svc_config_calibration_set(&cal) == QIRAN_OK);
    CHECK_TRUE(svc_config_calibration_valid());
    CHECK_EQ_U64(s_writes, 1U);

    stored = svc_config_calibration();
    CHECK_TRUE(stored != NULL);
    CHECK_EQ_U64(stored->li_count, 2U);
    CHECK_EQ_U64(stored->dark_count_baseline_cps[1], 120U);
    CHECK_EQ_U64(stored->crc,
                 svc_crc32(stored, (uint32_t)(sizeof(*stored) - sizeof(uint32_t))));

    TEST_CASE("it survives a reinitialise and reload");
    svc_config_init();
    (void)svc_config_set_storage(&k_storage);
    CHECK_TRUE(!svc_config_calibration_valid());
    (void)svc_config_load();
    CHECK_TRUE(svc_config_calibration_valid());
    CHECK_EQ_U64(svc_config_calibration()->li[1].power_nw, 600000U);

    TEST_CASE("a curve with too many points is refused");
    build_curve(&cal);
    cal.li_count = SVC_CAL_LI_POINTS + 1U;
    CHECK_TRUE(svc_config_calibration_set(&cal) == QIRAN_ERR_RANGE);
    CHECK_TRUE(svc_config_calibration_set(NULL) == QIRAN_ERR_PARAM);
}

static void test_curve_interpolation(void)
{
    svc_calibration_t cal;
    uint32_t power = 0U;

    TEST_CASE("the curve reads exactly at its own points");
    setup(false);
    build_curve(&cal);
    CHECK_TRUE(svc_config_calibration_set(&cal) == QIRAN_OK);

    CHECK_TRUE(svc_config_li_expected_nw(40000U, &power) == QIRAN_OK);
    CHECK_EQ_U64(power, 0U);
    CHECK_TRUE(svc_config_li_expected_nw(60000U, &power) == QIRAN_OK);
    CHECK_EQ_U64(power, 600000U);

    TEST_CASE("it interpolates between them");
    CHECK_TRUE(svc_config_li_expected_nw(50000U, &power) == QIRAN_OK);
    CHECK_EQ_U64(power, 300000U);
    /* The documented operating point. */
    CHECK_TRUE(svc_config_li_expected_nw(57500U, &power) == QIRAN_OK);
    CHECK_EQ_U64(power, 525000U);

    TEST_CASE("it refuses to extrapolate beyond what was calibrated");
    CHECK_TRUE(svc_config_li_expected_nw(39999U, &power) == QIRAN_ERR_RANGE);
    CHECK_TRUE(svc_config_li_expected_nw(60001U, &power) == QIRAN_ERR_RANGE);

    TEST_CASE("a curve of fewer than two points cannot be interpolated");
    build_curve(&cal);
    cal.li_count = 1U;
    CHECK_TRUE(svc_config_calibration_set(&cal) == QIRAN_OK);
    CHECK_TRUE(svc_config_li_expected_nw(50000U, &power) == QIRAN_ERR_UNSUPPORTED);

    TEST_CASE("a null destination is refused");
    CHECK_TRUE(svc_config_li_expected_nw(50000U, NULL) == QIRAN_ERR_PARAM);
}

int main(void)
{
    plat_cpu_init();

    test_crc_known_vectors();
    test_defaults_are_inside_their_own_limits();
    test_documented_values_are_the_ones_in_the_table();
    test_write_protection();
    test_save_and_load_round_trip();
    test_load_without_storage_or_content();
    test_load_rejects_a_corrupt_block();
    test_load_handles_a_shorter_stored_block();
    test_load_refuses_an_out_of_range_stored_value();
    test_verify_detects_corruption_in_place();
    test_calibration_block();
    test_curve_interpolation();

    return TEST_REPORT();
}
