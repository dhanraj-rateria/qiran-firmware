#include "qiran/comm/comm_process.h"
#include "qiran/data/data_path.h"
#include "qiran/exec/exec_core.h"
#include "qiran/exec/exec_major.h"
#include "qiran/mission/mission_state.h"
#include "qiran/photonic/photonic_sched.h"
#include "qiran/plat/plat_boot.h"
#include "qiran/plat/plat_timer.h"
#include "qiran/qiran_config.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_health.h"
#include "qiran/svc/svc_time.h"
#include "qiran/svc/svc_watchdog.h"

#if QIRAN_BRINGUP_TRACE
#include "xil_printf.h"
#endif

static void application_init(void)
{
    svc_fdir_init();
    svc_health_init();
    svc_watchdog_init(NULL);
    mission_state_init();
    photonic_sched_init();
    data_path_init();
    comm_process_init();
    exec_major_init();
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
    xil_printf("boot=%d timer_clk=%luHz load=%lu budget=%luus\r\n", (int)st,
               (unsigned long)plat_timer_clock_hz(),
               (unsigned long)plat_timer_load_value(),
               (unsigned long)exec_cycles_to_us(exec_budget_cycles()));
#endif

    /*
     * Boot ran outside the cyclic loop, so the tick backlog it accumulated is
     * discarded here rather than being charged against the first loop body as
     * an overrun.
     */
    exec_resync();

    for (;;) {
        exec_wait_for_minor_tick();

        state_machine_update();
        health_monitor_periodic();
        control_loop_service_calls();
        data_path_manage();
        communication_process();
        error_handling_service();
        watchdog_tickle_if_healthy();

        if (exec_major_tick_due()) {
            major_cycle_tasks();
        }
    }
}
