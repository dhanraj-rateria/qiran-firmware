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

## FW-09 — Four severity tiers, not five

The severity scheme names two kinds of Severe fault: one where the processor
keeps executing, and one where sequential execution is not guaranteed. Only the
first is a reportable severity here.

**Reasoning:** software cannot report the second kind. Software reporting a
fault is proof that software is still running, which is precisely what that case
denies. It is detected by the watchdog going unserviced and by the observer's
absence-of-communication timeout, neither of which is a call into this module.

So `qiran_severity_t` has four values, and the second kind appears in the design
as the watchdog gate in `svc_watchdog.c` plus the observer's own supervision. A
watchdog expiry that is somehow survived long enough to be seen is reported as
its own fault class.

---

## FW-10 — Escalation is raised in interrupt context, delivered in the loop

**Conflict in the requirements.** The fault-handling requirement says the
interrupt service routine shall itself raise the power-reset request to the
observer. The executive requirement says no interrupt service routine shall
perform blocking calls. Raising the request means a link transaction, so the two
cannot both be met literally.

**Decision:** `svc_fdir_report()` is interrupt-safe and does no delivery. It
records the occurrence and returns. `error_handling_service()`, which runs every
minor cycle, performs the classification and the delivery.

**Why this satisfies both.** The severe tier requires remedial action within the
next execution cycle. The loop runs every 20 ms, so a fault reported by an
interrupt is escalated in the very next cycle, which is the stated bound. The
interrupt-context rule is kept intact, and no interrupt ever waits on a link.

---

## FW-11 — One log entry per fault class per service pass

A fault present every cycle would otherwise write 50 log records a second, and
the service's execution time would scale with fault rate, which is exactly what
must not happen to a task inside a fixed cycle budget.

Each pass writes at most one record per class, carrying that pass's occurrence
count. Worst-case work per pass is therefore bounded by the number of fault
classes, a compile-time constant, not by how badly things are going. Every tier
still logs, and the cumulative occurrence count per class is exact.

---

## FW-12 — Fault tier does not drive the health verdict

The watchdog is serviced only while the health verdict is healthy. It would be
easy to wire a severe fault into that verdict. That would be wrong.

A severe fault where the processor still executes requires a power-reset request
followed by **continued normal execution**. Marking health failed would stop the
watchdog being serviced and so trigger a processor reset as well, which is a
second, unrequested recovery action on top of the one specified.

So the health verdict answers "can this processor keep executing correctly",
which is the health monitor's question, and the fault tier answers "how bad is
this payload condition". They stay separate.

---

## FW-13 — Boot retries the failed step in place, not by resetting the processor

**Requirement:** on failure of boot, self test or device initialisation, issue a
reset request and re-attempt the failed step, with no more than three reset
requests before requesting a power cycle.

**The ambiguity.** "Issue a reset request and re-attempt the failed step" reads
two ways. If the request is a processor reset, execution restarts and the whole
sequence repeats, which is not really re-attempting one step. If it is a reset
of the failing subsystem, the step is genuinely re-attempted in place.

**Decision: in place.** Three processor resets would repeat the entire sequence
each time, costing up to three times the full boot budget. The mission timing
model allocates no time for that, and it would eat most of the retry margin
before the first stage has run. Retrying only the failed step costs only that
step.

**OPEN for systems engineering.** The attempt count and the escalation are
identical under either reading; only where execution resumes differs. If the
processor-reset reading is intended, the count has to survive a warm reset,
which needs a persistent register whose bit allocation must be agreed with
whoever owns the boot image.

---

## FW-14 — Boot retry counters are the same Critical counters

The boot-step fault classes carry the reset-request limit as their Critical-tier
limit, exactly as the lock-acquisition classes carry the stage retry limit. So
the boot sequencer holds no counter of its own: it reports the fault, and
reaching the limit is what requests the power cycle.

Boot calls the fault service directly after reporting, because the cyclic loop
that would normally service it is not yet running.

**Correction to the earlier policy.** The boot classes were initially Severe with
no tolerance, which would have escalated on the first failure and allowed no
retries at all. They are Critical with a limit of three.

A step whose class has no tolerance is escalated once and **not** retried. Without
that check the sequencer would repeat such a step for ever, since a limit of zero
can never be reached.

---

## FW-15 — No safe-output actions registered is a failure, not a pass

Safe outputs are applied by actions registered by whoever owns the drive path,
because this module must not know how to reach a converter. That leaves the case
of nothing registered, and treating it as success would mean a boot that reports
the actuators safe without having touched one.

It returns a failure instead. On a bench with no drive paths yet implemented,
boot therefore ends terminal and says so, which is the truthful answer. The
cyclic loop still runs, so executive bring-up is unaffected.

Registration has to happen before boot, which means safe-output actions must
reach their outputs without depending on device initialisation: they belong to
whoever owns the raw output lines, not to the device abstraction layer.

---

## FW-16 — A task table, without a dispatch table

The executive requirements forbid dynamic event dispatch and a priority queue,
and the reference skeleton notes that a dynamic dispatch table is deliberately
absent. A task table therefore needs justifying carefully.

**What was added:** a const table of task definitions, and per-task measurement
around each call.

**What was not added:** any means of reaching a task through it. There is no
function pointer in the table, nothing iterates it to decide what to run, and
the seven calls plus the major-cycle call remain written out literally and in
order in `main()`. The order is still the entire scheduling policy, and it is
still readable in one place as consecutive statements.

**Why it is needed.** The remedy for a failed schedulability check is to move
tasks that do not need every cycle onto separate slots. Identifying *which* task
requires per-task figures; the whole-loop-body measurement already in place
gives one number for eight tasks and cannot answer it. Holding placement as data
also makes that remedy an edit to one line rather than a change to the executive.

**Cost:** two cycle-counter reads per task, about a hundred and fifty cycles per
minor cycle against a budget of thirteen million. Left enabled in flight, since
worst-case execution time is exactly the kind of thing worth telemetering.

---

## FW-17 — The schedulability check is a maximum over slots

Summing every task's worst case would be the conservative bound, and it is
wrong for this design: it assumes spread tasks always coincide, so moving a task
to a quieter slot would not change the answer. That makes the documented remedy
for a failed check impossible to verify.

The check instead sums, per slot, only the tasks due in that slot, and reports
the largest along with which slot it was. Exact given the placement, and it
makes relieving the worst slot measurable.

---

## FW-18 — Parameters are integers in named units, not floating point

Every scalar parameter is an integer carrying its unit in its name: millidegrees
Celsius, microamps, microkelvin, milliradians, nanowatts.

**Why.** At the precision the requirements state, all of these quantities are
integral: a temperature band of a tenth of a degree, a stability of one and a
half millikelvin, a lock error of a hundred and forty milliradians. Integers
make comparisons exact and repeatable, remove any rounding difference between
builds, and leave no question about floating-point context around an interrupt.

Floating point stays appropriate in the data-processing path, where visibility,
the correlation bound and the coincidence ratios are computed. That is a
different module with different constraints.

---

## FW-19 — Limits live in the parameter table, not at the point of use

Each parameter carries a minimum and a maximum, checked on every write and on
every integrity pass.

The requirement that a commanded temperature must under no condition leave its
absolute range is the clearest case: expressed as a limit in the table it is
stated once and enforced everywhere, whereas expressed as a check in the stage
that commands it, it holds only where somebody remembered to write it.

Thresholds the requirements describe without giving a number are deliberately
**absent** from the table rather than filled with plausible values. The
resonator lock error threshold, the drop-port target range and the splitting
specification must arrive from their owners as reviewed numbers.

---

## FW-20 — The live parameter block is re-checked every major cycle

`svc_config_verify()` recomputes a checksum over the live values and re-checks
each against its limits, restoring from storage or from defaults on a mismatch.

A parameter corrupted in place, by an upset or by a stray write, would otherwise
be discovered only when it produced a bad command. The checksum catches a flip
that leaves the value legal, which a limits check alone cannot; the limits check
catches a value written by something that bypassed the setter, which a checksum
alone cannot. Both are needed, and both are cheap over twenty-seven values.

---

## FW-21 — Loading reports parameters and calibration separately

`svc_config_load()` returns the outcome for the parameter block; calibration
validity is a separate query.

They were briefly folded into one status, which was wrong. Boot treats a load
failure as a step failure, so a missing calibration curve would have taken the
retries, requested a power cycle, and ended the run terminal. Without parameters
nothing can be commanded safely; without calibration the payload still runs and
only the comparisons needing the curve are unavailable. Different consequences
need different answers. Both are still reported as faults.

The load status distinguishes intact-and-legal, intact-with-some-values-fallen-
back-to-default, and unusable, so the caller can decide what is fatal rather
than being told only that something went wrong.

---

## FW-22 — The calibration curve refuses to extrapolate

Asking the curve for a current outside its calibrated span returns a range
error rather than an extrapolated figure. A measured power is compared against
this to a stated tolerance; an extrapolated expectation would make that
comparison look meaningful when it is not supported by any measurement.

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

Currently: watchdog timeout in minor cycles, the link-establishment budget
treated as bounded within the device-initialisation window, and the escalation
action for lock-acquisition retry exhaustion.

### Two questions for systems engineering

**Does stage retry exhaustion request a power reset, or only report a flag?**
The capability requirements say a stage that exhausts its retries raises an
error flag to the observer. The error-handling reconciliation says that flag
*is* the elevation from Critical to Severe, and the Severe response is a
power-reset request. Read strictly, one stubborn stage then costs a full reset
cycle out of the operating window. The policy table currently reports a flag
without requesting a reset. Confirm which is intended.

**Is a boot reset request a processor reset, or a reset of the failing
subsystem?** See FW-13. This one has a schedule consequence, not just a design
one, so it is worth closing early.

**Is the Critical threshold counted in occurrences or in execution cycles?**
The severity table says both in different sentences. Occurrences is implemented,
because the per-stage retry limit it has to equal is plainly a count of
attempts. For a fault that recurs every cycle the two coincide, so this only
matters for an intermittent fault.
