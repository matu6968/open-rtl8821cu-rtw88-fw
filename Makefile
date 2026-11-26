# SPDX-License-Identifier: GPL-2.0-only
PYTHON ?= python3
MIPS_PREFIX ?= $(CURDIR)/build/toolchain
BUILD ?= build/firmware$(if $(COEX_BOARD),-coex-$(COEX_BOARD))
OUTPUT ?= target_firmware$(if $(COEX_BOARD),/coex-$(COEX_BOARD))
BT_ROM1_TRANSPORT ?= usb
BT_ROM1_PROFILE ?= bringup
BT_ROM1_COEX ?= $(if $(filter usb,$(BT_ROM1_TRANSPORT)),1,0)
SOURCE_DATE_EPOCH ?= 1795021200
export SOURCE_DATE_EPOCH

MIPS_CC := $(MIPS_PREFIX)/bin/mipsel-elf-gcc
MIPS_OBJDUMP := $(MIPS_PREFIX)/bin/mipsel-elf-objdump
MIPS_OBJCOPY := $(MIPS_PREFIX)/bin/mipsel-elf-objcopy
MIPS_NM := $(MIPS_PREFIX)/bin/mipsel-elf-nm
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
 deep/deep.c deep/clock.c deep/resume.S fwlog/fwlog.c ap/ap.c
ifneq ($(COEX_BOARD),)
SOURCES += coex/coex.c
COEX_CFLAGS := -DRTL8821CU_COEX_BOARD=$(COEX_BOARD)
endif
WLAN_CFLAGS := $(CFLAGS) $(COEX_CFLAGS)
OBJECTS := $(addprefix $(BUILD)/,$(addsuffix .o,$(basename $(SOURCES))))
LINKERS := $(SRC)/deep/linker.ld $(SRC)/diagnostic/linker.ld
PACKER := $(SRC)/diagnostic/pack.py $(SRC)/diagnostic/format.py tools/pack_rtw.py tools/parse_rtw.py
RUNTIME_INPUTS := $(sort $(addprefix $(SRC)/,$(SOURCES)) $(wildcard $(SRC)/*/*.h $(SRC)/*/*.inc))
CORE_OPS := $(SRC)/interrupts/core_ops.json
IMAGE := $(OUTPUT)/rtw8821c_fw.bin
MANIFEST := $(OUTPUT)/rtw8821c_fw.json
ELF := $(BUILD)/rtw8821c_fw.elf
DIS := $(BUILD)/rtw8821c_fw.dis
BT_BUILD := build/bluetooth
BT_SRC := $(SRC)/bt-fw
BT_SOURCES := $(BT_SRC)/version.c $(BT_SRC)/config.c
BT_HEADERS :=
BT_LINKER := $(BT_SRC)/patch.ld
BT_ELF := $(BT_BUILD)/version.elf
BT_ROM1_CHIP := rtl8821c
ifeq ($(BT_ROM1_TRANSPORT),uart)
ifeq ($(BT_ROM1_COEX),1)
$(error BT_ROM1_COEX=1 requires BT_ROM1_TRANSPORT=usb)
endif
BT_SOURCES += $(BT_SRC)/uart.c
BT_ROM1_CHIP := rtl8821cs
BT_ROM1_SUFFIX := -uart
BT_ROM1_CFLAGS := -DBT_ROM1_UART
BT_ROM1_LDFLAGS := -Wl,--defsym=bt_rom1_uart=1
BT_ROM1_PACK := --transport uart
else ifeq ($(BT_ROM1_TRANSPORT),usb)
ifeq ($(BT_ROM1_COEX),1)
BT_SOURCES += $(BT_SRC)/coex.c $(BT_SRC)/afh.c
BT_HEADERS += $(BT_SRC)/afh.h
BT_ROM1_CFLAGS := -DBT_ROM1_COEX
BT_ROM1_PACK := --coex
BT_ROM1_SUFFIX := -coex
endif
else
$(error BT_ROM1_TRANSPORT must be usb or uart)
endif
ifeq ($(BT_ROM1_PROFILE),full)
ifeq ($(BT_ROM1_TRANSPORT),usb)
ifneq ($(BT_ROM1_COEX),1)
$(error BT_ROM1_PROFILE=full includes USB coexistence)
endif
endif
BT_SOURCES := $(filter-out $(BT_SRC)/config.c,$(BT_SOURCES)) $(BT_SRC)/rom1_config.c $(BT_SRC)/rom1_efuse.c
BT_HEADERS += $(BT_SRC)/rom1_config.h
ifeq ($(BT_ROM1_TRANSPORT),uart)
BT_SOURCES += $(BT_SRC)/uart_dma.c
endif
BT_ROM1_SUFFIX := -full-$(BT_ROM1_TRANSPORT)
BT_ROM1_CFLAGS += -DBT_ROM1_FULL
BT_ROM1_COVERAGE := $(BT_SRC)/rom1-coverage.json
BT_ROM1_PACK += --coverage "$(BT_ROM1_COVERAGE)" --nm "$(MIPS_NM)"
else ifneq ($(BT_ROM1_PROFILE),bringup)
$(error BT_ROM1_PROFILE must be bringup or full)
endif
BT_ROM1_BUILD ?= $(BT_BUILD)/rom1$(BT_ROM1_SUFFIX)
BT_ROM1_OUTPUT ?= target_firmware/bluetooth-rom1$(BT_ROM1_SUFFIX)
BT_ROM1_ELF := $(BT_ROM1_BUILD)/patch.elf
BT_ROM1_FILES := $(addprefix $(BT_ROM1_OUTPUT)/rtl_bt/$(BT_ROM1_CHIP)_,fw.bin config.bin)
BT_ROM1_MANIFEST := $(BT_ROM1_OUTPUT)/manifest.json

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.PHONY: all toolchain bluetooth bluetooth-rom1 check check-bluetooth clean
all: $(IMAGE) $(MANIFEST) bluetooth-rom1

toolchain:
	sh tools/build-toolchain.sh "$(MIPS_PREFIX)"

$(BUILD)/%.o: $(SRC)/%.c Makefile
	@mkdir -p "$(@D)"
	$(MIPS_CC) $(WLAN_CFLAGS) $(EXTRA_CFLAGS) -c "$<" -o "$@"

$(BUILD)/%.o: $(SRC)/%.S Makefile
	@mkdir -p "$(@D)"
	$(MIPS_CC) $(ASFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD)/station/iqk.o: EXTRA_CFLAGS := -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-sign-compare

$(ELF): $(OBJECTS) $(LINKERS) Makefile
	$(MIPS_CC) $(WLAN_CFLAGS) -nostdlib -static -T $(SRC)/deep/linker.ld \
		-Wl,--nmagic,--gc-sections,--no-undefined,-Map,$(BUILD)/rtw8821c_fw.map -o "$@" $(OBJECTS)

$(DIS): $(ELF)
	$(MIPS_OBJDUMP) -d -z -m mips16 -M no-aliases "$<" > "$@"

$(IMAGE) $(MANIFEST) &: $(ELF) $(DIS) $(PACKER) $(CORE_OPS)
	@mkdir -p "$(OUTPUT)"
	$(PYTHON) $(firstword $(PACKER)) --elf "$(ELF)" --output "$(IMAGE)" --fw-version 0.11.1 \
		--reserve-t9 --core-ops "$(CORE_OPS)" --compiler "$(MIPS_CC)" \
		--cflags "$(strip $(WLAN_CFLAGS))" --asflags "$(ASFLAGS)" --disassembly "$(DIS)" \
		$(foreach source,$(RUNTIME_INPUTS) $(LINKERS) $(PACKER) $(CORE_OPS) Makefile,--extra-source $(source))

bluetooth: $(BT_ELF) $(BT_BUILD)/version.dis

$(BT_ELF): $(BT_SRC)/version.c $(BT_LINKER) Makefile
	@mkdir -p "$(@D)"
	$(MIPS_CC) $(CFLAGS) -nostdlib -static -T $(BT_LINKER) \
		-Wl,--nmagic,--gc-sections,--no-undefined -o "$@" "$<"

$(BT_BUILD)/version.dis: $(BT_ELF)
	$(MIPS_OBJDUMP) -d -m mips16 -M no-aliases "$<" > "$@"

bluetooth-rom1: $(BT_ROM1_FILES) $(BT_ROM1_MANIFEST)

$(BT_ROM1_ELF): $(BT_SOURCES) $(BT_HEADERS) $(BT_LINKER) Makefile
	@mkdir -p "$(@D)"
	$(MIPS_CC) $(CFLAGS) -DBT_ROM1_IMAGE $(BT_ROM1_CFLAGS) -nostdlib -static -T $(BT_LINKER) \
		-Wl,--nmagic,--gc-sections,--no-undefined,--defsym=bt_rom1_layout=1 $(BT_ROM1_LDFLAGS) \
		-o "$@" $(BT_SOURCES)

$(BT_ROM1_BUILD)/patch.bin: $(BT_ROM1_ELF)
	$(MIPS_OBJCOPY) -O binary -j .text -j .patch_version --gap-fill 0 "$<" "$@"

$(BT_ROM1_FILES) $(BT_ROM1_MANIFEST) &: $(BT_ROM1_BUILD)/patch.bin $(BT_ROM1_COVERAGE) tools/build_bt_rom1.py tools/bt_epatch.py tools/bt_rom1_coverage.py
	$(PYTHON) tools/build_bt_rom1.py --elf "$(BT_ROM1_ELF)" --patch "$<" --output "$(BT_ROM1_OUTPUT)" $(BT_ROM1_PACK)

check: all
	FIRMWARE_IMAGE="$(IMAGE)" FIRMWARE_ELF="$(ELF)" \
		BT_ROM1_ARTIFACTS="$(BT_ROM1_OUTPUT)" BT_ROM1_COEX="$(BT_ROM1_COEX)" \
		BT_ROM1_TRANSPORT="$(BT_ROM1_TRANSPORT)" BT_ROM1_PROFILE="$(BT_ROM1_PROFILE)" \
		$(PYTHON) -m unittest discover -s tests -v

check-bluetooth: bluetooth-rom1
	BT_ROM1_ARTIFACTS="$(BT_ROM1_OUTPUT)" BT_ROM1_COEX="$(BT_ROM1_COEX)" \
		BT_ROM1_TRANSPORT="$(BT_ROM1_TRANSPORT)" BT_ROM1_PROFILE="$(BT_ROM1_PROFILE)" \
		$(PYTHON) -m unittest discover -s tests -p test_bt_epatch.py -v

clean:
	rm -rf -- "$(BUILD)" "$(OUTPUT)" "$(BT_ROM1_BUILD)" "$(BT_ROM1_OUTPUT)"

-include $(OBJECTS:.o=.d)
