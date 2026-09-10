#include "qiran/exec/exec_major.h"

#include "qiran/comm/comm_ccsds.h"
#include "qiran/comm/comm_cmd.h"
#include "qiran/comm/comm_output.h"
#include "qiran/comm/comm_rs485_hk.h"
#include "qiran/comm/comm_spw_link.h"
#include "qiran/data/data_path.h"
#include "qiran/data/data_product.h"
#include "qiran/data/storage_ddr.h"
#include "qiran/data/storage_nand.h"
#include "qiran/exec/exec_core.h"
#include "qiran/exec/exec_sched.h"
#include "qiran/mission/mission_seq.h"
#include "qiran/photonic/photonic_sched.h"
#include "qiran/plat/plat_irq.h"
#include "qiran/plat/plat_timer.h"
#include "qiran/qiran_config.h"
#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_health.h"
#include "qiran/svc/svc_log.h"
#include "qiran/svc/svc_watchdog.h"

#if QIRAN_BRINGUP_TRACE
#include "xil_printf.h"

static uint64_t s_last_table_dump;

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

static void trace_task_table(void)
{
    uint32_t i;

    xil_printf("  task            cls  runs      last    mean    worst  (us)\r\n");

    for (i = 0U; i < (uint32_t)EXEC_TASK_COUNT; i++) {
        exec_task_id_t id = (exec_task_id_t)i;
        const exec_task_def_t *def = exec_task_def(id);
        exec_task_stat_t st;

        exec_task_stat_get(id, &st);
        xil_printf("  %-14s %4d %6lu %8lu %7lu %8lu\r\n",
                   def->name, (int)def->cycle, (unsigned long)st.runs,
                   (unsigned long)exec_cycles_to_us(st.cycles_last),
                   (unsigned long)exec_cycles_to_us(exec_task_mean_cycles(id)),
                   (unsigned long)exec_cycles_to_us(st.cycles_worst));
    }
}

static void trace(void)
{
    exec_stats_t st;
    exec_sched_check_t chk;
    uint64_t period;

    exec_stats_get(&st);
    exec_sched_check(&chk);

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

    xil_printf("  sched worst_slot=%lu used=%luus margin=%luus fits=%d "
               "complete=%d\r\n",
               (unsigned long)chk.worst_slot,
               (unsigned long)exec_cycles_to_us(chk.worst_slot_cycles),
               (unsigned long)exec_cycles_to_us(chk.margin_cycles),
               (int)chk.fits, (int)chk.complete);

    xil_printf("  stage=%s for=%lums entries=%lu tries=%lu reentry=%lu "
               "payload=%d stall=%d abort=%d\r\n",
               mission_stage_name(mission_stage()),
               (unsigned long)mission_stage_elapsed_ms(),
               (unsigned long)mission_stage_entries(mission_stage()),
               (unsigned long)mission_stage_attempts(),
               (unsigned long)mission_reentries(),
               (int)mission_payload_status(),
               (int)mission_stalled(), (int)mission_aborted());

    xil_printf("  health=%d monitors=%lu gating=%lu flips=%lu loops=%lu\r\n",
               (int)svc_health_state(),
               (unsigned long)svc_health_count(),
               (unsigned long)svc_health_gating_count(),
               (unsigned long)svc_health_transitions(),
               (unsigned long)photonic_sched_enabled_count());

    xil_printf("  fdir sev=%lu/%lu/%lu/%lu undeliv=%lu log=%lu/%lu\r\n",
               (unsigned long)svc_fdir_severity_count(QIRAN_SEV_MINOR),
               (unsigned long)svc_fdir_severity_count(QIRAN_SEV_MEDIUM),
               (unsigned long)svc_fdir_severity_count(QIRAN_SEV_CRITICAL),
               (unsigned long)svc_fdir_severity_count(QIRAN_SEV_SEVERE),
               (unsigned long)svc_fdir_undelivered(),
               (unsigned long)svc_log_fault_total(),
               (unsigned long)svc_log_dropped());

    xil_printf("  cfg locked=%d changes=%lu rejected=%lu corrected=%lu "
               "cal=%d\r\n",
               (int)svc_config_locked(),
               (unsigned long)svc_config_changes(),
               (unsigned long)svc_config_rejections(),
               (unsigned long)svc_config_corrections(),
               (int)svc_config_calibration_valid());

    xil_printf("  cmd rx=%lu done=%lu rej=%lu fail=%lu resync=%lu busy=%d\r\n",
               (unsigned long)comm_cmd_received(),
               (unsigned long)comm_cmd_completed(),
               (unsigned long)comm_cmd_rejected(),
               (unsigned long)comm_cmd_failed(),
               (unsigned long)comm_cmd_resyncs(),
               (int)comm_cmd_in_progress());

    xil_printf("  hk sent=%lu status=%lu txfail=%lu smpfail=%lu link=%d\r\n",
               (unsigned long)comm_rs485_hk_frames_sent(),
               (unsigned long)comm_rs485_hk_status_sent(),
               (unsigned long)comm_rs485_hk_transmit_failures(),
               (unsigned long)comm_rs485_hk_sample_failures(),
               (int)comm_rs485_hk_ready());

    xil_printf("  spw=%s errs=%lu disc=%lu tx=%lu rx=%lu | out pend=%lu "
               "sent=%lu rej=%lu gaps=%lu bad=%lu\r\n",
               comm_spw_link_state_name(comm_spw_link_state()),
               (unsigned long)comm_spw_link_errors(),
               (unsigned long)comm_spw_link_disconnects(),
               (unsigned long)comm_spw_link_packets_sent(),
               (unsigned long)comm_spw_link_packets_received(),
               (unsigned long)comm_output_pending(),
               (unsigned long)comm_output_sent(),
               (unsigned long)comm_output_rejected(),
               (unsigned long)comm_ccsds_sequence_gaps(),
               (unsigned long)comm_ccsds_integrity_errors());

    xil_printf("  data dma=%lu ready=%lu armfail=%lu own_err=%lu | store "
               "used=%lu recs=%lu sent=%lu prod=%lu bad=%lu\r\n",
               (unsigned long)data_path_completions(),
               (unsigned long)data_path_ready_slots(),
               (unsigned long)data_path_arm_failures(),
               (unsigned long)storage_ddr_violations(),
               (unsigned long)storage_nand_used(),
               (unsigned long)storage_nand_records(),
               (unsigned long)storage_nand_transferred(),
               (unsigned long)data_product_computed(),
               (unsigned long)data_product_inconsistent());

    period = exec_minor_tick_count() /
             (uint64_t)(QIRAN_MINOR_PER_MAJOR * QIRAN_TRACE_TABLE_PERIOD);
    if (period != s_last_table_dump) {
        s_last_table_dump = period;
        trace_task_table();
    }
}
#endif

void exec_major_init(void)
{
#if QIRAN_BRINGUP_TRACE
    s_last_table_dump = 0U;
#endif
}

void major_cycle_tasks(void)
{
#if QIRAN_BRINGUP_TRACE
    trace();
#endif
}
