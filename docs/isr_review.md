# Interrupt Context Review

Standing review of the fixed interrupt set against the interrupt-context rules:
acknowledge, snapshot or timestamp, then flag-set or ring-buffer push, and
return. No blocking call, no floating point, bounded execution.

Re-run this review whenever a source is added or an acknowledge hook is
supplied. The set is closed by a compile-time assertion in
`include/qiran/plat/plat_irq.h`: adding a seventh source breaks the build.

## The six sources

| Source | Handler | Acknowledge hook | State |
|---|---|---|---|
Timer tick | `timer_isr`, `plat_timer.c` | device call, inline | **Implemented** |
Watchdog expiry | shared handler, `plat_irq.c` | not yet supplied | Framework ready |
DMA complete | shared handler | not yet supplied | Framework ready |
SpaceWire RX / link error | shared handler | not yet supplied | Framework ready |
UART / RS485 RX | shared handler via `plat_isr_uart.c` | not yet supplied | Framework ready |
PL error | shared handler | not yet supplied | Framework ready |

Five of the six run one reviewed handler body rather than five separate ones,
so the rules below hold structurally for all of them at once. What remains
per-source is the acknowledge hook, which belongs to the device owner.

## Review: `timer_isr` (plat_timer.c)

| Rule | Result |
|---|---|
Acknowledge | `XScuTimer_ClearInterruptStatus`, a single register write |
Work performed | one counter increment, via `exec_on_minor_tick()` |
Blocking calls | none |
Floating point | none |
Bounded | no loop, no branch on data; constant instruction count |
Duration | measured on target, retrievable via `plat_timer_isr_cycles()` |

**Pass.** Two cycle-counter reads were added for the duration measurement; they
are register reads with no ordering or memory effect.

## Review: shared handler `irq_handler` (plat_irq.c)

| Rule | Result |
|---|---|
Acknowledge | delegated to the registered hook, called exactly once |
Snapshot | `exec_raw_tick_count()`, a single volatile read |
Publish | one struct assignment into a pre-allocated slot, then a barrier and one index store |
Blocking calls | none in the handler body |
Floating point | none |
Bounded | no loop in the handler body; constant instruction count |
Full queue | counted in `dropped`, never blocks and never overwrites unread data |
Duration | measured per source, retrievable via `plat_irq_stats_get()` |

**Pass, conditional on the acknowledge hooks.**

## Contract imposed on acknowledge hooks

The hook is called in interrupt context. It must acknowledge at the device,
return a status snapshot, and nothing else. Specifically it must not:

- block or spin on a device flag with no bound
- perform an I2C or flash transaction
- perform floating-point arithmetic
- call into the mission, communication or data layers
- take a lock, or disable interrupts other than briefly at register level

Where a hook must move data, it moves it into a ring supplied by the framework
and its loop must be bounded by a fixed device depth, not by content.

**Sign-off required per hook when supplied.** Add a row here at that point.

## Known non-constant case

The serial receive hook drains the device receive FIFO, so its duration scales
with the number of bytes waiting. This is bounded by the hardware FIFO depth,
and each byte costs one ring push with no branch on content. It is bounded but
not constant-time, which is acceptable and is recorded here rather than assumed
away. Its measured worst case appears in `plat_irq_stats_get(PLAT_IRQ_UART)`.

## Reporting a fault from interrupt context

`svc_fdir_report()` and the `svc_log_*` functions are callable from an interrupt.
Both take a brief interrupt-disabled section, a handful of stores wide, because
several interrupts as well as the loop can write the same counter and that is
more than one producer. Neither performs delivery, allocation, or any device
access. See decision FW-10 for why delivery is deliberately left to the loop.

## Deliberate omission

Frame assembly is not done in interrupt context. See decision FW-08.
