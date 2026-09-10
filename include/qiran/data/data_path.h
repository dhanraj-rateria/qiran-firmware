#ifndef QIRAN_DATA_PATH_H
#define QIRAN_DATA_PATH_H

#include "qiran/data/storage_ddr.h"
#include "qiran/qiran_types.h"

/*
 * The data path's slot in the cyclic loop: buffer ownership, acquisition
 * completion, and driving the transfer out.
 */
void data_path_init(void);

/* Takes over the buffers the fabric reported finished. */
void data_path_service_interrupts(void);

void data_path_manage(void);

/*
 * Arms the free slot of a channel for the fabric to fill, which is the handoff
 * from nobody owning it to the fabric owning it. Fails when both slots of that
 * channel are still busy, which is the condition that says acquisition has
 * outrun reduction.
 */
qiran_status_t data_path_arm(uint32_t channel);

/* Slots the fabric has finished with and the processor now owns. */
uint32_t data_path_ready_slots(void);
bool     data_path_take_ready(ddr_region_t *region);

/* Starts moving the stored run out, and reports when it has all gone. */
qiran_status_t data_path_transfer_begin(void);
bool           data_path_transfer_done(void);

uint32_t data_path_completions(void);
uint32_t data_path_arm_failures(void);
uint32_t data_path_dropped(void);

#endif
