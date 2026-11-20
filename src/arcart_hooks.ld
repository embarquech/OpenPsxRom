/* the two hooks of src/arcart_hooks.s at the addresses Datel's locked code calls */
SECTIONS {
  .hook_pre 0x1F004BCC : { *(.hook_pre) }
  .hook_mid 0x1F0051E0 : { *(.hook_mid) }
  .hook_body 0x1F004000 : { *(.hook_body) }
  /DISCARD/ : { *(.text .data .bss .MIPS.abiflags .reginfo .comment .pdr .gnu.attributes) }
}
