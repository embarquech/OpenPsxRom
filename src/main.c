// OpenROM - open source cartridge firmware: menu and screens.
#include "rom.h"

static u16 pad_now, pad_old;
static u32 anim_tick;            // frames, for the animated save icons
static int rep_cnt;
static int pressed(u16 b) { return (pad_now & b) && !(pad_old & b); }
// d-pad with auto-repeat (held: repeats after 25 frames, then every 6)
static int moved(u16 b) {
    if (!(pad_now & b)) return 0;
    if (!(pad_old & b)) return 1;
    return rep_cnt > 25 && rep_cnt % 6 == 0;
}
// Debounced pad: a press counts at once, a release only after 3 frames in a row without the
// button. A contact bounce or a failed pad read (all released for one frame) used to give a
// second press, so one push moved the cursor twice.
static u16 pad_filtered(void) {
    static u16 held;
    static u8 off[16];
    u16 raw = pad_read();
    for (int i = 0; i < 16; i++) {
        u16 m = 1 << i;
        if (raw & m) { held |= m; off[i] = 0; }
        else if ((held & m) && ++off[i] >= 3) held &= ~m;
    }
    return held;
}
static int back(void) { return pressed(PAD_CIRCLE); }

// Screen layout (pGbX style): gradient background, title bar with the logo, content panel,
// footer with the button hints and the serial status.
static const char *cur_help;
static void frame_begin(const char *title) {
    gfx_grad(0, 0, 320, 240, 0x3A1A0C, 0x0A0404);
    gfx_grad(0, 0, 320, 30, 0xA05820, 0x401808);
    gfx_rect(0, 30, 320, 1, 0xF0C060);
    int x = gfx_text2(10, 7, 0x40D8FF, "OpenPsxRom");
    gfx_text(x + 4, 15, 0x90B0C0, "v" OPENROM_VERSION);
#ifdef BANK3
    gfx_text(x + 4, 4, 0xFF6040, "bank 3");          // the test copy started from bank 3
#endif
    gfx_text(320 - 10 - gfx_text_w(title), 15, C_TEXT, title);
    gfx_shade(8, 38, 304, 168, 0x000000);
    gfx_shade(0, 210, 320, 30, 0x000000);
    gfx_rect(0, 210, 320, 1, 0x604020);
    char s[64], *p = s;
    p = fmt_str(p, "Serial: ");
    const char *st = sio_status[0] ? sio_status : "waiting for NOPS (115200)";
    for (int i = 0; st[i] && p < s + sizeof s - 1; i++) *p++ = st[i];     // bounded copy
    *p = 0;
    gfx_text(10, 228, C_DIM, s);
    cur_help = 0;
}

static void frame_end(void) {
    anim_tick++;
    if (cur_help) gfx_text(10, 215, C_TEXT, cur_help);
    gfx_flip();
    sio_poll();
    pad_old = pad_now;
    pad_now = pad_filtered();
    if (pad_now & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) rep_cnt++; else rep_cnt = 0;
}

static void help(const char *s) { cur_help = s; }

// Progress screen for long operations (card, CD, serial uploads): one frame per call, with up to
// 4 optional lines of details under the bar, and a 5th one: a result line (green if "FLASH OK").
void ui_progress(const char *what, u32 done, u32 total) { ui_progress_info(what, done, total, 0); }
static void ui_progress_draw(const char *what, u32 done, u32 total, const char *const *info) {
    frame_begin(what);
    char s[48], *p = fmt_str(s, what);
    p = fmt_str(p, "... ");
    fmt_dec(p, total ? done * 100 / total : 0);
    fmt_str(s + strlen(s), "%");
    // the whole block (title line to result line) centred in the panel (y 38-206)
    gfx_text(40, 70, C_TEXT, s);
    gfx_rect(40, 86, 240, 10, 0x404040);
    if (total) gfx_rect(40, 86, 240 * done / total, 10, 0x40C040);
    for (int i = 0; info && i < 4; i++) if (info[i]) gfx_text(40, 106 + i * 14, i > 1 ? C_DIM : C_TEXT, info[i]);
    if (info && info[4])                                // result line (flash: FLASH OK / FAILED)
        gfx_text(40, 166, info[4][6] == 'O' ? C_OK : C_ERR, info[4]);
}
void ui_progress_info(const char *what, u32 done, u32 total, const char *const *info) {
    ui_progress_draw(what, done, total, info);
    gfx_flip();
}

// Simple list widget: returns the new selection, draws rows from y=26.
#define ROWS 13
static int list_draw(int n, int sel, int *top, const char *(*label)(int, char *), u32 (*color)(int)) {
    if (sel < *top) *top = sel;
    if (sel >= *top + ROWS) *top = sel - ROWS + 1;
    char buf[64];
    for (int i = *top; i < n && i < *top + ROWS; i++) {
        int y = 44 + (i - *top) * 12;
        if (i == sel) { gfx_grad(10, y - 2, 300, 12, 0xC07838, 0x602810); gfx_text(13, y, C_SEL, G_RIGHT); }
        gfx_text(22, y, i == sel ? C_SEL : (color ? color(i) : C_TEXT), label(i, buf));
    }
    return sel;
}
static int list_move(int n, int sel) {
    if (n <= 0) return 0;
    if (moved(PAD_UP)) sel = (sel + n - 1) % n;
    if (moved(PAD_DOWN)) sel = (sel + 1) % n;
    if (n > ROWS && moved(PAD_LEFT)) sel = sel >= ROWS ? sel - ROWS : 0;
    if (n > ROWS && moved(PAD_RIGHT)) sel = sel + ROWS < n ? sel + ROWS : n - 1;
    return sel;
}

static void message(const char *title, const char *l1, const char *l2, u32 col) {
    for (;;) {
        frame_begin(title);
        gfx_text(24, 80, col, l1);
        if (l2) gfx_text(24, 96, C_TEXT, l2);
        help(G_CROSS " / " G_CIRCLE " back");
        frame_end();
        if (pressed(PAD_CROSS) || back()) return;
    }
}

// Yes/no question shown in the middle of a serial command: cross = 1, circle = 0. The serial port
// is not polled meanwhile (frame_end would start another command from inside this one).
// End of a flash write: big "FLASH OK" (green) or "FLASH FAILED" (red) for ~3 s, then back to
// the menu (nothing to answer; the serial port is not polled meanwhile, as during the write).
int ui_ask(const char *title, const char *l1, const char *l2, const char *question) {
    pad_now = pad_old = pad_read();                     // buttons already held do not count
    for (;;) {
        frame_begin(title);
        gfx_text(24, 80, C_OK, l1);
        if (l2) gfx_text(24, 96, C_TEXT, l2);
        gfx_text(24, 124, C_SEL, question);
        gfx_text(10, 215, C_TEXT, G_CROSS " yes    " G_CIRCLE " no");
        gfx_flip();
        pad_old = pad_now;
        pad_now = pad_filtered();
        if (pressed(PAD_CROSS)) return 1;
        if (pressed(PAD_CIRCLE)) return 0;
    }
}

// hold START + cross to confirm a destructive action
static int confirm(const char *title, const char *l1, const char *l2) {
    for (;;) {
        frame_begin(title);
        gfx_text(24, 70, C_TEXT, l1);
        if (l2) gfx_text(24, 90, C_ERR, l2);
        help("hold START + " G_CROSS " confirm    " G_CIRCLE " cancel");
        frame_end();
        if (back()) return 0;
        if ((pad_now & PAD_START) && pressed(PAD_CROSS)) return 1;
    }
}

// ---------------------------------------------------------------- cartridge banks
// What each 128 KB bank holds (known firmware names, else the header text), whether it can be
// started, its CRC, and which one the switch starts.
int bank_available(int bank);
int chip2_present(void);
int rom_bank_ok(int bank);
static int cfg_bank(void);
static int find_text(const volatile u8 *p, u32 n, const char *s) {
    for (u32 i = 0; i + strlen(s) <= n; i++) {
        int k = 0;
        while (s[k] && p[i + k] == (u8)s[k]) k++;
        if (!s[k]) return 1;
    }
    return 0;
}
static void bank_name(int bank, char *out) {
    const volatile u8 *b = (const volatile u8 *)(FLASH_BASE + (bank - 1) * 0x20000u);
    int blank = 1;
    for (u32 i = 0; i < 0x200 && blank; i++) if (b[i] != 0xFF) blank = 0;
    if (blank) { fmt_str(out, "empty"); return; }
    static const char *const known[][2] = {
        {"OpenPsxRom", "OpenPsxRom"}, {"OpenROM", "OpenROM (old)"}, {"UNIROM", "Unirom"}, {"Unirom", "Unirom"},
        {"CAETLA", "Caetla"}, {"Caetla", "Caetla"}, {"XPLORER", "Xplorer"}, {"Xplorer", "Xplorer"},
        {"Action Replay", "Action Replay"}, {"GameShark", "GameShark"}, {"DATEL", "Datel firmware"},
    };
    for (u32 k = 0; k < sizeof known / sizeof known[0]; k++)
        if (find_text(b, 0x400, known[k][0])) { fmt_str(out, known[k][1]); return; }
    if (*(const volatile u32 *)(b + 0x84) != 0x6563694Cu) { fmt_str(out, "data (no boot header)"); return; }
    int n = 0;                                          // the text after "Licensed by Sony..."
    for (u32 i = 0xB0; i < 0xE8 && n < 26 && b[i] >= 32 && b[i] < 127; i++) out[n++] = b[i];
    out[n] = 0;
    if (!n) fmt_str(out, "unknown firmware");
}
static void screen_banks(void) {
    char name[3][32];
    u32 crc[3];
    int avail[3];
    for (int k = 0; k < 3; k++) {
        avail[k] = bank_available(k + 1);
        if (!avail[k]) continue;
        frame_begin("Cartridge banks");
        gfx_text(24, 100, C_TEXT, "reading the cartridge...");
        gfx_flip();
        bank_name(k + 1, name[k]);
        crc[k] = crc32((const void *)(FLASH_BASE + k * 0x20000u), 0x20000);
    }
    for (;;) {
        frame_begin("Cartridge banks");
        int sw = *(volatile u8 *)0x1F060000 & 1, boot = cfg_bank();
        char s[64], *p;
        for (int k = 0; k < 3; k++) {
            int y = 44 + k * 40;
            p = fmt_str(s, "Bank ");
            p = fmt_dec(p, k + 1);
            p = fmt_str(p, "  ");
            fmt_hex(p, 0x1F000000u + k * 0x20000u, 8);
            gfx_text(16, y, C_SEL, s);
            if (!avail[k]) { gfx_text(32, y + 12, C_DIM, k == 2 ? "not on this cartridge (no 2nd chip)" : "not on this cartridge"); continue; }
            gfx_text(32, y + 12, name[k][0] == 'e' ? C_DIM : C_OK, name[k]);
            if (name[k][0] == 'e') continue;                // empty: no CRC
            p = fmt_str(s, "CRC ");
            p = fmt_hex(p, crc[k], 8);
            if (k == 0) p = fmt_str(p, "  starts: switch ON");
            else if (rom_bank_ok(k + 1)) p = fmt_str(p, boot == k + 1 ? "  starts: switch OFF" : "  startable");
            gfx_text(32, y + 24, C_DIM, s);
        }
        p = fmt_str(s, "Switch now: ");
        p = fmt_str(p, sw ? "ON" : "OFF");
        p = fmt_str(p, "   OFF at power-on: ");
        fmt_str(p, boot == 2 ? "bank 2" : boot == 3 ? "bank 3" : "bypass");
        gfx_text(16, 168, C_TEXT, s);
        gfx_text(16, 182, C_DIM, "(set in Boot options)");
        help(G_CIRCLE " back");
        frame_end();
        if (back()) return;
    }
}

// ---------------------------------------------------------------- cartridge
static void screen_cart(void) {
    u8 mfr, dev, prot[8];
    int known = flash_identify(&mfr, &dev);
    // hardware sector protection only exists on AMD-style chips (Am29F010 of Datel carts);
    // Atmel/SST/Winbond page chips use software data protection: nothing to read per sector
    int lockable = mfr == 0x01 || mfr == 0x20 || mfr == 0x04;
    if (lockable) flash_protect(prot);
    // a second chip only if the first one is smaller than 512 KB (else 40000h is the same chip)
    int chip2 = chip2_present();                       // read only: no command to it
    u32 crc = crc32((const void *)FLASH_BASE, FLASH_SIZE);
    for (;;) {
        frame_begin("Cartridge");
        char s[64], *p;
        int y = 46;
        p = fmt_str(s, "Flash ID   ");
        p = fmt_hex(p, mfr, 2); p = fmt_str(p, "/"); fmt_hex(p, dev, 2);
        gfx_text(16, y, C_TEXT, s); y += 12;
        gfx_text(16, y, known ? C_OK : C_ERR, flash_chip_name(mfr, dev)); y += 12;
        if (lockable) {
            p = fmt_str(s, "Protected  ");
            int any = 0;
            for (int i = 0; i < 8; i++) if (prot[i]) { *p++ = '0' + i; *p++ = ' '; any = 1; }
            if (!any) p = fmt_str(p, "none");
            *p = 0;
            gfx_text(16, y, any ? C_ERR : C_OK, s); y += 12;
        } else {
            gfx_text(16, y, C_DIM, "Protection software (no sector lock)"); y += 12;
        }
        if (chip2) {
            p = fmt_str(s, "Chip 2     at 1F040000h (not probed)");
            gfx_text(16, y, C_OK, s); y += 12;
        }
        p = fmt_str(s, "ROM CRC32  "); fmt_hex(p, crc, 8);
        gfx_text(16, y, C_TEXT, s); y += 24;
        gfx_text(16, y, C_DIM, "Flash a new ROM from the PC with:"); y += 12;
        gfx_text(16, y, C_DIM, "  nops /rom FILE.ROM COMx");
        help(G_CIRCLE " back");
        frame_end();
        if (back()) return;
    }
}

// ---------------------------------------------------------------- card backup in the cartridge
u32 omc_pack(const u8 *src, u32 n, u8 *dst, u32 max);
u32 omc_unpack(const u8 *src, u8 *dst, u32 max);
int omc_valid(const u8 *src);
u32 omc_packed_size(const u8 *src);
// Flash progress screen, shared by every flash write (menu, CD, serial /rom): chip type, bar,
// block n / count with its step, blocks unchanged / written / failed, bank and address.
// The callback gets the 16 KB block in the low byte and the page offset (page chips) above.
static struct { char title[40], chip[32]; u32 off, first, count, same, written, failed; } fu;
static char l0[40], l1[40], l2[40], l3[40];         // flash screen details, kept for the result
void flash_ui_begin(const char *title, u32 off, u32 size) {
    u8 m = 0xFF, d = 0xFF;
    if (off >= ROM3_OFF) { m = 0xBF; d = 0x10; flash_identify(&m, &d); }   // chip 2: same type, not probed
    else flash_identify(&m, &d);
    char *p = fmt_str(fu.chip, flash_chip_name(m, d));
    p = fmt_str(p, " (");
    p = fmt_hex(p, m, 2);
    p = fmt_str(p, "/");
    p = fmt_hex(p, d, 2);
    fmt_str(p, ")");
    u32 bank = off >= ROM3_OFF ? 3 : off >= ROM2_OFF ? 2 : 1;
    p = fmt_str(fu.title, title);
    p = fmt_str(p, " bank ");
    fmt_dec(p, bank);
    fu.off = off;
    fu.first = off / FLASH_SECT;
    fu.count = size / FLASH_SECT;
    fu.same = fu.written = fu.failed = 0;
}
void flash_ui(u32 s, const char *what) {
    // never between two pages of a block: page chips need the bytes of a page < 150 us apart,
    // and drawing (GPU) right before a page load cut writes to the Xplorer's second chip
    if (what[0] == 'p') return;
    u32 blk = (s & 0xFF) - fu.first, page = s >> 8;
    int end = what[0] == 'o' || what[0] == 's' || what[0] == 'F';   // ok / same / FAILED
    if (what[0] == 's') fu.same++;
    else if (what[0] == 'o') fu.written++;
    else if (what[0] == 'F') fu.failed++;
    fmt_str(l0, fu.chip);
    char *p = fmt_str(l1, "block ");
    p = fmt_dec(p, blk + 1);
    p = fmt_str(p, " / ");
    p = fmt_dec(p, fu.count);
    p = fmt_str(p, " (16 KB): ");
    fmt_str(p, what[0] == 'p' ? "write" : what);
    p = fmt_str(l2, "unchanged ");
    p = fmt_dec(p, fu.same);
    p = fmt_str(p, "  written ");
    p = fmt_dec(p, fu.written);
    p = fmt_str(p, "  failed ");
    fmt_dec(p, fu.failed);
    u32 bank = fu.off >= ROM3_OFF ? 3 : fu.off >= ROM2_OFF ? 2 : 1;
    p = fmt_str(l3, "bank ");
    p = fmt_dec(p, bank);
    p = fmt_str(p, bank == 1 ? " (OpenPsxRom) at " : " at ");
    p = fmt_hex(p, 0x1F000000u + fu.off, 8);
    p = fmt_str(p, ", ");
    p = fmt_dec(p, fu.count * 16);
    fmt_str(p, " KB");
    // last block done: the result under the details, as the bar reaches its end
    const char *res = (end && blk + 1 == fu.count) ? (fu.failed ? "FLASH FAILED" : "FLASH OK") : 0;
    const char *info[5] = {l0, l1, l2, l3, res};
    ui_progress_info(fu.title, blk * FLASH_SECT + (end ? FLASH_SECT : page), fu.count * FLASH_SECT, info);
}
// End of a flash: the flash screen stays (bar, details, result line) until a button is pressed;
// meanwhile the serial port is served as in the menu, so another /rom can start right away; other
// commands (/ping...) leave it there. A flash started from this screen ends on it again: the new
// result replaces the shown one (no nesting), and only a button goes back to the menu.
void ui_flash_result(const char *result) {
    static const char *shown;
    if (shown) { shown = result; return; }              // called from the serial poll below
    shown = result;
    pad_now = pad_old = pad_read();                     // buttons already held do not count
    for (;;) {
        const char *info[5] = {l0, l1, l2, l3, shown};
        ui_progress_draw(fu.title, 1, 1, info);
        help("any button: back to the menu   (serial ready)");
        frame_end();
        if (pad_now & ~pad_old) break;
    }
    shown = 0;
}
static int flash_area(u32 off, const u8 *data, u32 size) {
    flash_ui_begin("saving to", off, size);
    return flash_write_at(off, data, size, flash_ui);
}

// ---------------------------------------------------------------- memory card manager
// Two file lists side by side: the memory card (left) and the saves kept in the cartridge (right).
static McEntry mce[15], ste[64];
static int mcn, stn;

static int store_writer(u32 off, const u8 *data, u32 size) { return flash_area(off, data, size); }

static void load_card(int port) {
    mcn = mc_list(port, mce);
    for (int i = 0; i < mcn; i++) if (mce[i].has_icon) gfx_icon_load(i, mce[i].icon, mce[i].has_icon, mce[i].clut);
}
static u32 st_kb;
static void load_store(void) {
    stn = store_count();
    st_kb = store_packed_kb();
    for (int i = 0; i < stn; i++) { store_entry(i, &ste[i]); if (ste[i].has_icon) gfx_icon_load(16 + i, ste[i].icon, ste[i].has_icon, ste[i].clut); }
}

#define PROWS 7
static void draw_pane(int x, const char *title, const char *info, McEntry *e, int n, int icon0,
                      int sel, int *top, int focus, const char *empty_msg) {
    gfx_shade(x, 36, 150, 172, focus ? 0x402010 : 0x000000);
    gfx_rect(x, 36, 150, 14, focus ? 0x9A5A28 : 0x402818);
    gfx_text(x + 4, 39, focus ? C_SEL : C_TEXT, title);
    gfx_text(x + 146 - gfx_text_w(info), 39, C_DIM, info);
    if (n <= 0) { gfx_text(x + 6, 60, C_DIM, empty_msg); return; }
    if (sel < *top) *top = sel;
    if (sel >= *top + PROWS) *top = sel - PROWS + 1;
    for (int i = *top; i < n && i < *top + PROWS; i++) {
        int y = 54 + (i - *top) * 21;
        if (i == sel && focus) gfx_grad(x + 2, y - 1, 146, 20, 0xC07838, 0x602810);
        else if (i == sel) gfx_rect(x + 2, y - 1, 146, 20, 0x3A2A20);
        if (e[i].has_icon) gfx_icon(icon0 + i, (anim_tick / 16) % e[i].has_icon, x + 4, y + 1);   // BIOS-like pace
        else gfx_rect(x + 4, y + 1, 16, 16, 0x404040);
        char t[24];
        int k = 0;
        for (; k < 20 && e[i].title[k]; k++) t[k] = e[i].title[k];
        t[k] = 0;
        gfx_text(x + 24, y + 1, i == sel && focus ? C_SEL : C_TEXT, t);
        char b[16], *p = fmt_dec(b, e[i].blocks);
        fmt_str(p, e[i].blocks > 1 ? " blocks" : " block");
        gfx_text(x + 24, y + 10, C_DIM, b);
    }
}

static void screen_cards(void) {
    int port = 0, side = 0, sel[2] = {0, 0}, top[2] = {0, 0}, reload_card = 1;
    frame_begin("Memory cards");
    gfx_text(24, 100, C_TEXT, "reading the cartridge...");
    gfx_flip();
    store_load();
    load_store();
    for (;;) {
        if (reload_card) {
            frame_begin("Memory cards");
            gfx_text(24, 100, C_TEXT, "reading the memory card...");
            gfx_flip();
            load_card(port);
            if (sel[0] >= mcn) sel[0] = mcn > 0 ? mcn - 1 : 0;
            reload_card = 0;
        }
        if (sel[1] >= stn) sel[1] = stn > 0 ? stn - 1 : 0;
        frame_begin("Memory cards");
        char li[16], ri[16], *p;
        if (mcn >= 0) { p = fmt_dec(li, mc_free_blocks()); fmt_str(p, " free"); } else fmt_str(li, "");
        p = fmt_dec(ri, st_kb); fmt_str(p, "/48 KB");
        draw_pane(6, port ? "Card slot 2" : "Card slot 1", li, mce, mcn, 0, sel[0], &top[0], side == 0,
                  mcn < 0 ? "no card" : "empty card");
        draw_pane(164, "Cartridge", ri, ste, stn, 16, sel[1], &top[1], side == 1, "no save yet");
        {   // full title of the selected save
            McEntry *e = side ? (stn > 0 ? &ste[sel[1]] : 0) : (mcn > 0 ? &mce[sel[0]] : 0);
            if (e) gfx_text(8, 199, 0xA0C0D0, e->title);
        }
        help(G_LEFT " " G_RIGHT " side " G_CROSS " copy " G_TRIANGLE " delete L1/R1 slot " G_CIRCLE);
        frame_end();
        if (back()) return;
        if (pressed(PAD_LEFT)) side = 0;
        if (pressed(PAD_RIGHT)) side = 1;
        if (pressed(PAD_L1) || pressed(PAD_R1)) { port ^= 1; reload_card = 1; sel[0] = top[0] = 0; }
        int n = side ? stn : mcn;
        if (n > 0) {
            if (moved(PAD_UP)) sel[side] = (sel[side] + n - 1) % n;
            if (moved(PAD_DOWN)) sel[side] = (sel[side] + 1) % n;
        }
        if (pressed(PAD_CROSS) && n > 0) {
            if (side == 0) {                                   // card -> cartridge
                McEntry *e = &mce[sel[0]];
                u8 *buf = (u8 *)0x80160000;
                static u8 dirf[128];
                ui_progress("reading the save", 0, 1);
                if (mc_read_file(port, e, buf, dirf)) { message("Copy", "could not read the save", 0, C_ERR); continue; }
                if (store_add(dirf, buf)) { message("Copy", "the cartridge is full", 0, C_ERR); continue; }
                int r = store_save(store_writer);
                if (r) {
                    store_load();                              // back to what is really in the flash
                    message("Copy", r == -2 ? "the cartridge is full (48 KB)" : "flash write error", 0, C_ERR);
                }
                load_store();
                for (int i = 0; i < stn; i++) {
                    int same = 1;
                    for (int k = 0; k < 21; k++) if (ste[i].name[k] != e->name[k]) { same = 0; break; }
                    if (same) sel[1] = i;
                }
            } else {                                           // cartridge -> card
                if (mcn < 0) { message("Copy", "no memory card in this slot", 0, C_ERR); continue; }
                int i = sel[1];
                ui_progress("writing the save", 0, 1);
                int r = mc_write_file(port, store_dirf(i), store_data(i), ste[i].blocks);
                if (r) message("Copy", r == -2 ? "not enough free blocks on the card" : r == -3 ?
                               "this save is already on the card" : "memory card write error", 0, C_ERR);
                reload_card = 1;
            }
        }
        if (pressed(PAD_TRIANGLE) && n > 0) {
            McEntry *e = side ? &ste[sel[1]] : &mce[sel[0]];
            if (!confirm("Delete", e->title, side ? "from the cartridge" : "from the memory card")) continue;
            if (side == 0) {
                if (mc_delete_file(port, e)) message("Delete", "memory card write error", 0, C_ERR);
                reload_card = 1;
            } else {
                store_delete(sel[1]);
                ui_progress("writing to the cartridge", 0, 1);
                if (store_save(store_writer)) { store_load(); message("Delete", "flash write error", 0, C_ERR); }
                load_store();
            }
        }
    }
}

// ---------------------------------------------------------------- CD browser
#define MAXE 128
static CdEntry cde[MAXE];
static int cdn;
static const char *cd_label(int i, char *b) {
    char *p = fmt_str(b, cde[i].name);
    if (cde[i].dir) { fmt_str(p, "/"); return b; }
    while (p < b + 18) *p++ = ' ';
    p = fmt_dec(p, (cde[i].size + 1023) / 1024);
    fmt_str(p, " KB");
    return b;
}
static int has_ext(int i, const char *e);
static u32 cd_color(int i) {
    if (cde[i].dir) return C_TITLE;
    int n = strlen(cde[i].name);
    if (n > 4 && cde[i].name[n - 4] == '.' && cde[i].name[n - 3] == 'R' && cde[i].name[n - 2] == 'O' &&
        cde[i].name[n - 1] == 'M') return 0x40A0FF;
    if (has_ext(i, ".STR")) return 0xFF80C0;                  // video
    return n > 4 && cde[i].name[n - 4] == '.' && cde[i].name[n - 3] == 'E' && cde[i].name[n - 2] == 'X'
           && cde[i].name[n - 1] == 'E' ? C_OK : C_TEXT;
}
static int has_ext(int i, const char *e) {
    int n = strlen(cde[i].name), m = strlen(e);
    if (cde[i].dir || n < m) return 0;
    for (int k = 0; k < m; k++) if (cde[i].name[n - m + k] != e[k]) return 0;
    return 1;
}
static int is_rom(int i) {
    int n = strlen(cde[i].name);
    return !cde[i].dir && n > 4 && cde[i].name[n - 4] == '.' && cde[i].name[n - 3] == 'R' &&
           cde[i].name[n - 2] == 'O' && cde[i].name[n - 1] == 'M';
}
// Confirmation (hold START + cross), then write + verify. Returns 0 if cancelled.
static int flash_confirm(const char *what) {
    u8 mfr, dev;
    int known = flash_identify(&mfr, &dev);
    for (;;) {
        frame_begin("Flash ROM");
        gfx_text(24, 52, C_TEXT, "Write this ROM to the cartridge?");
        gfx_text(24, 70, C_SEL, what);
        gfx_text(24, 92, known ? C_DIM : C_ERR, known ? flash_chip_name(mfr, dev) : "no known flash chip found!");
        gfx_text(24, 120, C_ERR, "The current ROM will be replaced.");
        gfx_text(24, 140, C_DIM, "Keep the console on until the end.");
        help("hold START + " G_CROSS " write    " G_CIRCLE " cancel");
        frame_end();
        if (back()) return 0;
        if (known && (pad_now & PAD_START) && pressed(PAD_CROSS)) return 1;
    }
}
// An OpenROM image only takes the first 64 KB: the saves and cheats after it are kept.
int is_openrom_image(const u8 *img) {
    // current name, and the former one (images flashed before the rename)
    static const char *const ids[2] = {"OpenPsxRom - open source", "OpenROM - open source"};
    for (int v = 0; v < 2; v++) {
        int k = 0;
        while (ids[v][k] && img[0x30 + k] == (u8)ids[v][k]) k++;
        if (!ids[v][k]) return 1;
    }
    return 0;
}
// Which bank an image is built for, from its pre-boot entry: 1F020000h-1F03FFFFh with Unirom's
// "ROM2" marker at 200h = bank 2 (Unirom's own lower half also jumps to 1F020100h, without the
// marker: that one is a bank 1 image), 1F040000h-1F05FFFFh = bank 3, anything else = bank 1.
int rom_image_bank(const u8 *img) {
    u32 entry = img[0x80] | (img[0x81] << 8) | (img[0x82] << 16) | ((u32)img[0x83] << 24);
    // by the entry point only: an image built for bank 1 can never start in 1F020000h-1F03FFFFh
    // (requiring Unirom's 'ROM2' tag sent a relocated Caetla to bank 1, over OpenPsxRom)
    if (entry >= 0x1F020000u && entry < 0x1F040000u) return 2;
    if (entry >= 0x1F040000u && entry < 0x1F060000u) return 3;
    return 1;
}
// A second chip at 40000h, found by reading only: a single 256 KB chip shows 00000h again there
// (address wrap). No command is ever sent to it outside a flash write: the Xplorer FX's second
// chip, once its software data protection got lost, takes every command as data, and the 6-byte
// ID probe at each boot wiped it (the BIOS check: 'Licensed by Sony' at bank 3 stays readable).
int chip2_present(void) {
    u8 m, d;
    flash_identify(&m, &d);
    // only behind a 256 KB first chip (Xplorer FX): a 512 KB chip covers 40000h itself, and a
    // 128 KB chip (Datel Am29F010) reads FFh or open bus there, which is no chip at all
    if (flash_chip_kb(m, d) != 256) return 0;
    const volatile u8 *a = (const volatile u8 *)FLASH_BASE, *b = a + ROM3_OFF;
    for (u32 i = 0; i < 0x1000; i++) if (a[i] != b[i]) return 1;
    return 0;
}
// room for a bank: 2 needs a chip of 256 KB+, 3 a second chip at 40000h
int bank_available(int bank) {
    u8 m, d;
    if (bank == 2) { flash_identify(&m, &d); return flash_chip_kb(m, d) >= 256; }
    if (bank == 3) return chip2_present();
    return 1;
}
static void flash_image(const u8 *img) {
    int bad, bank = rom_image_bank(img);
    if (bank > 1) {
        if (!bank_available(bank)) { message("Flash ROM", bank == 2 ? "no bank 2 on this cartridge" : "no bank 3 (second chip)", "nothing written", C_ERR); return; }
        flash_ui_begin("flashing", bank == 2 ? ROM2_OFF : ROM3_OFF, FLASH_SIZE);
        bad = flash_write_at(bank == 2 ? ROM2_OFF : ROM3_OFF, img, FLASH_SIZE, flash_ui);
        message(bank == 2 ? "Flash ROM bank 2" : "Flash ROM bank 3", bad ? "FAILED: some blocks are wrong" : "written and verified, OpenPsxRom untouched",
                bad ? "OpenPsxRom untouched" : "switch OFF + Boot options to start it", bad ? C_ERR : C_OK);
        return;
    }
    flash_ui_begin("flashing", 0, is_openrom_image(img) ? FL_ROM_SIZE : FLASH_SIZE);
    bad = flash_write(img, is_openrom_image(img) ? FL_ROM_SIZE : FLASH_SIZE, flash_ui);
    message("Flash ROM", bad ? "FAILED: some blocks are wrong" : "ROM written and verified",
            bad ? "do not reset: try again" : "reset the console to start it", bad ? C_ERR : C_OK);
}
static int rom_header_ok(const u8 *b) {
    static const char lic[] = "Licensed by Sony";
    for (int k = 0; lic[k]; k++) if (b[0x84 + k] != (u8)lic[k]) return 0;
    return 1;
}
static void flash_from_cd(int i, char *err) {
    u8 *buf = (u8 *)0x80100000;
    if (cde[i].size > FLASH_SIZE) { fmt_str(err, "ROM bigger than 128 KB"); return; }
    if (!flash_confirm(cde[i].name)) return;
    ui_progress("reading the CD", 0, 1);
    memset(buf, 0xFF, FLASH_SIZE);
    if (cd_read(cde[i].lba, (cde[i].size + 2047) / 2048, buf)) { fmt_str(err, "CD read error"); return; }
    if (!rom_header_ok(buf)) { fmt_str(err, "no ROM header at 84h: not written"); return; }
    flash_image(buf);
}
#ifdef BOOTDISC
extern const u8 _binary_openrom_rom_start[];
static void install_rom(void) {
    if (!flash_confirm("OpenPsxRom " OPENROM_VERSION " (from this disc)")) return;
    flash_image(_binary_openrom_rom_start);
}
#endif
static void screen_cd(void) {
    u32 stack_lba[8], stack_size[8];
    int depth = 0, sel = 0, top = 0, reload = 1, mounted = 0;
    char err[48] = "";
    for (;;) {
        if (reload) {
            frame_begin("CD browser");
            gfx_text(24, 100, C_TEXT, "reading the disc...");
            gfx_flip();
            if (!mounted) {
                cd_init();
                if (iso_root(&stack_lba[0], &stack_size[0]) == 0) mounted = 1;
                depth = 0;
            }
            cdn = mounted ? iso_list(stack_lba[depth], stack_size[depth], cde, MAXE) : -1;
            sel = top = 0;
            reload = 0;
        }
        frame_begin("CD browser");
        if (!mounted || cdn < 0) gfx_text(24, 60, C_ERR, "no readable disc (ISO9660)");
        else list_draw(cdn, sel, &top, cd_label, cd_color);
        if (err[0]) gfx_text(16, 196, C_ERR, err);
        help(G_CROSS " open/run    " G_CIRCLE " up    " G_TRIANGLE " reload disc");
        frame_end();
        if (pressed(PAD_TRIANGLE)) { mounted = 0; reload = 1; err[0] = 0; }
        if (back()) {
            if (depth == 0) return;
            depth--; reload = 1;
        }
        if (cdn > 0) {
            sel = list_move(cdn, sel);
            if (pressed(PAD_CROSS)) {
                if (cde[sel].dir && depth < 7) {
                    depth++;
                    stack_lba[depth] = cde[sel].lba;
                    stack_size[depth] = cde[sel].size;
                    reload = 1;
                } else if (is_rom(sel)) {
                    flash_from_cd(sel, err);
                } else if (has_ext(sel, ".STR")) {
#if CONFIG_STR_PLAYER && !defined(ARCART)   // no room for it on a Datel cart (sectors 1-4 only)
                    str_play(cde[sel].lba, cde[sel].size, err);
                    pad_now = pad_old = pad_read();
#else
                    fmt_str(err, "no video player in this build");
#endif
                } else if (cd_color(sel) == C_OK) {
                    frame_begin("CD browser");
                    gfx_text(24, 100, C_TEXT, "loading...");
                    gfx_flip();
                    exe_run_from_cd(cde[sel].lba, cde[sel].size, err);   // returns on error
                }
            }
        }
    }
}

// ---------------------------------------------------------------- hex editor
static void screen_hex(void) {
    static const u32 bases[] = {0x80000000, 0x801A0000, 0x1F000000, 0xBFC00000, 0x1F800000};
    static const char *const names[] = {"RAM", "OpenPsxRom", "cart ROM", "BIOS", "scratchpad"};
    static int region;
    static u32 addr = 0x80000000;
    int cur = 0;
    for (;;) {
        frame_begin("Hex editor");
        char s[64], *p;
        for (int r = 0; r < 16; r++) {
            u32 a = addr + r * 8;
            int y = 40 + r * 10;
            p = fmt_hex(s, a, 8);
            *p++ = ' ';
            for (int c = 0; c < 8; c++) {
                *p++ = ' ';
                p = fmt_hex(p, *(volatile u8 *)(a + c), 2);
            }
            *p++ = ' '; *p++ = ' ';
            for (int c = 0; c < 8; c++) {
                u8 v = *(volatile u8 *)(a + c);
                *p++ = v >= 32 && v < 127 ? v : '.';
            }
            *p = 0;
            if (r == cur / 8) {
                int x = 8 + (10 + (cur % 8) * 3) * 6;
                gfx_rect(x - 1, y - 1, 14, 10, 0x502810);
            }
            gfx_text(8, y, C_TEXT, s);
        }
        int writable = (addr >= 0x80000000 && addr < 0x80200000) || (addr >= 0x1F800000 && addr < 0x1F800400);
        help(writable ? "L1/R1 page  SELECT zone  " G_TRIANGLE " +1  " G_SQUARE " -1  " G_CIRCLE " back" : "L1/R1 page  SELECT zone  (read only)  " G_CIRCLE " back");
        frame_end();
        if (back()) return;
        if (moved(PAD_LEFT)) cur--;
        if (moved(PAD_RIGHT)) cur++;
        if (moved(PAD_UP)) cur -= 8;
        if (moved(PAD_DOWN)) cur += 8;
        if (cur < 0) { cur += 8; addr -= 8; }
        if (cur > 127) { cur -= 8; addr += 8; }
        if (pressed(PAD_L1)) addr -= 128;
        if (pressed(PAD_R1)) addr += 128;
        if (pressed(PAD_L2)) addr -= 0x10000;
        if (pressed(PAD_R2)) addr += 0x10000;
        if (pressed(PAD_SELECT)) {
            region = (region + 1) % 5;
            addr = bases[region];
            cur = 0;
            sio_status[0] = 0;
            fmt_str(fmt_str(sio_status, "hex editor: "), names[region]);
        }
        if (writable) {
            volatile u8 *b = (volatile u8 *)(addr + cur);
            if (pressed(PAD_TRIANGLE)) *b = *b + 1;
            if (pressed(PAD_SQUARE)) *b = *b - 1;
        }
    }
}

// ---------------------------------------------------------------- cheats (GameShark codes)
// Kept in the cartridge flash at 1C000h. 'OCH2': u32 lines, u32 cheats, char game[40], then lines x
// (u32 address, u32 value | enabled << 16 | cheat << 24), and at +400h the cheat names (32 bytes
// each). A cheat groups the lines of one code (tools/cheats.py writes them from a database).
// 'OCH1' (older): the same lines without names, one cheat per line. Applied by the resident core
// on every interrupt while the game runs.
#define MAXCH 96
#define CH_NAME 32
typedef struct { u32 addr; u16 val; u8 on, grp; } Cheat;
static Cheat ch[MAXCH];
static int nch, ngrp;
static char ch_name[MAXCH][CH_NAME], ch_game[40];

void cheats_load(void) {             // also after tools/cheats.py wrote the area (protocol.c)
    const u32 *f = (const u32 *)(FLASH_BASE + FL_CHEAT_OFF);
    const char *names = (const char *)f + 0x400;
    int v2 = f[0] == 0x3248434Fu;                                  // "OCH2"
    nch = ngrp = 0;
    ch_game[0] = 0;
    if ((f[0] != 0x3148434Fu && !v2) || f[1] > MAXCH) return;      // "OCH1"
    const u32 *l = v2 ? f + 13 : f + 2;
    u32 g = v2 ? f[2] : f[1];
    if (g > MAXCH) return;
    for (u32 i = 0; i < f[1]; i++) {
        ch[i].addr = l[i * 2];
        ch[i].val = (u16)l[i * 2 + 1];
        ch[i].on = (l[i * 2 + 1] >> 16) & 1;
        ch[i].grp = v2 ? (u8)(l[i * 2 + 1] >> 24) : (u8)i;
        if (ch[i].grp >= g) return;
    }
    for (u32 k = 0; k < g; k++) {
        int n = 0;
        while (v2 && n < CH_NAME - 1 && names[k * CH_NAME + n] >= 32 && names[k * CH_NAME + n] < 127) {
            ch_name[k][n] = names[k * CH_NAME + n];
            n++;
        }
        ch_name[k][n] = 0;
    }
    for (int n = 0; v2 && n < 39; n++) {
        char c = ((const char *)(f + 3))[n];
        if (c < 32 || c >= 127) break;
        ch_game[n] = c;
        ch_game[n + 1] = 0;
    }
    nch = f[1];
    ngrp = g;
}
static int cheats_save(void) {
    u32 *b = (u32 *)0x80100000;
    memset(b, 0xFF, 0x4000);
    b[0] = 0x3248434Fu;
    b[1] = nch;
    b[2] = ngrp;
    memset(b + 3, 0, 40);
    memcpy(b + 3, ch_game, strlen(ch_game));
    for (int i = 0; i < nch; i++) {
        b[13 + i * 2] = ch[i].addr;
        b[14 + i * 2] = ch[i].val | ((u32)ch[i].on << 16) | ((u32)ch[i].grp << 24);
    }
    memset((u8 *)b + 0x400, 0, ngrp * CH_NAME);
    for (int k = 0; k < ngrp; k++) memcpy((u8 *)b + 0x400 + k * CH_NAME, ch_name[k], strlen(ch_name[k]));
    memcpy(b + (FL_CFG_OFF - FL_CHEAT_OFF) / 4, (const void *)(FLASH_BASE + FL_CFG_OFF), 128);   // keep the settings
    return flash_area(FL_CHEAT_OFF, (u8 *)b, 0x4000);
}

// ---------------------------------------------------------------- persistent settings (flash)
// 'OCF1' + flags at 1FF80h, read by the pre-boot code (header.s) before anything else runs.
static u32 cfg_flags(void) {
    const u32 *f = (const u32 *)(FLASH_BASE + FL_CFG_OFF);
    return f[0] == CFG_MAGIC ? f[1] : 0;
}
// a firmware the switch can start: "Licensed by Sony" header + pre-boot entry inside its bank
int rom_bank_ok(int bank) {
    if (bank != 2 && bank != 3) return 0;
    u32 b = FLASH_BASE + (bank == 2 ? ROM2_OFF : ROM3_OFF);
    if (*(const u32 *)(b + 0x84) != 0x6563694Cu) return 0;                 // "Lice"
    u32 e = *(const u32 *)(b + 0x80);
    return e >= b && e < b + 0x20000u;
}
static int cfg_bank(void) {
    if (*(const u32 *)(FLASH_BASE + FL_CFG_OFF) != CFG_MAGIC)     // nothing saved: build default
        return CONFIG_DEF_SWITCH_OFF_BANK2 ? 2 : CONFIG_DEF_SWITCH_OFF_BANK3 ? 3 : 0;
    int v = cfg_flags() & CFG_BANK_MASK;
    return v == 1 ? 2 : v;
}
static int cfg_save(u32 flags) {
    u32 *b = (u32 *)0x80100000;
    memcpy(b, (const void *)(FLASH_BASE + FL_CHEAT_OFF), 0x4000);
    u32 *c = b + (FL_CFG_OFF - FL_CHEAT_OFF) / 4;
    memset(c, 0xFF, 128);
    c[0] = CFG_MAGIC;
    c[1] = flags;
    return flash_area(FL_CHEAT_OFF, (u8 *)b, 0x4000);   // unchanged pages are skipped: one page written
}
// all codes with their state: (address, value | enabled << 16); returns the count
static int cheats_all(u32 *out, int *any_on) {
    *any_on = 0;
    for (int i = 0; i < nch; i++) { out[2 * i] = ch[i].addr; out[2 * i + 1] = ch[i].val | ((u32)ch[i].on << 16); *any_on |= ch[i].on; }
    return nch;
}

// ---------------------------------------------------------------- boot options / resident core
// The core (core/core.c) is copied to 8000C000h when the CD is booted, if an option needs it.
extern const u8 _binary_core_bin_start[], _binary_core_bin_end[];
#include "shared.h"
#if defined(AUTOBOOT) && !defined(NOCORE)
static int opt_debug = 1, opt_tty = 1, opt_crash = 1, opt_video = VIDEO_TEST;
#elif defined(NOCORE)
static int opt_debug, opt_tty, opt_crash, opt_video;
#else                        // defaults set in make menuconfig (Boot options: defaults)
static int opt_debug = CONFIG_DEF_SERIAL_DEBUG, opt_tty = CONFIG_DEF_PRINTF, opt_crash = CONFIG_DEF_CRASH_SCREEN,
           opt_video = CONFIG_DEF_VIDEO_PAL ? 1 : CONFIG_DEF_VIDEO_NTSC ? 2 : 0;
#endif
static int opt_menu = CONFIG_DEF_INGAME_MENU;     // video: 0 auto, 1 force PAL, 2 force NTSC

void core_boot(void);
static int core_in;               // the core is in RAM and hooked: never hook it twice
void core_debug(void) {
    int save_d = opt_debug, save_t = opt_tty, save_c = opt_crash;
    opt_debug = opt_tty = opt_crash = 1;
    core_boot();
    opt_debug = save_d; opt_tty = save_t; opt_crash = save_c;
}
void core_boot(void) {
    static u32 codes[MAXCH * 2];
    int on, n = cheats_all(codes, &on);
    u32 f = (opt_debug ? F_DEBUG : 0) | (opt_tty ? F_TTY : 0) | (opt_crash ? F_CRASH : 0) |
            (opt_video == 1 ? F_PAL : 0) | (opt_video == 2 ? F_NTSC : 0) | (n ? F_CHEATS : 0) |
            (opt_menu ? F_MENU : 0);
    if (!f) return;
    if (core_in) {                     // already there (/debug): settings merged, hooks set again
        // (SetConf, when a disc boots, rebuilds the kernel's interrupt chains without our entry)
        f |= ((volatile Shared *)SH_ADDR)->flags;
        ((void (*)(u32, const u32 *, int))0x8000C000)(f, codes, n);
        return;
    }
    core_in = 1;
    memset((void *)0x8000C000, 0, 0x2000);
    memcpy((void *)0x8000C000, _binary_core_bin_start, _binary_core_bin_end - _binary_core_bin_start);
    flush_cache();
    ((void (*)(u32, const u32 *, int))0x8000C000)(f, codes, n);
    sio_puts("OpenPsxRom: resident core installed\r\n");
}

static void screen_options(void) {
    int sel = 0;
    static const char *const vid[] = {"as the game", "force PAL (50 Hz)", "force NTSC (60 Hz)"};
    u32 cfg = cfg_flags(), cfg0 = cfg;
    int bank = cfg_bank(), bank0 = bank;                // 0 bypass, 2 or 3
    static const char *const bank_name[] = {"bypass", "", "ROM bank 2", "ROM bank 3"};
    int any = rom_bank_ok(2) || rom_bank_ok(3);
    for (;;) {
        frame_begin("Boot options");
        const char *v[6] = {opt_menu ? "on" : "off", opt_debug ? "on" : "off", opt_tty ? "on" : "off", opt_crash ? "on" : "off", vid[opt_video],
                            bank_name[bank & 3]};
        static const char *const names[] = {"In-game menu (switch)", "Serial debug in game", "printf -> serial", "Crash screen", "Video mode",
                                            "Switch OFF at power-on"};
        const char *desc[] = {
            "switch off/on or L1R1L2R2+START+SELECT",
            "nops /dump /bin /exe /halt /cont while it runs",
            "game and BIOS printf go to the serial port",
            "registers on screen + serial if it crashes",
            "for imports: PAL or NTSC picture",
            bank == 2 ? "starts the firmware at 1F020000h (bank 2)" : bank == 3 ? "starts the firmware at 1F040000h (bank 3)"
            : any ? "normal boot, the cartridge stays invisible" : "no other firmware in banks 2/3: bypass only",
        };
        for (int i = 0; i < 6; i++) {
            int y = 44 + i * 18;
            if (i == sel) { gfx_grad(16, y - 4, 288, 16, 0xC07838, 0x602810); gfx_rect(16, y - 4, 2, 16, 0x40D8FF); }
            gfx_text(28, y, i == sel ? C_SEL : C_TEXT, names[i]);
            gfx_text(300 - gfx_text_w(v[i]), y, (v[i][1] == 'f' && v[i][2] == 'f') ? C_DIM : C_OK, v[i]);
        }
        gfx_text(160 - gfx_text_w(desc[sel]) / 2, 156, 0xA0C0D0, desc[sel]);
        gfx_text(28, 178, C_DIM, sel == 5 ? "Saved in the cartridge, used at power-on." : "Applied when the CD is booted.");
        help(G_LEFT " " G_RIGHT " change    " G_CIRCLE " back");
        frame_end();
        if (back()) {
            // a bank choice equal to "nothing saved" (bypass) must still be saved over a default
            if ((cfg != cfg0 || bank != bank0) && cfg_save((cfg & ~CFG_BANK_MASK) | (u32)bank))
                message("Boot options", "flash write error", 0, C_ERR);
            return;
        }
        sel = list_move(6, sel) % 6;
        if (moved(PAD_LEFT) || moved(PAD_RIGHT) || pressed(PAD_CROSS)) {
            int d = moved(PAD_LEFT) ? -1 : 1;
            if (sel == 0) opt_menu ^= 1;
            if (sel == 1) opt_debug ^= 1;
            if (sel == 2) opt_tty ^= 1;
            if (sel == 3) opt_crash ^= 1;
            if (sel == 4) opt_video = (opt_video + 3 + d) % 3;
            if (sel == 5) {                             // bypass -> bank 2 -> bank 3, only banks holding a ROM
                static const int order[3] = {0, 2, 3};
                int k = bank == 2 ? 1 : bank == 3 ? 2 : 0;
                for (int t = 0; t < 3; t++) {
                    k = (k + 3 + d) % 3;
                    if (order[k] == 0 || rom_bank_ok(order[k])) break;
                }
                bank = order[k];
                cfg = (cfg & ~CFG_BANK_MASK) | (u32)bank;
            }
        }
    }
}

// ---------------------------------------------------------------- about
static void screen_about(void) {
    for (;;) {
        frame_begin("About");
        int y = 44;
        static const char *const l[] = {
            "Open source PS1 cartridge firmware.",
            "",
            "Serial (NOPS, 115200 or /fast):",
            "  /exe /bin /poke /jmp /jal /dump",
            "  /watch /rom /mcup /mcdown",
            "",
            "Boot CD: leaves the menu, the BIOS boots.",
            "Unlock: lets the drive read burned",
            "  discs (until the lid is opened).",
            "",
            "Runs from RAM at 801A0000h: the flash",
            "chip can be rewritten at any time.",
        };
        for (u32 i = 0; i < sizeof l / sizeof l[0]; i++, y += 12) gfx_text(16, y, C_TEXT, l[i]);
        help(G_CIRCLE " back");
        frame_end();
        if (back()) return;
    }
}

// ---------------------------------------------------------------- VRAM viewer (like Caetla)
// Shows the video memory itself (nothing is drawn, so nothing in it is changed).
static void screen_vram(void) {
    static const u16 widths[] = {256, 320, 368, 512, 640};
    static const u8 hbits[] = {0x00, 0x01, 0x40, 0x02, 0x03};
    for (;;) {                                          // help page first
        frame_begin("VRAM viewer");
        static const char *const l[] = {
            "Shows the whole video memory (1024x512).",
            "",
            G_UP " " G_DOWN " " G_LEFT " " G_RIGHT "  scroll (hold: fast)",
            "L1 / R1   display width 256..640",
            "R2        15-bit / 24-bit (MDEC video)",
            "SELECT    240 / 480 lines",
            G_CIRCLE "         back to the menu",
        };
        for (u32 i = 0; i < sizeof l / sizeof l[0]; i++) gfx_text(24, 50 + i * 14, C_TEXT, l[i]);
        help(G_CROSS " start    " G_CIRCLE " back");
        frame_end();
        if (back()) return;
        if (pressed(PAD_CROSS)) break;
    }
    int x = 0, y = 0, w = 1, d24 = 0, tall = 0;
    for (;;) {
        u32 mode = hbits[w] | (d24 ? 0x10 : 0) | (tall ? 0x24 : 0);
        gfx_raw_display(x, y, mode);
        pad_old = pad_now;
        pad_now = pad_filtered();
        if (pad_now & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) rep_cnt++; else rep_cnt = 0;
        int step = rep_cnt > 30 ? 16 : 4;
        if (pad_now & PAD_LEFT) x -= step;
        if (pad_now & PAD_RIGHT) x += step;
        if (pad_now & PAD_UP) y -= step;
        if (pad_now & PAD_DOWN) y += step;
        if (pressed(PAD_L1) && w > 0) w--;
        if (pressed(PAD_R1) && w < 4) w++;
        if (pressed(PAD_R2)) d24 ^= 1;
        if (pressed(PAD_SELECT)) tall ^= 1;
        int vw = d24 ? widths[w] * 3 / 2 : widths[w];
        if (x < 0) x = 0;
        if (x > 1024 - vw) x = 1024 - vw > 0 ? 1024 - vw : 0;
        if (y < 0) y = 0;
        if (y > (tall ? 32 : 272)) y = tall ? 32 : 272;
        x &= ~1;
        sio_poll();
        if (pressed(PAD_CIRCLE)) break;
    }
    gfx_restore();
}

// ---------------------------------------------------------------- CD audio player
static void screen_audio(void) {
    int first = 0, last = 0, cur = 1, playing = 0, ok;
    frame_begin("CD audio");
    gfx_text(24, 100, C_TEXT, "reading the disc...");
    gfx_flip();
    cd_init();
    ok = cda_tracks(&first, &last) == 0;
    if (ok) cur = first < 2 && last >= 2 ? 2 : first;   // track 1 of a game disc is data
    for (;;) {
        frame_begin("CD audio");
        if (!ok) gfx_text(24, 60, C_ERR, "no disc");
        else {
            char s[48], *p;
            p = fmt_str(s, "Tracks ");
            p = fmt_dec(p, first);
            p = fmt_str(p, " - ");
            fmt_dec(p, last);
            gfx_text(24, 50, C_DIM, s);
            p = fmt_str(s, "Track ");
            fmt_dec(p, cur);
            gfx_text2(24, 74, playing ? C_OK : C_TEXT, s);
            int t, mm, ss;
            if (playing && cda_pos(&t, &mm, &ss) == 0) {
                p = fmt_dec(s, mm);
                p = fmt_str(p, ss < 10 ? ":0" : ":");
                fmt_dec(p, ss);
                gfx_text2(200, 74, C_SEL, s);
                if (t != cur && t >= first) cur = t;
            }
            gfx_text(24, 110, playing ? C_OK : C_DIM, playing ? "playing" : "stopped");
            if (cur == 1) gfx_text(24, 140, C_DIM, "(track 1 of a game disc is data, not audio)");
        }
        help(G_CROSS " play  " G_SQUARE " stop  " G_LEFT " " G_RIGHT " track  " G_CIRCLE " back");
        frame_end();
        if (back()) { if (playing) cda_pause(); return; }
        if (!ok) continue;
        int nt = cur;
        if (moved(PAD_LEFT) && cur > first) nt = cur - 1;
        if (moved(PAD_RIGHT) && cur < last) nt = cur + 1;
        if (nt != cur) { cur = nt; if (playing) cda_play(cur); }
        if (pressed(PAD_CROSS)) playing = cda_play(cur) == 0;
        if (pressed(PAD_SQUARE) && playing) { cda_pause(); playing = 0; }
    }
}

// ---------------------------------------------------------------- cheats screen
static char *code_str(char *p, u32 a, u16 v) { p = fmt_hex(p, a, 8); *p++ = ' '; return fmt_hex(p, v, 4); }

// 12-digit hex editor; returns 1 if confirmed
static int edit_code(u32 *a, u16 *v) {
    u8 d[12];
    for (int i = 0; i < 8; i++) d[i] = (*a >> ((7 - i) * 4)) & 15;
    for (int i = 0; i < 4; i++) d[8 + i] = (*v >> ((3 - i) * 4)) & 15;
    int cur = 0;
    for (;;) {
        frame_begin("Cheat code");
        gfx_text(24, 52, C_DIM, "GameShark / Action Replay code:");
        int x = 60;
        for (int i = 0; i < 12; i++) {
            char c[2] = {"0123456789ABCDEF"[d[i]], 0};
            if (i == cur) gfx_grad(x - 2, 78, 16, 22, 0xC07838, 0x602810);
            gfx_text2(x, 81, i == cur ? C_SEL : C_TEXT, c);
            x += i == 7 ? 24 : 16;
        }
        gfx_text(24, 120, C_DIM, "80 16-bit write   30 8-bit write");
        gfx_text(24, 132, C_DIM, "D0-D3 / E0-E3 conditions  50 repeat");
        gfx_text(24, 144, C_DIM, "10/11/20/21 increment / decrement");
        help(G_UP " " G_DOWN " digit  " G_LEFT " " G_RIGHT " move  " G_CROSS " ok  " G_CIRCLE " cancel");
        frame_end();
        if (back()) return 0;
        if (moved(PAD_LEFT) && cur > 0) cur--;
        if (moved(PAD_RIGHT) && cur < 11) cur++;
        if (moved(PAD_UP)) d[cur] = (d[cur] + 1) & 15;
        if (moved(PAD_DOWN)) d[cur] = (d[cur] + 15) & 15;
        if (pressed(PAD_CROSS)) {
            u32 na = 0; u16 nv = 0;
            for (int i = 0; i < 8; i++) na = (na << 4) | d[i];
            for (int i = 0; i < 4; i++) nv = (u16)((nv << 4) | d[8 + i]);
            *a = na; *v = nv;
            return 1;
        }
    }
}

static int grp_on(int g) { for (int i = 0; i < nch; i++) if (ch[i].grp == g) return ch[i].on; return 0; }
static int grp_first(int g) { for (int i = 0; i < nch; i++) if (ch[i].grp == g) return i; return -1; }
// a code added by hand: its own cheat, named after it
static void grp_add(u32 a, u16 v) {
    ch[nch].addr = a; ch[nch].val = v; ch[nch].on = 1; ch[nch].grp = (u8)ngrp;
    ch_name[ngrp][0] = 0;
    nch++; ngrp++;
}
static void grp_del(int g) {
    int k = 0;
    for (int i = 0; i < nch; i++) {
        if (ch[i].grp == g) continue;
        ch[k] = ch[i];
        if (ch[k].grp > g) ch[k].grp--;
        k++;
    }
    nch = k;
    for (int n = g; n < ngrp - 1; n++) memcpy(ch_name[n], ch_name[n + 1], CH_NAME);
    ngrp--;
}

#define CROWS 9
static void screen_cheats(void) {
    int sel = 0, top = 0, dirty = 0;
    for (;;) {
        frame_begin("Cheats");
        if (ngrp == 0) {
            gfx_text(24, 60, C_DIM, "No code yet.");
            gfx_text(24, 80, C_DIM, "Press " G_TRIANGLE " to add a GameShark code,");
            gfx_text(24, 92, C_DIM, "or send some with tools/cheats.py.");
        } else {
            gfx_text(16, 40, C_TEXT, ch_game[0] ? ch_game : "(no game named)");
            if (sel < top) top = sel;
            if (sel >= top + CROWS) top = sel - CROWS + 1;
            for (int g = top; g < ngrp && g < top + CROWS; g++) {
                int y = 56 + (g - top) * 12, on = grp_on(g);
                char b[40];
                if (ch_name[g][0]) {
                    int n = 0;
                    for (; n < CH_NAME - 1 && ch_name[g][n]; n++) b[n] = ch_name[g][n];
                    b[n] = 0;
                } else {
                    int f = grp_first(g);
                    code_str(b, ch[f].addr, ch[f].val);
                }
                if (g == sel) { gfx_grad(10, y - 2, 300, 12, 0xC07838, 0x602810); gfx_text(13, y, C_SEL, G_RIGHT); }
                gfx_text(22, y, g == sel ? C_SEL : on ? C_TEXT : C_DIM, b);
                gfx_text(300 - gfx_text_w(on ? "on" : "off"), y, on ? C_OK : C_DIM, on ? "on" : "off");
            }
            // the codes of the selected cheat: what it really writes
            char b[48], *p = b;
            int shown = 0, y = 170;
            for (int i = 0; i < nch && y < 194; i++) {
                if (ch[i].grp != sel) continue;
                p = code_str(p, ch[i].addr, ch[i].val);
                if (++shown % 3 == 0) { gfx_text(16, y, C_DIM, b); y += 11; p = b; }
                else p = fmt_str(p, "  ");
            }
            if (shown % 3) { p[-2] = 0; gfx_text(16, y, C_DIM, b); }
        }
        if (dirty) gfx_text(16, 196, C_ERR, "not saved: START saves to the cartridge");
        help(G_CROSS " on/off " G_TRIANGLE " add " G_SQUARE " edit SEL del START save");
        frame_end();
        if (back()) { if (!dirty) return; dirty = 0; }       // second press leaves without saving
        if (ngrp > 0) {
            if (moved(PAD_UP)) sel = (sel + ngrp - 1) % ngrp;
            if (moved(PAD_DOWN)) sel = (sel + 1) % ngrp;
        }
        if (pressed(PAD_CROSS) && ngrp > 0) {
            int on = !grp_on(sel);
            for (int i = 0; i < nch; i++) if (ch[i].grp == sel) ch[i].on = (u8)on;
            dirty = 1;
        }
        if (pressed(PAD_TRIANGLE) && nch < MAXCH && ngrp < MAXCH) {
            u32 a = 0x80000000; u16 v = 0;
            int f = ngrp > 0 ? grp_first(sel) : -1;
            if (f >= 0) a = ch[f].addr;
            if (edit_code(&a, &v)) { grp_add(a, v); sel = ngrp - 1; dirty = 1; }
        }
        if (pressed(PAD_SQUARE) && ngrp > 0) {                // the cheat's first line
            int f = grp_first(sel);
            u32 a = ch[f].addr; u16 v = ch[f].val;
            if (edit_code(&a, &v)) { ch[f].addr = a; ch[f].val = v; dirty = 1; }
        }
        if (pressed(PAD_SELECT) && ngrp > 0) {
            grp_del(sel);
            if (sel >= ngrp && sel > 0) sel--;
            dirty = 1;
        }
        if (pressed(PAD_START)) {
            ui_progress("saving the cheats", 0, 1);
            int r = cheats_save();
            if (r == 0) dirty = 0;
            else message("Cheats", "could not write the cartridge flash", r < 0 ? "(unknown chip)" : "(write error)", C_ERR);
        }
    }
}

// ---------------------------------------------------------------- controller test
static const char *pad_type(u8 id) {
    switch (id >> 4) {
    case 0x4: return "digital pad";
    case 0x7: return "DualShock / analog (red LED)";
    case 0x5: return "analog joystick (green LED)";
    case 0x1: return "mouse";
    case 0x2: return "neGcon";
    case 0x6: return "Guncon";
    case 0x3: return "Konami lightgun";
    case 0xF: return "config mode";
    default: return "unknown device";
    }
}
static void stick_box(int x, int y, const char *name, u8 sx, u8 sy) {
    gfx_rect(x, y, 34, 34, 0x403020);
    gfx_rect(x + 1, y + 1, 32, 32, 0x100808);
    gfx_rect(x + 16, y + 1, 1, 32, 0x403020);
    gfx_rect(x + 1, y + 16, 32, 1, 0x403020);
    gfx_rect(x + 1 + sx / 8 - 1, y + 1 + sy / 8 - 1, 3, 3, 0x40FFFF);
    char t[16], *p = fmt_str(t, name);
    p = fmt_hex(p, sx, 2); p = fmt_str(p, ","); fmt_hex(p, sy, 2);
    gfx_text(x - 6, y + 37, C_DIM, t);
}
static void screen_padtest(void) {
    static const struct { u16 bit; const char *name; } btn[16] = {
        {PAD_UP, G_UP}, {PAD_DOWN, G_DOWN}, {PAD_LEFT, G_LEFT}, {PAD_RIGHT, G_RIGHT},
        {PAD_CROSS, G_CROSS}, {PAD_CIRCLE, G_CIRCLE}, {PAD_SQUARE, G_SQUARE}, {PAD_TRIANGLE, G_TRIANGLE},
        {PAD_L1, "L1"}, {PAD_L2, "L2"}, {PAD_R1, "R1"}, {PAD_R2, "R2"},
        {PAD_SELECT, "SEL"}, {PAD_START, "START"}, {PAD_L3, "L3"}, {PAD_R3, "R3"},
    };
    int quit = 0;
    while (!quit) {
        frame_begin("Controller test");
        for (int port = 0; port < 2; port++) {
            u8 d[20];
            int n = pad_read_full(port, d, sizeof d);
            int y = 40 + port * 84;
            char t[64], *p = fmt_str(t, port ? "Port 2: " : "Port 1: ");
            fmt_str(p, n ? pad_type(d[0]) : "nothing connected");
            gfx_text(14, y, n ? C_TITLE : C_DIM, t);
            if (!n) continue;
            u16 b = n >= 2 ? (u16)~(d[1] | (d[2] << 8)) : 0;
            if (port == 0 && (b & (PAD_START | PAD_SELECT)) == (PAD_START | PAD_SELECT)) quit = 1;
            if ((d[0] >> 4) == 0x1) {                       // mouse: buttons + movement
                p = fmt_str(t, "left "); p = fmt_str(p, (b & 0x0800) ? "ON " : "-- ");
                p = fmt_str(p, " right "); p = fmt_str(p, (b & 0x0400) ? "ON " : "-- ");
                p = fmt_str(p, "  dx "); p = fmt_dec(p, (u8)d[3]); p = fmt_str(p, " dy "); fmt_dec(p, (u8)d[4]);
                gfx_text(14, y + 14, C_TEXT, t);
            } else {
                for (int i = 0; i < 16; i++) {
                    int x = 14 + (i % 8) * 37, yy = y + 14 + (i / 8) * 16;
                    int on = (b & btn[i].bit) != 0;
                    gfx_rect(x - 2, yy - 3, 33, 13, on ? 0xC07838 : 0x2A1810);
                    gfx_text(x + (32 - gfx_text_w(btn[i].name)) / 2 - 1, yy, on ? C_SEL : C_DIM, btn[i].name);
                }
                if (n >= 6 && ((d[0] >> 4) == 0x7 || (d[0] >> 4) == 0x5)) {   // analog sticks
                    stick_box(206, y - 2, "R ", d[3], d[4]);
                    stick_box(258, y - 2, "L ", d[5], d[6]);
                }
            }
            p = fmt_str(t, "raw ");
            p = fmt_hex(p, d[0], 2);
            for (int i = 1; i <= n && i < 9; i++) { *p++ = ' '; p = fmt_hex(p, d[i], 2); }
            gfx_text(14, y + 50, C_DIM, t);
        }
        help("press anything    START + SELECT (port 1) back");
        gfx_flip();
        sio_poll();
    }
    while ((pad_now = pad_read()) & (PAD_START | PAD_SELECT)) gfx_flip();   // wait for release
    pad_old = pad_now;
}

// ---------------------------------------------------------------- tools submenu
// Each tool can be left out of the build (make menuconfig): the lists below are built by the
// compiler from constants, and the call of a disabled tool is removed with its code.
#define T_ON(x) ((x) ? 1 : 0)
enum { T_COUNT = T_ON(CONFIG_CD_BROWSER) + T_ON(CONFIG_CD_AUDIO) + T_ON(CONFIG_VRAM_VIEWER) + T_ON(CONFIG_MEMCARDS) +
                 T_ON(CONFIG_PADTEST) + T_ON(CONFIG_HEXEDIT) + T_ON(CONFIG_CART_INFO) + T_ON(CONFIG_CART_BANKS) };
static void screen_tools(void) {
    static const char *const items[T_COUNT + 1] = {
#if CONFIG_CD_BROWSER
        "CD browser",
#endif
#if CONFIG_CD_AUDIO
        "CD audio player",
#endif
#if CONFIG_VRAM_VIEWER
        "VRAM viewer",
#endif
#if CONFIG_MEMCARDS
        "Memory cards",
#endif
#if CONFIG_PADTEST
        "Controller test",
#endif
#if CONFIG_HEXEDIT
        "Hex editor",
#endif
#if CONFIG_CART_INFO
        "Cartridge info",
#endif
#if CONFIG_CART_BANKS
        "Cartridge banks",
#endif
    };
    static const char *const desc[T_COUNT + 1] = {
#if CONFIG_CD_BROWSER
        "Run an EXE, play a .STR, flash a .ROM.",
#endif
#if CONFIG_CD_AUDIO
        "Play the audio tracks of a CD.",
#endif
#if CONFIG_VRAM_VIEWER
        "Look at the video memory (like Caetla).",
#endif
#if CONFIG_MEMCARDS
        "Files and free blocks of both slots.",
#endif
#if CONFIG_PADTEST
        "Buttons and analog sticks of both pads.",
#endif
#if CONFIG_HEXEDIT
        "View / edit RAM, ROM, BIOS, scratchpad.",
#endif
#if CONFIG_CART_INFO
        "Flash chip, protection, ROM checksum.",
#endif
#if CONFIG_CART_BANKS
        "Which program is in which bank.",
#endif
    };
    enum { N = T_COUNT };
    if (!N) return;
    static int sel;
    for (;;) {
        frame_begin("Tools");
        for (int i = 0; i < (int)N; i++) {
            int y = 50 + i * 18;
            if (i == sel) { gfx_grad(24, y - 4, 272, 16, 0xC07838, 0x602810); gfx_rect(24, y - 4, 2, 16, 0x40D8FF); }
            gfx_text(40, y, i == sel ? C_SEL : C_TEXT, items[i]);
        }
        gfx_text(160 - gfx_text_w(desc[sel]) / 2, 190, 0xA0C0D0, desc[sel]);
        help(G_CROSS " open    " G_CIRCLE " back");
        frame_end();
        if (back()) return;
        sel = list_move(N, sel);
        if (pressed(PAD_CROSS)) {
            int k = 0;                                  // the selected line -> its tool
            if (CONFIG_CD_BROWSER && sel == k++) screen_cd();
            else if (CONFIG_CD_AUDIO && sel == k++) screen_audio();
            else if (CONFIG_VRAM_VIEWER && sel == k++) screen_vram();
            else if (CONFIG_MEMCARDS && sel == k++) screen_cards();
            else if (CONFIG_PADTEST && sel == k++) screen_padtest();
            else if (CONFIG_HEXEDIT && sel == k++) screen_hex();
            else if (CONFIG_CART_INFO && sel == k++) screen_cart();
            else if (CONFIG_CART_BANKS && sel == k++) screen_banks();
        }
    }
}

// ---------------------------------------------------------------- boot log (serial, like Unirom)
static void blog(const char *s) { sio_puts(s); sio_puts("\r\n"); dbg(s); }
static void boot_log(void) {
    char t[96], *q;
    // BIOS: "System ROM Version x.y date region" at BFC7FF32h (v2.0 and later)
    const char *v = (const char *)0xBFC7FF32;
    q = fmt_str(t, "BIOS: ");
    if (v[0] == 'S' && v[1] == 'y') { int i = 0; while (i < 40 && v[i] >= 32 && v[i] < 127) { *q++ = v[i]; i++; } *q = 0; }
    else q = fmt_str(q, "v1.x (no version string)");
    blog(t);
    q = fmt_str(t, "Video: ");
    fmt_str(q, (*(volatile u32 *)0x1F801814 >> 20) & 1 ? "PAL (50 Hz)" : "NTSC (60 Hz)");
    blog(t);
    u8 m, d;
    int known = flash_identify(&m, &d);
    q = fmt_str(t, "Flash: ");
    q = fmt_str(q, known ? flash_chip_name(m, d) : "unknown chip");
    q = fmt_str(q, " (ID "); q = fmt_hex(q, m, 2); q = fmt_str(q, "/"); q = fmt_hex(q, d, 2);
    q = fmt_str(q, "), ROM CRC "); fmt_hex(q, crc32((const void *)FLASH_BASE, 0x10000), 8);
    blog(t);
    {   // second chip of double-bank carts (Xplorer FX): read only, never a command at boot
        extern int chip2_present(void);
        q = fmt_str(t, "Flash @40000h: ");
        fmt_str(q, chip2_present() ? "second chip present (not probed)" : "none");
        blog(t);
    }
    q = fmt_str(t, "Cheats: "); q = fmt_dec(q, nch); q = fmt_str(q, " codes, saves in flash: ");
    fmt_dec(q, store_load());
    blog(t);
    q = fmt_str(t, "Switch: ");
    q = fmt_str(q, (*(volatile u8 *)0x1F060000 & 1) ? "on" : "off");
    q = fmt_str(q, ", off at power-on = ");
    { int b = cfg_bank(); q = fmt_str(q, b == 2 ? "ROM bank 2" : b == 3 ? "ROM bank 3" : "bypass"); }
    q = fmt_str(q, "; firmware in bank 2: ");
    q = fmt_str(q, rom_bank_ok(2) ? "yes" : "no");
    q = fmt_str(q, ", bank 3: ");
    fmt_str(q, rom_bank_ok(3) ? "yes" : "no");
    blog(t);
    for (int port = 0; port < 2; port++) {
        u8 r[20];
        int n = pad_read_full(port, r, sizeof r);
        q = fmt_str(t, port ? "Pad 2: " : "Pad 1: ");
        fmt_str(q, n ? pad_type(r[0]) : "none");
        blog(t);
    }
    for (int port = 0; port < 2; port++) {
        static u8 fr[128];
        q = fmt_str(t, port ? "Card 2: " : "Card 1: ");
        if (!mc_read_frame(port, 0, fr)) q = fmt_str(q, "none");
        else if (fr[0] == 'M' && fr[1] == 'C') {
            int used = 0;
            for (int b = 1; b <= 15; b++)
                if (mc_read_frame(port, b, fr) && fr[0] >= 0x51 && fr[0] <= 0x53) used++;
            q = fmt_str(q, "PS1 card, "); q = fmt_dec(q, used); q = fmt_str(q, " blocks used, ");
            q = fmt_dec(q, 15 - used); q = fmt_str(q, " free");
        } else if (fr[0] == 0x34 && fr[1] == 0x12 && fr[2] == 0 && fr[3] == 0) q = fmt_str(q, "PSXLinux root file system");
        else q = fmt_str(q, "not formatted / unknown");
        blog(t);
    }
    q = fmt_str(t, "CD: ");
    fmt_str(q, cd_lid_open() ? "lid open or no drive answer" : "lid closed");
    blog(t);
    blog("Serial: 115200 bauds, nops commands accepted");
    blog("OpenPsxRom " OPENROM_VERSION " ready");
}

int main(void) {
    sio_init(0x12);
    sio_puts("\r\nOpenPsxRom " OPENROM_VERSION " (" __DATE__ " " __TIME__ ") starting\r\n");
    dbg("OpenPsxRom " OPENROM_VERSION " (" __DATE__ " " __TIME__ ") starting");
    pad_init();
    gfx_init();
    dbg("[OpenPsxRom menu]");
#ifdef PROBE_SWITCH
    {
        char t[80], *p = fmt_str(t, "IO ");
        for (int i = 0; i < 0x20; i++) { p = fmt_hex(p, REG8(0x1F060000 + i), 2); *p++ = ' '; *p = 0; }
        dbg(t);
    }
#endif
#if defined(BOOTDISC) && defined(AUTOINSTALL)
    { u8 m, d; flash_identify(&m, &d); char t[32], *q = fmt_str(t, "flash id "); q = fmt_hex(q, m, 2); fmt_hex(q, d, 2); dbg(t); }
    { int bad = flash_write(_binary_openrom_rom_start, FLASH_SIZE, 0); dbg(bad ? "[install FAILED]" : "[install OK]"); }
#endif
#ifdef BACKUPTEST
    {
        extern void backup_test(void);
        backup_test();
    }
#endif
#ifdef MCSCREEN
    screen_cards();
#endif
#ifdef PADTEST
    screen_padtest();
#endif
#ifdef STRTEST
    {
        extern int str_frames;
        u32 rl, rs;
        cd_init();
        if (iso_root(&rl, &rs) == 0) {
            int n = iso_list(rl, rs, cde, MAXE);
            for (int i = 0; i < n; i++)
                if (has_ext(i, ".STR") && cde[i].name[0] == 'G') {
                    char err[48] = "", t[64], *q;
                    dbg("strtest: playing");
                    dbg(cde[i].name);
                    int r = str_play(cde[i].lba, cde[i].size, err);
                    q = fmt_str(t, "strtest: result "); q = fmt_dec(q, -r); q = fmt_str(q, " frames ");
                    q = fmt_dec(q, str_frames); q = fmt_str(q, " "); fmt_str(q, err);
                    dbg(t);
                    break;
                }
        }
    }
#endif
#ifdef PCDRVTEST2
    {
        char err[64] = "", t[80], *q = fmt_str(t, "vector:");
        for (int i = 0; i < 6; i++) { *q++ = ' '; q = fmt_hex(q, ((volatile u32 *)0x80000080)[i], 8); }
        dbg(t);
#ifdef T_FLAGS
        opt_debug = T_FLAGS & 1; opt_tty = (T_FLAGS >> 1) & 1; opt_crash = (T_FLAGS >> 2) & 1; opt_menu = (T_FLAGS >> 3) & 1; core_boot();
#else
        core_debug();
#endif
        dbg("pcdrvtest2: booting the test disc"); cd_boot(err); dbg(err);
    }
#endif
#ifdef PCDRVTEST
    {
        core_debug();
        dbg("syscall test: enter critical");
        { register u32 a0 __asm__("a0") = 1; __asm__ volatile("syscall" : "+r"(a0) :: "v0", "memory"); }
        dbg("syscall test: back from 1, exit critical");
        { register u32 a0 __asm__("a0") = 2; __asm__ volatile("syscall" : "+r"(a0) :: "v0", "memory"); }
        dbg("syscall test: back from 2");
        dbg("pcdrvtest: break 0x101");
        register int v0 __asm__("v0"), v1 __asm__("v1");
        __asm__ volatile("break 0, 0x101" : "=r"(v0), "=r"(v1) :: "memory");
        char t[48], *q = fmt_str(t, "pcdrvtest: back, v0 ");
        fmt_hex(q, v0, 8);
        dbg(t);
    }
#endif
#ifdef CRASHTEST
    {
        opt_crash = 1;
        core_boot();
        __asm__ volatile("mtc0 %0, $12" :: "r"(0x401u));          // interrupts on, as in a game
        dbg("crashtest: break instruction");
        __asm__ volatile("break 0x123");
    }
#endif
#ifdef COPYTEST
    {
        char t[80], *q;
        static u8 dirf[128];
        u8 *buf = (u8 *)0x80160000, *chk = (u8 *)0x80180000;
        store_load();
        load_card(0);
        q = fmt_str(t, "ct: card1 files "); fmt_dec(q, mcn); dbg(t);
        int r1 = mc_read_file(0, &mce[0], buf, dirf);
        int r2 = store_add(dirf, buf);
        int r3 = store_save(store_writer);
        int n = store_load();
        q = fmt_str(t, "ct: read "); q = fmt_dec(q, -r1); q = fmt_str(q, " add "); q = fmt_dec(q, -r2);
        q = fmt_str(q, " save "); q = fmt_dec(q, -r3); q = fmt_str(q, " store count "); fmt_dec(q, n); dbg(t);
        McEntry e; store_entry(0, &e);
        q = fmt_str(t, "ct: store[0] "); q = fmt_str(q, e.title); q = fmt_str(q, " blocks "); fmt_dec(q, e.blocks); dbg(t);
        int w = mc_write_file(1, store_dirf(0), store_data(0), e.blocks);
        McEntry c2[15];
        int n2 = mc_list(1, c2);
        int rr = n2 > 0 ? mc_read_file(1, &c2[0], chk, dirf) : -9;
        int same = 1;
        for (u32 i = 0; i < (u32)e.blocks * 8192; i++) if (chk[i] != buf[i]) { same = 0; break; }
        q = fmt_str(t, "ct: write slot2 "); q = fmt_dec(q, -w); q = fmt_str(q, " slot2 files "); q = fmt_dec(q, n2);
        q = fmt_str(q, " reread "); q = fmt_dec(q, -rr); fmt_str(q, same ? " DATA SAME" : " DATA DIFFERENT"); dbg(t);
    }
#endif
#ifdef CARDTEST
    {
        u8 *raw = (u8 *)0x80100000, *pk = (u8 *)0x80120000, *back2 = (u8 *)0x80140000;
        char t[80], *q;
        int bad = mc_read_card(0, raw, 0);
        u32 n = omc_pack(raw, 131072, pk, 0x20000);
        u32 r = omc_unpack(pk, back2, 131072);
        int same = 1;
        for (u32 i = 0; i < 131072; i++) if (raw[i] != back2[i]) { same = 0; break; }
        q = fmt_str(t, "cardtest bad="); q = fmt_dec(q, bad); q = fmt_str(q, " packed="); q = fmt_dec(q, n);
        q = fmt_str(q, " unpacked="); q = fmt_dec(q, r); q = fmt_str(q, same ? " SAME" : " DIFFERENT");
        q = fmt_str(q, " crc="); fmt_hex(q, crc32(raw, 131072), 8);
        dbg(t);
    }
#endif
    enum { M_BOOT, M_UNLOCK, M_OPTIONS, M_CHEATS, M_TOOLS, M_ABOUT, M_RESTART, M_INSTALL };
    cheats_load();
#ifdef SETCFG
    // one-shot tool build (RAM exe): store the switch-off setting without the menu (no TV)
    if ((cfg_flags() & CFG_BANK_MASK) != (SETCFG) || *(const u32 *)(FLASH_BASE + FL_CFG_OFF) != CFG_MAGIC) {
        int e = cfg_save((cfg_flags() & ~CFG_BANK_MASK) | (SETCFG));
        blog(e ? "SETCFG: flash write FAILED" : "SETCFG: switch-off setting written");
    } else blog("SETCFG: setting already there");
#endif
    boot_log();
#ifdef SHOT
    // documentation screenshots in an emulator (no pad navigation): open one screen directly
    if (SHOT == 1) screen_options();
    if (SHOT == 2) screen_cart();
    if (SHOT == 3) screen_tools();
    if (SHOT == 4) screen_about();
#endif
#ifdef MENUTEST
    ch[0].addr = 0x800F1234; ch[0].val = 0x0063; ch[0].on = 1;
    ch[1].addr = 0x300F1238; ch[1].val = 0x0009; ch[1].on = 0;
    ch[0].grp = 0; ch[1].grp = 1; nch = ngrp = 2;
#endif
    // entries left out by make menuconfig: not in the lists, ids[] maps a line to its entry
    static const char *const items[] = {
        "Boot CD",
#if CONFIG_UNLOCK
        "Unlock drive + boot CD",
#endif
        "Boot options",
#if CONFIG_CHEATS
        "Cheats",
#endif
        "Tools", "About",
#if CONFIG_RESTART
        "Restart",
#endif
#ifdef BOOTDISC
        "Install OpenPsxRom to cartridge",
#endif
    };
    static const char *const desc[] = {
        "Start the disc in the drive.",
#if CONFIG_UNLOCK
        "For burned discs (no modchip needed).",
#endif
        "Serial debug, printf, crash screen, PAL/NTSC.",
#if CONFIG_CHEATS
        "GameShark codes, applied while the game runs.",
#endif
        "CD browser, CD audio, VRAM, memory cards...",
        "What OpenPsxRom can do.",
#if CONFIG_RESTART
        "Restart the console (as the reset button).",
#endif
        "Write OpenPsxRom " OPENROM_VERSION " to the cartridge flash.",
    };
    static const u8 ids[] = {
        M_BOOT,
#if CONFIG_UNLOCK
        M_UNLOCK,
#endif
        M_OPTIONS,
#if CONFIG_CHEATS
        M_CHEATS,
#endif
        M_TOOLS, M_ABOUT,
#if CONFIG_RESTART
        M_RESTART,
#endif
        M_INSTALL,
    };
    enum { N = sizeof items / sizeof items[0] };
    int sel = 0;
    pad_now = pad_read();
    for (;;) {
#ifdef BOOTDISC
        frame_begin("boot disc");
#else
        frame_begin(gfx_pal ? "PAL" : "NTSC");
#endif
        for (int i = 0; i < (int)N; i++) {
            int y = 46 + i * 19;
            if (i == sel) {
                gfx_grad(24, y - 4, 272, 16, 0xC07838, 0x602810);
                gfx_rect(24, y - 4, 2, 16, 0x40D8FF);
            }
            gfx_text(40, y, i == sel ? C_SEL : C_TEXT, items[i]);
        }
        gfx_text(160 - gfx_text_w(desc[sel]) / 2, 190, 0xA0C0D0, desc[sel]);
        help(G_CROSS " select    " G_UP " " G_DOWN " move");
        frame_end();
        sel = list_move(N, sel);
        int boot = 0;
#ifdef UITEST
        {   // emulator check of the progress screens (no serial / flash write in the emulator)
            static int t;
            if (++t == 30) {
                if (UITEST == 6) {           // a real flash of the bank 1 image onto itself, then the result
                    u8 *buf = (u8 *)0x80100000;
                    memcpy(buf, (const void *)FLASH_BASE, FLASH_SIZE);
                    buf[0xFFF0] ^= 1;            // one byte changed: one block really written
                    flash_ui_begin("flashing", 0, FLASH_SIZE);
                    int bad = flash_write(buf, FLASH_SIZE, flash_ui);
                    sio_puts(bad ? "UITEST6 flash FAILED" : "UITEST6 flash ok");
                    ui_flash_result(bad ? "FLASH FAILED: 1 block(s)" : "FLASH OK  CRC32 12345678");
                    sio_puts("UITEST6 result shown");
                }
                else if (UITEST == 5) ui_flash_result("FLASH OK  CRC32 1234ABCD");
                else if (UITEST == 4) screen_banks();
                else if (UITEST == 3) ui_ask("flash done", "OpenPsxRom written and verified", "CRC 29291983", "Restart on the new version now?");
                else if (UITEST == 1) {
                    flash_ui_begin("flashing", ROM2_OFF, FLASH_SIZE);
                    flash_ui(8, "same"); flash_ui(9, "ok"); flash_ui(10, "write"); flash_ui(10 | (0x2000 << 8), "page"); flash_ui(15, "ok");
                } else {
                    static const char *const info[5] = {"84 KB / 202 KB", "11.2 KB/s, 10 s left",
                        "load 80010000  entry 80010000", "Sony Computer Entertainment Inc"};
                    ui_progress_info("loading the program", 84 * 1024, 202 * 1024, info);
                }
                for (;;);
            }
        }
#endif
#ifdef EXETEST
        {   // emulator test of the serial /exe path: PGBX.EXE from the CD, exe_start() straight from the menu
            static int t;
            if (++t == 60) {
                static CdEntry e[16];
                static u8 h[2048];
                u32 rl, rs;
                cd_init();
                int n = iso_root(&rl, &rs) == 0 ? iso_list(rl, rs, e, 16) : 0;
                for (int i = 0; i < n; i++)
                    if (e[i].name[0] == 'P' && e[i].name[1] == 'G' && !cd_read(e[i].lba, 1, h)) {
                        const u32 *q = (const u32 *)h;
                        u32 len = (q[0x1C / 4] + 2047) & ~2047u;
                        cd_read(e[i].lba + 1, len / 2048, (u8 *)q[0x18 / 4]);
                        sio_puts("EXETEST: exe_start\r\n");
                        exe_start(h);
                    }
            }
        }
#elif defined(AUTOBOOT)
        { static int t; if (++t == 60) boot = 1; }
#endif
        if (pressed(PAD_CROSS) || pressed(PAD_START)) {
            int id = ids[sel];
            if (id == M_BOOT) boot = 1;
            if (CONFIG_UNLOCK && id == M_UNLOCK) {
                frame_begin("Unlock");
                gfx_text(24, 100, C_TEXT, "unlocking the drive...");
                gfx_flip();
                cd_init();
                if (cd_unlock(gfx_pal) == 0) boot = 1;
                else message("Unlock", "the drive did not accept the unlock", "(Japanese drive, or emulator?)", C_ERR);
            }
            if (id == M_OPTIONS) screen_options();
            if (CONFIG_CHEATS && id == M_CHEATS) screen_cheats();
            if (id == M_TOOLS) screen_tools();
            if (id == M_ABOUT) screen_about();
            if (CONFIG_RESTART && id == M_RESTART) {                     // like the REST serial command: BIOS entry
                gfx_shutdown();
                exec_jump(0xBFC00000, 0, 0x801FFF00);
            }
#ifdef BOOTDISC
            if (id == M_INSTALL) install_rom();
#endif
        }
        if (boot) {
            frame_begin("Boot CD");
            gfx_text(24, 100, C_TEXT, "reading the disc...");
            gfx_flip();
            char err[64] = "";
            cd_boot(err);                       // only returns on error
            message("Boot CD", "the disc could not be started:", err, C_ERR);
        }
    }
}
