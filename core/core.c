// OpenROM resident core (RAM at 8000C000h, in the unused kernel space) while a game runs.
// Only what runs often stays here; everything rare and big runs from the cartridge ROM
// (src/romx.c, entry points in the header at 1F0000F0h):
//   - kernel interrupt chain hook (SysEnqIntRP priority 0), on every interrupt:
//       PAL/NTSC forcing, GameShark codes, serial bytes -> ROM protocol handler,
//       Xplorer switch off->on, or L1+R1+L2+R2+START+SELECT -> in-game menu (ROM);
//       while the switch is off: cheats and serial paused
//   - TTY: BIOS putchar (A0h:3Ch, B0h:3Dh) -> serial port
//   - crash: A0h:40h (SystemErrorUnresolvedException) -> ROM crash report/screen
//   - hardware breakpoints: debug exception (bp.s) -> registers saved -> ROM halt loop
// State shared with the ROM code: src/shared.h.
#include "../src/shared.h"
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
#define REG8(a) (*(volatile u8 *)(a))
#define REG16(a) (*(volatile u16 *)(a))
#define REG32(a) (*(volatile u32 *)(a))

#define SIO_DATA REG8(0x1F801050)
#define SIO_STAT REG32(0x1F801054)
#define GP1 REG32(0x1F801814)

// call fn(arg) on the ROM code's own stack (the kernel's interrupt stack is small)
int call_on_stack(u32 fn, u32 arg, u32 sp);
void tcb_resume(void) __attribute__((noreturn));
extern u8 bp_entry[];

static int rom_ok(u32 fn) { return fn >= ROM_HDR + 0x100 && fn < ROM_HDR + 0x10000; }
static void rom_call(u32 fn, u32 arg) {
    if (!rom_ok(fn) || SH->in_rom) return;
    SH->in_rom = 1;
    call_on_stack(fn, arg, SH_STACK);
    SH->in_rom = 0;
}

// ------------------------------------------------------------------ video mode forcing
static void video_fix(void) {
    u32 st = GP1;
    int pal = (st >> 20) & 1, want = (SH->flags & F_PAL) ? 1 : 0;
    if (pal == want) return;
    u32 m = ((st >> 17) & 3) | (((st >> 19) & 1) << 2) | (want << 3) | (((st >> 21) & 1) << 4) |
            (((st >> 22) & 1) << 5) | (((st >> 16) & 1) << 6);
    GP1 = 0x08000000 | m;
    GP1 = want ? 0x07046C2B : 0x07040010;
}

// ------------------------------------------------------------------ cheats (GameShark format)
static void apply_cheats(void) {
    volatile Shared *s = SH;
    int n = s->nc;
    for (int i = 0; i < n; i++) {
        if (!s->c_on[i]) continue;
        u32 a = s->c_addr[i], t = a >> 24, ad = 0x80000000u | (a & 0x1FFFFF);
        u16 v = s->c_val[i];
        switch (t) {
        case 0x80: REG16(ad & ~1u) = v; break;
        case 0x30: REG8(ad) = (u8)v; break;
        case 0x10: REG16(ad & ~1u) += v; break;
        case 0x11: REG16(ad & ~1u) -= v; break;
        case 0x20: REG8(ad) += (u8)v; break;
        case 0x21: REG8(ad) -= (u8)v; break;
        case 0xD0: if (REG16(ad & ~1u) != v) i++; break;
        case 0xD1: if (REG16(ad & ~1u) == v) i++; break;
        case 0xD2: if (!(REG16(ad & ~1u) < v)) i++; break;
        case 0xD3: if (!(REG16(ad & ~1u) > v)) i++; break;
        case 0xE0: if (REG8(ad) != (u8)v) i++; break;
        case 0xE1: if (REG8(ad) == (u8)v) i++; break;
        case 0xE2: if (!(REG8(ad) < (u8)v)) i++; break;
        case 0xE3: if (!(REG8(ad) > (u8)v)) i++; break;
        case 0x50:
            if (i + 1 < n) {
                u32 cnt = (a >> 8) & 0xFF, step = a & 0xFF;
                u32 na = s->c_addr[i + 1], nt = na >> 24, nad = 0x80000000u | (na & 0x1FFFFF);
                u16 nv = s->c_val[i + 1];
                for (u32 k = 0; k < cnt; k++, nad += step, nv += v) {
                    if (nt == 0x80) REG16(nad & ~1u) = nv; else if (nt == 0x30) REG8(nad) = (u8)nv;
                }
                i++;
            }
            break;
        default: break;
        }
    }
}

// ------------------------------------------------------------------ pad combo -> in-game menu
// L1+R1+L2+R2+START+SELECT held: same as the Xplorer switch off->on. The core reads port 1 itself,
// on one vblank in 8 and only while SIO0 is idle (no select line asserted: never in the middle
// of the game's own pad or memory card transfer); the game's SIO0 settings are put back.
#define I_STAT REG32(0x1F801070)
#define SIO0_STAT REG32(0x1F801044)
#define SIO0_CTRL REG16(0x1F80104A)
// The pad is read by the ROM code (rom_combo, src/romx.c: no room here); this only decides when.
static u32 combo_tick;
static int combo_time(void) {
    if (!(I_STAT & 1) || (++combo_tick & 7)) return 0;        // vblank pending, 1 in 8
    return !(SIO0_CTRL & 0x0002) && (SIO0_STAT & 4);          // no transfer in progress
}

// ------------------------------------------------------------------ interrupt chain handler
static u32 *cur_tcb(void) { return *(u32 **)(*(u32 *)0x80000108); }
static u32 irq_verifier(void) {
    u32 f = SH->flags;
    // The kernel runs this chain for EVERY exception, registers already in the thread block.
    // A break (Bp, code 9) is handled here, before the BIOS's own break code (a dev-unit
    // feature that bus-errors on retail consoles): PCDRV or debug halt, then straight back
    // to the program (like ReturnFromException).
    if (f & (F_DEBUG | F_CRASH)) {
        u32 *t = cur_tcb();
        if (((t[38] >> 2) & 31) == 9) {
            u32 fn = ROM_BREAK;
            if (rom_ok(fn) && !SH->in_rom) {
                SH->in_rom = 1;
                int r = call_on_stack(fn, (u32)t, SH_STACK);
                SH->in_rom = 0;
                if (r == 1) tcb_resume();
            }
            return 0;
        }
    }
    if (f & (F_PAL | F_NTSC)) video_fix();
    u32 sw = REG8(0x1F060000) & 1;                    // Xplorer switch, 1 = on
    if ((f & F_MENU) && sw && !SH->sw_last) rom_call(ROM_MENU, 0);   // switch off -> on
#ifndef NOCOMBO
    else if ((f & F_MENU) && combo_time()) rom_call(ROM_COMBO, 0);    // or the pad combo
#endif
    SH->sw_last = sw;
    if (!sw) return 0;
    if (f & F_CHEATS) apply_cheats();
    if (f & F_DEBUG)
        while (SIO_STAT & 2) {
            SH->window = (SH->window >> 8) | ((u32)SIO_DATA << 24);
            rom_call(ROM_FEED, 0);                    // the ROM code reacts to known tokens
        }
    return 0;
}
static u32 irq_dummy(void) { return 0; }
static u32 int_rp[4];

// ------------------------------------------------------------------ TTY, crash, breakpoints
static void tty_putchar(int c) {
    if (c == '\n') { while (!(SIO_STAT & 1)); SIO_DATA = '\r'; }
    if (c) { while (!(SIO_STAT & 1)); SIO_DATA = (u8)c; }
}
// Unresolved exception (A0h:40h): a PCDRV request (break 101h-107h) is served by the ROM code,
// which returns 1, and the program goes on; anything else is a crash (report, never returns).
static void crash(void) {
    SH->in_rom = 0;
    u32 fn = ROM_CRASH;
    if (rom_ok(fn) && call_on_stack(fn, (u32)cur_tcb(), SH_STACK) == 1) tcb_resume();
    for (;;);
}
void bp_halted(void) {                                // from bp.s, already on its own stack
    u32 fn = ROM_HALTED;
    if (rom_ok(fn)) ((void (*)(void))fn)();
}

// ------------------------------------------------------------------ code finder scan
// Kept in RAM: scanning 2 MB from the cartridge code would take ~20 s (slow uncached bus).
// Finds value (width 1 or 2 bytes) in the game RAM 80010000h-801FFFFFh; stores up to max
// (address, value) pairs; returns how many were found in total.
static u32 scan(u32 value, u32 width, u32 *out, u32 max) {
    u32 n = 0;
    if (width == 2) {
        u16 v = (u16)value;
        for (volatile u16 *p = (u16 *)0x80010000; p < (u16 *)0x80200000; p++)
            if (*p == v) { if (n < max) { out[2 * n] = (u32)p; out[2 * n + 1] = v; } n++; }
    } else {
        u8 v = (u8)value;
        for (volatile u8 *p = (u8 *)0x80010000; p < (u8 *)0x80200000; p++)
            if (*p == v) { if (n < max) { out[2 * n] = (u32)p; out[2 * n + 1] = v; } n++; }
    }
    return n;
}

// ------------------------------------------------------------------ install
static void set_table(u32 table, u32 fn, void *p) { REG32(0x80000000u + table + fn * 4) = (u32)p; }

// codes: n x (address, value | enabled << 16)
__attribute__((section(".text.entry"))) void core_install(u32 f, const u32 *codes, int n) {
    volatile Shared *s = SH;
    for (u32 i = 0; i < sizeof(Shared) / 4; i++) ((volatile u32 *)s)[i] = 0;
    s->flags = f;
    for (int i = 0; i < n && i < MAXC; i++) {
        s->c_addr[i] = codes[2 * i];
        s->c_val[i] = (u16)codes[2 * i + 1];
        s->c_on[i] = (codes[2 * i + 1] >> 16) & 1;
    }
    s->nc = n < MAXC ? n : MAXC;
    s->sw_last = REG8(0x1F060000) & 1;
    s->bp_entry = (u32)bp_entry;
    s->scan_fn = (u32)scan;
    if (f & F_TTY) { set_table(0x200, 0x3C, (void *)tty_putchar); set_table(0x874, 0x3D, (void *)tty_putchar); }
    if (f & F_CRASH) set_table(0x200, 0x40, (void *)crash);
    // not twice in the chain (installed again after SetConf, which may or may not have reset it)
    int queued = 0;
    for (u32 *e = *(u32 **)REG32(0x80000100); e; e = (u32 *)e[0]) if (e == int_rp) queued = 1;
    if (!queued && (f & (F_DEBUG | F_CRASH | F_PAL | F_NTSC | F_CHEATS | F_MENU))) {
        int_rp[0] = 0;
        int_rp[1] = (u32)irq_dummy;
        int_rp[2] = (u32)irq_verifier;
        int_rp[3] = 0;
        register u32 t1 __asm__("t1") = 0x02;           // C0h:02h SysEnqIntRP(priority, struct)
        register u32 a0 __asm__("a0") = 0;
        register u32 a1 __asm__("a1") = (u32)int_rp;
        __asm__ volatile("jalr %3\n nop" : "+r"(t1), "+r"(a0), "+r"(a1) : "r"(0xC0)
                         : "ra", "v0", "v1", "a2", "a3", "t0", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9", "memory");
    }
}
