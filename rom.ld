/* OpenROM: header + stub at 1F000000h (flash), payload copied to RAM at 801A0000h. */
OUTPUT_FORMAT("elf32-littlemips")
ENTRY(rom_main_entry)
MEMORY {
  ROM (rx)  : ORIGIN = 0x1F000000, LENGTH = 0x20000
  RAM (rwx) : ORIGIN = 0x801A0000, LENGTH = 0x50000
}
SECTIONS {
  .header : { KEEP(*(.header)) . = ALIGN(16); } > ROM
  /* src/romx.c runs from the flash: all its sections stay in the ROM */
  .romx : { *romx.c.o(.text .text.* .rodata .rodata.* .data .data.* .sdata .sdata.*) . = ALIGN(16); } > ROM
  .text : { __payload_ram = .; *(.text.start) *(.text .text.*) } > RAM AT > ROM
  .rodata : { *(.rodata .rodata.*) } > RAM AT > ROM
  .data : { *(.data .data.*) *(.sdata .sdata.*) . = ALIGN(4); __data_end = .; } > RAM AT > ROM
  __payload_rom = LOADADDR(.text);
  __payload_size = __data_end - __payload_ram;
  .bss (NOLOAD) : { __bss_start = .; *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) . = ALIGN(4); __bss_end = .; } > RAM
  __rom_end = LOADADDR(.data) + SIZEOF(.data);
  /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.comment) *(.pdr) *(.gnu.attributes) }
}
