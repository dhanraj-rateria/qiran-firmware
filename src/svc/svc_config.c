#include "qiran/svc/svc_config.h"

#include "qiran/svc/svc_crc.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_log.h"

#define CONFIG_MAGIC  0x51434647U
#define CAL_MAGIC     0x51434C42U
#define CONFIG_VERSION 1U
#define CAL_VERSION    1U

/*
 * Values are those stated in the requirements. Where a requirement gives a
 * range, the default sits inside it and the range becomes the limits; where it
 * gives a ceiling, the ceiling is the maximum.
 *
 * Deliberately absent: thresholds the requirements describe without giving a
 * number, such as the resonator lock error threshold, the drop-port target
 * range and the splitting specification. Those must arrive from the subsystem
 * owners as values. Inventing plausible ones here would put unreviewed numbers
 * where reviewed ones belong.
 */
static const svc_config_def_t k_def[SVC_CONFIG_PARAM_COUNT] = {
    { "laser_tec_setpoint",  "m degC",   0,  50000, 27000, true  },
    { "laser_tec_band",      "m degC",   1,   1000,   100, true  },
    { "laser_tec_nominal",   "ms",    1000, 120000, 15000, false },
    { "laser_tec_timeout",   "ms",    1000, 120000, 30000, false },
    { "laser_bias_target",   "uA",   55000,  60000, 57500, true  },
    { "laser_bias_ramp",     "uA/ms",    1,   1000,  1000, true  },
    { "laser_li_tolerance",  "%",        1,     50,    30, true  },

    { "mrr_budget",          "ms",    1000, 120000, 10000, false },
    { "crow_budget",         "ms",    1000, 120000, 20000, false },
    { "crow_pump_budget",    "nW",       1,  10000,  1000, true  },
    { "umzi_budget",         "ms",    1000, 120000, 10000, false },

    { "dli_stable_band",     "uK",       1,  10000,  1500, true  },
    { "dli_settle_nominal",  "ms",    1000, 240000, 30000, false },
    { "dli_settle_max",      "ms",    1000, 240000, 60000, false },
    { "dli_phase_lock",      "ms",    1000, 120000, 10000, false },
    { "dli_budget",          "ms",    1000, 240000, 80000, false },
    { "dli_lock_rms",        "mrad",     1,   1000,   140, true  },
    { "dli_lock_rms_strict", "mrad",     1,   1000,   100, true  },
    { "dli_servo_bw",        "Hz",     300,    500,   400, true  },

    { "spad_budget",         "ms",    1000, 120000,  5000, false },
    { "spad_dark_check",     "ms",     100,  10000,  2000, false },
    { "spad_dark_abort",     "ratio",    2,    100,    10, true  },
    { "spad_dark_log",       "ratio",    2,     10,     2, true  },

    { "pvs_t1_budget",       "ms",    1000,  60000, 10000, false },
    { "pvs_t2_budget",       "ms",    1000, 300000,120000, false },
    { "pvs_sweep_points",    "count",   15,    128,    15, true  },
    { "pvs_sweep_settle",    "ms",       1,     10,     2, true  },
    { "pvs_integration",     "ms",    1000,  10000,  5000, true  }
};

QIRAN_STATIC_ASSERT(QIRAN_ARRAY_LEN(k_def) == (size_t)SVC_CONFIG_PARAM_COUNT,
                    every_parameter_is_defined);

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t reserved;
    int32_t  value[SVC_CONFIG_PARAM_COUNT];
    uint32_t crc;
} config_image_t;

static int32_t  s_value[SVC_CONFIG_PARAM_COUNT];
static uint32_t s_live_crc;
static bool     s_locked;

static svc_config_storage_t s_storage;
static bool                 s_storage_set;

static svc_calibration_t s_cal;
static bool              s_cal_valid;

static uint32_t s_changes;
static uint32_t s_rejections;
static uint32_t s_corrections;

static uint32_t live_crc(void)
{
    return svc_crc32(s_value, (uint32_t)sizeof(s_value));
}

static bool in_range(svc_config_param_t id, int32_t value)
{
    return (value >= k_def[id].min) && (value <= k_def[id].max);
}

static void load_defaults(void)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)SVC_CONFIG_PARAM_COUNT; i++) {
        s_value[i] = k_def[i].def;
    }
    s_live_crc = live_crc();
}

void svc_config_init(void)
{
    uint32_t i;

    load_defaults();
    s_locked = false;
    s_storage_set = false;
    s_storage.read = NULL;
    s_storage.write = NULL;
    s_cal_valid = false;
    s_changes = 0U;
    s_rejections = 0U;
    s_corrections = 0U;

    for (i = 0U; i < (uint32_t)(sizeof(s_cal) / sizeof(uint32_t)); i++) {
        ((uint32_t *)&s_cal)[i] = 0U;
    }
}

qiran_status_t svc_config_set_storage(const svc_config_storage_t *storage)
{
    if (storage == NULL) {
        s_storage_set = false;
        s_storage.read = NULL;
        s_storage.write = NULL;
        return QIRAN_OK;
    }

    if ((storage->read == NULL) || (storage->write == NULL)) {
        return QIRAN_ERR_PARAM;
    }

    s_storage = *storage;
    s_storage_set = true;
    return QIRAN_OK;
}

int32_t svc_config_get(svc_config_param_t id)
{
    if (id >= SVC_CONFIG_PARAM_COUNT) {
        return 0;
    }
    return s_value[id];
}

const svc_config_def_t *svc_config_def(svc_config_param_t id)
{
    return (id < SVC_CONFIG_PARAM_COUNT) ? &k_def[id] : NULL;
}

qiran_status_t svc_config_set(svc_config_param_t id, int32_t value)
{
    if (id >= SVC_CONFIG_PARAM_COUNT) {
        return QIRAN_ERR_PARAM;
    }

    if (!in_range(id, value)) {
        s_rejections++;
        svc_fdir_report(QIRAN_FAULT_CONFIG, (uint32_t)id);
        return QIRAN_ERR_RANGE;
    }

    if (s_locked && !k_def[id].writable_in_flight) {
        s_rejections++;
        return QIRAN_ERR_STATE;
    }

    s_value[id] = value;
    s_live_crc = live_crc();
    s_changes++;
    svc_log_event((uint16_t)id, (uint32_t)value);

    return QIRAN_OK;
}

void svc_config_lock(void)   { s_locked = true; }
void svc_config_unlock(void) { s_locked = false; }
bool svc_config_locked(void) { return s_locked; }

static qiran_status_t read_params(void)
{
    config_image_t image;
    uint32_t computed;
    uint32_t n;
    uint32_t i;
    uint32_t corrected;

    if (s_storage.read(SVC_CONFIG_OFFSET_PARAMS, &image, (uint32_t)sizeof(image))
        != QIRAN_OK) {
        return QIRAN_ERR_HARDWARE;
    }

    if ((image.magic != CONFIG_MAGIC) || (image.version != CONFIG_VERSION)) {
        return QIRAN_ERR_INTEGRITY;
    }

    computed = svc_crc32(&image, (uint32_t)(sizeof(image) - sizeof(uint32_t)));
    if (computed != image.crc) {
        return QIRAN_ERR_INTEGRITY;
    }

    /*
     * A stored block from an earlier build may carry fewer parameters. Those
     * present are taken, the rest keep their defaults; the reverse case takes
     * only as many as this build knows about.
     */
    n = (image.count < (uint32_t)SVC_CONFIG_PARAM_COUNT)
        ? image.count : (uint32_t)SVC_CONFIG_PARAM_COUNT;

    corrected = 0U;

    for (i = 0U; i < n; i++) {
        if (in_range((svc_config_param_t)i, image.value[i])) {
            s_value[i] = image.value[i];
        } else {
            /*
             * Intact by checksum but outside its limits, so it was written by
             * something that did not respect them. The default stands.
             */
            s_value[i] = k_def[i].def;
            s_corrections++;
            corrected++;
            svc_fdir_report(QIRAN_FAULT_CONFIG, i);
        }
    }

    s_live_crc = live_crc();
    return (corrected == 0U) ? QIRAN_OK : QIRAN_ERR_RANGE;
}

static qiran_status_t read_calibration(void)
{
    svc_calibration_t cal;
    uint32_t computed;

    if (s_storage.read(SVC_CONFIG_OFFSET_CAL, &cal, (uint32_t)sizeof(cal))
        != QIRAN_OK) {
        return QIRAN_ERR_HARDWARE;
    }

    if ((cal.magic != CAL_MAGIC) || (cal.version != CAL_VERSION)) {
        return QIRAN_ERR_INTEGRITY;
    }

    computed = svc_crc32(&cal, (uint32_t)(sizeof(cal) - sizeof(uint32_t)));
    if (computed != cal.crc) {
        return QIRAN_ERR_INTEGRITY;
    }

    if (cal.li_count > SVC_CAL_LI_POINTS) {
        return QIRAN_ERR_INTEGRITY;
    }

    s_cal = cal;
    s_cal_valid = true;
    return QIRAN_OK;
}

qiran_status_t svc_config_load(void)
{
    qiran_status_t params;

    if (!s_storage_set) {
        svc_fdir_report(QIRAN_FAULT_CONFIG, 0U);
        return QIRAN_ERR_UNSUPPORTED;
    }

    params = read_params();
    if ((params != QIRAN_OK) && (params != QIRAN_ERR_RANGE)) {
        load_defaults();
        params = QIRAN_ERR_INTEGRITY;
        svc_fdir_report(QIRAN_FAULT_CONFIG, 0U);
    }

    if (read_calibration() != QIRAN_OK) {
        s_cal_valid = false;
        svc_fdir_report(QIRAN_FAULT_CONFIG, 1U);
    }

    return params;
}

qiran_status_t svc_config_save(void)
{
    config_image_t image;
    uint32_t i;

    if (!s_storage_set) {
        return QIRAN_ERR_UNSUPPORTED;
    }

    image.magic = CONFIG_MAGIC;
    image.version = CONFIG_VERSION;
    image.count = (uint32_t)SVC_CONFIG_PARAM_COUNT;
    image.reserved = 0U;

    for (i = 0U; i < (uint32_t)SVC_CONFIG_PARAM_COUNT; i++) {
        image.value[i] = s_value[i];
    }

    image.crc = svc_crc32(&image, (uint32_t)(sizeof(image) - sizeof(uint32_t)));

    return s_storage.write(SVC_CONFIG_OFFSET_PARAMS, &image,
                           (uint32_t)sizeof(image));
}

qiran_status_t svc_config_verify(void)
{
    uint32_t i;
    uint32_t out_of_range = 0U;
    bool crc_bad = live_crc() != s_live_crc;

    for (i = 0U; i < (uint32_t)SVC_CONFIG_PARAM_COUNT; i++) {
        if (!in_range((svc_config_param_t)i, s_value[i])) {
            out_of_range++;
        }
    }

    if (!crc_bad && (out_of_range == 0U)) {
        return QIRAN_OK;
    }

    /*
     * Something changed the block without going through the setter. Storage is
     * authoritative if there is any; otherwise the defaults are.
     */
    s_corrections++;
    svc_fdir_report(QIRAN_FAULT_CONFIG, out_of_range);

    if (s_storage_set && (read_params() == QIRAN_OK)) {
        return QIRAN_ERR_INTEGRITY;
    }

    load_defaults();
    return QIRAN_ERR_INTEGRITY;
}

const svc_calibration_t *svc_config_calibration(void)
{
    return s_cal_valid ? &s_cal : NULL;
}

qiran_status_t svc_config_calibration_set(const svc_calibration_t *cal)
{
    svc_calibration_t updated;

    if (cal == NULL) {
        return QIRAN_ERR_PARAM;
    }
    if (cal->li_count > SVC_CAL_LI_POINTS) {
        return QIRAN_ERR_RANGE;
    }

    updated = *cal;
    updated.magic = CAL_MAGIC;
    updated.version = CAL_VERSION;
    updated.crc = svc_crc32(&updated,
                            (uint32_t)(sizeof(updated) - sizeof(uint32_t)));

    s_cal = updated;
    s_cal_valid = true;
    s_changes++;

    if (s_storage_set) {
        return s_storage.write(SVC_CONFIG_OFFSET_CAL, &s_cal,
                               (uint32_t)sizeof(s_cal));
    }

    return QIRAN_OK;
}

/*
 * Linear interpolation between the two bracketing points. Extrapolation is
 * refused rather than guessed: a current outside the calibrated span has no
 * expected power that the curve can support, and returning one would let a
 * comparison against it look meaningful.
 */
qiran_status_t svc_config_li_expected_nw(uint32_t current_ua, uint32_t *power_nw)
{
    uint32_t i;

    if (power_nw == NULL) {
        return QIRAN_ERR_PARAM;
    }
    if (!s_cal_valid || (s_cal.li_count < 2U)) {
        return QIRAN_ERR_UNSUPPORTED;
    }

    if ((current_ua < s_cal.li[0].current_ua) ||
        (current_ua > s_cal.li[s_cal.li_count - 1U].current_ua)) {
        return QIRAN_ERR_RANGE;
    }

    for (i = 1U; i < s_cal.li_count; i++) {
        const svc_cal_li_point_t *lo = &s_cal.li[i - 1U];
        const svc_cal_li_point_t *hi = &s_cal.li[i];
        uint32_t span;

        if (current_ua > hi->current_ua) {
            continue;
        }

        span = hi->current_ua - lo->current_ua;
        if (span == 0U) {
            *power_nw = lo->power_nw;
            return QIRAN_OK;
        }

        {
            uint64_t rise = (uint64_t)(hi->power_nw - lo->power_nw);
            uint64_t run = (uint64_t)(current_ua - lo->current_ua);

            *power_nw = lo->power_nw + (uint32_t)((rise * run) / (uint64_t)span);
        }
        return QIRAN_OK;
    }

    return QIRAN_ERR_RANGE;
}

uint32_t svc_config_changes(void)    { return s_changes; }
uint32_t svc_config_rejections(void) { return s_rejections; }
uint32_t svc_config_corrections(void) { return s_corrections; }
bool     svc_config_calibration_valid(void) { return s_cal_valid; }
