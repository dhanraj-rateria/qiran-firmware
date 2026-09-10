#include "qiran/data/data_path.h"

#include "qiran/data/storage_nand.h"
#include "qiran/mission/mission_seq.h"
#include "qiran/plat/plat_irq.h"
#include "qiran/svc/svc_fdir.h"

/*
 * Slots the fabric has finished writing, waiting to be reduced. One entry per
 * acquisition buffer is enough: a slot cannot complete twice without being
 * armed again in between.
 */
#define READY_SLOTS DDR_REGION_COUNT

static ddr_region_t s_ready[READY_SLOTS];
static uint32_t     s_ready_count;

static uint32_t s_completions;
static uint32_t s_arm_failures;
static uint32_t s_dropped;

void data_path_init(void)
{
    s_ready_count = 0U;
    s_completions = 0U;
    s_arm_failures = 0U;
    s_dropped = 0U;
}

qiran_status_t data_path_arm(uint32_t channel)
{
    ddr_region_t region = storage_ddr_free_raw_slot(channel);

    if (channel >= DDR_CHANNELS) {
        return QIRAN_ERR_PARAM;
    }

    if (region >= DDR_REGION_COUNT) {
        /*
         * Both slots of the pair are still held, so there is nowhere for the
         * next interval to go. Reported: acquisition has outrun reduction, and
         * the double buffering that exists to prevent this has been used up.
         */
        s_arm_failures++;
        svc_fdir_report(QIRAN_FAULT_DDR_OVERRUN, channel);
        return QIRAN_ERR_BUSY;
    }

    return storage_ddr_handoff(region, DDR_OWNER_NONE, DDR_OWNER_PL_DMA);
}

uint32_t data_path_ready_slots(void)
{
    return s_ready_count;
}

bool data_path_take_ready(ddr_region_t *region)
{
    uint32_t i;

    if ((region == NULL) || (s_ready_count == 0U)) {
        return false;
    }

    *region = s_ready[0];

    for (i = 1U; i < s_ready_count; i++) {
        s_ready[i - 1U] = s_ready[i];
    }
    s_ready_count--;

    return true;
}

/*
 * The completed buffer is identified by the index the interrupt captured, not
 * by which one this happens to think is in flight. Working it out from timing
 * is exactly what the buffer rule forbids.
 */
static void complete(uint32_t datum)
{
    ddr_region_t region = (ddr_region_t)(datum % (uint32_t)DDR_REGION_COUNT);

    if (storage_ddr_handoff(region, DDR_OWNER_PL_DMA, DDR_OWNER_PS_PROCESSING)
        != QIRAN_OK) {
        /* The ownership check already reported it. */
        s_dropped++;
        return;
    }

    if (s_ready_count >= READY_SLOTS) {
        s_dropped++;
        svc_fdir_report(QIRAN_FAULT_DDR_OVERRUN, (uint32_t)region);
        return;
    }

    s_ready[s_ready_count] = region;
    s_ready_count++;
    s_completions++;
}

qiran_status_t data_path_transfer_begin(void)
{
    return storage_nand_transfer_begin();
}

bool data_path_transfer_done(void)
{
    return storage_nand_transfer_complete();
}

void data_path_service_interrupts(void)
{
    plat_irq_event_t ev;

    while (plat_irq_take(PLAT_IRQ_DMA, &ev)) {
        complete(ev.datum);
    }
}

void data_path_manage(void)
{
    /*
     * The transfer runs only at the stage that owns it, so a run's data cannot
     * start leaving before the run has finished being reduced.
     */
    if (mission_stage() == STAGE_DATA_HANDLING) {
        storage_nand_transfer_service();
    }
}

uint32_t data_path_completions(void)  { return s_completions; }
uint32_t data_path_arm_failures(void) { return s_arm_failures; }
uint32_t data_path_dropped(void)      { return s_dropped; }
