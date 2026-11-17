// NOPS (NOTPSXSerial) protocol, PS1 side. Spec: tools/nops_protocol.md (from the nops source).
//
// Idle: a rolling 4-byte window is checked every frame from the menu; a known command is then
// handled to completion (blocking). Supported: SBIN (/bin, /poke*), SEXE (/exe), SROM (/rom),
// DUMP (/dump), HEXD (/watch), JUMP (/jmp), CALL (/jal), PING, REST, SLOW/MEDI/FAST.
// Others get UNSP (nops then aborts cleanly).
#include "rom.h"

#define ROM_BUF ((u8 *)0x80100000)      // /rom upload buffer (128 KB), as Unirom does
#define CHUNK 2048

static u32 window;              // last 4 bytes received
static int proto;               // 1: streamed, 2: CHEK/MORE per 2 KB chunk

static void status(const char *a, u32 v, int hex) {
    char *p = fmt_str(sio_status, a);
    if (hex >= 0) { p = fmt_str(p, hex ? " 0x" : " "); if (hex) fmt_hex(p, v, 8); else fmt_dec(p, v); }
}

// receive with a timeout (loops): -1 on timeout
static int rx_to(u32 loops) {
    while (loops--) if (sio_rx_ready()) return sio_getc();
    return -1;
}
static int aborted;
#define RX_LIMIT 60000000u                          // ~10 s without a byte: the PC is gone
static u8 rxb(void) {
    if (aborted) return 0;
    int c = rx_to(RX_LIMIT);
    if (c < 0) { aborted = 1; status("serial: timeout, command abandoned", -1, -1); return 0; }
    return (u8)c;
}
static u32 rx32(void) {
    u32 v = 0;
    for (int i = 0; i < 4; i++) v |= (u32)rxb() << (8 * i);
    return v;
}
static void tx32(u32 v) { for (int i = 0; i < 4; i++) sio_putc((u8)(v >> (8 * i))); }

static u32 sum8(const u8 *p, u32 n) { u32 s = 0; while (n--) s += *p++; return s; }

// Handshake: offer protocol V2 (CHEK/MORE flow control), then send the expected response.
static void accept(const char *resp) {
    sio_puts("OKV2");
    proto = 1;
    u32 w = 0;
    for (int i = 0; i < 4; i++) {
        int c = rx_to(3000000);           // ~100-200 ms: an older nops may not answer
        if (c < 0) break;
        w = (w >> 8) | ((u32)c << 24);
    }
    if (w == 0x32565055u) proto = 2;      // "UPV2"
    sio_puts(resp);
}

// Receive len bytes to dst. V2: CHEK / sum / MORE|ERR! per 2 KB chunk. Returns 0 if ok.
static int receive(u8 *dst, u32 len) {
    u32 done = 0;
    while (done < len) {
        u32 n = len - done < CHUNK ? len - done : CHUNK;
        if (proto < 2) {
            for (u32 i = 0; i < n && !aborted; i++) dst[done + i] = rxb();
            if (aborted) return -1;
            done += n;
            continue;
        }
        int lost = 0;
        for (u32 i = 0; i < n; i++) {
            int c = rx_to(20000000);
            if (c < 0) { lost = 1; break; }
            dst[done + i] = (u8)c;
        }
        sio_puts("CHEK");
        u32 want = rx32();
        if (aborted) return -1;
        if (lost || sum8(dst + done, n) != want) { sio_puts("ERR!"); continue; }
        sio_puts("MORE");
        done += n;
        status("receiving", done, 0);
    }
    return 0;
}

static void flush_rx(void) { while (rx_to(200000) >= 0); }

// --------------------------------------------------------------------------- commands
static void cmd_sbin(void) {
    accept("OKAY");
    u32 addr = rx32(), len = rx32(), sum = rx32();
    if (len <= 4 && ((addr & (len - 1)) == 0) && len != 3) {         // /poke8/16/32: one access
        u8 b[4];
        receive(b, len);
        if (len == 1) *(volatile u8 *)addr = b[0];
        else if (len == 2) *(volatile u16 *)addr = b[0] | (b[1] << 8);
        else if (len == 4) *(volatile u32 *)addr = b[0] | (b[1] << 8) | (b[2] << 16) | ((u32)b[3] << 24);
        status("poke", addr, 1);
        return;
    }
    receive((u8 *)addr, len);
    flush_cache();
    status(sum8((u8 *)addr, len) == sum ? "bin ok at" : "bin CHECKSUM ERROR at", addr, 1);
}

static u8 exe_hdr[2048];
static void cmd_sexe(void) {
    accept("OKAY");
    for (int i = 0; i < 2048; i++) exe_hdr[i] = rxb();
    u32 pc = rx32(), addr = rx32(), len = rx32(), sum = rx32();
    if (exe_overlaps_rom(addr, len)) {
        // would overwrite OpenROM itself: swallow the data and refuse to run it
        static u8 junk[CHUNK];
        for (u32 d = 0; d < len; d += CHUNK) receive(junk, len - d < CHUNK ? len - d : CHUNK);
        status("exe overlaps OpenPsxRom (801A0000-801F0000)", -1, -1);
        return;
    }
    receive((u8 *)addr, len);
    if (sum8((u8 *)addr, len) != sum) { status("exe CHECKSUM ERROR", -1, -1); return; }
    ((u32 *)exe_hdr)[0x10 / 4] = pc;
    exe_start(exe_hdr);
}

// /mcup CARD FILE: raw 128 KB image written frame by frame, then read back
static void mc_prog(int f) { ui_progress("memory card", f, 1024); }
static void cmd_mcup(void) {
    accept("OKAY");
    u32 card = rx32() & 1, len = rx32(), sum = rx32();
    if (len > 131072) len = 131072;
    memset(ROM_BUF, 0, 131072);
    receive(ROM_BUF, len);
    if (sum8(ROM_BUF, len) != sum) { status("card image CHECKSUM ERROR, nothing written", -1, -1); return; }
    if (len != 131072) { status("card image is not 128 KB, nothing written", len, 0); return; }
    int bad = mc_write_card(card, ROM_BUF, mc_prog);
    if (bad == 1024) status("no memory card in slot", card + 1, 0);
    else if (bad) status("memory card: frames failed", bad, 0);
    else status("memory card written, slot", card + 1, 0);
}

// /mcdown CARD FILE: read the card to RAM, then nops fetches it with a DUMP
static void cmd_mcdn(void) {
    accept("OKAY");                         // nops v14 waits for OKAY, v18 for HLTD: send both
    sio_puts("HLTD");
    u32 card = rx32() & 1;
    int bad = mc_read_card(card, ROM_BUF, mc_prog);
    if (bad == 1024) { sio_puts("HECK"); status("no memory card in slot", card + 1, 0); return; }
    sio_puts("MCRD");
    tx32((u32)ROM_BUF);
    tx32(131072);
    status(bad ? "memory card read, bad frames:" : "memory card read, slot", bad ? bad : card + 1, 0);
}

static void flash_progress(u32 s, const char *what) {
    if (s >> 8) return;                         // page steps: only the block steps are shown
    char *p = fmt_str(sio_status, "flash block ");
    p = fmt_dec(p, s);
    p = fmt_str(p, ": ");
    fmt_str(p, what);
    gfx_rect(0, 222, 320, 18, 0x301808);        // the menu is not drawn meanwhile: direct status
    gfx_text(8, 227, C_SEL, sio_status);
    gfx_flip();
    gfx_rect(0, 222, 320, 18, 0x301808);
    gfx_text(8, 227, C_SEL, sio_status);
    gfx_flip();
}

static void cmd_srom(void) {
    accept("OKAY");
    u32 len = rx32(), sum = rx32();
    u8 mfr, dev;
    int known = flash_identify(&mfr, &dev);
    if (mfr == 0xFF && dev == 0xFF) { sio_puts("NONE"); status("rom: no flash chip", -1, -1); return; }
    if (!known) { sio_puts("UNKN"); status("rom: unknown chip", (mfr << 8) | dev, 1); return; }
    if (len > FLASH_SIZE) { sio_puts("NOPE"); status("rom too big", len, 0); return; }
    sio_puts("FITS");
    memset(ROM_BUF, 0xFF, FLASH_SIZE);
    receive(ROM_BUF, len);
    if (sum8(ROM_BUF, len) != sum) { status("rom CHECKSUM ERROR, nothing written", -1, -1); return; }
    // never write a ROM that the BIOS would not start
    const char *lic = "Licensed by Sony";
    int ok84 = 1;
    for (int i = 0; lic[i]; i++) if (ROM_BUF[0x84 + i] != (u8)lic[i]) ok84 = 0;
    if (!ok84) { status("rom has no header at 84h, nothing written", -1, -1); return; }
    extern int is_openrom_image(const u8 *img);
    int bank = rom_image_bank(ROM_BUF);               // image for bank 2 or 3: OpenROM kept
    if (bank > 1) {
        extern int bank_available(int bank);
        if (!bank_available(bank)) { status(bank == 2 ? "rom for bank 2: no room on this chip" : "rom for bank 3: no second chip", -1, -1); return; }
        int bad = flash_write_at(bank == 2 ? ROM2_OFF : ROM3_OFF, ROM_BUF, FLASH_SIZE, flash_progress);
        if (bad == 0) status(bank == 2 ? "rom written in bank 2 and verified" : "rom written in bank 3 and verified", crc32(ROM_BUF, FLASH_SIZE), 1);
        else status("rom bank FAILED blocks:", bad, 0);
        return;
    }
    int bad = flash_write(ROM_BUF, is_openrom_image(ROM_BUF) ? FL_ROM_SIZE : FLASH_SIZE, flash_progress);
    if (bad == 0) status("rom written and verified, reset to boot it", crc32(ROM_BUF, FLASH_SIZE), 1);
    else status("rom FAILED blocks:", bad, 0);
}

static void send_block(u32 addr, u32 size, int with_more) {
    const volatile u8 *p = (const volatile u8 *)addr;
    u32 s = 0;
    for (u32 i = 0; i < size; i++) {
        u8 b = p[i];
        s += b;
        sio_putc(b);
        if (with_more > 0 && ((i + 1) % CHUNK) == 0) {
            // nops v18 sends "MORE" after every 2 KB (the last one included): wait for it.
            // nops v14 does not: if the first one does not come quickly, stop waiting.
            u32 w = 0;
            int got = 0;
            for (int k = 0; k < 64; k++) {
                int c = rx_to(with_more == 2 ? 30000000 : 1500000);
                if (c < 0) break;
                w = (w >> 8) | ((u32)c << 24);
                if (w == 0x45524F4Du) { got = 1; break; }
            }
            with_more = got ? 2 : -1;               // 2: the PC paces us; -1: it does not
        }
    }
    if (with_more) tx32(s);                 // checksum (also when the PC does not pace)
}

static void cmd_dump(void) {
    accept("OKAY");
    u32 addr = rx32(), size = rx32();
    status("dump", addr, 1);
    send_block(addr, size, 1);
}

// SHOT x y w h (u32 each; w = 0: the menu picture on screen): the VRAM rectangle is copied to RAM
// (15-bit pixels, 2 bytes each), then answered with u32 address, width, height. The PC fetches
// the pixels with a normal DUMP (tools/psxshot.py does both and writes a PNG).
#define SHOT_BUF 0x80100000u
static void cmd_shot(void) {
    accept("OKAY");
    int x = (int)rx32(), y = (int)rx32(), w = (int)rx32(), h = (int)rx32();
    gfx_shot((u16 *)SHOT_BUF, &x, &y, &w, &h);
    tx32(SHOT_BUF);
    tx32((u32)w);
    tx32((u32)h);
    status("screenshot taken", w, 0);
}

static void cmd_hexd(void) {
    accept("OKAY");
    u32 addr = rx32(), size = rx32();
    send_block(addr, size, 0);
}

static void cmd_jump(int call) {
    accept("OKAY");
    u32 addr = rx32();
    if (call) {
        status("call", addr, 1);
        flush_cache();
        call_addr(addr);
        status("returned from", addr, 1);
    } else {
        gfx_shutdown();
        flush_cache();
        exec_jump(addr, 0, 0x801FFF00);
    }
}

static u32 cur_div = 18;
static void set_baud_now(u32 div) { sio_set_div(div); cur_div = div; }
static void set_baud(u32 div) {
    if (div == cur_div) return;           // nops sends SLOW before every command
    // the PC reopens its port 100 ms after the token: switch within that time
    for (volatile int i = 0; i < 100000; i++);
    sio_init(div);
    cur_div = div;
}

#define TOK(a, b, c, d) ((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))

// /psxbaud N (nops v18): BAUD -> BPLS, then N is resent every 50 ms; BACP (or NOPE), let the
// extra copies of N go by, then switch. N must be 2073600 / divisor (x16 mode). Example for a
// CP210x adapter (fixed rates): nops /psxbaud 230400 /pcbaud 230400 -> 235200 baud, 2x faster.
static void cmd_baud(void) {
    sio_puts("BPLS");
    u32 n = rx32();
    u32 div = n ? 2073600u / n : 0;
    if (aborted || div < 1 || div > 73 || 2073600u / div != n) { sio_puts("NOPE"); flush_rx(); return; }
    sio_puts("BACP");
    // The PC switches as soon as it sees BACP and sends the next command right away: drop only
    // the copies of N already received (no waiting: waiting would eat that command, received at
    // the old speed), then change the divisor alone. Stray bytes are ignored by the 4-byte window.
    while (!(REG32(0x1F801054) & 4));
    while (sio_rx_ready()) (void)sio_getc();
    sio_set_div(div);
    cur_div = div;
    status("serial custom speed, divisor", div, 0);
}

static u32 idle_frames;
void sio_poll(void) {
    // after /fast or /medi the PC talks fast for one command only: if nothing comes for ~3 s,
    // go back to 115200 (where every nops session starts)
    if (cur_div != 18 && !sio_rx_ready() && ++idle_frames > 150) { set_baud_now(18); status("serial back to 115200", -1, -1); }
    if (sio_rx_ready()) idle_frames = 0;
    while (sio_rx_ready()) {
        window = (window >> 8) | ((u32)sio_getc() << 24);
        aborted = 0;
        switch (window) {
        case TOK('S','B','I','N'): cmd_sbin(); break;
        case TOK('S','E','X','E'): cmd_sexe(); break;
        case TOK('S','R','O','M'): cmd_srom(); break;
        case TOK('D','U','M','P'): cmd_dump(); break;
        case TOK('S','H','O','T'): cmd_shot(); break;
        case TOK('H','E','X','D'): cmd_hexd(); break;
        case TOK('J','U','M','P'): cmd_jump(0); break;
        case TOK('C','A','L','L'): cmd_jump(1); break;
        case TOK('P','I','N','G'): sio_puts("PONG"); break;
        case TOK('R','E','S','T'): exec_jump(0xBFC00000, 0, 0x801FFF00); break;
        case TOK('S','L','O','W'): set_baud(18); status("serial 115200", -1, -1); window = 0; continue;
        case TOK('M','E','D','I'): set_baud(5); status("serial 414720", -1, -1); window = 0; continue;
        case TOK('F','A','S','T'): set_baud(4); status("serial 518400", -1, -1); window = 0; continue;
        case TOK('B','A','U','D'): cmd_baud(); window = 0; continue;
        case TOK('M','C','U','P'): cmd_mcup(); break;
        case TOK('M','C','D','N'): cmd_mcdn(); break;
        case TOK('D','E','B','G'): {
            extern void core_debug(void);
            accept("OKAY");
            core_debug();
            status("debug core installed (next /exe keeps it)", -1, -1);
            break;
        }
        // only meaningful while a program runs under the debug core (as Unirom: "ONLY")
        case TOK('H','A','L','T'): case TOK('C','O','N','T'): case TOK('H','K','R','D'):
        case TOK('H','K','W','R'): case TOK('H','K','E','X'): case TOK('U','N','H','K'):
            sio_puts("ONLY");
            status("only while a program runs (nops /debug first)", -1, -1);
            break;
        case TOK('Q','Q','Q','Q'):
        case TOK('F','L','A','T'): case TOK('G','A','P','S'):
            sio_puts("UNSP");
            status("command not supported yet", -1, -1);
            break;
        default: continue;
        }
        window = 0;
        // nops opens every command at 115200 (with /fast it switches again): back to slow speed
        if (cur_div != 18) set_baud_now(18);
    }
}
