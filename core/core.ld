/* OpenROM resident core: 8 KB of unused kernel RAM at 8000C000h */
OUTPUT_FORMAT("elf32-littlemips")
ENTRY(core_install)
MEMORY { CORE (rwx) : ORIGIN = 0x8000C000, LENGTH = 0xC00 }
SECTIONS {
  .text : { *(.text.entry) *(.text .text.*) } > CORE
  .rodata : { *(.rodata .rodata.*) } > CORE
  .data : { *(.data .data.*) *(.sdata .sdata.*) *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) . = ALIGN(4); } > CORE
  /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.comment) *(.pdr) *(.gnu.attributes) }
}
