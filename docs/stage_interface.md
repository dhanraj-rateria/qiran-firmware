# Stage and Control Loop Interface

What a stage owner has to supply, and what the framework guarantees in return.
This is the contract to agree before writing stage code, not after.

## Two different things

A **stage step** answers "is this stage finished". It is called only while its
state is current, and it drives the sequence forward.

A **control loop** holds something steady. It is called from when it is enabled
until it is disabled, and it does not care which state is current. The two are
separate because they have different lifetimes: the thermal loop keeps running
long after laser bring-up has finished, and both interferometer phase locks must
stay active through detector enable and the whole experimental run. A design
that serviced loops by current state would drop them at the moment they matter
most.

## Stage step

    void step(void *ctx, stage_outcome_t *out);

Called once per minor cycle while its state is current. Set `out->result` to one
of:

| Result | Meaning |
|---|---|
`STAGE_BUSY` | Not finished. Called again next cycle. Keep your own progress. |
`STAGE_PASS` | Criteria met. The sequencer advances along the nominal path and clears this stage's retry count. |
`STAGE_RETRY` | This attempt failed. Counted against the stage's fault class; the stage re-enters itself where the model has a transition for it. |
`STAGE_REDIRECT` | Go to `out->target` instead. Used for the interlock failures, which each name a different stage to return to. |
`STAGE_FAIL` | Cannot proceed. Stops the stage being driven without spending retries. |

Optionally set `out->fault` to the specific fault class you hit, and
`out->detail` to a measured value worth logging. Leaving the fault as
`QIRAN_FAULT_NONE` uses the stage's own class.

### Rules

**Must not block.** The step shares a twenty millisecond budget with every other
task in the loop. Waiting on a device, a conversion or a settling time spends
that budget for everyone and will show up as a missed deadline. Anything that
takes longer than a cycle returns `STAGE_BUSY` and continues next time.

**Must not change the state itself.** Ask, do not act. A step that names a
destination the state model does not contain is refused, reported and stalls the
stage; it does not happen anyway.

**Must not implement its own retry counter.** The fault class carries the retry
limit. Report the failure and the framework counts it, escalates when the limit
is reached, and reports the flag to the observer.

**Read setpoints and thresholds from the configuration store.** They are range
checked there and adjustable from the ground; a constant compiled into a stage
is neither.

## Control loop

    qiran_status_t service(void *ctx);

Called once per minor cycle while enabled. Return `QIRAN_OK` when the iteration
did its work. Any other status is counted, reported against the loop's fault
class, and tracked as a consecutive-failure run; one bad iteration is not worth
acting on, the same one every cycle is.

Same no-blocking rule, for the same reason.

Register the loop, then enable it when its lock is acquired and disable it when
the lock is deliberately dropped. A loop that has not been registered cannot be
enabled, because that would report a lock as held by something that does not
exist.

## What the framework already does, so you do not

- Calls you at a fixed point in a fixed order, once per minor cycle.
- Measures your execution time, keeps the worst case, and feeds the
  schedulability result.
- Counts your failures, escalates them at the documented limit, and reports the
  flag to the observer.
- Refuses any state change the model does not contain.
- Holds the stage to the duration the requirements give it.
- Counts returns to an earlier stage against the global re-entry allowance.

## Currently registered

Stage zero only, which is integration glue rather than an algorithm. It calls
two checks that the board services must supply:

    mission_precond_checks_t { dss_health, qss_comms }

With neither supplied the precondition is not declared met. An absent check is
not a passed check.

No control loop is registered yet.
