#include "qiran/exec/exec_major.h"

#include "qiran/exec/exec_core.h"
#include "qiran/qiran_config.h"
#include "qiran/svc/svc_watchdog.h"

#if QIRAN_BRINGUP_TRACE
#include "xil_printf.h"
#endif

void exec_major_init(void)
{
}

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
#endif
}
