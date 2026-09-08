#include "qiran/plat/plat_boot.h"

#include "qiran/exec/exec_core.h"
#include "qiran/plat/plat_cpu.h"
#include "qiran/plat/plat_devinit.h"
#include "qiran/plat/plat_gic.h"
#include "qiran/plat/plat_irq.h"
#include "qiran/plat/plat_isr_uart.h"
#include "qiran/plat/plat_post.h"
#include "qiran/plat/plat_safe_outputs.h"
#include "qiran/plat/plat_timer.h"
#include "qiran/qiran_config.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_log.h"
#include "qiran/svc/svc_time.h"

qiran_status_t plat_early_init(void)
{
    qiran_status_t st;

    plat_cpu_init();

    st = plat_gic_init();
    if (st != QIRAN_OK) {
        return st;
    }

    plat_irq_init();

    /* Receive ring is valid before any device is attached to it. */
    st = plat_isr_uart_init(NULL);
    if (st != QIRAN_OK) {
        return st;
    }

    exec_init();

    st = plat_timer_init();
    if (st != QIRAN_OK) {
        return st;
    }

    plat_timer_start();
    plat_gic_start();

    svc_time_init();

    return QIRAN_OK;
}

static plat_boot_check_t s_platform_check;
static plat_boot_link_t  s_link_bringup;
static boot_report_t     s_report;

qiran_status_t plat_boot_set_platform_check(plat_boot_check_t check)
{
    s_platform_check = check;
    return QIRAN_OK;
}

qiran_status_t plat_boot_set_link_bringup(plat_boot_link_t link)
{
    s_link_bringup = link;
    return QIRAN_OK;
}

static qiran_status_t step_safe_outputs(void)
{
    return plat_safe_outputs_apply();
}

static qiran_status_t step_platform(void)
{
    uint32_t detail = 0U;

    if (s_platform_check == NULL) {
        return QIRAN_OK;
    }
    return s_platform_check(&detail);
}

static qiran_status_t step_post(void)
{
    return plat_post_run();
}

static qiran_status_t step_devices(void)
{
    return plat_devinit_run();
}

static qiran_status_t step_links(void)
{
    if (s_link_bringup == NULL) {
        return QIRAN_OK;
    }
    return s_link_bringup();
}

typedef struct {
    const char      *name;
    qiran_status_t (*run)(void);
    uint32_t         budget_ms;
    qiran_fault_id_t fault;
} boot_step_def_t;

/*
 * Safe outputs run first and unconditionally: nothing else may execute while an
 * actuator could still be holding whatever state a previous run left it in.
 *
 * OPEN: link establishment has no confirmed budget of its own and is treated as
 * bounded within the device-initialisation window.
 */
static const boot_step_def_t k_step[BOOT_STEP_COUNT] = {
    { "safe_outputs", step_safe_outputs, QIRAN_BOOTLOADER_BUDGET_MS,
      QIRAN_FAULT_SAFE_OUTPUTS },
    { "platform",     step_platform,     QIRAN_BOOTLOADER_BUDGET_MS,
      QIRAN_FAULT_BOOT_IMAGE },
    { "post",         step_post,         QIRAN_POST_BUDGET_MS,
      QIRAN_FAULT_POST },
    { "devices",      step_devices,      QIRAN_DEVINIT_BUDGET_MS,
      QIRAN_FAULT_DEVINIT },
    { "links",        step_links,        QIRAN_LINK_BUDGET_MS,
      QIRAN_FAULT_LINK_ESTABLISH }
};

QIRAN_STATIC_ASSERT(QIRAN_ARRAY_LEN(k_step) == (size_t)BOOT_STEP_COUNT,
                    every_boot_step_is_defined);

/*
 * A step is retried in place rather than by resetting the processor. Each
 * processor reset would repeat the whole sequence, so three of them cost up to
 * three times the full boot budget, which the mission timing model does not
 * allocate for; retrying only the failed step costs only that step.
 *
 * OPEN: the requirement can also be read as issuing a processor reset per
 * attempt. Confirm with systems engineering. The retry count and the escalation
 * are identical either way; only where execution resumes differs.
 */
static bool run_step(boot_step_t index)
{
    const boot_step_def_t *step = &k_step[index];
    const svc_fdir_policy_t *policy = svc_fdir_policy(step->fault);
    /*
     * A class with no tolerance is escalated on its first occurrence and must
     * not be retried, or the step would repeat for ever against a condition
     * already declared unrecoverable.
     */
    bool retryable = (policy != NULL) && (policy->limit > 0U);
    uint64_t step_started = svc_time_now_ms();

    for (;;) {
        uint64_t started = svc_time_now_ms();
        qiran_status_t st = step->run();
        uint32_t elapsed = (uint32_t)(svc_time_now_ms() - started);

        s_report.attempts[index]++;

        if (st == QIRAN_OK) {
            s_report.duration_ms[index] =
                (uint32_t)(svc_time_now_ms() - step_started);
            s_report.over_budget[index] = elapsed > step->budget_ms;

            if (s_report.over_budget[index]) {
                /* Completed, but not inside its budget: a real finding even
                   though the step itself succeeded. */
                svc_fdir_report(step->fault, elapsed);
                error_handling_service();
            }
            return true;
        }

        /*
         * The occurrence counter for this fault class is the retry counter, and
         * reaching its limit is what requests the power cycle. Servicing the
         * fault here rather than waiting for the cyclic loop is what makes that
         * escalation happen during boot, where the loop does not yet run.
         */
        svc_fdir_report(step->fault, (uint32_t)st);
        error_handling_service();

        if (!retryable || svc_fdir_retries_exhausted(step->fault)) {
            s_report.duration_ms[index] =
                (uint32_t)(svc_time_now_ms() - step_started);
            s_report.failed_step = index;
            return false;
        }
    }
}

qiran_status_t boot_and_init(void)
{
    uint64_t started = svc_time_now_ms();
    uint32_t i;

    for (i = 0U; i < (uint32_t)BOOT_STEP_COUNT; i++) {
        s_report.attempts[i] = 0U;
        s_report.duration_ms[i] = 0U;
        s_report.over_budget[i] = false;
    }
    s_report.outcome = BOOT_OUTCOME_PENDING;
    s_report.failed_step = BOOT_STEP_COUNT;

    for (i = 0U; i < (uint32_t)BOOT_STEP_COUNT; i++) {
        if (!run_step((boot_step_t)i)) {
            s_report.outcome = BOOT_OUTCOME_TERMINAL;
            s_report.total_ms = (uint32_t)(svc_time_now_ms() - started);
            svc_log_event((uint16_t)QIRAN_FAULT_DEVINIT, s_report.total_ms);
            return QIRAN_ERR_HARDWARE;
        }
    }

    s_report.outcome = BOOT_OUTCOME_SUCCESS;
    s_report.total_ms = (uint32_t)(svc_time_now_ms() - started);

    /*
     * Clearing on success means a later failure of the same class starts its
     * retry count from zero rather than inheriting boot's attempts.
     */
    for (i = 0U; i < (uint32_t)BOOT_STEP_COUNT; i++) {
        svc_fdir_clear(k_step[i].fault);
    }

    return QIRAN_OK;
}

void plat_boot_report(boot_report_t *out)
{
    if (out != NULL) {
        *out = s_report;
    }
}

const char *plat_boot_step_name(boot_step_t step)
{
    return (step < BOOT_STEP_COUNT) ? k_step[step].name : "?";
}
