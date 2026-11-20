# start-up of test/flashtest3.c (run from RAM at 80010000h): stack, BSS cleared, main
    .set noreorder
    .section .text.start, "ax"
    .global _start
_start:
    la    $sp, 0x801FFF00
    la    $t0, __bss_start
    la    $t1, __bss_end
1:  sltu  $t2, $t0, $t1
    beqz  $t2, 2f
    nop
    sw    $zero, 0($t0)
    b     1b
    addiu $t0, $t0, 4
2:  j     main
    nop
