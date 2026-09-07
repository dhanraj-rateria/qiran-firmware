#include "qiran/exec/exec_major.h"

#include "qiran/exec/exec_core.h"
#include "qiran/qiran_config.h"
#include "qiran/plat/plat_irq.h"
#include "qiran/plat/plat_timer.h"
#include "qiran/svc/svc_watchdog.h"

#if QIRAN_BRINGUP_TRACE
#include "xil_printf.h"
#endif

void exec_major_init(void)
{
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

void major_cycle_tasks(void)
{
#if QIRAN_BRINGUP_TRACE
    exec_stats_t st;

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
#endif
}
