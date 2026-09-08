#ifndef QIRAN_MISSION_STATE_H
#define QIRAN_MISSION_STATE_H

#include "qiran/qiran_types.h"
#include "qiran/svc/svc_fault.h"

/*
 * The complete state model. There is no safe or fault state: every failure
 * either returns to an earlier stage or ends the run where it stands with a
 * diagnostic report. Adding a state breaks the count assertion in the
 * implementation, which is deliberate.
 */
typedef enum {
    SPR_BOOT = 0,
    SPR_PRECOND,
    SPR_LASER_BRINGUP,
    SPR_MRR_TUNE,
    SPR_CROW_TUNE,
    SPR_UMZI_TUNE,
    SPR_DLI_LOCK,
    SPR_SPAD_ENABLE,
    SPR_PVS_T1,
    SPR_PVS_T2,
    SPR_PVS_T3,
    SPR_EXPERIMENT,
    SPR_PROCESS,
    SPR_DATA_HANDLING,
    SPR_CAL,
    SPR_COUNT
} mission_state_id_t;

typedef struct {
    uint32_t tick;
    uint8_t  from;
    uint8_t  to;
    uint8_t  reentry;
    uint8_t  reserved;
} mission_history_t;

#define MISSION_HISTORY_DEPTH 16U

void mission_state_init(void);

/*
 * Enforces the time a state is permitted to occupy where the requirements give
 * one. It does not decide transitions: the stage that owns a condition asks for
 * the transition, and this module decides only whether that is allowed.
 */
void state_machine_update(void);

mission_state_id_t mission_state_current(void);
const char        *mission_state_name(mission_state_id_t state);
uint32_t           mission_state_elapsed_ms(void);
uint32_t           mission_state_entries(mission_state_id_t state);

/* True only for a transition the state model actually contains. */
bool mission_state_transition_legal(mission_state_id_t from,
                                    mission_state_id_t to);

/*
 * Asks to move to a state. Refused, reported and counted if the model does not
 * contain that transition, so an illegal request is a detected fault rather
 * than an undefined state change.
 */
qiran_status_t mission_state_request(mission_state_id_t to);

/*
 * Ends the run in place. Nothing is suspended and no state is entered: the
 * current state is held and further transitions are refused, which is what
 * replaces entry to a safe state.
 */
void mission_state_terminate(qiran_fault_id_t cause);
bool mission_state_terminated(void);
qiran_fault_id_t mission_state_terminal_cause(void);

qiran_payload_status_t mission_payload_status(void);

/*
 * Set by whoever changes the hardware, not inferred from a transition: whether
 * detector bias is actually applied is a property of the hardware, and some
 * re-entries deliberately leave it applied.
 */
void mission_payload_status_set(qiran_payload_status_t status);

uint32_t mission_state_reentries(void);
uint32_t mission_state_history(mission_history_t *out, uint32_t max);
uint32_t mission_state_rejections(void);

#endif
