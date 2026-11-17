// Memory card: raw 128-byte frame access on slot 1/2 (SIO0) and directory listing.
// Base: pGbX memcard.c (same timing, works on real hardware).
#include "rom.h"

static void delay(int n) { for (volatile int i = 0; i < n; i++); }

static int xfer(u8 out, u8 *in, int want_ack) {
    I_STAT = ~0x80u;
    SIO0_CTRL |= 0x10;
    while (!(SIO0_STAT & 1));
    SIO0_DATA = out;
    int t = 0;
    while (!(SIO0_STAT & 2)) if (++t > 20000) return 0;
    *in = SIO0_DATA;
    if (!want_ack) return 1;
    t = 0;
    while (!(I_STAT & 0x80)) if (++t > 20000) return 0;
    return 1;
}

static void sel(int port) { SIO0_CTRL = 0x1003 | ((port & 1) << 13); delay(300); }
static void desel(void) {
    SIO0_CTRL = 0; I_STAT = ~0x80u; delay(300);
    for (int i = 0; i < 16 && (SIO0_STAT & 2); i++) (void)SIO0_DATA;
}

int mc_read_frame(int port, int f, u8 *buf) {
    u8 r, ck, msb = f >> 8, lsb = f & 0xFF;
    int ok = 0;
    sel(port);
    if (!xfer(0x81, &r, 1) || !xfer(0x52, &r, 1) || !xfer(0, &r, 1) || r != 0x5A || !xfer(0, &r, 1) || r != 0x5D) goto out;
    if (!xfer(msb, &r, 1) || !xfer(lsb, &r, 1) || !xfer(0, &r, 1) || !xfer(0, &r, 1)) goto out;
    if (!xfer(0, &r, 1) || !xfer(0, &r, 1)) goto out;
    ck = msb ^ lsb;
    for (int i = 0; i < 128; i++) { if (!xfer(0, &buf[i], 1)) goto out; ck ^= buf[i]; }
    if (!xfer(0, &r, 1) || r != ck) goto out;
    if (!xfer(0, &r, 0) || r != 0x47) goto out;
    ok = 1;
out:
    desel();
    return ok;
}

int mc_write_frame(int port, int f, const u8 *buf) {
    u8 r, msb = f >> 8, lsb = f & 0xFF, ck = msb ^ lsb;
    int ok = 0;
    sel(port);
    if (!xfer(0x81, &r, 1) || !xfer(0x57, &r, 1) || !xfer(0, &r, 1) || r != 0x5A || !xfer(0, &r, 1) || r != 0x5D) goto out;
    if (!xfer(msb, &r, 1) || !xfer(lsb, &r, 1)) goto out;
    for (int i = 0; i < 128; i++) { if (!xfer(buf[i], &r, 1)) goto out; ck ^= buf[i]; }
    if (!xfer(ck, &r, 1) || !xfer(0, &r, 1) || !xfer(0, &r, 1)) goto out;
    if (!xfer(0, &r, 0) || r != 0x47) goto out;
    ok = 1;
out:
    desel();
    delay(2000);
    return ok;
}

// Write one frame for real: the card may still be busy programming the previous frame (it then
// ignores the command or answers an error), so a frame counts as written only once read back
// identical; between attempts the card is given time (~20 ms, then longer). 1 = ok.
int mc_put_frame(int port, int f, const u8 *buf) {
    u8 chk[128];
    for (int t = 0; t < 8; t++) {
        if (t) delay(60000 * (t < 4 ? t : 4));
        if (!mc_write_frame(port, f, buf)) continue;
        int ok = 0;
        for (int k = 0; k < 3 && !ok; k++) ok = mc_read_frame(port, f, chk);
        for (int i = 0; ok && i < 128; i++) if (chk[i] != buf[i]) ok = 0;
        if (ok) return 1;
    }
    return 0;
}

// Whole card (1024 frames = 128 KB). Each frame is retried 3 times. Returns frames that failed.
int mc_read_card(int port, u8 *dst, void (*progress)(int)) {
    int bad = 0;
    for (int f = 0; f < 1024; f++) {
        int ok = 0;
        for (int t = 0; t < 3 && !ok; t++) ok = mc_read_frame(port, f, dst + f * 128);
        if (!ok) { bad++; if (f == 0) return 1024; }       // no card
        if (progress && !(f & 63)) progress(f);
    }
    return bad;
}

int mc_write_card(int port, const u8 *src, void (*progress)(int)) {
    int bad = 0;
    for (int f = 0; f < 1024; f++) {
        int ok = 0;
        // no card: frame 0 cannot even be read
        if (f == 0) { u8 tmp[128]; int any = 0; for (int t = 0; t < 3 && !any; t++) any = mc_read_frame(port, 0, tmp); if (!any) return 1024; }
        ok = mc_put_frame(port, f, src + f * 128);
        if (!ok) bad++;
        if (progress && !(f & 63)) progress(f);
    }
    return bad;
}

// ---------------------------------------------------------------- files
// Directory: frame b (1..15) describes block b: [0] state (51h first, 52h middle, 53h last,
// A0h free, A1h-A3h deleted), [4..7] file size (first block), [8..9] next block - 1 (FFFFh: end),
// [10..30] file name, [127] XOR checksum. Block b = frames b*64 .. b*64+63 (8 KB). The first
// data frame holds 'SC', the icon frame count, the Shift-JIS title (4..67) and the icon CLUT
// (60h..7Fh); the next frame is the first 16x16 4-bit icon.

static u8 dirs[16][128];        // directory of the card being worked on (frames 0..15)

static int read_dir(int port) {
    for (int f = 0; f < 16; f++) if (!mc_read_frame(port, f, dirs[f])) return -1;
    if (dirs[0][0] != 'M' || dirs[0][1] != 'C') return -1;
    return 0;
}

static void sjis_title(const u8 *t, char *out, int max) {
    int o = 0, m = 0;
    while (o < 64 && m < max - 1) {
        u8 c = t[o];
        if (!c) break;
        if (c < 0x80) { out[m++] = c; o++; continue; }
        u16 sj = (c << 8) | t[o + 1];
        o += 2;
        char x = '?';
        if (sj == 0x8140) x = ' ';
        else if (sj >= 0x824F && sj <= 0x8258) x = '0' + (sj - 0x824F);
        else if (sj >= 0x8260 && sj <= 0x8279) x = 'A' + (sj - 0x8260);
        else if (sj >= 0x8281 && sj <= 0x829A) x = 'a' + (sj - 0x8281);
        else if (sj == 0x815B || sj == 0x817C || sj == 0x815D) x = '-';
        else if (sj == 0x8146) x = ':';
        else if (sj == 0x8144) x = '.';
        else if (sj == 0x8143) x = ',';
        else if (sj == 0x8149) x = '!';
        else if (sj == 0x8148) x = '?';
        else if (sj == 0x815E) x = '/';
        else if (sj == 0x8169) x = '(';
        else if (sj == 0x816A) x = ')';
        else if (sj == 0x8175 || sj == 0x8176) x = '"';
        else if (sj == 0x8166 || sj == 0x8165) x = '\'';
        else if (sj == 0x8195) x = '&';
        else if (sj == 0x817B) x = '+';
        else if (sj == 0x8194) x = '#';
        out[m++] = x;
    }
    while (m && out[m - 1] == ' ') m--;
    out[m] = 0;
}

// Title and icon frames from the data frames of a save (header frame, then 1..3 icon frames:
// header byte 2 = 11h / 12h / 13h).
void mc_parse_header(const u8 *hdr, const u8 *icons, McEntry *e) {
    if (hdr[0] == 'S' && hdr[1] == 'C') {
        sjis_title(hdr + 4, e->title, sizeof e->title);
        memcpy(e->clut, hdr + 0x60, 32);
        int n = hdr[2] & 3;
        if (n < 1) n = 1;
        for (int f = 0; f < n; f++) memcpy(e->icon[f], icons + f * 128, 128);
        e->has_icon = n;
    } else {
        fmt_str(e->title, e->name);
        e->has_icon = 0;
    }
}

static void entry_from_dir(int b, McEntry *x) {
    const u8 *fr = dirs[b];
    int k = 0;
    for (; k < 20 && fr[10 + k]; k++) x->name[k] = fr[10 + k];
    x->name[k] = 0;
    x->size = fr[4] | (fr[5] << 8) | (fr[6] << 16);
    x->blocks = (x->size + 8191) / 8192;
    x->first = b;
}

// Files of the card in slot port (with titles and icons). -1: no card / unformatted.
int mc_list(int port, McEntry *e) {
    if (read_dir(port)) return -1;
    int n = 0;
    u8 hdr[128], icons[3 * 128];
    for (int b = 1; b <= 15; b++) {
        if (dirs[b][0] != 0x51) continue;
        McEntry *x = &e[n++];
        entry_from_dir(b, x);
        int ok = mc_read_frame(port, b * 64, hdr);
        int n = ok && hdr[0] == 'S' ? (hdr[2] & 3) : 1;
        for (int f = 0; ok && f < (n ? n : 1); f++) ok = mc_read_frame(port, b * 64 + 1 + f, icons + f * 128);
        if (ok) mc_parse_header(hdr, icons, x);
        else { fmt_str(x->title, x->name); x->has_icon = 0; }
    }
    return n;
}

int mc_free_blocks(void) {          // of the card read by the last mc_list
    int n = 0;
    for (int b = 1; b <= 15; b++) if (dirs[b][0] == 0xA0 || (dirs[b][0] >= 0xA1 && dirs[b][0] <= 0xA3)) n++;
    return n;
}

static void fix_ck(u8 *f) { u8 c = 0; for (int i = 0; i < 127; i++) c ^= f[i]; f[127] = c; }

// Read a whole save (blocks * 8 KB) following the block chain. Fills dirf with its first directory frame.
int mc_read_file(int port, const McEntry *e, u8 *dst, u8 *dirf) {
    if (read_dir(port)) return -1;
    memcpy(dirf, dirs[e->first], 128);
    int b = e->first;
    for (int k = 0; k < e->blocks; k++) {
        if (b < 1 || b > 15) return -1;
        for (int f = 0; f < 64; f++) {
            int ok = 0;
            for (int t = 0; t < 3 && !ok; t++) ok = mc_read_frame(port, b * 64 + f, dst + k * 8192 + f * 128);
            if (!ok) return -1;
        }
        u16 nx = dirs[b][8] | (dirs[b][9] << 8);
        b = nx == 0xFFFF ? -1 : nx + 1;
    }
    return 0;
}

// Write a save in the free blocks. dirf: its first directory frame (name, size).
// Returns 0 ok, -1 card error, -2 not enough free blocks, -3 a file with this name exists.
int mc_write_file(int port, const u8 *dirf, const u8 *src, int blocks) {
    if (read_dir(port)) return -1;
    for (int b = 1; b <= 15; b++)
        if (dirs[b][0] == 0x51) {
            int same = 1;
            for (int k = 0; k < 21; k++) if (dirs[b][10 + k] != dirf[10 + k]) { same = 0; break; }
            if (same) return -3;
        }
    int list[15], n = 0;
    for (int b = 1; b <= 15 && n < blocks; b++)
        if (dirs[b][0] == 0xA0 || (dirs[b][0] >= 0xA1 && dirs[b][0] <= 0xA3)) list[n++] = b;
    if (n < blocks) return -2;
    for (int k = 0; k < blocks; k++)
        for (int f = 0; f < 64; f++) {
            int ok = 0;
            ok = mc_put_frame(port, list[k] * 64 + f, src + k * 8192 + f * 128);
            if (!ok) return -1;
        }
    for (int k = 0; k < blocks; k++) {
        u8 fr[128];
        memset(fr, 0, 128);
        fr[0] = blocks == 1 ? 0x51 : k == 0 ? 0x51 : k == blocks - 1 ? 0x53 : 0x52;
        if (k == 0) memcpy(fr + 4, dirf + 4, 4);
        u16 nx = k == blocks - 1 ? 0xFFFF : list[k + 1] - 1;
        fr[8] = (u8)nx; fr[9] = (u8)(nx >> 8);
        memcpy(fr + 10, dirf + 10, 21);
        fix_ck(fr);
        if (!mc_put_frame(port, list[k], fr)) return -1;
    }
    return 0;
}

// Delete a save: its directory frames are marked deleted (A1h first, A2h middle, A3h last).
int mc_delete_file(int port, const McEntry *e) {
    if (read_dir(port)) return -1;
    int b = e->first;
    for (int k = 0; k < e->blocks && b >= 1 && b <= 15; k++) {
        u8 *fr = dirs[b];
        u16 nx = fr[8] | (fr[9] << 8);
        fr[0] = fr[0] == 0x51 ? 0xA1 : fr[0] == 0x52 ? 0xA2 : 0xA3;
        fix_ck(fr);
        if (!mc_put_frame(port, b, fr)) return -1;
        b = nx == 0xFFFF ? -1 : nx + 1;
    }
    return 0;
}
