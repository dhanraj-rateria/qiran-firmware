# MQIRAN Payload Flight Software

Bare-metal flight software for the payload controller (Zynq-7045, dual Cortex-A9,
no RTOS). Sequential cyclic executive: 20 ms minor cycle, 500 ms major cycle.

## Layout

    include/qiran/<layer>/   public headers, one per module
    src/<layer>/             implementations
    src/main.c               entry point and the cyclic loop
    test/host/               host unit tests for hardware-independent logic
    test/shim/               fake BSP headers, for compile checking only
    docs/                    design decisions taken during implementation

Layers: `plat` `exec` `svc` `mission` `photonic` `board` `dal` `hal` `comm` `data`.

## Host verification

    make test      run the unit tests
    make syntax    compile every source for cortex-a9 against the fake BSP
    make check     both

`make syntax` is a compile check, not a functional build: `test/shim` contains
placeholder declarations, not a real BSP. It exists to catch errors without a
Vitis round trip. It never produces a flashable image.

## Vitis project setup

1. Create a platform from the payload board's `.xsa` (not a generic eval-kit
   preset), targeting `ps7_cortexa9_0`, standalone BSP.
2. Create an application project on that platform, empty C application.
3. Point the application's sources at this repository:
   either add `src` as a linked source folder, or clone this repo inside the
   application's `src`. Vitis compiles every `.c` beneath it, recursively.
4. Add one include path: this repository's `include` directory.
   C/C++ Build Settings, ARM v7 gcc compiler, Directories.
5. Exclude `test/` from the build if it sits inside the source tree.
6. Build, then run over JTAG with the debug UART open at 115200 8N1.

If your Vitis version resolves devices by base address rather than device ID,
`plat_gic.c` and `plat_timer.c` already handle both; nothing to change.

## Executive bring-up verification

Expected UART output, one line per major cycle:

    boot=4 outcome=2 failed=safe_outputs total=0ms
      timer_clk=333333343Hz load=6666665 budget=20000us safe_out=0 post=1 dev=0
    t=25 maj=1 body=<n>us peak=<n>us budget=20000us ovr=0 lost=0 wdt=0
      isr timer=<n>c
      fdir sev=0/0/0/0 esc_undeliv=0 log=0/0 reentry=0

The second line reports measured handler durations in CPU cycles. Only the tick
appears until a device supplies an acknowledge hook for the other sources; each
one then adds `name=taken/dropped/worst`.

A terminal boot outcome is expected until a safe-output action is registered:
the payload cannot claim its actuators are safe when no drive path exists yet.
The cyclic loop runs regardless, so executive bring-up is unaffected by it.

The schedulability line is the one that matters as tasks are added: `used`
against `budget`, and `fits=0` is a design problem to act on before writing more
code. `complete=1` means every task due every cycle has run, so the figures are
real. A full task table follows every twentieth major cycle. See `docs/wcet.md`.

The fault line reports counts by tier, lowest first, then escalations that
could not be delivered because no uplink is registered, then logged faults and
dropped log records, then re-entries. All zeros is the expected steady state;
anything else on an idle bench is a real finding.

Check, in order:

1. `load` matches `timer_clk / 1000 * 20 - 1`. A wrong `timer_clk` is the one
   likely cause of a wrong tick period; override with `-DQIRAN_TIMER_CLK_HZ=`.
2. Tick period on a scope or logic analyser. Toggle a spare GPIO from the timer
   ISR, confirm 20 ms within tolerance.
3. `t` advances by exactly 25 between consecutive lines, and lines arrive every
   500 ms. Run for at least 10 minutes and confirm no drift and `ovr=0`.
4. `peak` is the measured worst-case loop body time. It is the schedulability
   number: it must stay below `budget` with margin as real tasks are added.

`wdt=0` is expected until a watchdog backend is registered; the gate logic runs
either way.

Set `-DQIRAN_BRINGUP_TRACE=0` for flight builds. The print itself costs
minor-cycle budget and must not be measured as if it were flight code.
