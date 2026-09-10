#include "qiran/comm/comm_cmd.h"
#include "qiran/comm/comm_output.h"
#include "qiran/comm/comm_process.h"
#include "qiran/comm/comm_rs485_hk.h"
#include "qiran/comm/comm_spw_link.h"
#include "qiran/data/data_path.h"
#include "qiran/data/data_product.h"
#include "qiran/data/storage_ddr.h"
#include "qiran/data/storage_nand.h"
#include "qiran/exec/exec_core.h"
#include "qiran/exec/exec_major.h"
#include "qiran/exec/exec_sched.h"
#include "qiran/mission/mission_seq.h"
#include "qiran/photonic/photonic_sched.h"
#include "qiran/plat/plat_boot.h"
#include "qiran/plat/plat_devinit.h"
#include "qiran/plat/plat_post.h"
#include "qiran/plat/plat_safe_outputs.h"
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

/* Deferred processing of what the interrupts published. */
static void service_interrupts(void)
{
    svc_fdir_service_interrupts();
    comm_spw_link_service_interrupts();
    data_path_service_interrupts();
    comm_cmd_service_interrupts();
}

/*
 * Runs before the telecommand handler, so a command received this cycle is
 * acted on with the data that arrived alongside it rather than a cycle later.
 */
static void communication(void)
{
    comm_spw_link_service();
    comm_output_service();
    comm_rs485_hk_transmit();
}

static void pl_control(void)
{
    mission_seq_step();
    control_loop_service_calls();
}

static void telemetry_write(void)
{
    comm_rs485_hk_stage();
}

static void subsystem_init(void)
{
    svc_log_init();
    svc_fdir_init();
    svc_config_init();
    svc_health_init();
    (void)svc_watchdog_init(NULL);

    plat_post_init();
    plat_devinit_init();

    mission_seq_init();
    photonic_sched_init();

    storage_ddr_init();
    storage_nand_init();
    data_product_init();
    data_path_init();

    comm_process_init();
    (void)svc_fdir_set_uplink(comm_rs485_hk_uplink());

    exec_major_init();
    exec_sched_init();
}

static void report_ready(void)
{
    comm_status_report_t report;
    uint32_t i;

    report.payload = mission_payload_status();
    report.health = svc_health_state();
    for (i = 0U; i < COMM_HK_INTERLOCKS; i++) {
        report.interlock[i] = COMM_INTERLOCK_UNEVALUATED;
    }
    report.fault = QIRAN_FAULT_NONE;
    report.severity = QIRAN_SEV_MINOR;

    (void)comm_rs485_status_send(&report);
}

/*
 * Everything done once. It runs inside the first minor cycle rather than ahead
 * of the loop, so there is one execution path through the software and the
 * timebase is already running while it happens.
 */
static void first_cycle(void)
{
    qiran_status_t st;

    subsystem_init();

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

    if (st == QIRAN_OK) {
        /* Settable while boot runs, locked once operational; those the ground
           may adjust by command stay writable either way. */
        svc_config_lock();
        report_ready();
    } else {
        mission_abort(QIRAN_FAULT_DEVINIT);
    }

    /* Initialisation ran for far longer than a minor cycle. The ticks it
       accumulated are discarded rather than charged as an overrun. */
    exec_resync();
}

int main(void)
{
    bool first = true;

    if (plat_early_init() != QIRAN_OK) {
        for (;;) {
        }
    }

    for (;;) {
        exec_wait_for_minor_tick();

        if (first) {
            first = false;
            first_cycle();
        }

        EXEC_RUN(EXEC_TASK_INTERRUPTS,          service_interrupts());
        EXEC_RUN(EXEC_TASK_HEALTH_CHECK,        health_check());
        EXEC_RUN(EXEC_TASK_COMMS,               communication());
        EXEC_RUN(EXEC_TASK_TELECOMMAND,         comm_cmd_service());
        EXEC_RUN(EXEC_TASK_PL_CONTROL,          pl_control());
        EXEC_RUN(EXEC_TASK_DATA_PATH,           data_path_manage());
        EXEC_RUN(EXEC_TASK_FDIR,                error_handling_service());
        EXEC_RUN(EXEC_TASK_HEALTH_CONSOLIDATE,  health_consolidate());
        EXEC_RUN(EXEC_TASK_TELEMETRY,           telemetry_write());

        if (exec_major_tick_due()) {
            EXEC_RUN(EXEC_TASK_MAJOR, major_cycle_tasks());
        }

        /* Last, so nothing that runs in this cycle is covered by a watchdog
           already refreshed on its behalf. */
        EXEC_RUN(EXEC_TASK_WATCHDOG, watchdog_tickle_if_healthy());
    }
}
