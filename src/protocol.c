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

// Bounded: "rom written and verified, reset to boot it 0x...." is 54 characters; written
// straight into sio_status[48] it ran into the next variable, the status lost its end, and the
// menu's next frame overran its line buffer on the stack (freeze after a flash, real hardware).
static void status(const char *a, u32 v, int hex) {
    char t[80], *p = t;
    int n = 0;
    while (a[n] && n < 60) { t[n] = a[n]; n++; }
    p = t + n; *p = 0;
    if (hex >= 0) { p = fmt_str(p, hex ? " 0x" : " "); if (hex) fmt_hex(p, v, 8); else fmt_dec(p, v); }
    int i = 0;
    for (; i < (int)sizeof sio_status - 1 && t[i]; i++) sio_status[i] = t[i];
    sio_status[i] = 0;
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
// With rx_what set, a progress bar is drawn once a whole chunk is in, before CHEK: the PC waits for
// CHEK before sending anything (it sends the next chunk right after the checksum, without waiting
// for MORE: drawing later loses its first bytes). V1 streams: no bar.
static const char *rx_what;
static u32 rx_addr, rx_entry;           // shown under the bar (0: not an EXE / not a RAM load)
static const char *rx_region;           // PS-EXE header marker (region text), or 0
static u32 rx_lines;                    // time since the start, in scanlines (root counter 1)
static u16 rx_last;
static void rx_progress(u32 done, u32 len) {
    u16 now = REG16(0x1F801110);
    rx_lines += (u16)(now - rx_last);
    rx_last = now;
    static char l0[40], l1[40], l2[40], l3[40];
    char *p = fmt_dec(l0, done >> 10);
    p = fmt_str(p, " KB / ");
    p = fmt_dec(p, (len + 1023) >> 10);
    fmt_str(p, " KB");
    u32 rate = rx_lines >= 100 ? done * 157u / (rx_lines / 100) : 0;   // bytes/s (~15700 lines/s)
    l1[0] = l2[0] = l3[0] = 0;
    if (rate) {
        p = fmt_dec(l1, rate / 1000);
        p = fmt_str(p, ".");
        p = fmt_dec(p, rate / 100 % 10);
        p = fmt_str(p, " KB/s, ");
        p = fmt_dec(p, (len - done) / rate);
        fmt_str(p, " s left");
    }
    if (rx_addr) {
        p = fmt_str(l2, "load ");
        p = fmt_hex(p, rx_addr, 8);
        if (rx_entry) { p = fmt_str(p, "  entry "); fmt_hex(p, rx_entry, 8); }
    }
    if (rx_region) {
        int i = 0;
        while (i < 30 && rx_region[i] >= 32 && rx_region[i] < 127) { l3[i] = rx_region[i]; i++; }
        l3[i] = 0;
    }
    const char *info[5] = {l0, l1[0] ? l1 : 0, l2[0] ? l2 : 0, l3[0] ? l3 : 0, 0};
    ui_progress_info(rx_what, done, len, info);
}
static int receive(u8 *dst, u32 len) {
    REG32(0x1F801114) = 0x0100;         // root counter 1 counts scanlines (upload speed)
    rx_last = REG16(0x1F801110);
    rx_lines = 0;
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
        if (rx_what && len > CHUNK && !lost) rx_progress(done + n, len);
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
    rx_what = "loading the file";
    rx_addr = addr; rx_entry = 0; rx_region = 0;
    receive((u8 *)addr, len);
    rx_what = 0;
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
    rx_what = "loading the program";
    rx_addr = addr; rx_entry = pc;
    rx_region = exe_hdr[0x4C] ? (const char *)exe_hdr + 0x4C : 0;
    receive((u8 *)addr, len);
    rx_what = 0;
    if (sum8((u8 *)addr, len) != sum) { status("exe CHECKSUM ERROR", -1, -1); return; }
    ((u32 *)exe_hdr)[0x10 / 4] = pc;
    exe_start(exe_hdr);
}

// /mcup CARD FILE: raw 128 KB image written frame by frame, then read back
// the progress screen says which way and which slot: "writing memory card 1" / "reading memory card 2"
static char mc_what[24];
static void mc_prog(int f) { ui_progress(mc_what, f, 1024); }
static void mc_title(const char *verb, u32 card) { char *p = fmt_str(mc_what, verb); p = fmt_str(p, " memory card "); fmt_dec(p, card + 1); }
static void cmd_mcup(void) {
    accept("OKAY");
    u32 card = rx32() & 1, len = rx32(), sum = rx32();
    if (len > 131072) len = 131072;
    memset(ROM_BUF, 0, 131072);
    rx_what = "receiving the card image";
    rx_addr = rx_entry = 0; rx_region = 0;
    receive(ROM_BUF, len);
    rx_what = 0;
    if (sum8(ROM_BUF, len) != sum) { status("card image CHECKSUM ERROR, nothing written", -1, -1); return; }
    if (len != 131072) { status("card image is not 128 KB, nothing written", len, 0); return; }
    mc_title("writing", card);                      // each frame is read back as it is written
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
    mc_title("reading", card);
    int bad = mc_read_card(card, ROM_BUF, mc_prog);
    if (bad == 1024) { sio_puts("HECK"); status("no memory card in slot", card + 1, 0); return; }
    sio_puts("MCRD");
    tx32((u32)ROM_BUF);
    tx32(131072);
    status(bad ? "memory card read, bad frames:" : "memory card read, slot", bad ? bad : card + 1, 0);
}

static void flash_result(int bad, int bank, u32 crc) {
    static char r[40];
    char *q;
    (void)bank;
    if (bad) { q = fmt_str(r, "FLASH FAILED: "); q = fmt_dec(q, bad); fmt_str(q, " block(s)"); }
    else { q = fmt_str(r, "FLASH OK  CRC32 "); fmt_hex(q, crc, 8); }
    ui_flash_result(r);
}

// Flash ROM_BUF (128 KB, already checked): bank from the image's entry, OpenPsxRom's saves kept.
// Returns the number of failed blocks, or -1 if refused (reason in the status line, and in *why).
static int rom_buf_flash(int *bank_out, const char **why) {
    u8 mfr, dev;
    int known = flash_identify(&mfr, &dev);
    *why = 0;
    if (mfr == 0xFF && dev == 0xFF) *why = "no flash chip";
    else if (!known) *why = "unknown flash chip";
    else {
        // never write a ROM that the BIOS would not start
        const char *lic = "Licensed by Sony";
        for (int i = 0; lic[i]; i++) if (ROM_BUF[0x84 + i] != (u8)lic[i]) *why = "no header at 84h";
    }
    if (*why) { status(*why, -1, -1); return -1; }
    extern int is_openrom_image(const u8 *img);
    extern int bank_available(int bank);
    int bank = rom_image_bank(ROM_BUF);               // image for bank 2 or 3: OpenPsxRom kept
    *bank_out = bank;
    if (bank > 1 && !bank_available(bank)) {
        *why = bank == 2 ? "rom for bank 2: no room on this chip" : "rom for bank 3: no second chip";
        status(*why, -1, -1);
        return -1;
    }
    u32 off = bank == 2 ? ROM2_OFF : bank == 3 ? ROM3_OFF : 0;
    u32 size = bank == 1 && is_openrom_image(ROM_BUF) ? FL_ROM_SIZE : FLASH_SIZE;
    flash_ui_begin("flashing", off, size);
    int bad = flash_write_at(off, ROM_BUF, size, flash_ui);
    if (bad) status("rom FAILED blocks:", bad, 0);
    else status(bank == 1 ? "rom written and verified, reset to boot it" : bank == 2 ? "rom written in bank 2 and verified"
                : "rom written in bank 3 and verified", crc32(ROM_BUF, FLASH_SIZE), 1);
    return bad;
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
    rx_what = "receiving the ROM";
    rx_addr = rx_entry = 0; rx_region = 0;
    receive(ROM_BUF, len);
    rx_what = 0;
    if (sum8(ROM_BUF, len) != sum) { status("rom CHECKSUM ERROR, nothing written", -1, -1); return; }
    int bank;
    const char *why;
    int bad = rom_buf_flash(&bank, &why);
    if (bad >= 0) flash_result(bad, bank, crc32(ROM_BUF, FLASH_SIZE));
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

// --------------------------------------------------------------------------- OPX1 extended link
// Used by tools/psx.py; nops never sends OPX1, so everything above stays as nops expects it.
// The token OPX1 is answered "OPXA", then frames go both ways until END, a flash, an exe, a reset
// or ~3 s of silence (the link then falls back to 115200, as after any nops command):
//   A5h, cmd, u16 len, payload[len], u32 CRC32 (zlib) of cmd..payload            (little endian)
// Each request gets one answer with the same cmd: payload = status byte + data. The PC sends the
// next request only once it has the answer, and every request can be repeated safely: a damaged
// frame is answered X_BADCRC (cmd FFh) and the PC sends it again. Commands: see tools/psx.py.
#define X_MAX 4108                          // request payload: up to 12 bytes of arguments + 4 KB
enum { X_PING = 0, X_INFO, X_BAUD, X_READ, X_WRITE, X_LZWR, X_CRCS, X_COPY, X_FLASH, X_EXEC,
       X_RESET, X_PROG, X_END, X_FLAREA };
enum { X_OK = 0, X_BADCRC, X_BADARG, X_REFUSED, X_FAILED, X_UNKNOWN };
static u8 xin[3 + X_MAX + 4];
static u32 xget(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

// 0: a frame is in xin (cmd xin[0], payload xin + 3, *len); -1: nothing came; -2: damaged frame
// A PC that lost the session (psx.py restarted) sends OPX1 again: answered, the session goes on.
static int x_recv(u32 *len, u32 idle) {
    int c;
    u32 w = 0;
    for (;;) {
        if ((c = rx_to(idle)) < 0) return -1;
        if (c == 0xA5) break;
        w = (w >> 8) | ((u32)c << 24);
        if (w == 0x3158504Fu) sio_puts("OPXA");         // "OPX1"
    }
    for (int i = 0; i < 3; i++) { if ((c = rx_to(2000000)) < 0) return -2; xin[i] = (u8)c; }
    u32 n = xin[1] | (xin[2] << 8);
    if (n > X_MAX) return -2;
    for (u32 i = 0; i < n + 4; i++) { if ((c = rx_to(2000000)) < 0) return -2; xin[3 + i] = (u8)c; }
    if (crc32(xin, 3 + n) != xget(xin + 3 + n)) return -2;
    *len = n;
    return 0;
}
static void x_send2(u8 cmd, u8 st, const void *a, u32 na, const void *b, u32 nb) {
    u32 n = 1 + na + nb;
    u8 h[5] = {0xA5, cmd, (u8)n, (u8)(n >> 8), st};
    sio_write(h, 5);
    sio_write(a, na);
    sio_write(b, nb);
    tx32(crc32_upd(crc32_upd(crc32_upd(0, h + 1, 4), a, na), b, nb));
}
static void x_send(u8 cmd, u8 st, const void *d, u32 n) { x_send2(cmd, st, d, n, 0, 0); }
static void x_text(u8 cmd, u8 st, const char *t) { x_send(cmd, st, t, strlen(t)); }

// RAM a request may write to: main RAM (any segment) or the scratchpad, never OpenPsxRom's own
static int x_ram_ok(u32 addr, u32 len) {
    u32 ph = addr & 0x1FFFFFFFu;
    if (ph >= 0x1F800000u && ph + len <= 0x1F800400u) return 1;
    if (ph + len > 0x200000u || ph + len < ph) return 0;
    return !exe_overlaps_rom(0x80000000u | ph, len);
}

static void x_info(void) {
    static char t[640];
    u8 m = 0xFF, d = 0xFF;
    flash_identify(&m, &d);
    extern int bank_available(int bank);
    extern u8 __bss_end[], __payload_ram[];
    char *p = fmt_str(t, "version=" OPENROM_VERSION "\nproto=1\nbuild=");
#if defined(ARCART)
    p = fmt_str(p, "arcart");
#elif defined(BANK3)
    p = fmt_str(p, "bank3");
#else
    p = fmt_str(p, "bank1");
#endif
    p = fmt_str(p, "\nchip="); p = fmt_str(p, flash_chip_name(m, d));
    p = fmt_str(p, "\nid="); p = fmt_hex(p, m, 2); p = fmt_str(p, "/"); p = fmt_hex(p, d, 2);
    p = fmt_str(p, "\nkb="); p = fmt_dec(p, flash_chip_kb(m, d));
    p = fmt_str(p, "\nbank2="); p = fmt_dec(p, bank_available(2));
    p = fmt_str(p, "\nbank3="); p = fmt_dec(p, bank_available(3));
    p = fmt_str(p, "\nrombuf="); p = fmt_hex(p, (u32)ROM_BUF, 8);
    p = fmt_str(p, "\nromram="); p = fmt_hex(p, (u32)__payload_ram, 8);
    p = fmt_str(p, "-"); p = fmt_hex(p, (u32)__bss_end, 8);
    p = fmt_str(p, "\nopenrom_size="); p = fmt_hex(p, FL_ROM_SIZE, 8);
    p = fmt_str(p, "\nmax="); p = fmt_dec(p, X_MAX - 12);
    p = fmt_str(p, "\ndiv="); p = fmt_dec(p, cur_div);
    // the console's BIOS: "System ROM Version 4.1 12/16/97 E" at BFC7FF32h (none before v2.0)
    p = fmt_bios(fmt_str(p, "\nbios="));
    p = fmt_str(p, "\nbios_crc="); p = fmt_hex(p, crc32((const void *)0xBFC00000, 0x80000), 8);
    p = fmt_str(p, "\nmodel="); p = fmt_str(p, bios_model());
    p = fmt_str(p, "\nstarted=");
#if defined(BOOTDISC)
    p = fmt_str(p, "ram");
#else
    p = fmt_str(p, "rom");
#endif
    p = fmt_str(p, "\nswitch="); p = fmt_str(p, (*(volatile u8 *)0x1F060000 & 1) ? "on" : "off");
    p = fmt_str(p, "\ncd_unlocked="); p = fmt_dec(p, cd_unlocked);
    extern char *fmt_banks(char *p);
    p = fmt_banks(fmt_str(p, "\nbanks="));
    fmt_str(p, "\n");
    x_text(X_INFO, X_OK, t);
}

// PROG: progress screen drawn for the PC: u32 done, u32 total, then title, line 1..4 (NUL-separated)
static void x_prog(const u8 *a, u32 n) {
    static char txt[5][44];
    const char *info[5] = {0, 0, 0, 0, 0};
    u32 i = 8;
    for (int k = 0; k < 5; k++) {
        int j = 0;
        while (i < n && a[i] && j < 43) txt[k][j++] = a[i++];
        while (i < n && a[i]) i++;
        i++;
        txt[k][j] = 0;
        if (k) info[k - 1] = txt[k][0] ? txt[k] : 0;
    }
    ui_progress_info(txt[0], xget(a), xget(a + 4), info);
}

static void cmd_opx(void) {
    sio_puts("OPXA");
    status("psx.py link", -1, -1);
    u32 idle = 18000000;                            // ~3 s
    for (;;) {
        u32 n = 0;
        int r = x_recv(&n, idle);
        if (r == -1) break;                         // silence (or the speed change failed)
        if (r == -2) { flush_rx(); x_send(0xFF, X_BADCRC, 0, 0); continue; }
        idle = 18000000;
        u8 cmd = xin[0], *a = xin + 3;
        u32 a0 = n >= 4 ? xget(a) : 0, a1 = n >= 8 ? xget(a + 4) : 0, a2 = n >= 12 ? xget(a + 8) : 0;
        switch (cmd) {
        case X_PING: x_send(cmd, X_OK, 0, 0); break;
        case X_INFO: x_info(); break;
        case X_BAUD:                                // u32 divisor of 2073600 (x16 mode)
            if (a0 < 1 || a0 > 73) { x_send(cmd, X_BADARG, 0, 0); break; }
            x_send(cmd, X_OK, 0, 0);
            while (!(REG32(0x1F801054) & 4));       // once the answer is out: port reset at the new
            sio_init(a0);                           // speed, as FAST (only the divisor did not work)
            cur_div = a0;
            status("psx.py link, divisor", a0, 0);
            idle = 6000000;                         // the PC switches too: ~1 s for its next frame
            break;
        case X_READ:                                // u32 addr, u32 len -> data
            if (a1 > X_MAX - 12) { x_send(cmd, X_BADARG, 0, 0); break; }
            x_send(cmd, X_OK, (const void *)a0, a1);
            break;
        case X_WRITE:                               // u32 addr, data
            if (n < 4 || !x_ram_ok(a0, n - 4)) { x_send(cmd, X_REFUSED, 0, 0); break; }
            memcpy((void *)a0, a + 4, n - 4);
            x_send(cmd, X_OK, 0, 0);
            break;
        case X_LZWR: {                              // u32 addr, OMC1 stream (tools/omc.py)
            extern u32 omc_unpack(const u8 *src, u8 *dst, u32 max);
            extern u32 omc_raw_size(const u8 *src);
            extern int omc_valid(const u8 *src);
            if (n < 20 || !omc_valid(a + 4)) { x_send(cmd, X_BADARG, 0, 0); break; }
            u32 raw = omc_raw_size(a + 4);
            if (!x_ram_ok(a0, raw)) { x_send(cmd, X_REFUSED, 0, 0); break; }
            x_send(cmd, omc_unpack(a + 4, (u8 *)a0, raw) == raw ? X_OK : X_FAILED, 0, 0);
            break;
        }
        case X_CRCS: {                              // u32 addr, u32 step, u32 count -> count CRC32s
            static u32 c[512];
            if (a2 > 512 || !a1) { x_send(cmd, X_BADARG, 0, 0); break; }
            for (u32 k = 0; k < a2; k++) c[k] = crc32((const void *)(a0 + k * a1), a1);
            x_send(cmd, X_OK, c, a2 * 4);
            break;
        }
        case X_COPY:                                // u32 dst, u32 src, u32 len (e.g. a bank to rombuf)
            if (!x_ram_ok(a0, a2)) { x_send(cmd, X_REFUSED, 0, 0); break; }
            for (u32 k = 0; k < a2; k++) ((u8 *)a0)[k] = ((const volatile u8 *)a1)[k];
            x_send(cmd, X_OK, 0, 0);
            break;
        case X_FLASH: {                             // u32 CRC32 of the 128 KB in rombuf -> u32 bad, bank
            if (crc32(ROM_BUF, FLASH_SIZE) != a0) { x_text(cmd, X_FAILED, "rombuf CRC differs"); break; }
            int bank = 1;
            const char *why;
            int bad = rom_buf_flash(&bank, &why);
            if (bad < 0) { x_text(cmd, X_REFUSED, why); break; }
            u32 res[2] = {(u32)bad, (u32)bank};
            x_send(cmd, bad ? X_FAILED : X_OK, res, 8);
            if (cur_div != 18) set_baud_now(18);
            flash_result(bad, bank, a0);            // stays on the flash screen (serial served)
            return;
        }
        case X_EXEC: {                              // 2048-byte PS-X EXE header (data loaded before)
            static const char magic[] = "PS-X EXE";
            int ok = n == 2048;
            for (int k = 0; ok && k < 8; k++) if (a[k] != (u8)magic[k]) ok = 0;
            if (!ok) { x_send(cmd, X_BADARG, 0, 0); break; }
            memcpy(exe_hdr, a, 2048);
            x_send(cmd, X_OK, 0, 0);
            while (!(REG32(0x1F801054) & 4));
            exe_start(exe_hdr);
        }
        case X_RESET:
            x_send(cmd, X_OK, 0, 0);
            while (!(REG32(0x1F801054) & 4));
            exec_jump(0xBFC00000, 0, 0x801FFF00);
        case X_PROG: x_prog(a, n); x_send(cmd, X_OK, 0, 0); break;
        case X_END: x_send(cmd, X_OK, 0, 0); goto out;
        case X_FLAREA: {                            // u32 offset, u32 length, u32 CRC32 of rombuf[0..length]
            // a data area of bank 1 (saves, cheats, settings: tools/cheats.py), whole 16 KB sectors,
            // never below FL_ROM_SIZE (OpenPsxRom's code; on a Datel cart its sectors 1-4 too)
#ifdef ARCART
            u32 lo = 0x18000;
#else
            u32 lo = FL_ROM_SIZE;
#endif
            if (a0 < lo || a0 % FLASH_SECT || !a1 || a1 % FLASH_SECT || a0 + a1 > FLASH_SIZE) {
                x_text(cmd, X_REFUSED, "only whole sectors of the data area");
                break;
            }
            if (crc32(ROM_BUF, a1) != a2) { x_text(cmd, X_FAILED, "rombuf CRC differs"); break; }
            // only on a cartridge running OpenPsxRom: its data area is another firmware's code
            // elsewhere (Xplorer, Unirom...)
            extern int is_openrom_image(const u8 *img);
#ifdef ARCART
            const u8 *me = (const u8 *)FLASH_BASE + 0x5200;
#else
            const u8 *me = (const u8 *)FLASH_BASE;
#endif
            if (!is_openrom_image(me)) { x_text(cmd, X_REFUSED, "no OpenPsxRom on this cartridge"); break; }
            flash_ui_begin("saving data", a0, a1);
            int bad = flash_write_at(a0, ROM_BUF, a1, flash_ui);
            extern void cheats_load(void);
            cheats_load();                          // the menu and the next game see the new codes
            status(bad ? "data area FAILED blocks:" : "data area written at", bad ? (u32)bad : a0, bad ? 0 : 1);
            x_send(cmd, bad ? X_FAILED : X_OK, &bad, 4);
            break;
        }
        default: x_send(cmd, X_UNKNOWN, 0, 0); break;
        }
    }
out:
    if (cur_div != 18) set_baud_now(18);
    status("psx.py link closed", -1, -1);
}

static void unsp(void) { sio_puts("UNSP"); status("command not supported yet", -1, -1); }

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
        // left out of the build (make menuconfig): answered UNSP, the code is not linked
        case TOK('S','H','O','T'): if (CONFIG_SHOT) cmd_shot(); else unsp(); break;
        case TOK('H','E','X','D'): cmd_hexd(); break;
        case TOK('J','U','M','P'): cmd_jump(0); break;
        case TOK('C','A','L','L'): cmd_jump(1); break;
        case TOK('P','I','N','G'): sio_puts("PONG"); break;
        case TOK('R','E','S','T'): exec_jump(0xBFC00000, 0, 0x801FFF00); break;
        case TOK('S','L','O','W'): set_baud(18); status("serial 115200", -1, -1); window = 0; continue;
        case TOK('M','E','D','I'): set_baud(5); status("serial 414720", -1, -1); window = 0; continue;
        case TOK('F','A','S','T'): set_baud(4); status("serial 518400", -1, -1); window = 0; continue;
        case TOK('B','A','U','D'): cmd_baud(); window = 0; continue;
        case TOK('M','C','U','P'): if (CONFIG_MCARD_SERIAL) cmd_mcup(); else unsp(); break;
        case TOK('O','P','X','1'): if (CONFIG_OPX_LINK) cmd_opx(); else unsp(); break;
        case TOK('M','C','D','N'): if (CONFIG_MCARD_SERIAL) cmd_mcdn(); else unsp(); break;
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
