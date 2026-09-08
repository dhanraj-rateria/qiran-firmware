# Host-side verification only. The flight build is produced by Vitis; see README.
# make test   run the host unit tests
# make syntax compile-check every source against a fake BSP with the ARM compiler
# make check  both

BUILD    := build
INC      := -Iinclude

WARN     := -std=c99 -Wall -Wextra -Werror -Wshadow -Wconversion \
            -Wsign-conversion -Wpointer-arith -Wstrict-prototypes \
            -Wmissing-prototypes -Wcast-qual -Wundef

HOST_CC  := gcc
HOST_INC := $(INC) -Itest/host -D_POSIX_C_SOURCE=200809L

# Each binary compiles one module into its test translation unit so that
# file-static state can be preset and interrupt-context entry points invoked.
EXEC_SRC := test/host/test_exec.c src/svc/svc_time.c src/plat/plat_cpu.c
IRQ_SRC  := test/host/test_irq.c src/exec/exec_core.c src/plat/plat_cpu.c \
            src/plat/plat_gic.c src/plat/plat_isr_uart.c src/svc/svc_ring.c
FDIR_SRC := test/host/test_fdir.c src/exec/exec_core.c src/plat/plat_cpu.c \
            src/plat/plat_gic.c src/plat/plat_isr_uart.c src/svc/svc_ring.c \
            src/svc/svc_log.c
BOOT_SRC := test/host/test_boot.c src/exec/exec_core.c src/plat/plat_cpu.c \
            src/plat/plat_gic.c src/plat/plat_irq.c src/plat/plat_isr_uart.c \
            src/plat/plat_boot.c src/plat/plat_timer.c \
            src/plat/plat_devinit.c src/plat/plat_post.c \
            src/plat/plat_safe_outputs.c src/svc/svc_fdir.c src/svc/svc_log.c \
            src/svc/svc_ring.c src/svc/svc_time.c
SCHED_SRC := test/host/test_sched.c src/exec/exec_core.c src/plat/plat_cpu.c
CFG_SRC  := test/host/test_config.c src/exec/exec_core.c src/plat/plat_cpu.c \
            src/plat/plat_gic.c src/plat/plat_irq.c src/plat/plat_isr_uart.c \
            src/svc/svc_crc.c src/svc/svc_fdir.c src/svc/svc_log.c \
            src/svc/svc_ring.c
HOST_BIN := $(BUILD)/test_exec $(BUILD)/test_irq $(BUILD)/test_fdir \
            $(BUILD)/test_boot $(BUILD)/test_sched $(BUILD)/test_config

ARM_CC   := arm-none-eabi-gcc
ARM_SRC  := $(shell find src -name '*.c')
ARM_INC  := $(INC) -Itest/shim
ARM_FLAGS:= -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -ffreestanding

.PHONY: check test syntax clean

check: test syntax

test: $(HOST_BIN)
	@for b in $(HOST_BIN); do echo "== $$b"; $$b || exit 1; done

$(BUILD)/test_exec: $(EXEC_SRC) | $(BUILD)
	@$(HOST_CC) $(WARN) -O1 -g $(HOST_INC) $(EXEC_SRC) -o $@

$(BUILD)/test_irq: $(IRQ_SRC) | $(BUILD)
	@$(HOST_CC) $(WARN) -O1 -g $(HOST_INC) $(IRQ_SRC) -o $@

$(BUILD)/test_fdir: $(FDIR_SRC) | $(BUILD)
	@$(HOST_CC) $(WARN) -O1 -g $(HOST_INC) $(FDIR_SRC) -o $@

$(BUILD)/test_boot: $(BOOT_SRC) | $(BUILD)
	@$(HOST_CC) $(WARN) -O1 -g $(HOST_INC) $(BOOT_SRC) -o $@

$(BUILD)/test_sched: $(SCHED_SRC) | $(BUILD)
	@$(HOST_CC) $(WARN) -O1 -g $(HOST_INC) $(SCHED_SRC) -o $@

$(BUILD)/test_config: $(CFG_SRC) | $(BUILD)
	@$(HOST_CC) $(WARN) -O1 -g $(HOST_INC) $(CFG_SRC) -o $@

syntax: | $(BUILD)
	@for f in $(ARM_SRC); do \
	    $(ARM_CC) $(WARN) $(ARM_FLAGS) -Os $(ARM_INC) -c $$f -o $(BUILD)/syntax.o || exit 1; \
	done
	@echo "syntax: $(words $(ARM_SRC)) sources compiled clean for cortex-a9"

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	@rm -rf $(BUILD)
