#ifndef QIRAN_STORAGE_DDR_H
#define QIRAN_STORAGE_DDR_H

#include "qiran/qiran_types.h"

/*
 * Regions of external memory, and who owns each one at any moment.
 *
 * Ownership is stated and checked, never worked out from when something
 * happened. A handoff names both the owner it expects to be taking over from
 * and the one taking over, and fails if the first is wrong. That turns the
 * race between the fabric writing a buffer and the processor reading it into a
 * refused call rather than a corrupted product, and it is why the buffer
 * management rule says ownership must not be inferred from timing.
 */
typedef enum {
    DDR_OWNER_NONE = 0,       /* free, and cleared */
    DDR_OWNER_PL_DMA,         /* the fabric is filling it */
    DDR_OWNER_PS_PROCESSING,  /* the processor is reducing it */
    DDR_OWNER_STORAGE,        /* being written to non-volatile storage */
    DDR_OWNER_COMMS,          /* queued for transmission */
    DDR_OWNER_COUNT
} ddr_owner_t;

/*
 * One acquisition buffer per receiver channel, each with two slots, so
 * acquisition of the next interval can start while the previous one is still
 * being reduced.
 */
#define DDR_CHANNELS   2U
#define DDR_RAW_SLOTS  2U

typedef enum {
    DDR_REGION_RAW_CH0_A = 0,
    DDR_REGION_RAW_CH0_B,
    DDR_REGION_RAW_CH1_A,
    DDR_REGION_RAW_CH1_B,
    DDR_REGION_PROCESSED,
    DDR_REGION_TX_PACKET,
    DDR_REGION_SCRATCH,
    DDR_REGION_COUNT
} ddr_region_t;

void storage_ddr_init(void);

/*
 * Region bases and sizes come from the platform rather than being fixed here:
 * the memory map is a hardware decision and the sizes in the requirements are
 * marked as draft.
 */
qiran_status_t storage_ddr_set_region(ddr_region_t region, void *base,
                                      uint32_t bytes);
bool           storage_ddr_configured(ddr_region_t region);

ddr_owner_t storage_ddr_owner(ddr_region_t region);
const char *storage_ddr_owner_name(ddr_owner_t owner);
const char *storage_ddr_region_name(ddr_region_t region);

/*
 * Hands a region from one owner to the next. Refused and reported if the
 * region is not currently owned by `from`.
 */
qiran_status_t storage_ddr_handoff(ddr_region_t region, ddr_owner_t from,
                                   ddr_owner_t to);

/* Releases a region and marks it empty. Only its current owner may. */
qiran_status_t storage_ddr_clear(ddr_region_t region, ddr_owner_t from);

/* Appends to a region. Only its current owner may write. */
qiran_status_t storage_ddr_append(ddr_region_t region, ddr_owner_t owner,
                                  const void *data, uint32_t len);

/* Records how much the fabric wrote, since it does not append through here. */
qiran_status_t storage_ddr_set_used(ddr_region_t region, ddr_owner_t owner,
                                    uint32_t used);

qiran_status_t storage_ddr_read(ddr_region_t region, ddr_owner_t owner,
                                const uint8_t **data, uint32_t *len);

uint32_t storage_ddr_used(ddr_region_t region);
uint32_t storage_ddr_capacity(ddr_region_t region);
uint32_t storage_ddr_free(ddr_region_t region);

/* The free slot of a channel's pair, or region count if both are busy. */
ddr_region_t storage_ddr_free_raw_slot(uint32_t channel);

uint32_t storage_ddr_violations(void);
uint32_t storage_ddr_handoffs(void);
uint32_t storage_ddr_overruns(void);

#endif
