// OpenROM - open source cartridge firmware: menu and screens.
#include "rom.h"

static u16 pad_now, pad_old;
static u32 anim_tick;            // frames, for the animated save icons
static int rep_cnt;
static int pressed(u16 b) { return (pad_now & b) && !(pad_old & b); }
// d-pad with auto-repeat (held: repeats after 20 frames, then every 4)
static int moved(u16 b) {
    if (!(pad_now & b)) return 0;
    if (!(pad_old & b)) return 1;
    return rep_cnt > 20 && (rep_cnt & 3) == 0;
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
    gfx_text(320 - 10 - gfx_text_w(title), 15, C_TEXT, title);
    gfx_shade(8, 38, 304, 168, 0x000000);
    gfx_shade(0, 210, 320, 30, 0x000000);
    gfx_rect(0, 210, 320, 1, 0x604020);
    char s[64], *p = s;
    p = fmt_str(p, "Serial: ");
    fmt_str(p, sio_status[0] ? sio_status : "waiting for NOPS (115200)");
    gfx_text(10, 228, C_DIM, s);
    cur_help = 0;
}

static void frame_end(void) {
    anim_tick++;
    if (cur_help) gfx_text(10, 215, C_TEXT, cur_help);
    gfx_flip();
    sio_poll();
    pad_old = pad_now;
    pad_now = pad_read();
    if (pad_now & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) rep_cnt++; else rep_cnt = 0;
}

static void help(const char *s) { cur_help = s; }

// Progress screen for long operations (card, CD): one frame per call.
void ui_progress(const char *what, u32 done, u32 total) {
    frame_begin(what);
    char s[48], *p = fmt_str(s, what);
    p = fmt_str(p, "... ");
    fmt_dec(p, total ? done * 100 / total : 0);
    fmt_str(s + strlen(s), "%");
    gfx_text(40, 100, C_TEXT, s);
    gfx_rect(40, 116, 240, 10, 0x404040);
    if (total) gfx_rect(40, 116, 240 * done / total, 10, 0x40C040);
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

// ---------------------------------------------------------------- cartridge
static void screen_cart(void) {
    u8 mfr, dev, prot[8], m2, d2;
    int known = flash_identify(&mfr, &dev);
    // hardware sector protection only exists on AMD-style chips (Am29F010 of Datel carts);
    // Atmel/SST/Winbond page chips use software data protection: nothing to read per sector
    int lockable = mfr == 0x01 || mfr == 0x20 || mfr == 0x04;
    if (lockable) flash_protect(prot);
    // a second chip only if the first one is smaller than 512 KB (else 40000h is the same chip)
    int chip2 = flash_chip_kb(mfr, dev) < 512 && flash_identify_at(0x40000, &m2, &d2);
    if (chip2) {                                       // a single chip also answers there (wrap)
        const volatile u8 *a = (const volatile u8 *)FLASH_BASE, *b = a + 0x40000;
        chip2 = 0;
        for (u32 i = 0; i < 0x1000 && !chip2; i++) if (a[i] != b[i]) chip2 = 1;
    }
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
            p = fmt_str(s, "Chip 2     ");
            p = fmt_hex(p, m2, 2); p = fmt_str(p, "/"); p = fmt_hex(p, d2, 2);
            p = fmt_str(p, " at 1F040000h");
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
// flash progress: sector in the low byte, page offset (for page chips) above
static u32 fp_first, fp_count;
static void fl_prog(u32 s, const char *what) {
    (void)what;
    u32 sec = s & 0xFF, off = s >> 8;
    ui_progress("writing to the cartridge", (sec - fp_first) * 16384 + off, fp_count * 16384);
}
static int flash_area(u32 off, const u8 *data, u32 size) {
    fp_first = off / FLASH_SECT;
    fp_count = size / FLASH_SECT;
    return flash_write_at(off, data, size, fl_prog);
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
static void flash_prog(u32 s, const char *what) {
    u32 sec = s & 0xFF, off = s >> 8;
    char t[40], *p = fmt_str(t, "block ");
    p = fmt_dec(p, sec);
    p = fmt_str(p, ": ");
    fmt_str(p, what);
    ui_progress(t, sec * 16384 + off, 8 * 16384);
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
    if (entry >= 0x1F020000u && entry < 0x1F040000u &&
        img[0x200] == 'R' && img[0x201] == 'O' && img[0x202] == 'M' && img[0x203] == '2') return 2;
    if (entry >= 0x1F040000u && entry < 0x1F060000u) return 3;
    return 1;
}
// room for a bank: 2 needs a chip of 256 KB+, 3 a second chip answering at 40000h
int bank_available(int bank) {
    u8 m, d;
    if (bank == 2) { flash_identify(&m, &d); return flash_chip_kb(m, d) >= 256; }
    if (bank == 3) {
        if (!flash_identify_at(ROM3_OFF, &m, &d)) return 0;
        // a single 256 KB chip also answers there (address wrap): then 40000h shows 00000h again
        const volatile u8 *a = (const volatile u8 *)FLASH_BASE, *b = a + ROM3_OFF;
        for (u32 i = 0; i < 0x1000; i++) if (a[i] != b[i]) return 1;
        return 0;
    }
    return 1;
}
static void flash_image(const u8 *img) {
    int bad, bank = rom_image_bank(img);
    if (bank > 1) {
        if (!bank_available(bank)) { message("Flash ROM", bank == 2 ? "no bank 2 on this cartridge" : "no bank 3 (second chip)", "nothing written", C_ERR); return; }
        bad = flash_write_at(bank == 2 ? ROM2_OFF : ROM3_OFF, img, FLASH_SIZE, flash_prog);
        message(bank == 2 ? "Flash ROM bank 2" : "Flash ROM bank 3", bad ? "FAILED: some blocks are wrong" : "written and verified, OpenPsxRom untouched",
                bad ? "OpenPsxRom untouched" : "switch OFF + Boot options to start it", bad ? C_ERR : C_OK);
        return;
    }
    bad = flash_write(img, is_openrom_image(img) ? FL_ROM_SIZE : FLASH_SIZE, flash_prog);
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
                    str_play(cde[sel].lba, cde[sel].size, err);
                    pad_now = pad_old = pad_read();
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
// Kept in the cartridge flash at 1C000h: 'OCH1', u32 count, then count x (u32 address, u32 value |
// enabled << 16). Applied by the resident core on every interrupt while the game runs.
#define MAXCH 96
typedef struct { u32 addr; u16 val; u8 on; } Cheat;
static Cheat ch[MAXCH];
static int nch;

static void cheats_load(void) {
    const u32 *f = (const u32 *)(FLASH_BASE + FL_CHEAT_OFF);
    nch = 0;
    if (f[0] != 0x3148434Fu || f[1] > MAXCH) return;          // "OCH1"
    for (u32 i = 0; i < f[1]; i++) {
        ch[i].addr = f[2 + i * 2];
        ch[i].val = (u16)f[3 + i * 2];
        ch[i].on = (f[3 + i * 2] >> 16) & 1;
    }
    nch = f[1];
}
static int cheats_save(void) {
    u32 *b = (u32 *)0x80100000;
    memset(b, 0xFF, 0x4000);
    b[0] = 0x3148434Fu;
    b[1] = nch;
    for (int i = 0; i < nch; i++) { b[2 + i * 2] = ch[i].addr; b[3 + i * 2] = ch[i].val | ((u32)ch[i].on << 16); }
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
static int cfg_bank(void) { int v = cfg_flags() & CFG_BANK_MASK; return v == 1 ? 2 : v; }
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
#else
static int opt_debug, opt_tty, opt_crash = 1, opt_video;
#endif
static int opt_menu = 1;     // video: 0 auto, 1 force PAL, 2 force NTSC

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
    int bank = cfg_bank();                              // 0 bypass, 2 or 3
    static const char *const bank_name[] = {"bypass", "", "ROM bank 2", "ROM bank 3"};
    int any = rom_bank_ok(2) || rom_bank_ok(3);
    for (;;) {
        frame_begin("Boot options");
        const char *v[6] = {opt_menu ? "on" : "off", opt_debug ? "on" : "off", opt_tty ? "on" : "off", opt_crash ? "on" : "off", vid[opt_video],
                            bank_name[bank & 3]};
        static const char *const names[] = {"In-game menu (switch)", "Serial debug in game", "printf -> serial", "Crash screen", "Video mode",
                                            "Switch OFF at power-on"};
        const char *desc[] = {
            "Xplorer switch off then on: menu over the game",
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
            if (cfg != cfg0 && cfg_save(cfg)) message("Boot options", "flash write error", 0, C_ERR);
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
        pad_now = pad_read();
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

static const char *cheat_label(int i, char *b) {
    char *p = code_str(b, ch[i].addr, ch[i].val);
    fmt_str(p, ch[i].on ? "      on" : "      off");
    return b;
}
static u32 cheat_color(int i) { return ch[i].on ? C_OK : C_DIM; }

static void screen_cheats(void) {
    int sel = 0, top = 0, dirty = 0;
    for (;;) {
        frame_begin("Cheats");
        if (nch == 0) {
            gfx_text(24, 60, C_DIM, "No code yet.");
            gfx_text(24, 80, C_DIM, "Press " G_TRIANGLE " to add a GameShark code.");
        } else list_draw(nch, sel, &top, cheat_label, cheat_color);
        if (dirty) gfx_text(16, 196, C_ERR, "not saved: START saves to the cartridge");
        help(G_CROSS " on/off " G_TRIANGLE " add " G_SQUARE " edit SEL del START save " G_CIRCLE);
        frame_end();
        if (back()) { if (!dirty) return; dirty = 0; }       // second press leaves without saving
        if (nch > 0) sel = list_move(nch, sel);
        if (pressed(PAD_CROSS) && nch > 0) { ch[sel].on ^= 1; dirty = 1; }
        if (pressed(PAD_TRIANGLE) && nch < MAXCH) {
            u32 a = 0x80000000; u16 v = 0;
            if (nch > 0) a = ch[sel].addr;
            if (edit_code(&a, &v)) { ch[nch].addr = a; ch[nch].val = v; ch[nch].on = 1; sel = nch++; dirty = 1; }
        }
        if (pressed(PAD_SQUARE) && nch > 0) {
            u32 a = ch[sel].addr; u16 v = ch[sel].val;
            if (edit_code(&a, &v)) { ch[sel].addr = a; ch[sel].val = v; dirty = 1; }
        }
        if (pressed(PAD_SELECT) && nch > 0) {
            for (int i = sel; i < nch - 1; i++) ch[i] = ch[i + 1];
            nch--;
            if (sel >= nch && sel > 0) sel--;
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
static void screen_tools(void) {
    static const char *const items[] = {
        "CD browser", "CD audio player", "VRAM viewer", "Memory cards", "Controller test", "Hex editor", "Cartridge info",
    };
    static const char *const desc[] = {
        "Run an EXE, play a .STR, flash a .ROM.",
        "Play the audio tracks of a CD.",
        "Look at the video memory (like Caetla).",
        "Files and free blocks of both slots.",
        "Buttons and analog sticks of both pads.",
        "View / edit RAM, ROM, BIOS, scratchpad.",
        "Flash chip, protection, ROM checksum.",
    };
    enum { N = sizeof items / sizeof items[0] };
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
            switch (sel) {
            case 0: screen_cd(); break;
            case 1: screen_audio(); break;
            case 2: screen_vram(); break;
            case 3: screen_cards(); break;
            case 4: screen_padtest(); break;
            case 5: screen_hex(); break;
            case 6: screen_cart(); break;
            }
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
    {   // second chip of double-bank carts (Xplorer FX Pro): commands at 40000h
        u8 m2, d2;
        int a2 = flash_identify_at(0x40000, &m2, &d2);
        q = fmt_str(t, "Flash @40000h: ");
        if (a2) {
            q = fmt_str(q, flash_chip_name(m2, d2));
            q = fmt_str(q, " (ID "); q = fmt_hex(q, m2, 2); q = fmt_str(q, "/"); q = fmt_hex(q, d2, 2); fmt_str(q, ")");
        } else {
            q = fmt_str(q, "no answer (data "); q = fmt_hex(q, m2, 2); q = fmt_str(q, " "); q = fmt_hex(q, d2, 2); fmt_str(q, ")");
        }
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
    enum { M_BOOT, M_UNLOCK, M_OPTIONS, M_CHEATS, M_TOOLS, M_ABOUT, M_INSTALL };
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
    nch = 2;
#endif
    static const char *const items[] = {
        "Boot CD", "Unlock drive + boot CD", "Boot options", "Cheats", "Tools", "About",
#ifdef BOOTDISC
        "Install OpenPsxRom to cartridge",
#endif
    };
    static const char *const desc[] = {
        "Start the disc in the drive.",
        "For burned discs (no modchip needed).",
        "Serial debug, printf, crash screen, PAL/NTSC.",
        "GameShark codes, applied while the game runs.",
        "CD browser, CD audio, VRAM, memory cards...",
        "What OpenPsxRom can do.",
        "Write OpenPsxRom " OPENROM_VERSION " to the cartridge flash.",
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
            if (i == M_TOOLS) gfx_text(282, y, i == sel ? C_SEL : C_DIM, G_RIGHT);
        }
        gfx_text(160 - gfx_text_w(desc[sel]) / 2, 190, 0xA0C0D0, desc[sel]);
        help(G_CROSS " select    " G_UP " " G_DOWN " move");
        frame_end();
        sel = list_move(N, sel);
        int boot = 0;
#ifdef AUTOBOOT
        { static int t; if (++t == 60) boot = 1; }
#endif
        if (pressed(PAD_CROSS) || pressed(PAD_START)) {
            if (sel == M_BOOT) boot = 1;
            if (sel == M_UNLOCK) {
                frame_begin("Unlock");
                gfx_text(24, 100, C_TEXT, "unlocking the drive...");
                gfx_flip();
                cd_init();
                if (cd_unlock(gfx_pal) == 0) boot = 1;
                else message("Unlock", "the drive did not accept the unlock", "(Japanese drive, or emulator?)", C_ERR);
            }
            if (sel == M_OPTIONS) screen_options();
            if (sel == M_CHEATS) screen_cheats();
            if (sel == M_TOOLS) screen_tools();
            if (sel == M_ABOUT) screen_about();
#ifdef BOOTDISC
            if (sel == M_INSTALL) install_rom();
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
