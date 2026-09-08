#include "qiran/mission/mission_ops_seq.h"

#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_log.h"

typedef struct {
    mission_state_id_t on_pass;
    qiran_fault_id_t   fault;
} stage_def_t;

/*
 * Where each stage goes when it passes, and the fault class its failures are
 * counted against. The class matters as much as the destination: it carries the
 * retry limit, so a stage's retry counter and its Critical-tier counter are one
 * counter rather than two that have to be kept in agreement.
 *
 * The extended characterisation state has no nominal successor because the
 * model gives it no transitions at all; it is left that way rather than
 * invented. Boot is driven by the boot sequencer, not from here.
 */
static const stage_def_t k_stage[SPR_COUNT] = {
    /* BOOT          */ { SPR_COUNT,         QIRAN_FAULT_NONE },
    /* PRECOND       */ { SPR_LASER_BRINGUP, QIRAN_FAULT_PRECOND },
    /* LASER_BRINGUP */ { SPR_MRR_TUNE,      QIRAN_FAULT_LASER_TEC },
    /* MRR_TUNE      */ { SPR_CROW_TUNE,     QIRAN_FAULT_MRR_LOCK },
    /* CROW_TUNE     */ { SPR_UMZI_TUNE,     QIRAN_FAULT_CROW_TUNE },
    /* UMZI_TUNE     */ { SPR_DLI_LOCK,      QIRAN_FAULT_UMZI_TUNE },
    /* DLI_LOCK      */ { SPR_SPAD_ENABLE,   QIRAN_FAULT_DLI_LOCK },
    /* SPAD_ENABLE   */ { SPR_PVS_T1,        QIRAN_FAULT_SPAD_OPTICAL_POWER },
    /* PVS_T1        */ { SPR_EXPERIMENT,    QIRAN_FAULT_MRR_RELOCK },
    /* PVS_T2        */ { SPR_EXPERIMENT,    QIRAN_FAULT_MRR_RELOCK },
    /* PVS_T3        */ { SPR_COUNT,         QIRAN_FAULT_NONE },
    /* EXPERIMENT    */ { SPR_PROCESS,       QIRAN_FAULT_DDR_OVERRUN },
    /* PROCESS       */ { SPR_DATA_HANDLING, QIRAN_FAULT_DDR_OVERRUN },
    /* DATA_HANDLING */ { SPR_DATA_HANDLING, QIRAN_FAULT_NAND_WRITE },
    /* CAL           */ { SPR_MRR_TUNE,      QIRAN_FAULT_MRR_LOCK }
};

QIRAN_STATIC_ASSERT(QIRAN_ARRAY_LEN(k_stage) == (size_t)SPR_COUNT,
                    every_state_has_a_stage_definition);

static stage_step_t s_step[SPR_COUNT];
static void        *s_ctx[SPR_COUNT];

static bool               s_stalled;
static mission_state_id_t s_stalled_state;
static uint32_t           s_steps;

static mission_precond_checks_t s_precond;
static bool                     s_precond_set;

/* --- Stage 0 --- */

static void precond_step(void *ctx, stage_outcome_t *out)
{
    uint32_t detail = 0U;

    QIRAN_UNUSED(ctx);

    out->fault = QIRAN_FAULT_PRECOND;

    /*
     * With no way to interrogate the hardware the precondition cannot be
     * declared met. Reporting that is the honest answer; treating an absent
     * check as a passed one would advance the sequence on no evidence.
     */
    if (!s_precond_set) {
        out->result = STAGE_RETRY;
        out->detail = 0U;
        return;
    }

    if (s_precond.dss_health(&detail) != QIRAN_OK) {
        out->result = STAGE_RETRY;
        out->detail = detail;
        return;
    }

    if (s_precond.qss_comms(&detail) != QIRAN_OK) {
        out->result = STAGE_RETRY;
        out->detail = detail;
        return;
    }

    out->result = STAGE_PASS;
}

qiran_status_t mission_precond_set_checks(const mission_precond_checks_t *checks)
{
    if (checks == NULL) {
        s_precond_set = false;
        s_precond.dss_health = NULL;
        s_precond.qss_comms = NULL;
        return QIRAN_OK;
    }

    if ((checks->dss_health == NULL) || (checks->qss_comms == NULL)) {
        return QIRAN_ERR_PARAM;
    }

    s_precond = *checks;
    s_precond_set = true;
    return QIRAN_OK;
}

/* --- sequencer --- */

void mission_ops_init(void)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)SPR_COUNT; i++) {
        s_step[i] = NULL;
        s_ctx[i] = NULL;
    }

    s_stalled = false;
    s_stalled_state = SPR_COUNT;
    s_steps = 0U;
    s_precond_set = false;
    s_precond.dss_health = NULL;
    s_precond.qss_comms = NULL;

    /* Stage 0 is integration glue and belongs to this module. */
    s_step[SPR_PRECOND] = precond_step;
}

qiran_status_t mission_ops_register(mission_state_id_t state,
                                    stage_step_t step,
                                    void *ctx)
{
    if ((state >= SPR_COUNT) || (step == NULL)) {
        return QIRAN_ERR_PARAM;
    }
    if (s_step[state] != NULL) {
        return QIRAN_ERR_STATE;
    }

    s_step[state] = step;
    s_ctx[state] = ctx;
    return QIRAN_OK;
}

static void stall(mission_state_id_t state)
{
    s_stalled = true;
    s_stalled_state = state;
    svc_log_event((uint16_t)state, s_steps);
}

void state_machine_update(void)
{
    mission_state_id_t state;
    const stage_def_t *def;
    stage_outcome_t out;
    qiran_fault_id_t fault;

    mission_state_service();

    if (mission_state_terminated() || s_stalled) {
        return;
    }

    state = mission_state_current();
    if (s_step[state] == NULL) {
        return;
    }

    def = &k_stage[state];

    out.result = STAGE_BUSY;
    out.target = SPR_COUNT;
    out.fault = QIRAN_FAULT_NONE;
    out.detail = 0U;

    s_steps++;
    s_step[state](s_ctx[state], &out);

    fault = (out.fault != QIRAN_FAULT_NONE) ? out.fault : def->fault;

    switch (out.result) {
    case STAGE_BUSY:
        break;

    case STAGE_PASS:
        /* Cleared so a later return to this stage starts its retries afresh. */
        svc_fdir_clear(fault);
        if (def->on_pass < SPR_COUNT) {
            (void)mission_state_request(def->on_pass);
        }
        break;

    case STAGE_RETRY:
        svc_fdir_report(fault, out.detail);
        if (svc_fdir_retries_exhausted(fault)) {
            /*
             * The retries are spent and the escalation has already reported the
             * flag. Several stages have no failure transition in the model, so
             * there is nowhere to go: the state is held and driving stops.
             */
            stall(state);
        } else if (mission_state_transition_legal(state, state)) {
            (void)mission_state_request(state);
        }
        break;

    case STAGE_REDIRECT:
        svc_fdir_report(fault, out.detail);
        if (mission_state_request(out.target) != QIRAN_OK) {
            stall(state);
        }
        break;

    case STAGE_FAIL:
    default:
        svc_fdir_report(fault, out.detail);
        stall(state);
        break;
    }
}

bool mission_ops_stalled(void)
{
    return s_stalled;
}

mission_state_id_t mission_ops_stalled_state(void)
{
    return s_stalled_state;
}

uint32_t mission_ops_steps(void)
{
    return s_steps;
}

bool mission_ops_driven(mission_state_id_t state)
{
    return (state < SPR_COUNT) && (s_step[state] != NULL);
}
