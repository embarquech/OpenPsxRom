# Payload entry (runs from RAM). Called by the mid-boot stub with the BIOS's $ra/$sp.
    .set noreorder
    .section .text.start, "ax"

    .global rom_main_entry
rom_main_entry:
    la      $t0, bios_ctx
    sw      $ra, 0($t0)
    sw      $sp, 4($t0)
    sw      $gp, 8($t0)
    sw      $s0, 12($t0)
    sw      $s1, 16($t0)
    sw      $s2, 20($t0)
    sw      $s3, 24($t0)
    sw      $s4, 28($t0)
    sw      $s5, 32($t0)
    sw      $s6, 36($t0)
    sw      $s7, 40($t0)
    sw      $fp, 44($t0)
    mfc0    $t1, $12
    sw      $t1, 48($t0)
    li      $t2, -2             # interrupts off while the menu runs (polled I/O only)
    and     $t1, $t1, $t2
    mtc0    $t1, $12
    nop
    la      $t0, __bss_start
    la      $t1, __bss_end
1:  sltu    $t2, $t0, $t1
    beqz    $t2, 2f
    nop
    sw      $zero, 0($t0)
    b       1b
    addiu   $t0, $t0, 4
2:  jal     main
    nop
    # main returned: fall through

# Back to the BIOS boot sequence (it then boots the CD). Callable from anywhere.
    .global bios_return
bios_return:
    la      $t0, bios_ctx
    lw      $t1, 48($t0)
    nop
    mtc0    $t1, $12
    lw      $ra, 0($t0)
    lw      $sp, 4($t0)
    lw      $gp, 8($t0)
    lw      $s0, 12($t0)
    lw      $s1, 16($t0)
    lw      $s2, 20($t0)
    lw      $s3, 24($t0)
    lw      $s4, 28($t0)
    lw      $s5, 32($t0)
    lw      $s6, 36($t0)
    lw      $s7, 40($t0)
    lw      $fp, 44($t0)
    jr      $ra
    nop

# void exec_jump(u32 pc, u32 gp, u32 sp)  -- start a loaded program (never returns)
    .global exec_jump
exec_jump:
    move    $gp, $a1
    move    $sp, $a2
    move    $fp, $a2
    move    $ra, $zero
    jr      $a0
    nop

# u32 call_addr(u32 addr)  -- jal to an address and come back
    .global call_addr
call_addr:
    addiu   $sp, $sp, -8
    sw      $ra, 0($sp)
    jalr    $a0
    nop
    lw      $ra, 0($sp)
    addiu   $sp, $sp, 8
    jr      $ra
    nop

# BIOS A0h:44h FlushCache
    .global flush_cache
flush_cache:
    # never while a DMA runs (GPU lists of the menu, CD): cache isolation then freezes the console
    lui     $t0, 0x1F80
    ori     $t0, $t0, 0x1088
    li      $t1, 7
1:  lui     $t3, 0x0010
2:  lw      $t2, 0($t0)
    nop
    srl     $t2, $t2, 24
    andi    $t2, $t2, 1
    beqz    $t2, 3f
    addiu   $t3, $t3, -1
    bnez    $t3, 2b
    nop
3:  addiu   $t1, $t1, -1
    bnez    $t1, 1b
    addiu   $t0, $t0, 0x10
    li      $t2, 0xA0
    jr      $t2
    li      $t1, 0x44

    .section .bss
    .balign 4
bios_ctx: .space 64

# u32 bios_a0(u32 fn, u32 a0, u32 a1, u32 a2) / bios_b0 -- BIOS function tables (kernel ready)
    .text
    .global bios_a0
bios_a0:
    move    $t1, $a0
    move    $a0, $a1
    move    $a1, $a2
    move    $a2, $a3
    li      $t2, 0xA0
    jr      $t2
    nop
    .global bios_b0
bios_b0:
    move    $t1, $a0
    move    $a0, $a1
    move    $a1, $a2
    move    $a2, $a3
    li      $t2, 0xB0
    jr      $t2
    nop

# Last step of an EXE start, copied to 8000DF00h first (the BSS clear may cover OpenROM itself):
# void exe_tramp(u32 b_addr, u32 b_size, u32 pc, u32 gp_sp[2])
    .global exe_tramp, exe_tramp_end
exe_tramp:
    # The menu runs inside the debug exception that caught the shell (never returned with rfe):
    # start the program like the BIOS would, kernel mode and interrupts off in all three levels
    # of the SR mode stack (the program enables them itself).
    mfc0    $t0, $12
    li      $t1, 0xFFFFFFC0
    and     $t0, $t0, $t1
    mtc0    $t0, $12
    nop
    lw      $gp, 0($a3)
    lw      $sp, 4($a3)
    beqz    $a1, 2f
    nop
1:  sb      $zero, 0($a0)
    addiu   $a1, $a1, -1
    bnez    $a1, 1b
    addiu   $a0, $a0, 1
2:  move    $fp, $sp
    move    $ra, $zero
    jr      $a2
    nop
exe_tramp_end:
