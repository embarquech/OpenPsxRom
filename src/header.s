# OpenROM cartridge header and boot code, linked at 1F000000h (EXP1, 128 KB flash).
#
# The BIOS calls [1F000080h] very early (pre-boot: SCPH-5502 BFC06EFCh), before the kernel is set
# up, so nothing useful can run there. Like Unirom, we only install a hook and return:
#   - debug exception vector 80000040h -> jump to shell_hook (below)
#   - hardware execution breakpoint on 80030000h, where the BIOS starts the shell once the kernel
#     is ready (the shell would then boot the CD)
# When the breakpoint fires, the kernel is fully initialised: the payload is copied to RAM (so the
# flash can be rewritten while it runs), the menu runs, and leaving it resumes the shell exactly
# where it was stopped (registers and status restored, rfe).
# [1F000000h] (mid-boot) gets the same pointer, as PS Hacker and Unirom do.

    .set noreorder
    .set noat
    .section .header, "ax"

    .word   preboot
    .ascii  "Licensed by Sony Computer Entertainment Inc."
    .ascii  "OpenPsxRom - open source cartridge firmware"
    .balign 0x80, 0
    .word   preboot
    .ascii  "Licensed by Sony Computer Entertainment Inc."
    .ascii  "OpenPsxRom"
    .org    0xEC, 0
    # entry points of the code run from the ROM while a game runs (src/romx.c, src/shared.h)
    .word   rom_break
    .word   rom_feed
    .word   rom_halted
    .word   rom_crash
    .word   rom_menu

    .equ    SAVE, 0x1F800300    # scratchpad: registers of the interrupted shell (nothing uses it yet)
    .equ    SHELL, 0x80030000

preboot:
    # Xplorer switch (1F060000h bit 0, 1 = on): on -> OpenROM. Off -> what the menu chose (settings
    # word 'OCF1' at 1F01FF80h, bit 0 of the next word): bypass (normal boot, the default) or ROM 2,
    # the second firmware in the upper 128 KB (Unirom on Xplorers): its own pre-boot entry, entered
    # exactly like the BIOS does (tail jump, $ra still points into the BIOS).
    lui     $t0, 0x1F06
    lbu     $t1, 0($t0)
    nop
    andi    $t1, $t1, 1
    bnez    $t1, 1f
    nop
    lui     $t0, 0x1F02
    lw      $t1, -0x80($t0)     # 1F01FF80h: settings magic
    li      $t2, 0x3146434F     # "OCF1"
    bne     $t1, $t2, 2f
    lw      $t1, -0x7C($t0)     # 1F01FF84h: settings flags (load delay covered by the branch)
    nop
    andi    $t1, $t1, 0xFF      # 0 bypass, 1/2 bank 2 (1F020000h), 3 bank 3 (1F040000h)
    beqz    $t1, 2f
    li      $t2, 3
    bne     $t1, $t2, 3f
    nop
    lui     $t0, 0x1F04         # bank 3
3:  sltiu   $t2, $t1, 4         # anything else: bypass
    beqz    $t2, 2f
    nop
    lw      $t1, 0x84($t0)      # firmware header: "Lice"(nsed by Sony) at bank + 84h
    li      $t2, 0x6563694C
    bne     $t1, $t2, 2f
    lw      $t1, 0x80($t0)      # its pre-boot entry, which must be inside that bank
    nop
    subu    $t2, $t1, $t0
    lui     $t3, 0x0002
    sltu    $t2, $t2, $t3       # 0 <= entry - bank < 20000h
    beqz    $t2, 2f
    nop
    jr      $t1
    nop
2:  jr      $ra
    nop
1:  mtc0    $zero, $7           # DCIC: breakpoints off while installing
    nop
    # debug exception vector: lui k0,hi(shell_hook); addiu k0,lo; jr k0; nop
    la      $t0, vector_code
    lui     $t1, 0x8000
    lw      $t2, 0($t0)
    lw      $t3, 4($t0)
    lw      $t4, 8($t0)
    lw      $t5, 12($t0)
    sw      $t2, 0x40($t1)
    sw      $t3, 0x44($t1)
    sw      $t4, 0x48($t1)
    sw      $t5, 0x4C($t1)
    li      $t0, SHELL
    mtc0    $t0, $3             # BPC: execution breakpoint address
    li      $t0, -1
    mtc0    $t0, $11            # BPCM: all address bits compared
    lui     $t0, 0xEB80         # DCIC: exec breakpoint, kernel+user, trap (same value as Unirom)
    mtc0    $t0, $7
    nop
    jr      $ra
    nop

vector_code:
    lui     $k0, %hi(shell_hook)
    addiu   $k0, $k0, %lo(shell_hook)
    jr      $k0
    nop

shell_hook:
    mtc0    $zero, $7           # one shot: breakpoint off
    nop
    li      $k0, SAVE
    sw      $1, 4($k0)
    sw      $2, 8($k0)
    sw      $3, 12($k0)
    sw      $4, 16($k0)
    sw      $5, 20($k0)
    sw      $6, 24($k0)
    sw      $7, 28($k0)
    sw      $8, 32($k0)
    sw      $9, 36($k0)
    sw      $10, 40($k0)
    sw      $11, 44($k0)
    sw      $12, 48($k0)
    sw      $13, 52($k0)
    sw      $14, 56($k0)
    sw      $15, 60($k0)
    sw      $16, 64($k0)
    sw      $17, 68($k0)
    sw      $18, 72($k0)
    sw      $19, 76($k0)
    sw      $20, 80($k0)
    sw      $21, 84($k0)
    sw      $22, 88($k0)
    sw      $23, 92($k0)
    sw      $24, 96($k0)
    sw      $25, 100($k0)
    sw      $28, 112($k0)
    sw      $29, 116($k0)
    sw      $30, 120($k0)
    sw      $31, 124($k0)
    mfc0    $t0, $14            # EPC (= 80030000h)
    nop
    sw      $t0, 0($k0)
    mfc0    $t0, $12            # SR
    nop
    sw      $t0, 128($k0)
    mfhi    $t0
    sw      $t0, 132($k0)
    mflo    $t0
    sw      $t0, 136($k0)

    # payload: stored compressed (OMC1, tools/packrom.py) at __payload_rom -> __payload_ram
    # rom_unpack (src/romx.c, 232 bytes, position independent) is copied to RAM first: run from
    # the cartridge's 8-bit uncached bus it would take seconds
    la      $t0, rom_unpack
    lui     $t1, 0x8000
    ori     $t1, $t1, 0xDF00
    addiu   $t2, $t0, 0x100
2:  lw      $t3, 0($t0)
    addiu   $t0, $t0, 4
    sw      $t3, 0($t1)
    bne     $t0, $t2, 2b
    addiu   $t1, $t1, 4
    addiu   $sp, $sp, -16
    li      $t2, 0xA0           # FlushCache before running it
    jalr    $t2
    li      $t1, 0x44
    la      $a0, __payload_rom
    la      $a1, __payload_ram
    lui     $t0, 0x8000
    ori     $t0, $t0, 0xDF00
    jalr    $t0
    nop
    addiu   $sp, $sp, 16
    li      $t2, 0xA0           # FlushCache (kernel is ready now)
    jalr    $t2
    li      $t1, 0x44
    la      $t0, rom_main_entry
    jalr    $t0                 # menu; returns when the user boots the CD
    nop

    li      $k0, SAVE
    lw      $t0, 132($k0)
    nop
    mthi    $t0
    lw      $t0, 136($k0)
    nop
    mtlo    $t0
    lw      $t0, 128($k0)
    nop
    mtc0    $t0, $12
    nop
    lw      $1, 4($k0)
    lw      $2, 8($k0)
    lw      $3, 12($k0)
    lw      $4, 16($k0)
    lw      $5, 20($k0)
    lw      $6, 24($k0)
    lw      $7, 28($k0)
    lw      $8, 32($k0)
    lw      $9, 36($k0)
    lw      $10, 40($k0)
    lw      $11, 44($k0)
    lw      $12, 48($k0)
    lw      $13, 52($k0)
    lw      $14, 56($k0)
    lw      $15, 60($k0)
    lw      $16, 64($k0)
    lw      $17, 68($k0)
    lw      $18, 72($k0)
    lw      $19, 76($k0)
    lw      $20, 80($k0)
    lw      $21, 84($k0)
    lw      $22, 88($k0)
    lw      $23, 92($k0)
    lw      $24, 96($k0)
    lw      $25, 100($k0)
    lw      $28, 112($k0)
    lw      $29, 116($k0)
    lw      $30, 120($k0)
    lw      $31, 124($k0)
    lw      $k0, 0($k0)         # EPC
    nop
    jr      $k0
    rfe
