/* test/flashtest3.c: an EXE at 80010000h (code + the optional 128 KB image) */
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x80010000, LENGTH = 0x40000 }
SECTIONS {
  .text : { *(.text.start) *(.text .text.*) *(.rodata .rodata.*) *(.data .data.*) . = ALIGN(4); } > RAM
  .bss (NOLOAD) : { __bss_start = .; *(.bss .bss.*) *(COMMON) . = ALIGN(4); __bss_end = .; } > RAM
  /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.comment) *(.pdr) *(.gnu.attributes) }
}
