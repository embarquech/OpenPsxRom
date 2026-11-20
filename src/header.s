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
# [1F000000h] (mid-boot) gets the same pointer, as PS Hacker and Unirom do. (A firmware with two
# different entries, like Caetla, would need [bank+00h] for mid-boot: tried, then reverted after
# black screens at power-on on a real Xplorer FX; not understood yet.)

    .set noreorder
    .set noat
    .section .header, "ax"

    .word   preboot
.ifdef BANK3
    # The bank 3 copy must never be started by the BIOS itself: the Xplorer FX can come up with
    # its second chip seen at 1F000000h (a state kept until the switch is set OFF), and the BIOS
    # would then jump into this code at the wrong address (black screen). The BIOS compares more
    # than the first 16 characters (BIOS 4.1 rejects a change at the 17th, tested in DuckStation):
    # one changed there; OpenPsxRom's chaining checks "Lice", /rom "Licensed by Sony".
    .ascii  "Licensed by Sony_Computer Entertainment Inc."
.else
    .ascii  "Licensed by Sony Computer Entertainment Inc."
.endif
    .ascii  "OpenPsxRom - open source cartridge firmware"
    .balign 0x80, 0
    .word   preboot
.ifdef BANK3
    .ascii  "Licensed by Sony_Computer Entertainment Inc."
.else
    .ascii  "Licensed by Sony Computer Entertainment Inc."
.endif
    .ascii  "OpenPsxRom"
    .org    0xE8, 0
    # entry points of the code run from the ROM while a game runs (src/romx.c, src/shared.h)
    .word   rom_combo
    .word   rom_break
    .word   rom_feed
    .word   rom_halted
    .word   rom_crash
    .word   rom_menu

    # Boot stage markers on the serial port (115200), sent straight to SIO1, for the black screens
    # at power-on: P pre-boot, then 1 (switch ON) or 0 (OFF); H shell hook; U payload unpacked.
    # Bounded wait for the transmitter: never blocks the boot. Uses t0-t2 only.
    .macro  SIOINIT
    lui     $t0, 0x1F80
    li      $t1, 0x40
    sh      $t1, 0x105A($t0)
    li      $t1, 0xCE
    sh      $t1, 0x1058($t0)
    li      $t1, 0x12
    sh      $t1, 0x105E($t0)
    li      $t1, 0x05
    sh      $t1, 0x105A($t0)
    .endm
    .macro  MARK ch
    lui     $t0, 0x1F80
    li      $t1, 20000
91: lw      $t2, 0x1054($t0)
    nop
    andi    $t2, $t2, 1
    bnez    $t2, 92f
    addiu   $t1, $t1, -1
    bnez    $t1, 91b
    nop
92: li      $t2, \ch
    sb      $t2, 0x1050($t0)
    .endm

    # Wait (bounded) until no DMA channel is running: the BIOS FlushCache isolates the cache and
    # switches the BIU to tag test mode, which freezes the console for good if a DMA (left by the
    # BIOS logo / CD init at a cold power-on) is still transferring. Seen as "H C" then nothing.
    .macro  DMAWAIT
    lui     $t0, 0x1F80
    ori     $t0, $t0, 0x1088    # DMA0 CHCR, then +10h per channel up to DMA6
    li      $t1, 7
93: lui     $t3, 0x0010
94: lw      $t2, 0($t0)
    nop
    srl     $t2, $t2, 24
    andi    $t2, $t2, 1
    beqz    $t2, 95f
    addiu   $t3, $t3, -1
    bnez    $t3, 94b
    nop
    # still busy: send 'a' + channel (bounded wait for the transmitter, t2/t3 free here)
    lui     $t3, 0x1F80
    li      $t2, 0x68           # 'h' - (7 - remaining) = 'a' + channel
    subu    $t2, $t2, $t1
    sb      $t2, 0x1050($t3)
95: addiu   $t1, $t1, -1
    bnez    $t1, 93b
    addiu   $t0, $t0, 0x10
    .endm

    .equ    SAVE, 0x1F800300    # scratchpad: registers of the interrupted shell (nothing uses it yet)
    .equ    SHELL, 0x80030000

preboot:
.ifdef BANK3
    b       1f                  # the bank 3 build was chosen by the switch already: start here
    nop
.endif
    # Xplorer switch (1F060000h bit 0, 1 = on): on -> OpenROM. Off -> what the menu chose (settings
    # word 'OCF1' at 1F01FF80h, bit 0 of the next word): bypass (normal boot, the default) or ROM 2,
    # the second firmware in the upper 128 KB (Unirom on Xplorers): its own pre-boot entry, entered
    # exactly like the BIOS does (tail jump, $ra still points into the BIOS).
    # The switch is sampled for ~0.2 s, not once: at a cold power-on the Xplorer FX sometimes
    # reads OFF for a moment although the switch is ON (seen on hardware: the boot then followed
    # the OFF setting, bank 2 or 3). ON seen once -> OpenROM; the OFF setting only if OFF throughout.
    SIOINIT
    MARK    0x50                # 'P'
    lui     $t0, 0x1F06
    lui     $t2, 0x0004         # 40000h samples
5:  lbu     $t1, 0($t0)
    nop
    andi    $t1, $t1, 1
    bnez    $t1, 1f
    addiu   $t2, $t2, -1        # (delay slot: harmless on the taken branch)
    bnez    $t2, 5b
    nop
    MARK    0x30                # '0': switch read OFF
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
1:  MARK    0x31                # '1': switch read ON
    mtc0    $zero, $7           # DCIC: breakpoints off while installing
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
    SIOINIT
    MARK    0x48                # 'H': the shell hook fired

    # payload: stored compressed (OMC1, tools/packrom.py) at __payload_rom -> __payload_ram
    # rom_unpack (src/romx.c, position independent, < 512 bytes) is copied to RAM first: run from
    # the cartridge's 8-bit uncached bus it would take seconds. It goes just below the payload
    # (8019FE00h), not in the kernel's RAM (8000DF00h, used before: power-on black screens stopped
    # there, with RAM still holding the previous run). It checks the CRC32 of what it unpacked:
    # on a mismatch (bad reads from the cartridge) 'X' is sent and everything is read again.
    .equ    UNPACK_RAM, 0x8019FE00
3:  la      $t0, rom_unpack
    li      $t1, UNPACK_RAM
    addiu   $t2, $t0, 0x200
2:  lw      $t3, 0($t0)
    addiu   $t0, $t0, 4
    sw      $t3, 0($t1)
    bne     $t0, $t2, 2b
    addiu   $t1, $t1, 4
    MARK    0x43                # 'C': unpacker copied
    DMAWAIT
    MARK    0x44                # 'D': no DMA running
    addiu   $sp, $sp, -16
    la      $t0, own_flush      # our cache flush (not the BIOS one: it never returned on some
    lui     $t1, 0xA000         # cold power-ons), run uncached through the BF00xxxxh mirror
    or      $t0, $t0, $t1
    jalr    $t0
    nop
    MARK    0x46                # 'F': cache flushed
    la      $a0, __payload_rom
    la      $a1, __payload_ram
    li      $t0, UNPACK_RAM
    jalr    $t0
    nop
    addiu   $sp, $sp, 16
    beqz    $v0, 4f
    nop
    MARK    0x58                # 'X': CRC mismatch, read again after a pause
    lui     $t0, 0x0010
5:  addiu   $t0, $t0, -1
    bnez    $t0, 5b
    nop
    b       3b
    nop
4:  MARK    0x55                # 'U': payload unpacked and checked
    DMAWAIT
    la      $t0, own_flush
    lui     $t1, 0xA000
    or      $t0, $t0, $t1
    jalr    $t0
    nop
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

# Instruction cache flush without the BIOS (same sequence as BIOS A(44h) and pGbX): must run
# uncached (called through BF00xxxxh). BIU in tag test mode + isolated cache: clear every line's
# tag, then the data words; BIU and status back. Clobbers t0-t5; interrupts are off here.
    .global own_flush
own_flush:
    mfc0    $t0, $12
    nop
    lui     $t5, 0xFFFE
    li      $t1, 0x804
    sw      $t1, 0x130($t5)
    lui     $t4, 0x0001
    mtc0    $t4, $12
    nop
    nop
    li      $t2, 0
    li      $t3, 0x0F80
2:  sw      $zero, 0x00($t2)
    sw      $zero, 0x10($t2)
    sw      $zero, 0x20($t2)
    sw      $zero, 0x30($t2)
    sw      $zero, 0x40($t2)
    sw      $zero, 0x50($t2)
    sw      $zero, 0x60($t2)
    sw      $zero, 0x70($t2)
    bne     $t2, $t3, 2b
    addiu   $t2, $t2, 0x80
    mtc0    $zero, $12
    nop
    li      $t1, 0x800
    sw      $t1, 0x130($t5)
    mtc0    $t4, $12
    nop
    nop
    li      $t2, 0
    li      $t3, 0x0F80
3:
    .irp    o, 0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64,68,72,76,80,84,88,92,96,100,104,108,112,116,120,124
    sw      $zero, \o($t2)
    .endr
    bne     $t2, $t3, 3b
    addiu   $t2, $t2, 0x80
    mtc0    $zero, $12
    nop
    lui     $t1, 0x0001
    ori     $t1, $t1, 0xE988
    sw      $t1, 0x130($t5)
    mtc0    $t0, $12
    nop
    jr      $ra
    nop
