// Code run straight from the cartridge flash (execute in place, 1F00xxxxh) while a game runs.
// The linker puts every section of this file in the ROM (rom.ld), so string literals and
// constants are fine, but there must be no writable static data: state lives in the shared
// block (shared.h), buffers on the stack (8000DC00h-8000DEFFh, set by the core).
// Called by the resident core through the pointers at 1F0000F0h (header.s).
//   rom_feed    serial command dispatcher (NOPS protocol, debug subset)
//   rom_halted  a hardware breakpoint fired: report, then serve commands until /cont
//   rom_crash   unresolved exception: serial report + registers screen, commands still served
//   rom_menu    in-game menu (Xplorer switch off->on): cheats on/off, resume
//   rom_combo   pad combo check (core, 1 vblank in 8, SIO0 idle) -> rom_menu
#include "shared.h"
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
#define REG8(a) (*(volatile u8 *)(a))
#define REG16(a) (*(volatile u16 *)(a))
#define REG32(a) (*(volatile u32 *)(a))
#define GP0 REG32(0x1F801810)
#define GP1 REG32(0x1F801814)
#define I_STAT REG32(0x1F801070)
#define DMA_DPCR REG32(0x1F8010F0)
#define DMA2_MADR REG32(0x1F8010A0)
#define DMA2_BCR REG32(0x1F8010A4)
#define DMA2_CHCR REG32(0x1F8010A8)
#define SIO_DATA REG8(0x1F801050)
#define SIO_STAT REG32(0x1F801054)
#define SIO0_DATA REG8(0x1F801040)
#define SIO0_STAT REG16(0x1F801044)
#define SIO0_MODE REG16(0x1F801048)
#define SIO0_CTRL REG16(0x1F80104A)
#define SIO0_BAUD REG16(0x1F80104E)

static const u8 font8[][8] = {
#include "font.inc"
};

// ================================================================== boot
// Unpack the OMC1-compressed payload (see src/lz.c for the format) into RAM.
// Returns 0 if the result matches the CRC32 of the header, 1 otherwise (bad header or data read
// from the cartridge: the boot code then reads it again instead of running garbage).
int rom_unpack(const u8 *src, u8 *dst) {
    if (src[0] != 'O' || src[1] != 'M' || src[2] != 'C' || src[3] != '1') return 1;
    u32 n = src[4] | (src[5] << 8) | (src[6] << 16) | ((u32)src[7] << 24);
    u32 crc = src[8] | (src[9] << 8) | (src[10] << 16) | ((u32)src[11] << 24);
    u32 end = src[12] | (src[13] << 8) | (src[14] << 16) | ((u32)src[15] << 24);
    if (n > 0x50000 || end > 0x10000) return 1;
    const u8 *p = src + 16, *e = src + end;
    u8 *o = dst, *oe = dst + n;
    while (o < oe && p < e) {
        u8 flags = *p++;
        for (int bit = 0; bit < 8 && o < oe && p < e; bit++) {
            if (flags & (1 << bit)) *o++ = *p++;
            else {
                u32 d = (p[0] | ((p[1] >> 4) << 8)) + 1, l = (p[1] & 15) + 3;
                p += 2;
                while (l-- && o < oe) { *o = o[-(int)d]; o++; }
            }
        }
    }
    u32 c = 0xFFFFFFFFu;                               // CRC32 (no table: position independent)
    for (const u8 *q = dst; q < oe; q++) {
        c ^= *q;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return o != oe || ~c != crc;
}

// ================================================================== serial
static int rx_ready(void) { return (SIO_STAT & 2) != 0; }
static u8 getc_(void) { while (!(SIO_STAT & 2)); return SIO_DATA; }
static int getc_to(u32 n) { while (n--) if (SIO_STAT & 2) return SIO_DATA; return -1; }
static void putc_(u8 c) { while (!(SIO_STAT & 1)); SIO_DATA = c; }
// serial speed while a program runs (nops /fast, /medi, /psxbaud): divisor of 2073600 bauds
static void set_div(u32 d) {
    while (!(SIO_STAT & 4));                           // last byte fully sent
    REG16(0x1F80105E) = (u16)d;
    SH->baud_div = d;
}
static void puts_(const char *s) { while (*s) putc_((u8)*s++); }
static u32 rx32(void) { u32 v = 0; for (int i = 0; i < 4; i++) v |= (u32)getc_() << (8 * i); return v; }
static void tx32(u32 v) { for (int i = 0; i < 4; i++) putc_((u8)(v >> (8 * i))); }
static void hexs(u32 v) { for (int i = 7; i >= 0; i--) putc_("0123456789ABCDEF"[(v >> (i * 4)) & 15]); }

static void flush_cache(void) {
    register u32 t1 __asm__("t1") = 0x44;
    __asm__ volatile("jalr %1\n nop" : "+r"(t1) : "r"(0xA0) : "ra", "v0", "v1", "a0", "a1", "a2", "a3",
                     "t0", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9", "memory");
}

// ================================================================== NOPS protocol (in game)
static void accept(const char *resp) {
    puts_("OKV2");
    SH->proto = 1;
    u32 w = 0;
    for (int i = 0; i < 4; i++) { int c = getc_to(3000000); if (c < 0) break; w = (w >> 8) | ((u32)c << 24); }
    if (w == 0x32565055u) SH->proto = 2;
    puts_(resp);
}
static u32 sum8(const u8 *p, u32 n) { u32 s = 0; while (n--) s += *p++; return s; }
static void receive(u8 *dst, u32 len) {
    u32 done = 0;
    while (done < len) {
        u32 n = len - done < 2048 ? len - done : 2048;
        if (SH->proto < 2) { for (u32 i = 0; i < n; i++) dst[done + i] = getc_(); done += n; continue; }
        int lost = 0;
        for (u32 i = 0; i < n; i++) { int c = getc_to(20000000); if (c < 0) { lost = 1; break; } dst[done + i] = (u8)c; }
        puts_("CHEK");
        u32 want = rx32();
        if (lost || sum8(dst + done, n) != want) { puts_("ERR!"); continue; }
        puts_("MORE");
        done += n;
    }
}
static void send_block(u32 addr, u32 size, int more) {
    const volatile u8 *p = (const volatile u8 *)addr;
    u32 s = 0;
    for (u32 i = 0; i < size; i++) {
        u8 b = p[i];
        s += b;
        putc_(b);
        if (more > 0 && ((i + 1) & 2047) == 0) {       // see protocol.c: v18 paces, v14 does not
            u32 w = 0;
            int got = 0;
            for (int k = 0; k < 64; k++) {
                int c = getc_to(more == 2 ? 30000000 : 1500000);
                if (c < 0) break;
                w = (w >> 8) | ((u32)c << 24);
                if (w == 0x45524F4Du) { got = 1; break; }
            }
            more = got ? 2 : -1;
        }
    }
    if (more) tx32(s);
}
static void exec_jump(u32 pc, u32 gp, u32 sp) {
    __asm__ volatile("move $gp, %1\n move $sp, %2\n move $fp, %2\n jr %0\n nop" :: "r"(pc), "r"(gp), "r"(sp));
    for (;;);
}
static void cmd_sexe(void) {
    accept("OKAY");
    u32 f[10];                                          // header words 4..13 (pc0 .. s_size)
    for (int i = 0; i < 2048; i++) {
        u8 b = getc_();
        if (i >= 16 && i < 56) ((u8 *)f)[i - 16] = b;
    }
    u32 pc = rx32(), addr = rx32(), len = rx32();
    (void)rx32();
    receive((u8 *)addr, len);
    for (u32 i = 0; i < f[7]; i += 4) *(u32 *)(f[6] + i) = 0;
    flush_cache();
    exec_jump(pc, f[1], f[8] ? f[8] + f[9] : 0x801FFF00);
}

// hardware breakpoints: COP0 BPC(3)/BPCM(11) exec, BDA(5)/BDAM(9) data, DCIC(7) control.
// The debug exception vector (80000040h) jumps to the core's bp.s, which saves the registers.
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r "\n nop" :: "r"(v))
static void set_hook(u32 dcic, u32 addr, int data) {
    MTC0(7, 0);
    // vector: lui k0,hi; addiu k0,lo; jr k0; nop  (bp_entry address published by the core)
    u32 e = SH->bp_entry;
    u32 *v = (u32 *)0x80000040;
    v[0] = 0x3C1A0000 | ((e + 0x8000) >> 16);
    v[1] = 0x275A0000 | (e & 0xFFFF);
    v[2] = 0x03400008;
    v[3] = 0;
    flush_cache();
    if (data) { MTC0(5, addr); MTC0(9, 0xFFFFFFFF); }
    else { MTC0(3, addr); MTC0(11, 0xFFFFFFFF); }
    MTC0(7, dcic);
}

#define TOK(a, b, c, d) ((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))

// One command if SH->window holds a known token. Returns 1 when one was executed.
int rom_feed(void) {
    switch (SH->window) {
    case TOK('D','U','M','P'): { accept("OKAY"); u32 a = rx32(), n = rx32(); send_block(a, n, 1); break; }
    case TOK('H','E','X','D'): { accept("OKAY"); u32 a = rx32(), n = rx32(); send_block(a, n, 0); break; }
    case TOK('S','B','I','N'): {
        accept("OKAY");
        u32 a = rx32(), n = rx32();
        (void)rx32();
        if (n == 4 && !(a & 3)) { u32 v; receive((u8 *)&v, 4); REG32(a) = v; }
        else if (n == 2 && !(a & 1)) { u16 v; receive((u8 *)&v, 2); REG16(a) = v; }
        else receive((u8 *)a, n);
        flush_cache();
        break;
    }
    case TOK('S','E','X','E'): cmd_sexe(); break;
    case TOK('J','U','M','P'): { accept("OKAY"); u32 a = rx32(); flush_cache(); exec_jump(a, 0, 0x801FFF00); break; }
    case TOK('C','A','L','L'): { accept("OKAY"); u32 a = rx32(); flush_cache(); ((void (*)(void))a)(); break; }
    case TOK('P','I','N','G'): puts_("PONG"); break;
    case TOK('D','E','B','G'): accept("OKAY"); break;
    case TOK('H','A','L','T'):                          // freeze the game here until /cont
        accept("HLTD");
        SH->halted = 1;
        SH->window = 0;
        while (SH->halted)
            if (rx_ready()) { SH->window = (SH->window >> 8) | ((u32)getc_() << 24); rom_feed(); }
        return 1;
    case TOK('C','O','N','T'): accept("OKAY"); SH->halted = 0; break;
    case TOK('R','E','S','T'): exec_jump(0xBFC00000, 0, 0x801FFF00); break;
    case TOK('H','K','E','X'): { accept("OKAY"); u32 a = rx32(); set_hook(0xC1800000, a, 0); break; }
    case TOK('H','K','R','D'): { accept("PONG"); u32 a = rx32(); set_hook(0xC6800000, a, 1); break; }
    case TOK('H','K','W','R'): { accept("OKAY"); u32 a = rx32(); set_hook(0xCA800000, a, 1); break; }
    case TOK('U','N','H','K'): accept("OKAY"); MTC0(7, 0); break;
    case TOK('S','R','O','M'): case TOK('M','C','U','P'): case TOK('M','C','D','N'):
        puts_("UNSP"); break;
    // speed for the next command (then back to 115200, where every nops session starts)
    case TOK('F','A','S','T'): set_div(4); SH->window = 0; return 1;      // 518400
    case TOK('M','E','D','I'): set_div(5); SH->window = 0; return 1;      // 414720
    case TOK('S','L','O','W'): set_div(18); SH->window = 0; return 1;     // 115200
    case TOK('B','A','U','D'): {                       // nops /psxbaud N: BPLS, N (resent), BACP|NOPE
        puts_("BPLS");
        u32 n = rx32();
        u32 div = n ? 2073600u / n : 0;
        if (div < 1 || div > 73 || 2073600u / div != n) { puts_("NOPE"); SH->window = 0; return 1; }
        puts_("BACP");
        while (!(SIO_STAT & 4));
        while (rx_ready()) (void)SIO_DATA;            // the copies of N sent meanwhile
        set_div(div);
        SH->window = 0;
        return 1;
    }
    default: return 0;
    }
    SH->window = 0;
    if (SH->baud_div && SH->baud_div != 18) set_div(18);
    return 1;
}

static void serve_forever(void) {
    for (;;) if (rx_ready()) { SH->window = (SH->window >> 8) | ((u32)getc_() << 24); rom_feed(); }
}

// Breakpoint: registers already in the thread block. Tell nops, serve commands until /cont.
void rom_halted(void) {
    puts_("HLTD");
    SH->halted = 1;
    SH->window = 0;
    while (SH->halted)
        if (rx_ready()) { SH->window = (SH->window >> 8) | ((u32)getc_() << 24); rom_feed(); }
}

// ================================================================== drawing (no VRAM texture)
// Text is drawn by sending each glyph as a small image straight into the displayed frame
// (GP0 A0h by DMA from the stack), rectangles with GP0 02h: both ignore the game's drawing
// environment, so nothing of the game's GPU setup is changed.
static void gpu_idle(void) {
    for (u32 t = 0; t < 2000000; t++) if ((GP1 & (1u << 26)) && !(DMA2_CHCR & 0x01000000)) return;
}
static void gp0w(u32 w) { while (!(GP1 & (1u << 26))); GP0 = w; }
static void fill(int x, int y, int w, int h, u32 rgb) {
    gp0w(0x02000000 | (rgb & 0xFFFFFF));
    GP0 = (y << 16) | x;
    GP0 = (h << 16) | w;
}
static u16 c15(u32 rgb) { return ((rgb >> 3) & 31) | (((rgb >> 11) & 31) << 5) | (((rgb >> 19) & 31) << 10); }
static void glyph(int x, int y, int g, u16 fg, u16 bg) {
    u32 px[24];                                         // 6 x 8 pixels, 15-bit
    u16 *p = (u16 *)px;
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 6; b++) *p++ = (g > 0 && (font8[g][r] & (0x80 >> b))) ? fg : bg;
    gpu_idle();
    gp0w(0x01000000);
    gp0w(0xA0000000);
    GP0 = (y << 16) | x;
    GP0 = (8 << 16) | 6;
    u32 dir = (GP1 >> 29) & 3;                          // the game's DMA direction, restored after
    GP1 = 0x04000002;
    DMA_DPCR |= 0x800;
    DMA2_MADR = (u32)px;
    DMA2_BCR = (3 << 16) | 8;
    DMA2_CHCR = 0x01000201;
    while (DMA2_CHCR & 0x01000000);
    GP1 = 0x04000000 | dir;
}
static int text(int x, int y, const char *s, u32 fg, u32 bg) {
    u16 f = c15(fg), b = c15(bg);
    for (; *s; s++, x += 6) {
        int g = (u8)*s - 32;
        glyph(x, y, g >= 0 && g < 95 ? g : 0, f, b);
    }
    return x;
}
static void hex8(char *o, u32 v) { for (int i = 0; i < 8; i++) o[i] = "0123456789ABCDEF"[(v >> ((7 - i) * 4)) & 15]; o[8] = 0; }
static void hex4(char *o, u32 v) { for (int i = 0; i < 4; i++) o[i] = "0123456789ABCDEF"[(v >> ((3 - i) * 4)) & 15]; o[4] = 0; }

// display our 320x240 picture at VRAM (0,0); returns the game's GPUSTAT to restore the mode
static u32 screen_take(void) {
    u32 st = GP1;
    gpu_idle();
    GP1 = 0x08000001 | (((st >> 20) & 1) << 3);       // 320x240, 15-bit, game's PAL/NTSC
    GP1 = 0x05000000;
    GP1 = 0x03000000;
    return st;
}
static void screen_give(u32 st) {
    u32 m = ((st >> 17) & 3) | (((st >> 19) & 1) << 2) | (((st >> 20) & 1) << 3) | (((st >> 21) & 1) << 4) |
            (((st >> 22) & 1) << 5) | (((st >> 16) & 1) << 6);
    GP1 = 0x08000000 | m;                               // the game sets its display start each frame
}

// ================================================================== crash
static const char rn[] = "zratv0v1a0a1a2a3t0t1t2t3t4t5t6t7s0s1s2s3s4s5s6s7t8t9k0k1gpspfpra";
// ================================================================== PCDRV (host files, nops /m)
// break 0,101h..107h (PSn00bSDK / Sony SDK): args in a1-a3, returns v0 = error (<0), v1 = result.
// Wire (nops PCDrv.cs): 00 'p' + u32 function -> OKAY, then per function (tools/nops_protocol.md).
static int wait_reply(void) {                       // 1: OKAY, 0: NOPE / timeout
    u32 w = 0;
    for (int k = 0; k < 4096; k++) {
        int c = getc_to(60000000);
        if (c < 0) return 0;
        w = (w >> 8) | ((u32)c << 24);
        if (w == TOK('O','K','A','Y')) return 1;
        if (w == TOK('N','O','P','E')) return 0;
    }
    return 0;
}
static int pcdrv(u32 fn, u32 *r) {                  // r: r0..r31 of the program
    u32 a1 = r[5], a2 = r[6], a3 = r[7];
    putc_(0); putc_('p'); tx32(fn);
    if (!wait_reply()) return -1;
    switch (fn) {
    case 0x101: (void)getc_to(60000000); r[3] = 0; return 0;            // OKAY + 00
    case 0x102: case 0x103: {
        const char *n = (const char *)a1;
        for (int i = 0; i < 255 && n[i]; i++) putc_((u8)n[i]);
        putc_(0);
        tx32(a2);
        if (!wait_reply()) return -1;
        r[3] = rx32();
        return 0;
    }
    case 0x104:
        tx32(a1); tx32(0); tx32(0);
        if (!wait_reply()) return -1;
        r[3] = rx32();
        return 0;
    case 0x105: {
        tx32(a1); tx32(a2); tx32(a3);
        if (!wait_reply()) return -1;
        u32 len = rx32();
        (void)rx32();                                   // whole-file checksum (chunks are checked)
        SH->proto = 2;                                  // CHEK/MORE forced on by nops
        receive((u8 *)a3, len);
        r[3] = len;
        return 0;
    }
    case 0x106:
        tx32(a1); tx32(a2); tx32(a3);
        if (!wait_reply()) return -1;
        send_block(a3, a2, 1);
        if (!wait_reply()) return -1;
        r[3] = rx32();
        return 0;
    case 0x107:
        tx32(a1); tx32(a2); tx32(a3);
        if (!wait_reply()) return -1;
        r[3] = rx32();
        return 0;
    }
    return -1;
}

// From the core on an unresolved exception: 1 = PCDRV call served (resume), else crash screen.
static int try_pcdrv(u32 *tcb) {
    u32 cause = tcb[38], epc = tcb[34];
    if (((cause >> 2) & 31) != 9 || (cause & 0x80000000)) return 0;   // Bp, not in a delay slot
    u32 ins = *(const u32 *)epc;
    if ((ins & 0xFC00003F) != 0x0D) return 0;           // break
    u32 code = (ins >> 6) & 0xFFFFF;                    // PSn00bSDK: break 0, 101h
    if (code < 0x101 || code > 0x107) {
        code = (ins >> 16) & 0x3FF;                     // Sony SDK: break 101h (code in bits 16-25)
        if (code < 0x101 || code > 0x107 || ((ins >> 6) & 0x3FF)) return 0;
    }
    u32 *r = tcb + 2;
    r[2] = pcdrv(code, r);                              // v0: 0 ok, -1 error
    tcb[34] = epc + 4;                                  // continue after the break
    return 1;
}

// From the core's interrupt-chain hook on a break: PCDRV, or a halt in debug mode (GDB software
// breakpoint); 1 = resume the program, 0 = leave it to the BIOS.
int rom_break(u32 *tcb) {
    if (try_pcdrv(tcb)) return 1;
    if (SH->flags & F_DEBUG) {
        rom_halted();
        flush_cache();
        return 1;
    }
    return 0;
}

int rom_crash(u32 *tcb) {
    if (try_pcdrv(tcb)) return 1;
    // debug mode: a break (GDB software breakpoint) or a fault halts like a hardware breakpoint:
    // registers already in the thread block, HLTD, commands served until /cont, then resume
    // (GDB/nops may have changed the pc and the code meanwhile)
    if (SH->flags & F_DEBUG) {
        rom_halted();
        flush_cache();
        return 1;
    }
    const u32 *r = tcb + 2;
    char s[12];
    puts_("\r\n*** OpenPsxRom: CRASH ***\r\nepc "); hexs(tcb[34]); puts_(" cause "); hexs(tcb[38]); puts_(" badv "); hexs(tcb[1]);
    for (int i = 0; i < 32; i++) { puts_(i % 4 ? "  " : "\r\n"); putc_(rn[i * 2]); putc_(rn[i * 2 + 1]); putc_(' '); hexs(r[i]); }
    puts_("\r\nserial commands still work (nops /dump ...)\r\n");
    GP1 = 0x00000000; GP1 = 0x03000001; GP1 = 0x06C60260;
    int pal = REG8(0xBFC7FF52) == 'E';
    GP1 = pal ? 0x07046C2B : 0x07040010;
    GP1 = 0x08000001 | (pal ? 8 : 0);
    GP1 = 0x05000000;
    fill(0, 0, 320, 240, 0x402010);
    GP1 = 0x03000000;
    u32 bg = 0x102040;
    text(16, 12, "OpenPsxRom - the program crashed", 0x4040FF, bg);
    text(16, 30, "pc", 0xFFFF40, bg); hex8(s, tcb[34]); text(40, 30, s, 0xFFFFFF, bg);
    text(112, 30, "cause", 0xFFFF40, bg); hex8(s, tcb[38]); text(150, 30, s, 0xFFFFFF, bg);
    text(16, 42, "badv", 0xFFFF40, bg); hex8(s, tcb[1]); text(52, 42, s, 0xFFFFFF, bg);
    for (int i = 0; i < 32; i++) {
        int x = 16 + (i % 3) * 100, y = 60 + (i / 3) * 12;
        char n[3] = {rn[i * 2], rn[i * 2 + 1], 0};
        text(x, y, n, 0xFFFF40, bg);
        hex8(s, r[i]);
        text(x + 18, y, s, 0xFFFFFF, bg);
    }
    text(16, 200, "Serial still works: nops /dump, /exe ...", 0x80C080, bg);
    text(16, 212, "Press reset to restart.", 0x80C080, bg);
    serve_forever();
    return 0;
}

// ================================================================== in-game menu
static void pad_init(void) { SIO0_CTRL = 0x40; SIO0_BAUD = 0x88; SIO0_MODE = 0x0D; SIO0_CTRL = 0; }
static void dly(int n) { for (volatile int i = 0; i < n; i++); }
static int xfer(u8 out, u8 *in, int ack) {
    I_STAT = ~0x80u;
    SIO0_CTRL |= 0x10;
    while (!(SIO0_STAT & 1));
    SIO0_DATA = out;
    int t = 0;
    while (!(SIO0_STAT & 2)) if (++t > 5000) return 0;
    *in = SIO0_DATA;
    if (!ack) return 1;
    t = 0;
    while (!(I_STAT & 0x80)) if (++t > 3000) return 0;
    return 1;
}
static u16 pad_read(void) {
    u8 b[5] = {0, 0, 0xFF, 0xFF, 0xFF};
    for (int i = 0; i < 16 && (SIO0_STAT & 2); i++) (void)SIO0_DATA;
    SIO0_CTRL = 0x1003;
    dly(300);
    int ok = xfer(0x01, &b[0], 1) && xfer(0x42, &b[1], 1) && xfer(0x00, &b[0], 1) &&
             xfer(0x00, &b[2], 1) && xfer(0x00, &b[3], 0);
    SIO0_CTRL = 0;
    I_STAT = ~0x80u;
    return ok ? (u16)~(b[2] | (b[3] << 8)) : 0;
}
enum { P_SELECT = 1, P_START = 8, P_UP = 16, P_DOWN = 64, P_CROSS = 1 << 14, P_CIRCLE = 1 << 13 };
static void vsync(void) {
    I_STAT = ~1u;
    for (u32 t = 0; t < 1000000 && !(I_STAT & 1); t++);
    I_STAT = ~1u;
}

#define MROWS 11
static const u32 BG = 0x301808, HL = 0x905020, HEAD = 0x6A3010;

static void frame_head(const char *title, const char *help) {
    fill(0, 0, 320, 240, BG);
    fill(0, 0, 320, 34, HEAD);
    text(16, 18, title, 0x40D8FF, HEAD);
    text(16, 212, help, 0xA0A0A0, BG);
}
static void row(int y, int sel, const char *s, u32 col) {
    u32 b = sel ? HL : BG;
    if (sel) fill(8, y - 3, 304, 13, HL);
    text(16, y, s, sel ? 0x40FFFF : col, b);
}
static char *dec5(char *o, u32 v) {               // 5 digits, leading zeros
    for (int i = 4; i >= 0; i--) { o[i] = '0' + v % 10; v /= 10; }
    o[5] = 0;
    return o + 5;
}
static char *cat(char *o, const char *s) { while (*s) *o++ = *s++; *o = 0; return o; }

// pad with edge detection, inside the menu (the game is frozen)
#define PAD_INIT_OLD 0xFFFF
static u16 pad_hits(u16 *old) {
    vsync();
    // debounced: a press counts at once, a release only after 3 reads in a row without the button
    // (a contact bounce or a failed read used to give on/off/on for one push on a cheat)
    u16 raw = pad_read();
    u32 held = SH->pad_held;
    for (int i = 0; i < 16; i++) {
        u32 m = 1u << i;
        if (raw & m) { held |= m; SH->pad_off[i] = 0; }
        else if ((held & m) && ++SH->pad_off[i] >= 3) held &= ~m;
    }
    SH->pad_held = held;
    u16 now = (u16)held, hit = now & ~*old;
    *old = now;
    return hit;
}

// Number entry: decimal digits, up/down change the digit, left/right move, L1/R1 toggle the size
// (if width != 0). Returns 0 if cancelled.
enum { P_LEFT = 128, P_RIGHT = 32, P_L1 = 1 << 10, P_R1 = 1 << 11, P_TRI = 1 << 12, P_SQR = 1 << 15 };
static int edit_number(const char *title, u32 *value, u32 *width) {
    u8 d[5];
    u32 v = *value;
    for (int i = 4; i >= 0; i--) { d[i] = v % 10; v /= 10; }
    int cur = 4, dirty = 1;
    u16 old = PAD_INIT_OLD;
    for (;;) {
        if (dirty) {
            frame_head(title, width ? "UP/DOWN digit  L1/R1 size  X ok  O cancel" : "UP/DOWN digit  X ok  O cancel");
            for (int i = 0; i < 5; i++) {
                char c[2] = {(char)('0' + d[i]), 0};
                if (i == cur) fill(118 + i * 14, 77, 12, 13, HL);
                text(121 + i * 14, 80, c, i == cur ? 0x40FFFF : 0xFFFFFF, i == cur ? HL : BG);
            }
            if (width) text(110, 110, *width == 2 ? "16-bit value" : "8-bit value", 0xC0C0C0, BG);
            dirty = 0;
        }
        u16 h = pad_hits(&old);
        if (h & P_LEFT && cur > 0) { cur--; dirty = 1; }
        if (h & P_RIGHT && cur < 4) { cur++; dirty = 1; }
        if (h & P_UP) { d[cur] = (d[cur] + 1) % 10; dirty = 1; }
        if (h & P_DOWN) { d[cur] = (d[cur] + 9) % 10; dirty = 1; }
        if (width && (h & (P_L1 | P_R1))) { *width = *width == 2 ? 1 : 2; dirty = 1; }
        if (h & P_CIRCLE) return 0;
        if (h & P_CROSS) {
            u32 n = 0;
            for (int i = 0; i < 5; i++) n = n * 10 + d[i];
            u32 lim = (width ? *width : 2) == 2 ? 65535 : 255;
            *value = n > lim ? lim : n;
            return 1;
        }
    }
}

static void message(const char *l1, const char *l2) {
    frame_head("Code finder", "X / O continue");
    text(16, 70, l1, 0xFFFFFF, BG);
    if (l2) text(16, 86, l2, 0xC0C0C0, BG);
    u16 old = PAD_INIT_OLD;
    for (;;) if (pad_hits(&old) & (P_CROSS | P_CIRCLE)) return;
}

static u32 cand_read(u32 a) { return SH->cand_width == 2 ? *(volatile u16 *)a : *(volatile u8 *)a; }

// keep the candidates that pass: 0 equal to v, 1 changed, 2 unchanged, 3 increased, 4 decreased
static void cand_filter(int how, u32 v) {
    u32 *c = (u32 *)CAND_ADDR;
    u32 n = SH->cand_n, k = 0;
    for (u32 i = 0; i < n; i++) {
        u32 a = c[2 * i], old = c[2 * i + 1], now = cand_read(a);
        int keep = how == 0 ? now == v : how == 1 ? now != old : how == 2 ? now == old :
                   how == 3 ? now > old : now < old;
        if (keep) { c[2 * k] = a; c[2 * k + 1] = now; k++; }
    }
    SH->cand_n = k;
}

static void finder(void) {
    static const char *const ops[5] = {"Keep: equal to a value", "Keep: changed", "Keep: unchanged",
                                       "Keep: increased", "Keep: decreased"};
    int sel = 0, top = 0, dirty = 1;
    u16 old = PAD_INIT_OLD;
    for (;;) {
        u32 n = SH->cand_n;
        int nops = n ? 5 : 0, nitems = 1 + nops + (int)n;
        if (sel >= nitems) sel = nitems - 1;
        if (sel < top) top = sel;
        if (sel >= top + MROWS) top = sel - MROWS + 1;
        if (dirty) {
            char s[48], *o;
            frame_head("Code finder", "X select   O back");
            for (int i = top; i < nitems && i < top + MROWS; i++) {
                int y = 46 + (i - top) * 14;
                if (i == 0) { o = cat(s, "New search ("); o = dec5(o, n); cat(o, " results)"); row(y, i == sel, s, 0xE0E0E0); }
                else if (i <= nops) row(y, i == sel, ops[i - 1], 0xE0E0E0);
                else {
                    u32 *c = (u32 *)CAND_ADDR + 2 * (i - 1 - nops);
                    hex8(s, c[0]);
                    o = cat(s + 8, "  now ");
                    o = dec5(o, cand_read(c[0]));
                    o = cat(o, "  was ");
                    dec5(o, c[1]);
                    row(y, i == sel, s, 0xC0FFC0);
                }
            }
            dirty = 0;
        }
        u16 h = pad_hits(&old);
        if (h & P_UP) { sel = (sel + nitems - 1) % nitems; dirty = 1; }
        if (h & P_DOWN) { sel = (sel + 1) % nitems; dirty = 1; }
        if (h & P_CIRCLE) return;
        if (!(h & P_CROSS)) continue;
        dirty = 1;
        if (sel == 0) {                                     // new search for a known value
            u32 v = 0, w = SH->cand_width ? SH->cand_width : 2;
            if (!edit_number("New search: current value", &v, &w)) continue;
            frame_head("Code finder", "");
            text(16, 80, "searching the game RAM...", 0xFFFFFF, BG);
            SH->cand_width = w;
            u32 total = ((u32 (*)(u32, u32, u32 *, u32))SH->scan_fn)(v, w, (u32 *)CAND_ADDR, CAND_MAX);
            SH->cand_n = total < CAND_MAX ? total : CAND_MAX;
            if (total > CAND_MAX) {
                char m[48], *o = cat(m, "found ");
                o = dec5(o, total);
                cat(o, ": too many, only 384 kept");
                message(m, "Better: search a rarer value (e.g. money).");
            }
            sel = 0;
        } else if (sel <= nops) {                           // refine
            u32 v = 0;
            if (sel == 1 && !edit_number("Keep the ones equal to", &v, 0)) continue;
            cand_filter(sel - 1, v);
        } else {                                            // make a cheat code from a result
            u32 *c = (u32 *)CAND_ADDR + 2 * (sel - 1 - nops);
            u32 v = cand_read(c[0]);
            if (!edit_number("Cheat: value to keep", &v, 0)) continue;
            u32 i = SH->nc;
            if (i >= MAXC) { message("no room for more codes (96)", 0); continue; }
            SH->c_addr[i] = (SH->cand_width == 2 ? 0x80000000u : 0x30000000u) | (c[0] & 0x1FFFFF);
            SH->c_val[i] = (u16)v;
            SH->c_on[i] = 1;
            SH->nc = i + 1;
            SH->flags |= F_CHEATS;
            char m[32], *o;
            hex8(m, SH->c_addr[i]);
            o = cat(m + 8, " ");
            hex4(o, v);
            message("Code added and active:", m);
        }
    }
}

static void menu_draw(int sel, int top) {
    frame_head("OpenPsxRom - in-game menu", "X select / on-off   START or O resume");
    int n = SH->nc + 2;
    for (int i = top; i < n && i < top + MROWS; i++) {
        int y = 46 + (i - top) * 14;
        if (i == 0) { row(y, i == sel, "Resume the game", 0xE0E0E0); continue; }
        if (i == 1) { row(y, i == sel, "Code finder", 0xE0E0E0); continue; }
        int c = i - 2;
        char s[24], *o;
        hex8(s, SH->c_addr[c]);
        o = cat(s + 8, " ");
        hex4(o, SH->c_val[c]);
        u32 b = i == sel ? HL : BG;
        if (i == sel) fill(8, y - 3, 304, 13, HL);
        int x = text(16, y, s, i == sel ? 0x40FFFF : 0xE0E0E0, b);
        text(x + 24, y, SH->c_on[c] ? "on " : "off", SH->c_on[c] ? 0x40FF40 : 0x808080, b);
    }
    if (SH->nc == 0) text(16, 88, "no cheat code (add some, or use the finder)", 0x808080, BG);
}

void rom_menu(void) {
    u16 sm = SIO0_MODE, sb = SIO0_BAUD, sc = SIO0_CTRL;
    u32 st = screen_take();
    pad_init();
    int sel = 0, top = 0, dirty = 1;
    u16 old = PAD_INIT_OLD;                             // ignore buttons held when it opens
    for (int k = 0; k < 4; k++) { vsync(); old = pad_read(); }
    for (;;) {
        int n = SH->nc + 2;
        if (sel < top) top = sel;
        if (sel >= top + MROWS) top = sel - MROWS + 1;
        if (dirty) { menu_draw(sel, top); dirty = 0; }
        u16 hit = pad_hits(&old);
        if (hit & P_UP) { sel = (sel + n - 1) % n; dirty = 1; }
        if (hit & P_DOWN) { sel = (sel + 1) % n; dirty = 1; }
        if (hit & (P_START | P_CIRCLE)) break;
        if (hit & P_CROSS) {
            if (sel == 0) break;
            if (sel == 1) { finder(); old = PAD_INIT_OLD; }
            else SH->c_on[sel - 2] ^= 1;
            dirty = 1;
        }
    }
    fill(0, 0, 320, 240, 0);
    screen_give(st);
    SIO0_MODE = sm; SIO0_BAUD = sb; SIO0_CTRL = sc;
}

// L1+R1+L2+R2+START+SELECT on pad 1: the in-game menu, as the Xplorer switch off->on. Called by the
// core on one vblank in 8, only while SIO0 is idle; the game's SIO0 settings are put back.
void rom_combo(void) {
    enum { COMBO = 0x0F09 };
    u16 sm = SIO0_MODE, sb = SIO0_BAUD, sc = SIO0_CTRL;
    pad_init();
    u32 held = (pad_read() & COMBO) == COMBO;
#ifdef COMBOTEST
    { static volatile u32 *n = (volatile u32 *)(SH_ADDR + 0x3FC); if (++*n == 15) held = 1; }  // emulator test
#endif
    SIO0_MODE = sm; SIO0_BAUD = sb; SIO0_CTRL = sc & ~0x10;
    u32 last = SH->combo_last;
    SH->combo_last = held;
    if (held && !last) rom_menu();
}
