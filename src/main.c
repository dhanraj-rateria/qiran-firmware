#include "qiran/comm/comm_process.h"
#include "qiran/data/data_path.h"
#include "qiran/exec/exec_core.h"
#include "qiran/exec/exec_major.h"
#include "qiran/exec/exec_sched.h"
#include "qiran/mission/mission_state.h"
#include "qiran/photonic/photonic_sched.h"
#include "qiran/plat/plat_boot.h"
#include "qiran/plat/plat_safe_outputs.h"
#include "qiran/plat/plat_devinit.h"
#include "qiran/plat/plat_post.h"
#include "qiran/plat/plat_timer.h"
#include "qiran/qiran_config.h"
#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_health.h"
#include "qiran/svc/svc_log.h"
#include "qiran/svc/svc_time.h"
#include "qiran/svc/svc_watchdog.h"

#if QIRAN_BRINGUP_TRACE
#include "xil_printf.h"
#endif

static void application_init(void)
{
    svc_log_init();
    svc_fdir_init();
    svc_config_init();
    plat_post_init();
    plat_devinit_init();
    svc_health_init();
    svc_watchdog_init(NULL);
    mission_state_init();
    photonic_sched_init();
    data_path_init();
    comm_process_init();
    exec_major_init();
    exec_sched_init();
}

int main(void)
{
    qiran_status_t st;

    st = plat_early_init();
    if (st != QIRAN_OK) {
#if QIRAN_BRINGUP_TRACE
        xil_printf("early init failed: %d\r\n", (int)st);
#endif
        for (;;) {
        }
    }

    application_init();

    st = boot_and_init();
#if QIRAN_BRINGUP_TRACE
    {
        boot_report_t br;

        plat_boot_report(&br);
        xil_printf("boot=%d outcome=%d failed=%s total=%lums\r\n", (int)st,
                   (int)br.outcome, plat_boot_step_name(br.failed_step),
                   (unsigned long)br.total_ms);
        xil_printf("  timer_clk=%luHz load=%lu budget=%luus safe_out=%lu "
                   "post=%lu dev=%lu\r\n",
                   (unsigned long)plat_timer_clock_hz(),
                   (unsigned long)plat_timer_load_value(),
                   (unsigned long)exec_cycles_to_us(exec_budget_cycles()),
                   (unsigned long)plat_safe_outputs_registered(),
                   (unsigned long)plat_post_count(),
                   (unsigned long)plat_devinit_count());
    }
#endif

    /*
     * Boot ran outside the cyclic loop, so the tick backlog it accumulated is
     * discarded here rather than being charged against the first loop body as
     * an overrun.
     */
    /*
     * Parameters are settable while boot runs and locked once operational;
     * those the ground may adjust by command stay writable either way.
     */
    if (st == QIRAN_OK) {
        svc_config_lock();
        (void)mission_state_request(SPR_PRECOND);
    } else {
        /*
         * Boot exhausted its retries and a power cycle has been requested. The
         * run ends where it stands; the cyclic loop keeps running so status and
         * diagnostics remain readable.
         */
        mission_state_terminate(QIRAN_FAULT_DEVINIT);
    }

    exec_resync();

    for (;;) {
        exec_wait_for_minor_tick();

        EXEC_RUN(EXEC_TASK_STATE_MACHINE, state_machine_update());
        EXEC_RUN(EXEC_TASK_HEALTH,         health_monitor_periodic());
        EXEC_RUN(EXEC_TASK_CONTROL_LOOPS,  control_loop_service_calls());
        EXEC_RUN(EXEC_TASK_DATA_PATH,      data_path_manage());
        EXEC_RUN(EXEC_TASK_COMMS,          communication_process());
        EXEC_RUN(EXEC_TASK_FDIR,           error_handling_service());
        EXEC_RUN(EXEC_TASK_WATCHDOG,       watchdog_tickle_if_healthy());

        if (exec_major_tick_due()) {
            EXEC_RUN(EXEC_TASK_MAJOR, major_cycle_tasks());
        }
    }
}
