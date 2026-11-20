// Bank 3 write test / flasher (second chip of a double-chip Xplorer FX, 1F040000h-1F05FFFFh), run from RAM:
//   nops /exe build/flashtest3.exe /m /dest COMx       (from the OpenPsxRom menu)
// make flash3 IMG=build/openrom_bank3.rom (or any 128 KB image built for 1F040000h): writes that
// image instead of the test pattern (build/flash3.exe).
// Reads the ID of both chips, writes a test pattern page by page (SST 29EE020 / Atmel page write:
// unlock AA-55-A0, 128 bytes < 150 us apart, ~10 ms internal cycle), then reads everything back
// and reports per 16 KB block on the serial port. No screen, no interrupts: nothing can delay a
// page load. Bank 1 (OpenPsxRom) is never written. The pattern has no "Licensed by Sony" header,
// so neither the BIOS nor OpenPsxRom will ever try to start it.
typedef unsigned char u8;
typedef unsigned int u32;
#define REG32(a) (*(volatile u32 *)(a))
#define SIO_DATA (*(volatile u8 *)0x1F801050)
#define SIO_STAT REG32(0x1F801054)

#define CHIP2 0xBF040000u                   // uncached
#define F2(o) (*(volatile u8 *)(CHIP2 + (o)))
#define F1(o) (*(volatile u8 *)(0xBF000000u + (o)))
#define SIZE 0x20000u                        // visible part of the second chip (registers above)
static int PAGE = 128;                // from the chip ID: SST 29EE020 128, Atmel AT29C0x0 256

static void putc_(int c) { for (u32 t = 0; t < 1000000 && !(SIO_STAT & 1); t++); SIO_DATA = (u8)c; }
static void puts_(const char *s) { while (*s) { if (*s == '\n') putc_('\r'); putc_(*s++); } }
static void hex(u32 v, int n) { while (n--) putc_("0123456789ABCDEF"[(v >> (n * 4)) & 15]); }
static void dec(u32 v) { char b[12]; int i = 0; do b[i++] = '0' + v % 10; while (v /= 10); while (i) putc_(b[--i]); }
static void pause(u32 n) { for (volatile u32 i = 0; i < n; i++); }

#ifdef IMAGE
extern const u8 _binary_image_bin_start[];          // the 128 KB image to write (objcopy blob)
static u8 pattern(u32 a) { return _binary_image_bin_start[a]; }
#else
static u8 pattern(u32 a) { return (u8)((a * 7) ^ (a >> 8) ^ 0x5A); }
#endif

static void cmd2(u8 c) { F2(0x5555) = 0xAA; pause(20); F2(0x2AAA) = 0x55; pause(20); F2(0x5555) = c; pause(20); }
static void cmd1(u8 c) { F1(0x5555) = 0xAA; pause(20); F1(0x2AAA) = 0x55; pause(20); F1(0x5555) = c; pause(20); }

static int unprotected;
static void ids(void) {
    cmd1(0x90); pause(5000);
    u8 m1 = F1(0), d1 = F1(1);
    cmd1(0xF0); pause(150000);
    u8 o0 = F2(0), o1 = F2(1);
    cmd2(0x90); pause(5000);
    u8 m2 = F2(0), d2 = F2(1);
    cmd2(0xF0); pause(150000);
    if (m2 == 0x1F && (d2 == 0xA4 || d2 == 0xDA)) PAGE = 256;
    // No ID answer from the second chip (same bytes as before the command): its software data
    // protection is off (seen on hardware: the unlock bytes AA/55/A0 then land as data at
    // 5555h/2AAAh). Pages are then written plainly, without the unlock sequence.
    unprotected = (m2 == o0 && d2 == o1) || !(m2 == 0xBF || m2 == 0x1F || m2 == 0xDA);
    puts_("chip 1 (1F000000) ID "); hex(m1, 2); putc_('/'); hex(d1, 2);
    puts_("   chip 2 (1F040000) ID "); hex(m2, 2); putc_('/'); hex(d2, 2);
    puts_("   page "); dec(PAGE); puts_(unprotected ? " bytes, chip 2 UNPROTECTED: plain page writes\n" : " bytes\n");
}

static u8 page_buf[256];
// 0 ok, else number of tries that failed (3 = gave up)
static int write_page(u32 a) {
    for (int i = 0; i < PAGE; i++) page_buf[i] = pattern(a + i);
    for (int k = 0; k < 3; k++) {
        // plain write first (chip without software data protection: no command at all, every
        // command byte would be stored as data); a protected chip ignores it, then the unlock
        if (k > 0 || !unprotected) cmd2(0xA0);
        for (int i = 0; i < PAGE; i++) F2(a + i) = page_buf[i];   // back to back: well under 150 us
        pause(3000);                                               // end of the page load
        for (u32 t = 0; t < 3000000; t++) {                        // DATA polling on the last byte
            if (!((F2(a + PAGE - 1) ^ page_buf[PAGE - 1]) & 0x80)) break;
        }
        pause(2000);
        int ok = 1;
        for (int i = 0; i < PAGE; i++) if (F2(a + i) != page_buf[i]) { ok = 0; break; }
        if (ok) return k;
        if (k == 0 && unprotected) {                               // the plain write did not stick:
            unprotected = 0;                                       // the chip is protected, keep the
            puts_("  chip 2 is protected: writes with the unlock sequence\n");   // unlock from now on
        }
    }
    return 3;
}

void main(void) {
#ifdef IMAGE
    puts_("\n=== bank 3 flasher (second chip): writing the embedded image ===\n");
#else
    puts_("\n=== bank 3 write test (second chip) ===\n");
#endif
    // No ID probe: on a chip without software data protection the probe itself is stored as
    // data. Pages are written plainly first, with the unlock sequence only if that did not stick.
    unprotected = 1;
    puts_("writing 128 KB, page by page...\n");
    u32 retries = 0, failed = 0;
    for (u32 a = 0; a < SIZE; a += PAGE) {
        int r = write_page(a);
        if (r == 3) failed++; else retries += r;
        if ((a & 0x3FFF) == 0x4000u - PAGE) {
            puts_("  block "); dec(a >> 14); puts_(" written, pages failed so far "); dec(failed);
            puts_(", retries "); dec(retries); puts_("\n");
        }
    }
    if (0) {   // (re-enabling the protection did not work on hardware: kept for reference)
        // Software data protection back on: a page written after AA-55-A0 enables it (SST/Atmel).
        // Then the pages holding the command addresses are written again, now with the unlock
        // sequence understood as a command (unprotected, those bytes landed as data).
        puts_("enabling the chip's software data protection...\n");
        unprotected = 0;
        write_page(0);
        write_page(0x2AAA & ~(u32)(PAGE - 1));
        write_page(0x5555 & ~(u32)(PAGE - 1));
        ids();
        puts_(unprotected ? "  still unprotected (no ID answer)\n" : "  protected again (ID answers)\n");
    }
    puts_("reading back...\n");
    u32 bad_total = 0, shown = 0;
    for (u32 b = 0; b < SIZE / 0x4000; b++) {
        u32 bad = 0;
        for (u32 i = 0; i < 0x4000; i++) {
            u32 a = b * 0x4000 + i;
            u8 v = F2(a), w = pattern(a);
            if (v != w) {
                bad++;
                if (shown < 16) { shown++; puts_("    "); hex(0x1F040000 + a, 8); puts_(" want "); hex(w, 2); puts_(" got "); hex(v, 2); puts_("\n"); }
            }
        }
        puts_("  block "); dec(b); puts_(bad ? ": BAD bytes " : ": ok"); if (bad) dec(bad); puts_("\n");
        bad_total += bad;
    }
    puts_(bad_total ? "RESULT: second chip has errors\n" : "RESULT: second chip written and verified, 128 KB ok\n");
    puts_("=== done: reset the console ===\n");
    for (;;);
}
