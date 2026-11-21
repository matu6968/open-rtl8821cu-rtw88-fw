# SPDX-License-Identifier: GPL-2.0-only
PYTHON ?= python3
MIPS_PREFIX ?= $(CURDIR)/build/toolchain
BUILD ?= build/firmware
OUTPUT ?= target_firmware
SOURCE_DATE_EPOCH ?= 1795021200
export SOURCE_DATE_EPOCH

MIPS_CC := $(MIPS_PREFIX)/bin/mipsel-elf-gcc
MIPS_OBJDUMP := $(MIPS_PREFIX)/bin/mipsel-elf-objdump
CFLAGS := -march=mips1 -mips16 -mno-mips16e2 -mfp32 -mabi=32 -EL -G0 \
 -mno-abicalls -msoft-float -ffreestanding -fno-pic -fno-builtin \
 -fno-delayed-branch -fno-stack-protector -fno-common \
 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables \
 -std=c11 -Os -falign-functions=4 -ffixed-25 -Wa,-O0 -Wall -Wextra -Werror -MMD -MP
ASFLAGS := -march=mips32r2 -mips16 -mmips16e2 -mabi=32 -EL -G0 -mno-abicalls \
 -msoft-float -ffreestanding -nostdlib -fno-pic -Wa,-O0

SRC := src
SOURCES := diagnostic/entry.S diagnostic/main.c diagnostic/events.c diagnostic/chip.c \
 diagnostic/abi.c diagnostic/mailbox.c diagnostic/layout.c compat/efuse.c compat/control.c \
 interrupts/core.S interrupts/timing.c interrupts/foundation.c station/station.c \
 station/packet.c station/iqk.c station/tx_report.c broader/broader.c station/rate.c \
 station/power.c powersave/lps.c doze/doze.c doze/beacon.c doze/radio.c wowlan/wowlan.c \
 deep/deep.c deep/clock.c deep/resume.S fwlog/fwlog.c
OBJECTS := $(addprefix $(BUILD)/,$(addsuffix .o,$(basename $(SOURCES))))
LINKERS := $(SRC)/deep/linker.ld $(SRC)/diagnostic/linker.ld
PACKER := $(SRC)/diagnostic/pack.py $(SRC)/diagnostic/format.py tools/pack_rtw.py tools/parse_rtw.py
RUNTIME_INPUTS := $(sort $(addprefix $(SRC)/,$(SOURCES)) $(wildcard $(SRC)/*/*.h $(SRC)/*/*.inc))
CORE_OPS := $(SRC)/interrupts/core_ops.json
IMAGE := $(OUTPUT)/rtw8821c_fw.bin
MANIFEST := $(OUTPUT)/rtw8821c_fw.json
ELF := $(BUILD)/rtw8821c_fw.elf
DIS := $(BUILD)/rtw8821c_fw.dis

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.PHONY: all toolchain check clean
all: $(IMAGE) $(MANIFEST)

toolchain:
	sh tools/build-toolchain.sh "$(MIPS_PREFIX)"

$(BUILD)/%.o: $(SRC)/%.c Makefile
	@mkdir -p "$(@D)"
	$(MIPS_CC) $(CFLAGS) $(EXTRA_CFLAGS) -c "$<" -o "$@"

$(BUILD)/%.o: $(SRC)/%.S Makefile
	@mkdir -p "$(@D)"
	$(MIPS_CC) $(ASFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD)/station/iqk.o: EXTRA_CFLAGS := -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-sign-compare

$(ELF): $(OBJECTS) $(LINKERS) Makefile
	$(MIPS_CC) $(CFLAGS) -nostdlib -static -T $(SRC)/deep/linker.ld \
		-Wl,--nmagic,--gc-sections,--no-undefined,-Map,$(BUILD)/rtw8821c_fw.map -o "$@" $(OBJECTS)

$(DIS): $(ELF)
	$(MIPS_OBJDUMP) -d -z -m mips16 -M no-aliases "$<" > "$@"

$(IMAGE) $(MANIFEST) &: $(ELF) $(DIS) $(PACKER) $(CORE_OPS)
	@mkdir -p "$(OUTPUT)"
	$(PYTHON) $(firstword $(PACKER)) --elf "$(ELF)" --output "$(IMAGE)" --fw-version 0.11.1 \
		--reserve-t9 --core-ops "$(CORE_OPS)" --compiler "$(MIPS_CC)" \
		--cflags "$(CFLAGS)" --asflags "$(ASFLAGS)" --disassembly "$(DIS)" \
		$(foreach source,$(RUNTIME_INPUTS) $(LINKERS) $(PACKER) $(CORE_OPS) Makefile,--extra-source $(source))

check: all
	FIRMWARE_IMAGE="$(IMAGE)" FIRMWARE_ELF="$(ELF)" $(PYTHON) -m unittest discover -s tests -v

clean:
	rm -rf -- "$(BUILD)" "$(OUTPUT)"

-include $(OBJECTS:.o=.d)
