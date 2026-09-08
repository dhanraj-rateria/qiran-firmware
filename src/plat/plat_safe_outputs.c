#include "qiran/plat/plat_safe_outputs.h"

typedef struct {
    const char        *name;
    plat_safe_action_t action;
} safe_entry_t;

static safe_entry_t s_entry[PLAT_SAFE_OUTPUT_MAX];
static uint32_t     s_count;
static uint32_t     s_failures;
static uint32_t     s_applications;
static const char  *s_last_failure;

qiran_status_t plat_safe_outputs_register(const char *name, plat_safe_action_t action)
{
    if ((name == NULL) || (action == NULL)) {
        return QIRAN_ERR_PARAM;
    }
    if (s_count >= PLAT_SAFE_OUTPUT_MAX) {
        return QIRAN_ERR_RANGE;
    }

    s_entry[s_count].name = name;
    s_entry[s_count].action = action;
    s_count++;

    return QIRAN_OK;
}

qiran_status_t plat_safe_outputs_apply(void)
{
    qiran_status_t result = QIRAN_OK;
    uint32_t i;

    s_applications++;

    if (s_count == 0U) {
        return QIRAN_ERR_STATE;
    }

    for (i = 0U; i < s_count; i++) {
        if (s_entry[i].action() != QIRAN_OK) {
            s_failures++;
            s_last_failure = s_entry[i].name;
            result = QIRAN_ERR_HARDWARE;
        }
    }

    return result;
}

uint32_t plat_safe_outputs_registered(void)   { return s_count; }
uint32_t plat_safe_outputs_failures(void)     { return s_failures; }
uint32_t plat_safe_outputs_applications(void) { return s_applications; }

const char *plat_safe_outputs_last_failure(void)
{
    return (s_last_failure == NULL) ? "" : s_last_failure;
}

void plat_safe_outputs_reset_registry(void)
{
    s_count = 0U;
    s_failures = 0U;
    s_applications = 0U;
    s_last_failure = NULL;
}
