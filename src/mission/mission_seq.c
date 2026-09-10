#include "qiran/mission/mission_seq.h"

#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_log.h"
#include "qiran/svc/svc_time.h"

typedef struct {
    const char        *name;
    qiran_fault_id_t   fault;
    svc_config_param_t budget;
} stage_def_t;

/*
 * The fault class carries the stage's retry limit, so a stage's retry counter
 * and its Critical-tier counter are one counter. The budget names the parameter
 * holding how long the stage may take; stages the requirements give no duration
 * for are not timed.
 */
static const stage_def_t k_stage[STAGE_COUNT] = {
    { "BOOT",          QIRAN_FAULT_DEVINIT,            SVC_CONFIG_PARAM_COUNT },
    { "PRECOND",       QIRAN_FAULT_PRECOND,            SVC_CONFIG_PARAM_COUNT },
    { "LASER_BRINGUP", QIRAN_FAULT_LASER_TEC,          CFG_LASER_TEC_TIMEOUT_MS },
    { "MRR_TUNE",      QIRAN_FAULT_MRR_LOCK,           CFG_MRR_BUDGET_MS },
    { "CROW_TUNE",     QIRAN_FAULT_CROW_TUNE,          CFG_CROW_BUDGET_MS },
    { "UMZI_TUNE",     QIRAN_FAULT_UMZI_TUNE,          CFG_UMZI_BUDGET_MS },
    { "DLI_LOCK",      QIRAN_FAULT_DLI_LOCK,           CFG_DLI_BUDGET_MS },
    { "SPAD_ENABLE",   QIRAN_FAULT_SPAD_OPTICAL_POWER, CFG_SPAD_BUDGET_MS },
    { "PVS_T1",        QIRAN_FAULT_MRR_RELOCK,         CFG_PVS_T1_BUDGET_MS },
    { "PVS_T2",        QIRAN_FAULT_MRR_RELOCK,         CFG_PVS_T2_BUDGET_MS },
    { "EXPERIMENT",    QIRAN_FAULT_DDR_OVERRUN,        SVC_CONFIG_PARAM_COUNT },
    { "PROCESS",       QIRAN_FAULT_DDR_OVERRUN,        SVC_CONFIG_PARAM_COUNT },
    { "DATA_HANDLING", QIRAN_FAULT_NAND_WRITE,         SVC_CONFIG_PARAM_COUNT }
};

QIRAN_STATIC_ASSERT(QIRAN_ARRAY_LEN(k_stage) == (size_t)STAGE_COUNT,
                    every_stage_is_defined);

static mission_step_fn_t s_step[STAGE_COUNT];
static void             *s_ctx[STAGE_COUNT];
static uint32_t          s_entries[STAGE_COUNT];

static mission_stage_t s_stage;
static uint64_t        s_entered_ms;
static uint32_t        s_attempts;
static bool            s_timed_out;
static bool            s_stalled;

static bool             s_aborted;
static qiran_fault_id_t s_abort_cause;
static uint32_t         s_reentries;

static qiran_payload_status_t s_payload;

static void enter(mission_stage_t stage)
{
    s_stage = stage;
    s_entered_ms = svc_time_now_ms();
    s_timed_out = false;
    s_entries[stage]++;
    svc_log_event((uint16_t)stage, s_entries[stage]);
}

void mission_seq_init(void)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)STAGE_COUNT; i++) {
        s_step[i] = NULL;
        s_ctx[i] = NULL;
        s_entries[i] = 0U;
    }

    s_stage = STAGE_BOOT;
    s_entered_ms = svc_time_now_ms();
    s_attempts = 0U;
    s_timed_out = false;
    s_stalled = false;
    s_aborted = false;
    s_abort_cause = QIRAN_FAULT_NONE;
    s_reentries = 0U;
    s_payload = QIRAN_PAYLOAD_NON_OPERATIONAL;
    s_entries[STAGE_BOOT] = 1U;
}

qiran_status_t mission_seq_register(mission_stage_t stage,
                                    mission_step_fn_t step, void *ctx)
{
    if ((stage >= STAGE_COUNT) || (step == NULL)) {
        return QIRAN_ERR_PARAM;
    }
    if (s_step[stage] != NULL) {
        return QIRAN_ERR_STATE;
    }

    s_step[stage] = step;
    s_ctx[stage] = ctx;
    return QIRAN_OK;
}

/* Reported once per entry to a stage: an overrun stays overrun, and saying so
   every cycle fills the log without adding anything. */
static void check_budget(void)
{
    svc_config_param_t param = k_stage[s_stage].budget;
    uint32_t elapsed;
    int32_t budget;

    if ((param >= SVC_CONFIG_PARAM_COUNT) || s_timed_out) {
        return;
    }

    budget = svc_config_get(param);
    if (budget <= 0) {
        return;
    }

    elapsed = mission_stage_elapsed_ms();
    if (elapsed > (uint32_t)budget) {
        s_timed_out = true;
        svc_fdir_report(QIRAN_FAULT_STAGE_TIMEOUT,
                        ((uint32_t)s_stage << 24) | (elapsed & 0x00FFFFFFU));
    }
}

void mission_seq_step(void)
{
    const stage_def_t *def = &k_stage[s_stage];
    mission_step_t out;
    qiran_fault_id_t fault;

    if (s_aborted || s_stalled) {
        return;
    }

    check_budget();

    if (s_step[s_stage] == NULL) {
        return;
    }

    out.result = STEP_WORKING;
    out.back_to = s_stage;
    out.fault = QIRAN_FAULT_NONE;
    out.detail = 0U;

    s_step[s_stage](s_ctx[s_stage], &out);

    fault = (out.fault != QIRAN_FAULT_NONE) ? out.fault : def->fault;

    switch (out.result) {
    case STEP_WORKING:
        break;

    case STEP_DONE:
        svc_fdir_clear(fault);
        s_attempts = 0U;
        if (s_stage == STAGE_SPAD_ENABLE) {
            s_payload = QIRAN_PAYLOAD_OPERATIONAL;
        }
        if ((uint32_t)s_stage < ((uint32_t)STAGE_COUNT - 1U)) {
            enter((mission_stage_t)((uint32_t)s_stage + 1U));
        }
        break;

    case STEP_RETRY:
        s_attempts++;
        svc_fdir_report(fault, out.detail);
        if (svc_fdir_retries_exhausted(fault)) {
            s_stalled = true;
        } else {
            enter(s_stage);
        }
        break;

    case STEP_BACK:
        svc_fdir_report(fault, out.detail);
        /* Only backwards. Forwards would skip the stages in between, which
           nothing has run. */
        if (out.back_to >= s_stage) {
            s_stalled = true;
            break;
        }
        s_attempts = 0U;
        s_reentries++;
        svc_fdir_reentry_note(fault);
        enter(out.back_to);
        break;

    case STEP_ABORT:
    default:
        svc_fdir_report(fault, out.detail);
        s_stalled = true;
        break;
    }
}

mission_stage_t mission_stage(void)
{
    return s_stage;
}

const char *mission_stage_name(mission_stage_t stage)
{
    return (stage < STAGE_COUNT) ? k_stage[stage].name : "?";
}

uint32_t mission_stage_elapsed_ms(void)
{
    return (uint32_t)(svc_time_now_ms() - s_entered_ms);
}

uint32_t mission_stage_attempts(void)
{
    return s_attempts;
}

uint32_t mission_stage_entries(mission_stage_t stage)
{
    return (stage < STAGE_COUNT) ? s_entries[stage] : 0U;
}

bool mission_stalled(void)
{
    return s_stalled;
}

void mission_abort(qiran_fault_id_t cause)
{
    if (s_aborted) {
        return;
    }

    s_aborted = true;
    s_abort_cause = cause;
    s_payload = QIRAN_PAYLOAD_NON_OPERATIONAL;
    svc_log_fault(cause, QIRAN_SEV_SEVERE, (uint32_t)s_stage);
}

bool mission_aborted(void)
{
    return s_aborted;
}

qiran_fault_id_t mission_abort_cause(void)
{
    return s_abort_cause;
}

uint32_t mission_reentries(void)
{
    return s_reentries;
}

qiran_payload_status_t mission_payload_status(void)
{
    return s_payload;
}

void mission_payload_status_set(qiran_payload_status_t status)
{
    s_payload = status;
}
