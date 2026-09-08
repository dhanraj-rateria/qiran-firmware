#include "qiran/mission/mission_state.h"

#include "qiran/exec/exec_core.h"
#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_log.h"
#include "qiran/svc/svc_time.h"

QIRAN_STATIC_ASSERT(SPR_COUNT == 15, state_model_has_fifteen_states);

typedef struct {
    uint8_t     from;
    uint8_t     to;
    const char *reason;
    /*
     * Whether this transition is a return to an earlier stage, which is what
     * the global re-entry count counts. A retry of the same stage is not one:
     * retries are already counted per stage as their own fault class, and
     * counting them here as well would consume the re-entry allowance twice
     * over for a single condition.
     */
    bool        reentry;
} transition_t;

static const transition_t k_transition[] = {
    { SPR_BOOT,          SPR_PRECOND,       "initialised",            false },
    { SPR_BOOT,          SPR_BOOT,          "init retry",             false },

    { SPR_PRECOND,       SPR_LASER_BRINGUP, "precondition met",       false },

    { SPR_LASER_BRINGUP, SPR_MRR_TUNE,      "tec stable, ramp ok",    false },
    { SPR_LASER_BRINGUP, SPR_LASER_BRINGUP, "bringup retry",          false },

    { SPR_MRR_TUNE,      SPR_CROW_TUNE,     "mrr locked",             false },
    { SPR_MRR_TUNE,      SPR_MRR_TUNE,      "sweep retry",            false },

    { SPR_CROW_TUNE,     SPR_UMZI_TUNE,     "drop ports in range",    false },
    { SPR_CROW_TUNE,     SPR_MRR_TUNE,      "mrr lock lost",          true  },
    { SPR_CROW_TUNE,     SPR_CROW_TUNE,     "crow retry",             false },

    { SPR_UMZI_TUNE,     SPR_DLI_LOCK,      "splitting in spec",      false },
    { SPR_UMZI_TUNE,     SPR_UMZI_TUNE,     "umzi retry",             false },

    { SPR_DLI_LOCK,      SPR_SPAD_ENABLE,   "both dli locked",        false },
    { SPR_DLI_LOCK,      SPR_CROW_TUNE,     "no classical fringe",    true  },
    { SPR_DLI_LOCK,      SPR_DLI_LOCK,      "dli retry",              false },

    { SPR_SPAD_ENABLE,   SPR_MRR_TUNE,      "interlock 1 failed",     true  },
    { SPR_SPAD_ENABLE,   SPR_CROW_TUNE,     "interlock 2 failed",     true  },
    { SPR_SPAD_ENABLE,   SPR_UMZI_TUNE,     "interlock 3 failed",     true  },
    { SPR_SPAD_ENABLE,   SPR_DLI_LOCK,      "interlock 4 failed",     true  },
    { SPR_SPAD_ENABLE,   SPR_PVS_T1,        "interlocks passed",      false },

    { SPR_PVS_T1,        SPR_EXPERIMENT,    "tier 1 within limits",   false },
    { SPR_PVS_T1,        SPR_PVS_T2,        "tier 1 out of limits",   false },

    { SPR_PVS_T2,        SPR_EXPERIMENT,    "coefficients in range",  false },
    { SPR_PVS_T2,        SPR_CAL,           "recoverable degradation",false },

    { SPR_CAL,           SPR_MRR_TUNE,      "recalibrate",            true  },

    { SPR_EXPERIMENT,    SPR_PROCESS,       "all cycles complete",    false },
    { SPR_EXPERIMENT,    SPR_LASER_BRINGUP, "laser lock lost",        true  },
    { SPR_EXPERIMENT,    SPR_MRR_TUNE,      "mrr lock lost",          true  },
    { SPR_EXPERIMENT,    SPR_CROW_TUNE,     "crow drift",             true  },
    { SPR_EXPERIMENT,    SPR_UMZI_TUNE,     "umzi drift",             true  },
    { SPR_EXPERIMENT,    SPR_DLI_LOCK,      "dli lock lost",          true  },

    { SPR_PROCESS,       SPR_DATA_HANDLING, "processed to memory",    false },

    { SPR_DATA_HANDLING, SPR_DATA_HANDLING, "transfer and clear",     false }
};

/*
 * The extended characterisation state has no transition of its own. It is
 * reached only on ground command, and the state model as written gives neither
 * the states from which that command is legal nor where it returns to. Left
 * absent rather than invented, which makes any request to it refused and
 * reported until the rule is supplied.
 *
 * OPEN: entry and exit conditions for the extended characterisation state.
 *
 * OPEN: the behaviour after data handling completes. The model has data
 * handling repeat while there is data to transfer, and nothing after it. What
 * ends the run is undefined, so nothing is assumed here.
 */

static const char *const k_name[SPR_COUNT] = {
    "BOOT", "PRECOND", "LASER_BRINGUP", "MRR_TUNE", "CROW_TUNE", "UMZI_TUNE",
    "DLI_LOCK", "SPAD_ENABLE", "PVS_T1", "PVS_T2", "PVS_T3", "EXPERIMENT",
    "PROCESS", "DATA_HANDLING", "CAL"
};

/*
 * Time a state may occupy, where the requirements give a figure. Parameters
 * rather than constants, so the limits stay in one validated store; states
 * whose duration is not specified are not policed.
 */
static const svc_config_param_t k_budget[SPR_COUNT] = {
    SVC_CONFIG_PARAM_COUNT,      /* BOOT: enforced by the boot sequencer */
    SVC_CONFIG_PARAM_COUNT,      /* PRECOND: no figure given */
    CFG_LASER_TEC_TIMEOUT_MS,
    CFG_MRR_BUDGET_MS,
    CFG_CROW_BUDGET_MS,
    CFG_UMZI_BUDGET_MS,
    CFG_DLI_BUDGET_MS,
    CFG_SPAD_BUDGET_MS,
    CFG_PVS_T1_BUDGET_MS,
    CFG_PVS_T2_BUDGET_MS,
    SVC_CONFIG_PARAM_COUNT,      /* PVS_T3: ground commanded, no figure */
    SVC_CONFIG_PARAM_COUNT,      /* EXPERIMENT: bounded by its cycle count */
    SVC_CONFIG_PARAM_COUNT,      /* PROCESS */
    SVC_CONFIG_PARAM_COUNT,      /* DATA_HANDLING */
    SVC_CONFIG_PARAM_COUNT       /* CAL */
};

static mission_state_id_t     s_state;
static uint64_t               s_entered_ms;
static bool                   s_timeout_reported;
static uint32_t               s_entries[SPR_COUNT];
static qiran_payload_status_t s_payload;
static bool                   s_terminated;
static qiran_fault_id_t       s_terminal_cause;
static uint32_t               s_reentries;
static uint32_t               s_rejections;

static mission_history_t s_history[MISSION_HISTORY_DEPTH];
static uint32_t          s_history_head;

void mission_state_init(void)
{
    uint32_t i;

    s_state = SPR_BOOT;
    s_entered_ms = svc_time_now_ms();
    s_timeout_reported = false;
    s_payload = QIRAN_PAYLOAD_NON_OPERATIONAL;
    s_terminated = false;
    s_terminal_cause = QIRAN_FAULT_NONE;
    s_reentries = 0U;
    s_rejections = 0U;
    s_history_head = 0U;

    for (i = 0U; i < (uint32_t)SPR_COUNT; i++) {
        s_entries[i] = 0U;
    }
    s_entries[SPR_BOOT] = 1U;
}

mission_state_id_t mission_state_current(void)
{
    return s_state;
}

const char *mission_state_name(mission_state_id_t state)
{
    return (state < SPR_COUNT) ? k_name[state] : "?";
}

uint32_t mission_state_elapsed_ms(void)
{
    return (uint32_t)(svc_time_now_ms() - s_entered_ms);
}

uint32_t mission_state_entries(mission_state_id_t state)
{
    return (state < SPR_COUNT) ? s_entries[state] : 0U;
}

static const transition_t *find(mission_state_id_t from, mission_state_id_t to)
{
    uint32_t i;

    for (i = 0U; i < QIRAN_ARRAY_LEN(k_transition); i++) {
        if ((k_transition[i].from == (uint8_t)from) &&
            (k_transition[i].to == (uint8_t)to)) {
            return &k_transition[i];
        }
    }

    return NULL;
}

bool mission_state_transition_legal(mission_state_id_t from,
                                    mission_state_id_t to)
{
    if ((from >= SPR_COUNT) || (to >= SPR_COUNT)) {
        return false;
    }
    return find(from, to) != NULL;
}

static void record(mission_state_id_t from, mission_state_id_t to, bool reentry)
{
    mission_history_t *h = &s_history[s_history_head % MISSION_HISTORY_DEPTH];

    h->tick = exec_raw_tick_count();
    h->from = (uint8_t)from;
    h->to = (uint8_t)to;
    h->reentry = reentry ? 1U : 0U;
    h->reserved = 0U;
    s_history_head++;
}

qiran_status_t mission_state_request(mission_state_id_t to)
{
    const transition_t *t;
    mission_state_id_t from = s_state;

    if (to >= SPR_COUNT) {
        s_rejections++;
        svc_fdir_report(QIRAN_FAULT_STATE_TRANSITION, (uint32_t)to);
        return QIRAN_ERR_PARAM;
    }

    if (s_terminated) {
        s_rejections++;
        return QIRAN_ERR_STATE;
    }

    t = find(from, to);
    if (t == NULL) {
        s_rejections++;
        svc_fdir_report(QIRAN_FAULT_STATE_TRANSITION,
                        ((uint32_t)from << 8) | (uint32_t)to);
        return QIRAN_ERR_STATE;
    }

    s_state = to;
    s_entered_ms = svc_time_now_ms();
    s_timeout_reported = false;
    s_entries[to]++;
    record(from, to, t->reentry);
    svc_log_event((uint16_t)(((uint32_t)from << 8) | (uint32_t)to),
                  (uint32_t)s_entries[to]);

    if (t->reentry) {
        s_reentries++;
        /*
         * The re-entry allowance is global and shared with every other stage,
         * so it is held in the fault service rather than here, which is also
         * what escalates once the allowance is spent.
         */
        svc_fdir_reentry_note(QIRAN_FAULT_NONE);
    }

    /* Declared operational once the interlocks have passed and bias is applied. */
    if ((from == SPR_SPAD_ENABLE) && (to == SPR_PVS_T1)) {
        s_payload = QIRAN_PAYLOAD_OPERATIONAL;
    }

    return QIRAN_OK;
}

void state_machine_update(void)
{
    svc_config_param_t budget_param;
    uint32_t elapsed;
    int32_t budget;

    if (s_terminated) {
        return;
    }

    budget_param = k_budget[s_state];
    if (budget_param >= SVC_CONFIG_PARAM_COUNT) {
        return;
    }

    budget = svc_config_get(budget_param);
    if (budget <= 0) {
        return;
    }

    elapsed = mission_state_elapsed_ms();
    if ((elapsed > (uint32_t)budget) && !s_timeout_reported) {
        /*
         * Reported once per entry to the state: a state that has overrun stays
         * overrun, and repeating the report every cycle would say nothing new
         * while filling the log.
         */
        s_timeout_reported = true;
        svc_fdir_report(QIRAN_FAULT_STATE_TIMEOUT,
                        ((uint32_t)s_state << 24) | (elapsed & 0x00FFFFFFU));
    }
}

void mission_state_terminate(qiran_fault_id_t cause)
{
    if (s_terminated) {
        return;
    }

    s_terminated = true;
    s_terminal_cause = cause;
    s_payload = QIRAN_PAYLOAD_NON_OPERATIONAL;
    svc_log_fault(cause, QIRAN_SEV_SEVERE, (uint32_t)s_state);
}

bool mission_state_terminated(void)
{
    return s_terminated;
}

qiran_fault_id_t mission_state_terminal_cause(void)
{
    return s_terminal_cause;
}

qiran_payload_status_t mission_payload_status(void)
{
    return s_payload;
}

void mission_payload_status_set(qiran_payload_status_t status)
{
    s_payload = status;
}

uint32_t mission_state_reentries(void)
{
    return s_reentries;
}

uint32_t mission_state_rejections(void)
{
    return s_rejections;
}

/* Most recent first, which is the order a diagnostic report wants. */
uint32_t mission_state_history(mission_history_t *out, uint32_t max)
{
    uint32_t available;
    uint32_t n;
    uint32_t i;

    if (out == NULL) {
        return 0U;
    }

    available = (s_history_head < MISSION_HISTORY_DEPTH)
                ? s_history_head : MISSION_HISTORY_DEPTH;
    n = (max < available) ? max : available;

    for (i = 0U; i < n; i++) {
        out[i] = s_history[(s_history_head - 1U - i) % MISSION_HISTORY_DEPTH];
    }

    return n;
}
