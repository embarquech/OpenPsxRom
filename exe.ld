/* OpenROM boot disc: the same menu as a PS-EXE, loaded at 801A0000h. */
OUTPUT_FORMAT("elf32-littlemips")
ENTRY(rom_main_entry)
MEMORY { RAM (rwx) : ORIGIN = 0x801A0000, LENGTH = 0x50000 }
SECTIONS {
  .text : { __payload_ram = .; *(.text.start) *(.text .text.*) } > RAM
  .rodata : { *(.rodata .rodata.*) } > RAM
  .data : { *(.data .data.*) *(.sdata .sdata.*) . = ALIGN(4); __data_end = .; } > RAM
  .bss (NOLOAD) : { __bss_start = .; *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) . = ALIGN(4); __bss_end = .; } > RAM
  /DISCARD/ : { *(.header) *(.MIPS.abiflags) *(.reginfo) *(.comment) *(.pdr) *(.gnu.attributes) }
}
