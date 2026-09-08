#include "qiran/plat/plat_devinit.h"

typedef struct {
    const char          *name;
    plat_devinit_stage_t stage;
    plat_devinit_fn_t    fn;
    bool                 required;
} devinit_entry_t;

static devinit_entry_t       s_entry[PLAT_DEVINIT_MAX];
static plat_devinit_result_t s_result[PLAT_DEVINIT_MAX];
static uint32_t              s_count;
static uint32_t              s_failures;
static const char           *s_first_failure;

void plat_devinit_init(void)
{
    s_count = 0U;
    s_failures = 0U;
    s_first_failure = NULL;
}

qiran_status_t plat_devinit_register(const char *name,
                                     plat_devinit_stage_t stage,
                                     plat_devinit_fn_t fn,
                                     bool required)
{
    if ((name == NULL) || (fn == NULL) || (stage >= PLAT_DEVINIT_STAGE_COUNT)) {
        return QIRAN_ERR_PARAM;
    }
    if (s_count >= PLAT_DEVINIT_MAX) {
        return QIRAN_ERR_RANGE;
    }

    s_entry[s_count].name = name;
    s_entry[s_count].stage = stage;
    s_entry[s_count].fn = fn;
    s_entry[s_count].required = required;
    s_count++;

    return QIRAN_OK;
}

qiran_status_t plat_devinit_run_stage(plat_devinit_stage_t stage)
{
    qiran_status_t overall = QIRAN_OK;
    uint32_t i;

    if (stage >= PLAT_DEVINIT_STAGE_COUNT) {
        return QIRAN_ERR_PARAM;
    }

    for (i = 0U; i < s_count; i++) {
        qiran_status_t st;

        if (s_entry[i].stage != stage) {
            continue;
        }

        st = s_entry[i].fn();

        s_result[i].name = s_entry[i].name;
        s_result[i].stage = stage;
        s_result[i].result = st;
        s_result[i].required = s_entry[i].required;
        s_result[i].ran = true;

        if (st != QIRAN_OK) {
            s_failures++;
            if (s_first_failure == NULL) {
                s_first_failure = s_entry[i].name;
            }
            if (s_entry[i].required) {
                overall = st;
            }
        }
    }

    return overall;
}

/*
 * A stage that fails stops the sequence: devices in a later stage are reached
 * through the ones in an earlier stage, so initialising them after a failure
 * would exercise a path already known to be broken.
 */
qiran_status_t plat_devinit_run(void)
{
    uint32_t stage;

    s_failures = 0U;
    s_first_failure = NULL;

    for (stage = 0U; stage < (uint32_t)PLAT_DEVINIT_STAGE_COUNT; stage++) {
        qiran_status_t st = plat_devinit_run_stage((plat_devinit_stage_t)stage);

        if (st != QIRAN_OK) {
            return st;
        }
    }

    return QIRAN_OK;
}

uint32_t plat_devinit_count(void)    { return s_count; }
uint32_t plat_devinit_failures(void) { return s_failures; }

bool plat_devinit_result_get(uint32_t index, plat_devinit_result_t *out)
{
    if ((index >= s_count) || (out == NULL)) {
        return false;
    }
    *out = s_result[index];
    return true;
}

const char *plat_devinit_first_failure(void)
{
    return (s_first_failure == NULL) ? "" : s_first_failure;
}
