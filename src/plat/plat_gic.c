#include "qiran/plat/plat_gic.h"

#if defined(__arm__)

#include "xscugic.h"
#include "xil_exception.h"
#include "xparameters.h"

static XScuGic s_gic;
static bool    s_ready;

qiran_status_t plat_gic_init(void)
{
    XScuGic_Config *cfg;

    if (s_ready) {
        return QIRAN_OK;
    }

#if defined(XPAR_SCUGIC_SINGLE_DEVICE_ID)
    cfg = XScuGic_LookupConfig(XPAR_SCUGIC_SINGLE_DEVICE_ID);
#elif defined(XPAR_XSCUGIC_0_DEVICE_ID)
    cfg = XScuGic_LookupConfig(XPAR_XSCUGIC_0_DEVICE_ID);
#else
    cfg = XScuGic_LookupConfig(XPAR_XSCUGIC_0_BASEADDR);
#endif
    if (cfg == NULL) {
        return QIRAN_ERR_HARDWARE;
    }

    if (XScuGic_CfgInitialize(&s_gic, cfg, cfg->CpuBaseAddress) != XST_SUCCESS) {
        return QIRAN_ERR_HARDWARE;
    }

    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_IRQ_INT,
                                 (Xil_ExceptionHandler)XScuGic_InterruptHandler,
                                 &s_gic);

    s_ready = true;
    return QIRAN_OK;
}

qiran_status_t plat_gic_connect(uint32_t irq_id,
                                plat_gic_handler_t handler,
                                void *ref,
                                uint8_t priority,
                                uint8_t trigger)
{
    if (!s_ready) {
        return QIRAN_ERR_STATE;
    }
    if (handler == NULL) {
        return QIRAN_ERR_PARAM;
    }

    XScuGic_SetPriorityTriggerType(&s_gic, irq_id, priority, trigger);

    if (XScuGic_Connect(&s_gic, irq_id, (Xil_InterruptHandler)handler, ref)
        != XST_SUCCESS) {
        return QIRAN_ERR_HARDWARE;
    }

    return QIRAN_OK;
}

void plat_gic_enable(uint32_t irq_id)
{
    if (s_ready) {
        XScuGic_Enable(&s_gic, irq_id);
    }
}

void plat_gic_disable(uint32_t irq_id)
{
    if (s_ready) {
        XScuGic_Disable(&s_gic, irq_id);
    }
}

void plat_gic_start(void)
{
    Xil_ExceptionEnable();
}

void *plat_gic_instance(void)
{
    return s_ready ? (void *)&s_gic : NULL;
}

#else /* host build: the framework above is exercised without a controller */

qiran_status_t plat_gic_init(void)
{
    return QIRAN_OK;
}

qiran_status_t plat_gic_connect(uint32_t irq_id,
                                plat_gic_handler_t handler,
                                void *ref,
                                uint8_t priority,
                                uint8_t trigger)
{
    QIRAN_UNUSED(irq_id);
    QIRAN_UNUSED(ref);
    QIRAN_UNUSED(priority);
    QIRAN_UNUSED(trigger);
    return (handler == NULL) ? QIRAN_ERR_PARAM : QIRAN_OK;
}

void plat_gic_enable(uint32_t irq_id) { QIRAN_UNUSED(irq_id); }
void plat_gic_disable(uint32_t irq_id) { QIRAN_UNUSED(irq_id); }
void plat_gic_start(void) { }
void *plat_gic_instance(void) { return NULL; }

#endif
