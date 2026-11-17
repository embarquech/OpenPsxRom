// PCDRV test program (run with: nops /debug, then nops /exe pcdrvtest.exe /m from a folder that
// holds TEST.TXT): reads TEST.TXT from the PC, writes OUT.TXT back, prints each step (printf goes
// to the serial port through OpenROM's debug core).
typedef unsigned int u32;

// PSn00bSDK calling convention (psxapi/_syscalls.s): args a1-a3, v0 error, v1 result
static int pc_call(u32 code_dummy, u32 a1, u32 a2, u32 a3, int which, int *result) {
    register u32 r_a1 __asm__("a1") = a1, r_a2 __asm__("a2") = a2, r_a3 __asm__("a3") = a3;
    register int v0 __asm__("v0"), v1 __asm__("v1");
    (void)code_dummy;
    switch (which) {
    case 1: __asm__ volatile("break 0, 0x101" : "=r"(v0), "=r"(v1) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory"); break;
    case 2: __asm__ volatile("break 0, 0x102" : "=r"(v0), "=r"(v1) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory"); break;
    case 3: __asm__ volatile("break 0, 0x103" : "=r"(v0), "=r"(v1) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory"); break;
    case 4: __asm__ volatile("break 0, 0x104" : "=r"(v0), "=r"(v1) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory"); break;
    case 5: __asm__ volatile("break 0, 0x105" : "=r"(v0), "=r"(v1) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory"); break;
    case 6: __asm__ volatile("break 0, 0x106" : "=r"(v0), "=r"(v1) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory"); break;
    default: __asm__ volatile("break 0, 0x107" : "=r"(v0), "=r"(v1) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory"); break;
    }
    *result = v1;
    return v0;
}

static int bios_printf(const char *fmt, int a, int b) {
    register const char *r_a0 __asm__("a0") = fmt;
    register int r_a1 __asm__("a1") = a, r_a2 __asm__("a2") = b;
    register u32 t1 __asm__("t1") = 0x3F;
    register int v0 __asm__("v0");
    __asm__ volatile("jalr %5\n nop" : "=r"(v0), "+r"(t1) : "r"(r_a0), "r"(r_a1), "r"(r_a2), "r"(0xA0)
                     : "ra", "v1", "a3", "t0", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9", "memory");
    return v0;
}

static char buf[256];
static const char msg[] = "written by the PS1 through OpenROM PCDRV\n";

int main(void) {
    int res, e;
    *(volatile char *)0x1F802023 = '0'; *(volatile char *)0x1F802023 = 10;     // TRACE
#ifndef NO_START_PRINTF
    bios_printf("pcdrvtest: start\n", 0, 0);
#endif
    *(volatile char *)0x1F802023 = '1'; *(volatile char *)0x1F802023 = 10;     // TRACE
    e = pc_call(0, 0, 0, 0, 1, &res);
    *(volatile char *)0x1F802023 = '2'; *(volatile char *)0x1F802023 = 10;     // TRACE
    bios_printf("PCinit -> %d\n", e, 0);
    e = pc_call(0, (u32)"TEST.TXT", 0, 0, 3, &res);
    int fd = res;
    bios_printf("PCopen TEST.TXT -> err %d fd %d\n", e, fd);
    if (e >= 0) {
        for (int i = 0; i < 256; i++) buf[i] = 0;
        e = pc_call(0, fd, 200, (u32)buf, 5, &res);
        bios_printf("PCread -> err %d len %d\n", e, res);
        bios_printf("content: %s\n", (int)buf, 0);
        e = pc_call(0, fd, 0, 0, 4, &res);
        bios_printf("PCclose -> %d\n", e, 0);
    }
    e = pc_call(0, (u32)"OUT.TXT", 0, 0, 2, &res);
    fd = res;
    bios_printf("PCcreat OUT.TXT -> err %d fd %d\n", e, fd);
    if (e >= 0) {
        e = pc_call(0, fd, sizeof msg - 1, (u32)msg, 6, &res);
        bios_printf("PCwrite -> err %d len %d\n", e, res);
        e = pc_call(0, fd, 0, 0, 4, &res);
        bios_printf("PCclose -> %d\n", e, 0);
    }
    bios_printf("pcdrvtest: done\n", 0, 0);
    for (;;);
}
