# OpenPsxRom - open-source firmware for PlayStation 1 cheat cartridges (GPL-3.0-or-later)
# Toolchain prefix: make TOOLS=/path/to/mipsel-none-elf-   (or just mipsel-none-elf- if on PATH)
TOOLS ?= ../tools/gcc/bin/mipsel-none-elf-
CC := $(TOOLS)gcc
OBJCOPY := $(TOOLS)objcopy
# objcopy run from inside build/ (keeps the _binary_* symbol names short)
BOBJCOPY := $(if $(findstring /,$(TOOLS)),$(abspath $(OBJCOPY)),$(OBJCOPY))
CFLAGS := -march=r3000 -mtune=r3000 -mabi=32 -EL -msoft-float -fno-pic -mno-abicalls -G0 \
          -ffreestanding -fno-builtin -nostdlib -Os -Wall -ffunction-sections -fdata-sections $(DEFS)
SRC := src/header.s src/crt.s src/main.c src/gfx.c src/pad.c src/sio.c src/protocol.c src/flash.c src/util.c src/cd.c src/mcard.c src/exe.c src/boot.c src/lz.c src/store.c src/romx.c src/strplay.c src/str/vlc.c src/str/vlc.s
OBJ := $(patsubst src/%,build/%.o,$(SRC)) build/core_blob.o

all: build/openrom.rom

# Build configuration (Kconfig): python tools/kconfig.py menuconfig, then make cart -> build/cart.rom.
# Without a .config, configs/defconfig: the standard build. C files see build/config.h.
CONFIG ?= .config
-include $(CONFIG)
# checked at every make (CONFIG may change): rewritten only when its content changes
build/config.h: FORCE
	@python tools/kconfig.py header $(CONFIG) $@
FORCE:
CCFG := -include build/config.h
# the pre-boot code (src/header.s) gets the switch OFF default as an assembler symbol
DEF_SWITCH_OFF := $(if $(filter y,$(CONFIG_DEF_SWITCH_OFF_BANK2)),2,$(if $(filter y,$(CONFIG_DEF_SWITCH_OFF_BANK3)),3,0))
menuconfig:
	python tools/kconfig.py menuconfig
defconfig:
	python tools/kconfig.py defconfig
# the cartridge type chosen in menuconfig, with its options -> build/cart.rom
cart: build/config.h
ifeq ($(CONFIG_TARGET_BANK3),y)
	$(MAKE) bank3
	cp build/openrom_bank3.rom build/cart.rom
	@echo "build/cart.rom ready: python tools/psx.py flash build/cart.rom (fallback: make flash3 IMG=build/cart.rom)"
else ifeq ($(CONFIG_TARGET_DATEL),y)
	$(MAKE) arcart DUMP=$(subst ",,$(CONFIG_DATEL_DUMP))
	cp build/openrom_arcart.rom build/cart.rom
	@echo "build/cart.rom ready: python tools/psx.py flash build/cart.rom"
else
	$(MAKE) build/openrom.rom
	cp build/openrom.rom build/cart.rom
	@echo "build/cart.rom ready: python tools/psx.py flash build/cart.rom"
endif
# Datel cart with locked sectors 0 and 5 ("PS Hacker (Version 1999)"): OpenPsxRom at 1F005200h,
# entered through two hooks; DUMP = a /dump of that cart (128 KB) -> build/openrom_arcart.rom
DUMP ?= backup/ar_cart_128k.bin
arcart:
	$(MAKE) -B LDS=rom_ar.ld DEFS="$(DEFS) -DARCART -Wa,--defsym,ARCART=1" build/openrom.rom
	cp build/openrom.rom build/openrom_ar.rom
	cp build/openrom.elf build/openrom_ar.elf
	$(CC) $(CFLAGS) -T src/arcart_hooks.ld -Wl,--defsym,ARPRE=0x$$($(TOOLS)nm build/openrom_ar.elf | awk '/ preboot$$/{print $$1}' | tail -c 9) -Wl,--defsym,OWNFLUSH=0x$$($(TOOLS)nm build/openrom_ar.elf | awk '/ own_flush$$/{print $$1}' | tail -c 9) src/arcart_hooks.s -o build/arcart_hooks.elf
	python tools/arcart_image.py $(DUMP) build/openrom_ar.rom build/arcart_hooks.elf build/openrom_arcart.rom $(TOOLS)nm
	$(MAKE) -B build/openrom.rom

# bank 3 writer run from RAM (nops /exe): writes IMG (128 KB built for 1F040000h) to the Xplorer
# FX's second chip, then reads it back; without IMG, a test pattern. -> build/flash3.exe
IMG ?=
flash3:
	@mkdir -p build/flash3
	$(if $(IMG),cp $(IMG) build/flash3/image.bin && cd build/flash3 && $(BOBJCOPY) -I binary -O elf32-littlemips -B mips image.bin image.o)
	$(CC) $(CFLAGS) $(if $(IMG),-DIMAGE) -O2 -T test/flash3.ld test/flash3_crt.s test/flashtest3.c $(if $(IMG),build/flash3/image.o) -o build/flash3/flash3.elf -lgcc
	$(OBJCOPY) -O binary build/flash3/flash3.elf build/flash3/flash3.bin
	python tools/elf2exe.py build/flash3/flash3.bin build/flash3.exe 0x80010000 0x80010000
# a second OpenPsxRom for bank 3 (1F040000h), "bank 3" shown in the title bar: build/openrom_bank3.rom
bank3:
	$(MAKE) -B LDS=rom_bank3.ld DEFS="$(DEFS) -DBANK3 -Wa,--defsym,BANK3=1" build/openrom.rom
	cp build/openrom.rom build/openrom_bank3.rom
	$(MAKE) -B build/openrom.rom
disc: release/openrom.bin
lo: build/openrom_lo.exe

build/%.s.o: src/%.s build/config.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Wa,--defsym,DEF_SWITCH_OFF=$(DEF_SWITCH_OFF) -I src/str -c $< -o $@
build/%.c.o: src/%.c src/rom.h src/shared.h build/config.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CCFG) -c $< -o $@
# resident core: linked at 8000C000h, embedded in the ROM payload as data
build/core.elf: core/core.c core/bp.s core/core.ld src/shared.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c core/core.c -o build/core.o
	$(CC) $(CFLAGS) -c core/bp.s -o build/core_bp.o
	$(CC) $(CFLAGS) -T core/core.ld -Wl,--gc-sections build/core.o build/core_bp.o -o $@ -lgcc
build/core_blob.o: build/core.elf
	$(OBJCOPY) -O binary $< build/core.bin
	cd build && $(BOBJCOPY) -I binary -O elf32-littlemips -B mips --rename-section .data=.rodata,alloc,load,readonly,data,contents core.bin core_blob.o

LDS ?= rom.ld
build/openrom.elf: $(OBJ) $(LDS)
	$(CC) $(CFLAGS) -T $(LDS) -Wl,--gc-sections $(OBJ) -o $@ -lgcc
build/openrom.rom: build/openrom.elf
	$(OBJCOPY) -O binary -j .header -j .romx -j .text -j .rodata -j .data $< build/openrom.raw
	python tools/packrom.py build/openrom.elf build/openrom.raw build/openrom.packed $(TOOLS)nm
	python tools/pad.py build/openrom.packed $@ 131072

# boot disc: the same menu as an EXE (main.c with -DBOOTDISC) + the ROM image to install
BD_OBJ := $(patsubst src/%,build/bd/%.o,$(filter-out src/header.s src/romx.c,$(SRC))) build/core_blob.o build/rom_blob.o
build/bd/%.s.o: src/%.s
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DBOOTDISC -I src/str -c $< -o $@
build/bd/%.c.o: src/%.c src/rom.h build/config.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CCFG) -DBOOTDISC -c $< -o $@
build/rom_blob.o: build/openrom.rom
	cd build && $(BOBJCOPY) -I binary -O elf32-littlemips -B mips --rename-section .data=.rodata,alloc,load,readonly,data,contents openrom.rom rom_blob.o
build/openrom_exe.elf: $(BD_OBJ) exe.ld
	$(CC) $(CFLAGS) -T exe.ld -Wl,--gc-sections $(BD_OBJ) -o $@ -lgcc
build/openrom.exe: build/openrom_exe.elf
	$(OBJCOPY) -O binary $< build/openrom_exe.bin
	python tools/elf2exe.py build/openrom_exe.bin $@ 0x801A0000 0x801A0000
# the same menu loaded low (80010000h): sent with "nops /exe" to replace a running OpenPsxRom or
# to flash a cartridge from Unirom ("nops /rom" right after)
build/openrom_lo.elf: $(BD_OBJ) exe_lo.ld
	$(CC) $(CFLAGS) -T exe_lo.ld -Wl,--gc-sections $(BD_OBJ) -o $@ -lgcc
build/openrom_lo.exe: build/openrom_lo.elf
	$(OBJCOPY) -O binary $< build/openrom_lo.bin
	python tools/elf2exe.py build/openrom_lo.bin $@ 0x80010000 0x80010000
release/openrom.bin: build/openrom.exe build/openrom.rom tools/mkdisc.py
	@mkdir -p release
	python tools/mkdisc.py --exe build/openrom.exe --rom build/openrom.rom --out release/openrom

clean:
	rm -rf build release
.PHONY: lo all disc clean cart menuconfig defconfig
