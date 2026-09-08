# Task and Execution Time Table

The living table required as a deliverable is maintained by the firmware itself
rather than by hand. Every task in the cyclic loop is measured on target on
every execution, and the schedulability check is computed from those
measurements. What follows is how to read it and how to act on it.

## Where the numbers come from

`exec_sched.c` holds one entry per task: name, cycle class, placement, and the
budget from the requirements documents. `EXEC_RUN` in the main loop brackets each
call with a cycle-counter read, so runs, last, mean and worst are collected
continuously, including in flight.

The call order is still written out literally in `main()`. Nothing here decides
what runs next.

## Reading the trace

    task                cls  runs      last    mean    worst  (us)
    state_machine         0  1250         0       0        0
    ...
    sched worst_slot=0 used=118us budget=20000us margin=19882us fits=1 complete=1

`cls` is 0 for every minor cycle, 1 for once per major cycle, 2 for spread.
`complete=1` means every task due every cycle has run at least once, so the
figures are real rather than partial. `fits=0` is a design problem, not an
implementation one, and needs acting on before more code is added.

## Why the check is a maximum, not a total

The check sums, for each of the twenty-five slots, only the tasks actually due
in that slot, and reports the largest. Summing every task instead would assume
spread tasks always coincide, which is the very thing spreading exists to
prevent: under a naive total, moving a task to a quieter slot would change
nothing, and the documented remedy for a failed check would be unmeasurable.

`worst_slot` names the slot that costs most, which is the slot to relieve.

## When the check fails

1. Read the table for that run and find the task with the largest worst case.
2. Decide whether it genuinely needs to run every minor cycle.
3. If not, change its entry to the spread class with a period and a phase, and
   place it on a slot the check reports as cheap.
4. Re-run and confirm `worst_slot_cycles` has dropped.

Placement is data in one table, so this is an edit to one line, not a change to
the executive.

## Status

**No requirements-doc estimate exists for any task in this table.** The source
documents budget the mission stages in seconds; they give no per-task figure for
the executive's own task list, and the timing diagram's estimate column is empty
for every row. The estimate-based first-pass check is therefore vacuous, and
saying so is more useful than filling the column with invented numbers.

The measured check is real and runs continuously. Stage tasks will carry genuine
estimates when they are integrated, since those do have documented budgets.

## Figures still to be collected on hardware

Nothing in this table has been measured on the target yet. Every figure arrives
on the first run. Until then the table is structure, not data.

Also outstanding: interrupt handler durations, which `plat_irq_stats_get()` and
`plat_timer_isr_cycles()` collect by the same means, and the boot step durations
in `plat_boot_report()`.
