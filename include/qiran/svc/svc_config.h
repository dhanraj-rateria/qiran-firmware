#ifndef QIRAN_SVC_CONFIG_H
#define QIRAN_SVC_CONFIG_H

#include "qiran/qiran_types.h"

/*
 * Scalar parameters are held as integers in explicitly named units rather than
 * as floating point. The quantities involved are naturally integral at the
 * precision the requirements state, comparisons are then exact and repeatable,
 * and no question arises about floating-point context around an interrupt.
 * Floating point remains appropriate in the data-processing path, which is not
 * this module.
 *
 * Every parameter carries a minimum and a maximum, so a limit such as an
 * absolute maximum commanded temperature is stated once, in one reviewable
 * table, rather than being re-checked at each place a setpoint is used.
 */
typedef enum {
    CFG_LASER_TEC_SETPOINT_MDEGC = 0,
    CFG_LASER_TEC_BAND_MDEGC,
    CFG_LASER_TEC_NOMINAL_MS,
    CFG_LASER_TEC_TIMEOUT_MS,
    CFG_LASER_BIAS_TARGET_UA,
    CFG_LASER_BIAS_RAMP_UA_PER_MS,
    CFG_LASER_LI_TOLERANCE_PCT,

    CFG_MRR_BUDGET_MS,
    CFG_CROW_BUDGET_MS,
    CFG_CROW_PUMP_BUDGET_NW,
    CFG_UMZI_BUDGET_MS,

    CFG_DLI_STABLE_BAND_UK,
    CFG_DLI_SETTLE_NOMINAL_MS,
    CFG_DLI_SETTLE_MAX_MS,
    CFG_DLI_PHASE_LOCK_MS,
    CFG_DLI_LOCK_RMS_MRAD,
    CFG_DLI_LOCK_RMS_STRICT_MRAD,
    CFG_DLI_SERVO_BW_HZ,

    CFG_SPAD_BUDGET_MS,
    CFG_SPAD_DARK_CHECK_MS,
    CFG_SPAD_DARK_ABORT_RATIO,
    CFG_SPAD_DARK_LOG_RATIO,

    CFG_PVS_T1_BUDGET_MS,
    CFG_PVS_T2_BUDGET_MS,
    CFG_PVS_SWEEP_POINTS,
    CFG_PVS_SWEEP_SETTLE_MS,
    CFG_PVS_INTEGRATION_MS,

    SVC_CONFIG_PARAM_COUNT
} svc_config_param_t;

typedef struct {
    const char *name;
    const char *unit;
    int32_t     min;
    int32_t     max;
    int32_t     def;
    /*
     * Whether the parameter may be changed once operational. Setpoints and
     * thresholds may, because correcting a degraded condition is a ground
     * decision applied by command. Durations and budgets may not, because
     * changing one mid-run invalidates the timing analysis rather than
     * adjusting an operating point.
     */
    bool        writable_in_flight;
} svc_config_def_t;

/* Read and write within the configuration partition. */
typedef struct {
    qiran_status_t (*read)(uint32_t offset, void *dst, uint32_t len);
    qiran_status_t (*write)(uint32_t offset, const void *src, uint32_t len);
} svc_config_storage_t;

#define SVC_CONFIG_OFFSET_PARAMS  0U
#define SVC_CONFIG_OFFSET_CAL     512U

#define SVC_CAL_LI_POINTS 16U
#define SVC_CAL_CHANNELS   2U

typedef struct {
    uint32_t current_ua;
    uint32_t power_nw;
} svc_cal_li_point_t;

/*
 * Ground calibration, plus the few quantities the requirements say are written
 * back into it during a run. All fields are four bytes wide and four-byte
 * aligned so the layout carries no padding and the checksum covers exactly what
 * is written.
 */
typedef struct {
    uint32_t           magic;
    uint32_t           version;
    uint32_t           li_count;
    uint32_t           reserved;
    svc_cal_li_point_t li[SVC_CAL_LI_POINTS];
    int32_t            dli_setpoint_uk[SVC_CAL_CHANNELS];
    uint32_t           branch_efficiency_ppm[SVC_CAL_CHANNELS];
    uint32_t           dark_count_baseline_cps[SVC_CAL_CHANNELS];
    uint32_t           spectral_bandwidth_pm;
    uint32_t           pvs_amplitude;
    uint32_t           pvs_linewidth_khz;
    uint32_t           pvs_noise_floor;
    uint32_t           crc;
} svc_calibration_t;

void svc_config_init(void);

qiran_status_t svc_config_set_storage(const svc_config_storage_t *storage);

/*
 * Loads parameters and calibration. The returned status describes the parameter
 * block only:
 *
 *   QIRAN_OK           intact, every value legal
 *   QIRAN_ERR_RANGE    intact, some values fell back to their default
 *   QIRAN_ERR_INTEGRITY unusable, every value is its default
 *   QIRAN_ERR_UNSUPPORTED no storage attached
 *
 * Calibration is reported separately through svc_config_calibration_valid(),
 * because the two failures have different consequences: without parameters
 * nothing can be commanded safely, whereas without calibration the payload
 * still runs and only the comparisons that need the curve are unavailable.
 * Folding them into one status would make a missing curve fail boot.
 *
 * Both failures are reported as faults regardless.
 */
qiran_status_t svc_config_load(void);
qiran_status_t svc_config_save(void);

int32_t                 svc_config_get(svc_config_param_t id);
qiran_status_t          svc_config_set(svc_config_param_t id, int32_t value);
const svc_config_def_t *svc_config_def(svc_config_param_t id);

/*
 * Write protection for normal operation. Parameters marked writable in flight
 * are still settable while locked; everything else is refused.
 */
void svc_config_lock(void);
void svc_config_unlock(void);
bool svc_config_locked(void);

/*
 * Re-checks the live parameter block against its checksum and every value
 * against its limits, and restores from storage or from defaults if either
 * fails. Intended to run on the major cycle: a value corrupted in place would
 * otherwise be found only when it produced a bad command.
 */
qiran_status_t svc_config_verify(void);

const svc_calibration_t *svc_config_calibration(void);
qiran_status_t           svc_config_calibration_set(const svc_calibration_t *cal);

/* Interpolates the calibration curve; fails if it is empty or out of range. */
qiran_status_t svc_config_li_expected_nw(uint32_t current_ua, uint32_t *power_nw);

uint32_t svc_config_changes(void);
uint32_t svc_config_rejections(void);
uint32_t svc_config_corrections(void);
bool     svc_config_calibration_valid(void);

#endif
