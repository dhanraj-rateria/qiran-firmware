#include "qiran/plat/plat_boot.h"

#include "qiran/exec/exec_core.h"
#include "qiran/plat/plat_cpu.h"
#include "qiran/plat/plat_gic.h"
#include "qiran/plat/plat_irq.h"
#include "qiran/plat/plat_isr_uart.h"
#include "qiran/plat/plat_timer.h"
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

qiran_status_t boot_and_init(void)
{
    return QIRAN_OK;
}
