# Hooks for a Datel cart whose sectors 0 and 5 are locked ("PS Hacker (Version 1999)" firmware,
# Am29F010): the locked sector 0 keeps the BIOS entries and Datel's boot code; OpenPsxRom is placed
# in the writable sectors (1F005200h, make arcart) and these two routines take over where Datel's
# locked code first calls into a writable sector. Each one installs OpenPsxRom's hook (pre-boot
# code, linked address ARPRE) and returns straight to the BIOS, unwinding Datel's stack frames.
# Offsets and frame sizes come from the disassembly of that firmware (tools/arcart_image.py checks
# the call sites before building an image).
    .set noreorder
    .set noat

# Pre-boot: Datel's entry 1F000120h (frame: ra 0, s0 4, gp 8, 12 bytes) calls 1F004BCCh at
# 1F00015Ch, after saving DPCR in s0 (and setting gp to 1F80h).
    .section .hook_pre, "ax"
    .global hook_pre
hook_pre:
    lui     $t0, 0x1F80
    sw      $s0, 0x10F0($t0)    # DPCR back as Datel's code found it
    jal     ARPRE               # OpenPsxRom pre-boot: debug vector + breakpoint on the shell
    nop
    lw      $ra, 0($sp)
    lw      $s0, 4($sp)
    lw      $gp, 8($sp)
    jr      $ra                 # back to the BIOS
    addiu   $sp, $sp, 12

# Mid-boot: entry 1F0001D0h (4 bytes, ra) -> 1F0001F8h (16: ra, gp, s8, s7) -> 1F001140h (4) ->
# 1F0002A8h (4) calls 1F0051E0h at 1F0002C4h: sp is the BIOS's sp - 28. Datel's code changed on
# the way: B(xx) vector at 000000B0h (saved at s8+7Ch), words 5F0h/5F4h (saved at s8+8Ch).
    .section .hook_mid, "ax"           # 1F0051E0h: only 32 bytes before OpenPsxRom (5200h)
    .global hook_mid
hook_mid:
    j       hook_mid_body
    nop

    .section .hook_body, "ax"          # free space at the start of sector 1
hook_mid_body:
    lui     $t0, 0x801F         # Datel's s8 area (801F0000h)
    lw      $t1, 0x7C($t0)
    lw      $t2, 0x80($t0)
    lw      $t3, 0x84($t0)
    lw      $t4, 0x88($t0)
    sw      $t1, 0xB0($zero)
    sw      $t2, 0xB4($zero)
    sw      $t3, 0xB8($zero)
    sw      $t4, 0xBC($zero)
    lw      $t1, 0x8C($t0)
    lw      $t2, 0x90($t0)
    sw      $t1, 0x5F0($zero)
    sw      $t2, 0x5F4($zero)
    la      $t0, OWNFLUSH       # instruction cache flush, run uncached (vector B0h rewritten)
    lui     $t1, 0xA000
    or      $t0, $t0, $t1
    jalr    $t0
    nop
    jal     ARPRE               # hook installed again, as at every BIOS call
    nop
    lw      $gp, 12($sp)
    lw      $s8, 16($sp)
    lw      $s7, 20($sp)
    lw      $ra, 24($sp)
    jr      $ra                 # back to the BIOS
    addiu   $sp, $sp, 28
