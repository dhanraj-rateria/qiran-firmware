# Implementation Decision Record

Decisions taken while implementing that depart from, or resolve a gap in, the
Execution Specification's reference material. Feeds the project Decision Log.

---

## FW-01 — Tick handover uses a counter pair, not a boolean flag

**Reference skeleton:** `volatile bool s_minor_tick_flag`, set by the ISR,
tested and cleared by `exec_wait_for_minor_tick()`.

**Problem:** two defects.

1. *Lost ticks.* Test-then-clear on a flag the ISR also writes is a
   read-modify-write race. A tick arriving between the test and the clear is
   erased, and the loop silently runs a cycle late forever after.
2. *Silent overrun.* A boolean cannot distinguish "one tick due" from "three
   ticks due". If a loop body exceeds 20 ms, the flag is already set when the
   loop comes back round, so `wait_for_minor_tick()` returns instantly and the
   overrun is invisible. For a design whose central deliverable is a
   schedulability result, an undetectable deadline miss is the worst possible
   failure mode.

**Decision:** the ISR only ever *increments* a `uint32_t` counter. The loop
keeps its own mirror and advances it by unsigned difference. Nothing shared is
ever cleared, so no tick can be lost, and the difference *is* the overrun
measurement. Aligned 32-bit accesses are single-copy atomic on the target, so
the loop collects a tick with no critical section and no interrupt masking.
Counter wrap is handled by unsigned subtraction and is unit tested.

**Overrun policy:** missed cycles are dropped, not replayed. Replaying compounds
an overrun into a cascade; dropping keeps the task set phase locked to real
time. Every dropped cycle is counted and reported.

---

## FW-02 — Time base is brought up before boot, not after

**Reference skeleton:** `boot_and_init(); exec_init();`

**Problem:** boot has hard budgets — bootloader, self test, device
initialisation, link establishment — and under that ordering there is no time
source running while boot executes, so none of those budgets can be enforced or
measured.

**Decision:** `plat_early_init()` brings up the cycle counter, interrupt
controller, tick source and time service, and starts the tick, before
`boot_and_init()` runs. `svc_time` therefore reads the *raw* interrupt-side tick
counter rather than the loop-consumed count, so it advances during boot when the
cyclic loop does not yet exist.

**Consequence:** boot legitimately accumulates a tick backlog. `exec_resync()`
absorbs it once, before the loop starts, so a 12 s device-initialisation phase is
not charged as a 600-cycle overrun.

---

## FW-03 — Executive measures its own loop body time

Not in the reference material. The executive timestamps each loop body with the
cycle counter and retains last and worst-case values.

**Justification:** the schedulability check requires the summed worst-case
execution time of the minor-cycle task list. Measuring the assembled body
directly gives that number under real integrated contention, continuously and in
flight, rather than as a sum of per-module bench figures taken in isolation. It
costs two register reads per cycle.

---

## FW-04 — Slot index exposed for lower-rate task spreading

Not in the reference material. `exec_slot()` returns position 0-24 within the
major cycle, phase locked to elapsed time across overruns.

**Justification:** the schedulability check anticipates that tasks which do not
need every-cycle execution must be spread across distinct minor-cycle slots if
the budget does not close. Providing the slot index now costs one counter and
means that redesign is a change of call site rather than a change of executive.

---

## FW-05 — Interrupt priority ladder fixed at the controller

Not in the reference material, which fixes the interrupt *set* but not relative
priority.

**Decision:** tick highest, watchdog next, then DMA, SpaceWire, UART, PL error.
The tick is highest because every deadline is referenced to it and late delivery
appears as jitter no later task can recover. The watchdog backstop must not be
starved by data-path traffic. Remaining sources are ordered by how quickly the
source overflows if unserviced.

---

## FW-06 — Watchdog device is registered, not assumed

Which timer issues the processor reset is an open hardware-design item. The
health gate is implemented in `svc_watchdog`; the device sits behind a registered
backend. With no backend registered the gate runs and is observable but nothing
is armed, which is the intended state during executive bring-up. Closing the
open item is a backend registration, not a change to the gate.

---

## FW-07 — One reviewed handler body, not five hand-written ones

**Reference material:** "Implement each ISR from Section 7.3's table as its own
module, GIC-registered."

**Decision:** five of the six sources share one handler body in `plat_irq.c`.
Each source keeps its own event queue, statistics and acknowledge hook, but the
sequence acknowledge, timestamp, publish, return is written once.

**Justification.** The interrupt-context rules are a property that has to hold
for *every* handler. Written five times, compliance is five separate review
findings that can each regress independently. Written once, it is structural:
there is no per-source code in which a blocking call or a floating-point
operation could appear. The handler also has no loop, so bounded execution is by
inspection rather than by argument. Per-source duration measurement, queue
overflow counting and sequence-gap detection come out of the same single
implementation instead of being reimplemented or, more likely, omitted.

The interrupt set stays closed: `PLAT_IRQ_COUNT` is asserted equal to six at
compile time, so a seventh source cannot be added without breaking the build.
Relative priority is chosen by the framework from the source identity rather
than passed in at the call site, so the ordering cannot be set inconsistently.

**Cost.** One indirect call per interrupt, and the interrupt-context rules become
a contract on the acknowledge hooks rather than on code this task owns. That
contract is written down in `docs/isr_review.md` and requires per-hook sign-off.

---

## FW-08 — Frame assembly is not done in interrupt context

**Reference material:** the serial receive interrupt should "store byte in ring
buffer, set flag on complete frame."

**Problem:** detecting a complete frame requires knowing the frame format. The
command frame format is not yet defined. Worse, content-driven parsing makes
handler duration a function of what arrives, which is exactly the property that
makes an interrupt's worst case hard to bound and hard to defend.

**Decision:** the interrupt pushes bytes into a ring and publishes a
content-independent inter-frame idle gap as an event, using the device's receive
timeout condition. The communication layer assembles frames from the ring in the
cyclic loop, where an unbounded parse costs loop budget that is already measured
rather than interrupt latency that is not.

**Consequence:** framing works out to be independent of the eventual frame
format, so fixing the format later does not touch interrupt code.

---

## Additions not named in the module architecture

These modules are not in the layer table and were added as implementation
necessities. Recorded so the table can be updated at the next revision.

| Module | Layer | Why |
|---|---|---|
`plat_cpu.c` | 1 | Cycle counter, barriers, critical sections. Also the seam that makes upper layers host-testable. |
`plat_irq.c` | 1 | The interrupt framework. See FW-07. |
`plat_isr_uart.c` | 1 | Receive ring ownership. See FW-08. |
`svc_ring.c` | 3 | Single-producer single-consumer ring, shared by the interrupt paths. |
`svc_health.c` | 3 | Aggregate health verdict the watchdog gate reads. |
`svc_watchdog.c` | 3 | The watchdog gate. See FW-06. |
`exec_major.c` | 2 | The major-cycle task list. |
`photonic_sched.c` | 5 | The calling framework for stage modules owned by others. |
`comm_process.c` | 9 | Communication entry point called from the loop. |
`data_path.c` | 10 | Data-path entry point called from the loop. |

`photonic_crow1/2` and `photonic_dli1/2` are taken as two files each, matching
the two physical banks and the two receiver channels.

---

## Open items carried in code

Every provisional value is tagged `OPEN:` in `include/qiran/qiran_config.h`.

    grep -rn "OPEN:" include src

Currently: watchdog timeout in minor cycles, and the link-establishment budget
treated as bounded within the device-initialisation window.
