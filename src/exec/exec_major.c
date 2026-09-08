#include "qiran/exec/exec_major.h"

#include "qiran/comm/comm_rs485_hk.h"
#include "qiran/exec/exec_core.h"
#include "qiran/exec/exec_sched.h"
#include "qiran/mission/mission_ops_seq.h"
#include "qiran/mission/mission_state.h"
#include "qiran/photonic/photonic_sched.h"
#include "qiran/qiran_config.h"
#include "qiran/plat/plat_irq.h"
#include "qiran/plat/plat_timer.h"
#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_log.h"
#include "qiran/svc/svc_watchdog.h"

#if QIRAN_BRINGUP_TRACE
#include "xil_printf.h"
#endif

#if QIRAN_BRINGUP_TRACE
static uint64_t s_last_table_dump;
#endif

void exec_major_init(void)
{
#if QIRAN_BRINGUP_TRACE
    s_last_table_dump = 0U;
#endif
}

#if QIRAN_BRINGUP_TRACE
static void trace_interrupts(void)
{
    uint32_t last;
    uint32_t worst;
    uint32_t i;

    plat_timer_isr_cycles(&last, &worst);
    xil_printf("  isr timer=%luc", (unsigned long)worst);

    for (i = 0U; i < (uint32_t)PLAT_IRQ_COUNT; i++) {
        plat_irq_id_t id = (plat_irq_id_t)i;
        plat_irq_stats_t s;

        if (!plat_irq_registered(id)) {
            continue;
        }

        plat_irq_stats_get(id, &s);
        xil_printf(" %s=%lu/%lu/%luc", plat_irq_name(id),
                   (unsigned long)s.taken, (unsigned long)s.dropped,
                   (unsigned long)s.cycles_worst);
    }

    xil_printf("\r\n");
}
#endif

#if QIRAN_BRINGUP_TRACE
static void trace_task_table(void)
{
    uint32_t i;

    xil_printf("  task                cls  runs      last    mean    worst  (us)\r\n");

    for (i = 0U; i < (uint32_t)EXEC_TASK_COUNT; i++) {
        exec_task_id_t id = (exec_task_id_t)i;
        const exec_task_def_t *def = exec_task_def(id);
        exec_task_stat_t st;

        exec_task_stat_get(id, &st);
        xil_printf("  %-18s %4d %6lu %8lu %7lu %8lu\r\n",
                   def->name, (int)def->cycle, (unsigned long)st.runs,
                   (unsigned long)exec_cycles_to_us(st.cycles_last),
                   (unsigned long)exec_cycles_to_us(exec_task_mean_cycles(id)),
                   (unsigned long)exec_cycles_to_us(st.cycles_worst));
    }
}
#endif

void major_cycle_tasks(void)
{
    /*
     * A parameter corrupted in place would otherwise be found only when it
     * produced a bad command, so the block is re-checked against its checksum
     * and its limits once per major cycle.
     */
    (void)svc_config_verify();

#if QIRAN_BRINGUP_TRACE
    exec_stats_t st;
    exec_sched_check_t chk;

    exec_stats_get(&st);
    xil_printf("t=%lu maj=%lu body=%luus peak=%luus budget=%luus "
               "ovr=%lu lost=%lu wdt=%d\r\n",
               (unsigned long)exec_minor_tick_count(),
               (unsigned long)st.major_cycles,
               (unsigned long)exec_cycles_to_us(st.body_cycles_last),
               (unsigned long)exec_cycles_to_us(st.body_cycles_worst),
               (unsigned long)exec_cycles_to_us(exec_budget_cycles()),
               (unsigned long)st.overrun_events,
               (unsigned long)st.overrun_cycles_lost,
               (int)svc_watchdog_armed());

    trace_interrupts();

    xil_printf("  fdir sev=%lu/%lu/%lu/%lu esc_undeliv=%lu log=%lu/%lu reentry=%lu\r\n",
               (unsigned long)svc_fdir_severity_count(QIRAN_SEV_MINOR),
               (unsigned long)svc_fdir_severity_count(QIRAN_SEV_MEDIUM),
               (unsigned long)svc_fdir_severity_count(QIRAN_SEV_CRITICAL),
               (unsigned long)svc_fdir_severity_count(QIRAN_SEV_SEVERE),
               (unsigned long)svc_fdir_undelivered(),
               (unsigned long)svc_log_fault_total(),
               (unsigned long)svc_log_dropped(),
               (unsigned long)svc_fdir_reentry_count());
    xil_printf("  state=%s for=%lums entries=%lu reentry=%lu payload=%d "
               "term=%d rej=%lu\r\n",
               mission_state_name(mission_state_current()),
               (unsigned long)mission_state_elapsed_ms(),
               (unsigned long)mission_state_entries(mission_state_current()),
               (unsigned long)mission_state_reentries(),
               (int)mission_payload_status(),
               (int)mission_state_terminated(),
               (unsigned long)mission_state_rejections());
    xil_printf("  ops steps=%lu stalled=%d at=%s loops=%lu\r\n",
               (unsigned long)mission_ops_steps(),
               (int)mission_ops_stalled(),
               mission_state_name(mission_ops_stalled_state()),
               (unsigned long)photonic_sched_enabled_count());
    xil_printf("  hk sent=%lu status=%lu txfail=%lu smpfail=%lu link=%d\r\n",
               (unsigned long)comm_rs485_hk_frames_sent(),
               (unsigned long)comm_rs485_hk_status_sent(),
               (unsigned long)comm_rs485_hk_transmit_failures(),
               (unsigned long)comm_rs485_hk_sample_failures(),
               (int)comm_rs485_hk_ready());
    xil_printf("  cfg locked=%d changes=%lu rejected=%lu corrected=%lu cal=%d\r\n",
               (int)svc_config_locked(),
               (unsigned long)svc_config_changes(),
               (unsigned long)svc_config_rejections(),
               (unsigned long)svc_config_corrections(),
               (int)svc_config_calibration_valid());

    exec_sched_check(&chk);
    xil_printf("  sched worst_slot=%lu used=%luus budget=%luus margin=%luus "
               "fits=%d complete=%d\r\n",
               (unsigned long)chk.worst_slot,
               (unsigned long)exec_cycles_to_us(chk.worst_slot_cycles),
               (unsigned long)exec_cycles_to_us(chk.budget_cycles),
               (unsigned long)exec_cycles_to_us(chk.margin_cycles),
               (int)chk.fits, (int)chk.complete);

    if ((exec_minor_tick_count() /
         (uint64_t)(QIRAN_MINOR_PER_MAJOR * QIRAN_TRACE_TABLE_PERIOD)) !=
        s_last_table_dump) {
        s_last_table_dump = exec_minor_tick_count() /
            (uint64_t)(QIRAN_MINOR_PER_MAJOR * QIRAN_TRACE_TABLE_PERIOD);
        trace_task_table();
    }
#endif
}
