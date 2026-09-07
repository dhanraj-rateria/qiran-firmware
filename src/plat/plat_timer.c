#include "qiran/plat/plat_timer.h"

#if defined(__arm__)

#include "qiran/exec/exec_core.h"
#include "qiran/plat/plat_gic.h"
#include "qiran/qiran_config.h"

#include "xscutimer.h"
#include "xparameters.h"
#include "xparameters_ps.h"

/*
 * The private timer is clocked from the CPU 3x2x domain. QIRAN_TIMER_CLK_HZ may
 * be defined by the build to override this if the BSP reports it differently;
 * the programmed period is verified against a scope during bring-up either way.
 */
#if !defined(QIRAN_TIMER_CLK_HZ)
#  if defined(XPAR_PS7_SCUTIMER_0_CLK_FREQ_HZ)
#    define QIRAN_TIMER_CLK_HZ XPAR_PS7_SCUTIMER_0_CLK_FREQ_HZ
#  else
#    define QIRAN_TIMER_CLK_HZ (XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ / 2U)
#  endif
#endif

#define TIMER_PRESCALER 0U

#define TIMER_LOAD_VALUE                                                      \
    ((uint32_t)((((uint64_t)QIRAN_TIMER_CLK_HZ * (uint64_t)QIRAN_MINOR_CYCLE_MS) \
                 / 1000ULL) - 1ULL))

QIRAN_STATIC_ASSERT((((uint64_t)QIRAN_TIMER_CLK_HZ *
                      (uint64_t)QIRAN_MINOR_CYCLE_MS) / 1000ULL) <= 0xFFFFFFFFULL,
                    tick_period_fits_timer_width);

static XScuTimer s_timer;

static void timer_isr(void *ref)
{
    XScuTimer_ClearInterruptStatus((XScuTimer *)ref);
    exec_on_minor_tick();
}

qiran_status_t plat_timer_init(void)
{
    XScuTimer_Config *cfg;
    qiran_status_t st;

#if defined(XPAR_XSCUTIMER_0_DEVICE_ID)
    cfg = XScuTimer_LookupConfig(XPAR_XSCUTIMER_0_DEVICE_ID);
#else
    cfg = XScuTimer_LookupConfig(XPAR_XSCUTIMER_0_BASEADDR);
#endif
    if (cfg == NULL) {
        return QIRAN_ERR_HARDWARE;
    }

    if (XScuTimer_CfgInitialize(&s_timer, cfg, cfg->BaseAddr) != XST_SUCCESS) {
        return QIRAN_ERR_HARDWARE;
    }

    XScuTimer_SetPrescaler(&s_timer, TIMER_PRESCALER);
    XScuTimer_LoadTimer(&s_timer, TIMER_LOAD_VALUE);
    XScuTimer_EnableAutoReload(&s_timer);

    st = plat_gic_connect(XPS_SCU_TMR_INT_ID, timer_isr, &s_timer,
                          PLAT_GIC_PRIO_TIMER, PLAT_GIC_TRIGGER_LEVEL_HIGH);
    if (st != QIRAN_OK) {
        return st;
    }

    plat_gic_enable(XPS_SCU_TMR_INT_ID);
    XScuTimer_EnableInterrupt(&s_timer);

    return QIRAN_OK;
}

void plat_timer_start(void)
{
    XScuTimer_Start(&s_timer);
}

void plat_timer_stop(void)
{
    XScuTimer_Stop(&s_timer);
}

uint32_t plat_timer_load_value(void)
{
    return TIMER_LOAD_VALUE;
}

uint32_t plat_timer_clock_hz(void)
{
    return (uint32_t)QIRAN_TIMER_CLK_HZ;
}

#endif
