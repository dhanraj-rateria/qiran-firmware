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

## Open items carried in code

Every provisional value is tagged `OPEN:` in `include/qiran/qiran_config.h`.

    grep -rn "OPEN:" include src

Currently: watchdog timeout in minor cycles, and the link-establishment budget
treated as bounded within the device-initialisation window.
