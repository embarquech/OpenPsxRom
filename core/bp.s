# Hardware breakpoint handler (COP0 debug exception, vector 80000040h).
# Saves every register into the thread control block that nops reads (/regs: [80000110h]),
# halts in the serial command loop until /cont, then resumes from the (possibly edited) block.
# TCB layout (words): 0 stat, 1 -, 2..33 r0..r31, 34 epc, 35 hi, 36 lo, 37 sr, 38 cause.
    .set noreorder
    .set noat
    .text

# void call_on_stack(u32 fn, u32 arg, u32 sp): fn(arg) on another stack
    .global call_on_stack
call_on_stack:
    addiu   $a2, $a2, -8
    sw      $sp, 0($a2)
    sw      $ra, 4($a2)
    move    $sp, $a2
    move    $t0, $a0
    jalr    $t0
    move    $a0, $a1
    lw      $ra, 4($sp)
    lw      $sp, 0($sp)
    jr      $ra
    nop

    .global bp_entry
bp_entry:
    mtc0    $zero, $7               # one shot: breakpoint off (else it fires again on return)
    la      $k1, bp_halted          # handler called once the registers are saved
# common: every register into the thread block, then the handler in k1 on our own stack
save_common:
    lui     $k0, 0x8000
    lw      $k0, 0x110($k0)         # TCB
    nop
    sw      $zero, 0x08($k0)
    sw      $1, 0x0C($k0)
    sw      $2, 0x10($k0)
    sw      $3, 0x14($k0)
    sw      $4, 0x18($k0)
    sw      $5, 0x1C($k0)
    sw      $6, 0x20($k0)
    sw      $7, 0x24($k0)
    sw      $8, 0x28($k0)
    sw      $9, 0x2C($k0)
    sw      $10, 0x30($k0)
    sw      $11, 0x34($k0)
    sw      $12, 0x38($k0)
    sw      $13, 0x3C($k0)
    sw      $14, 0x40($k0)
    sw      $15, 0x44($k0)
    sw      $16, 0x48($k0)
    sw      $17, 0x4C($k0)
    sw      $18, 0x50($k0)
    sw      $19, 0x54($k0)
    sw      $20, 0x58($k0)
    sw      $21, 0x5C($k0)
    sw      $22, 0x60($k0)
    sw      $23, 0x64($k0)
    sw      $24, 0x68($k0)
    sw      $25, 0x6C($k0)
    sw      $zero, 0x70($k0)
    sw      $zero, 0x74($k0)
    sw      $28, 0x78($k0)
    sw      $29, 0x7C($k0)
    sw      $30, 0x80($k0)
    sw      $31, 0x84($k0)
    mfc0    $t0, $14
    mfhi    $t1
    sw      $t0, 0x88($k0)
    sw      $t1, 0x8C($k0)
    mflo    $t0
    mfc0    $t1, $12
    sw      $t0, 0x90($k0)
    sw      $t1, 0x94($k0)
    mfc0    $t0, $13
    nop
    sw      $t0, 0x98($k0)

    lui     $sp, 0x8000             # own stack: 8000DF00h-8000DFEFh (the EXE start
    ori     $sp, $sp, 0xDFF0        # trampoline area, free while the game runs)
    jalr    $k1                     # bp_halted (serial loop until /cont) or bp_break
    nop

# resume the interrupted program from its thread block (after a PCDRV call)
    .global tcb_resume
tcb_resume:
    lui     $k0, 0x8000
    lw      $k0, 0x108($k0)         # process control block
    nop
    lw      $k0, 0($k0)             # current thread block (the kernel saved the registers there)
    nop
    lw      $t0, 0x8C($k0)
    lw      $t1, 0x90($k0)
    mthi    $t0
    mtlo    $t1
    lw      $t0, 0x94($k0)
    nop
    mtc0    $t0, $12
    lw      $1, 0x0C($k0)
    lw      $2, 0x10($k0)
    lw      $3, 0x14($k0)
    lw      $4, 0x18($k0)
    lw      $5, 0x1C($k0)
    lw      $6, 0x20($k0)
    lw      $7, 0x24($k0)
    lw      $8, 0x28($k0)
    lw      $9, 0x2C($k0)
    lw      $10, 0x30($k0)
    lw      $11, 0x34($k0)
    lw      $12, 0x38($k0)
    lw      $13, 0x3C($k0)
    lw      $14, 0x40($k0)
    lw      $15, 0x44($k0)
    lw      $16, 0x48($k0)
    lw      $17, 0x4C($k0)
    lw      $18, 0x50($k0)
    lw      $19, 0x54($k0)
    lw      $20, 0x58($k0)
    lw      $21, 0x5C($k0)
    lw      $22, 0x60($k0)
    lw      $23, 0x64($k0)
    lw      $24, 0x68($k0)
    lw      $25, 0x6C($k0)
    lw      $28, 0x78($k0)
    lw      $29, 0x7C($k0)
    lw      $30, 0x80($k0)
    lw      $31, 0x84($k0)
    lw      $k0, 0x88($k0)          # epc
    nop
    jr      $k0
    rfe
