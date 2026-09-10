#ifndef QIRAN_CONFIG_H
#define QIRAN_CONFIG_H

#include "qiran/qiran_types.h"

/*
 * Single point of change for every tunable constant in the flight software.
 * Values tagged OPEN: are working defaults that must be confirmed or revised
 * before flight configuration lock; grep for "OPEN:" to enumerate them.
 */

/*
 * Bring-up tracing prints one executive summary line per major cycle over the
 * debug UART. It is a bring-up and bench-measurement aid only and must be off
 * for flight builds, since the print itself consumes minor-cycle budget.
 */
#if !defined(QIRAN_BRINGUP_TRACE)
#define QIRAN_BRINGUP_TRACE 1
#endif

/*
 * Execution-time instrumentation: per-task and per-interrupt timing. Needed to
 * characterise the schedule on the bench and to produce the timing evidence,
 * and off for flight, where the measurement itself is the only thing it costs.
 */
#if !defined(QIRAN_INSTRUMENT)
#define QIRAN_INSTRUMENT 1
#endif

/* Major cycles between full task-table dumps in the bring-up trace. */
#if !defined(QIRAN_TRACE_TABLE_PERIOD)
#define QIRAN_TRACE_TABLE_PERIOD 20U
#endif

/* --- Executive timing base --- */

#define QIRAN_MINOR_CYCLE_MS        20U
#define QIRAN_MINOR_PER_MAJOR       25U
#define QIRAN_MAJOR_CYCLE_MS        (QIRAN_MINOR_CYCLE_MS * QIRAN_MINOR_PER_MAJOR)

QIRAN_STATIC_ASSERT(QIRAN_MAJOR_CYCLE_MS == 500U, major_cycle_is_500ms);

/* --- Mission timing envelope --- */

#define QIRAN_MISSION_WINDOW_MS     600000U
#define QIRAN_SETUP_BUDGET_MS       250000U
#define QIRAN_OPERATIONS_BUDGET_MS  250000U

QIRAN_STATIC_ASSERT(
    (QIRAN_SETUP_BUDGET_MS + QIRAN_OPERATIONS_BUDGET_MS) < QIRAN_MISSION_WINDOW_MS,
    mission_budget_leaves_retry_margin);

/* --- Boot budgets --- */

#define QIRAN_BOOTLOADER_BUDGET_MS  3000U
#define QIRAN_POST_BUDGET_MS        5000U
#define QIRAN_DEVINIT_BUDGET_MS     12000U

/*
 * OPEN: link establishment has no confirmed dedicated budget. It is treated as
 * bounded within the device-initialisation window rather than additive to it.
 */
#define QIRAN_LINK_BUDGET_MS        QIRAN_DEVINIT_BUDGET_MS

/* --- Interrupt framework --- */

/*
 * Events buffered per interrupt source between the interrupt and the loop that
 * consumes them. Must be a power of two. One minor cycle's worth is the design
 * intent; overflow is counted per source rather than assumed impossible, so the
 * depth can be raised against measured evidence instead of guesswork.
 */
#define QIRAN_IRQ_QUEUE_DEPTH   8U

QIRAN_STATIC_ASSERT((QIRAN_IRQ_QUEUE_DEPTH & (QIRAN_IRQ_QUEUE_DEPTH - 1U)) == 0U,
                    irq_queue_depth_is_power_of_two);

/* Receive bytes buffered by the serial interrupt ahead of frame parsing. */
#define QIRAN_UART_RX_RING_BYTES 256U

QIRAN_STATIC_ASSERT((QIRAN_UART_RX_RING_BYTES &
                     (QIRAN_UART_RX_RING_BYTES - 1U)) == 0U,
                    uart_rx_ring_is_power_of_two);

/* --- Fault log --- */

/*
 * Records held in RAM before a flush to non-volatile storage. Must be a power
 * of two. When full the newest record is dropped rather than the oldest being
 * overwritten, because the first fault in a cascade is usually the root cause
 * and later ones its consequences; the drop count preserves how much was lost.
 */
#define QIRAN_LOG_DEPTH 128U

QIRAN_STATIC_ASSERT((QIRAN_LOG_DEPTH & (QIRAN_LOG_DEPTH - 1U)) == 0U,
                    log_depth_is_power_of_two);

/* --- Global fault-management limits --- */

#define QIRAN_STAGE_RETRY_LIMIT     3U
#define QIRAN_RESET_REQUEST_LIMIT   3U
#define QIRAN_REENTRY_LIMIT         5U

/*
 * OPEN: hardware watchdog timeout expressed in minor cycles. Wide enough to
 * tolerate one missed cycle without false-triggering, tight enough to catch a
 * genuine hang early relative to the mission window. The reset-trigger and
 * reset-completion counts derived from it are provisional on the same basis.
 */
#define QIRAN_WATCHDOG_TIMEOUT_MINOR_CYCLES  5U
#define QIRAN_WATCHDOG_TIMEOUT_MS \
    (QIRAN_WATCHDOG_TIMEOUT_MINOR_CYCLES * QIRAN_MINOR_CYCLE_MS)

QIRAN_STATIC_ASSERT(QIRAN_WATCHDOG_TIMEOUT_MINOR_CYCLES > 1U,
                    watchdog_tolerates_one_missed_cycle);

#endif
