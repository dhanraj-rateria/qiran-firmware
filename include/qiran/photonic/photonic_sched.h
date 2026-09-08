#ifndef QIRAN_PHOTONIC_SCHED_H
#define QIRAN_PHOTONIC_SCHED_H

#include "qiran/qiran_types.h"
#include "qiran/svc/svc_fault.h"

/*
 * The calling framework for the stage control loops. The loops themselves are
 * implemented by their owners; what is here is when they are called and what
 * happens when one of them fails.
 *
 * Loops are not selected by the current state. Several run at once and outlast
 * the stage that started them: the thermal loop continues autonomously once the
 * source is at its operating point, and both interferometer phase locks must
 * stay active through detector enable and the whole experimental run. A loop
 * therefore runs from when it is enabled until it is disabled, and the current
 * state has no say in it.
 */
typedef enum {
    PHOTONIC_LOOP_LASER_TEC = 0,
    PHOTONIC_LOOP_MRR,
    PHOTONIC_LOOP_CROW,
    PHOTONIC_LOOP_UMZI,
    PHOTONIC_LOOP_DLI1,
    PHOTONIC_LOOP_DLI2,
    PHOTONIC_LOOP_COUNT
} photonic_loop_id_t;

/*
 * Called once per minor cycle while enabled. Must return promptly and must not
 * block: it runs inside a twenty millisecond budget shared with every other
 * task, and a loop that waits on a device spends that budget for everyone.
 */
typedef qiran_status_t (*photonic_loop_fn_t)(void *ctx);

typedef struct {
    uint32_t runs;
    uint32_t failures;
    uint32_t consecutive_failures;
    uint32_t cycles_last;
    uint32_t cycles_worst;
    bool     enabled;
    bool     registered;
} photonic_loop_stat_t;

void photonic_sched_init(void);

qiran_status_t photonic_sched_register(photonic_loop_id_t id,
                                       const char *name,
                                       photonic_loop_fn_t service,
                                       void *ctx,
                                       qiran_fault_id_t fault);

qiran_status_t photonic_sched_enable(photonic_loop_id_t id);
void           photonic_sched_disable(photonic_loop_id_t id);
void           photonic_sched_disable_all(void);
bool           photonic_sched_enabled(photonic_loop_id_t id);

/* Services every enabled loop, in a fixed order. */
void control_loop_service_calls(void);

const char *photonic_loop_name(photonic_loop_id_t id);
void photonic_loop_stat_get(photonic_loop_id_t id, photonic_loop_stat_t *out);
uint32_t photonic_sched_enabled_count(void);

#endif
