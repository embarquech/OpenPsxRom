// Minimal CD-ROM driver (direct controller access, polled) + ISO9660 directory listing.
// Base: pGbX cdrom.c. Adds directory listing and the "nocash" unlock commands.
#include "rom.h"

#define CD_STAT REG8(0x1F801800)
#define CD_REG1 REG8(0x1F801801)
#define CD_REG2 REG8(0x1F801802)
#define CD_REG3 REG8(0x1F801803)

static u8 resp[16];
static int nresp;

static void delay(int n) { for (volatile int i = 0; i < n; i++); }

// Wait for an interrupt from the controller; returns its type (1..5) or 0 on timeout.
static int cd_wait(int timeout) {
    CD_STAT = 1;
    int t = 0;
    while (!(CD_REG3 & 7)) if (++t > timeout) return 0;
    int type = CD_REG3 & 7;
    nresp = 0;
    while ((CD_STAT & 0x20) && nresp < 16) resp[nresp++] = CD_REG1;
    CD_STAT = 1;
    CD_REG3 = 0x07;
    while (CD_REG3 & 7);
    return type;
}

static void cd_cmd(u8 cmd, const u8 *p, int n) {
    while (CD_STAT & 0x80);
    CD_STAT = 1;
    CD_REG3 = 0x1F;
    CD_REG3 = 0x40;
    CD_STAT = 0;
    for (int i = 0; i < n; i++) CD_REG2 = p[i];
    CD_STAT = 0;
    CD_REG1 = cmd;
}

static int cd_simple(u8 cmd, const u8 *p, int n) {
    cd_cmd(cmd, p, n);
    int r = cd_wait(2000000);
    if (r != 3) return -1;
    return resp[0];
}

static u8 bcd(int v) { return (u8)(((v / 10) << 4) | (v % 10)); }

int cd_init(void) {
    CD_STAT = 1;
    CD_REG2 = 0x1F;
    CD_REG3 = 0x1F;
    for (int tries = 0; tries < 50; tries++) {
        int st = cd_simple(0x01, 0, 0);
        if (st >= 0 && !(st & 0x10)) break;
        delay(200000);
    }
    cd_cmd(0x0A, 0, 0);                 // Init
    if (cd_wait(2000000) == 3) cd_wait(20000000);
    u8 mode = 0x80;                     // double speed, 2048-byte sectors
    if (cd_simple(0x0E, &mode, 1) < 0) return -1;
    return 0;
}

int cd_lid_open(void) {
    int st = cd_simple(0x01, 0, 0);
    return st < 0 || (st & 0x10);
}

int cd_read(u32 lba, u32 n, u8 *dst) {
    for (int attempt = 0; attempt < 3; attempt++) {
        u32 a = lba + 150;
        u8 loc[3] = {bcd(a / 4500), bcd((a / 75) % 60), bcd(a % 75)};
        if (cd_simple(0x02, loc, 3) < 0) continue;
        if (cd_simple(0x06, 0, 0) < 0) continue;
        u32 got = 0;
        int err = 0;
        while (got < n) {
            CD_STAT = 1;
            int t = 0;
            while (!(CD_REG3 & 7)) if (++t > 20000000) break;
            if ((CD_REG3 & 7) != 1) { err = 1; break; }
            nresp = 0;
            while ((CD_STAT & 0x20) && nresp < 16) resp[nresp++] = CD_REG1;
            CD_STAT = 0;
            CD_REG3 = 0x00;
            CD_REG3 = 0x80;
            u8 *d = dst + got * 2048;
            while (!(CD_STAT & 0x40));
            for (int i = 0; i < 2048; i++) d[i] = CD_REG2;
            CD_STAT = 0;
            CD_REG3 = 0x00;
            CD_STAT = 1;
            CD_REG3 = 0x07;
            got++;
        }
        cd_cmd(0x09, 0, 0);             // Pause
        if (cd_wait(2000000) == 3) cd_wait(20000000);
        if (!err) return 0;
    }
    return -1;
}

// "nocash" unlock (psx-spx, CDROM secret unlock commands 50h-57h): after it, the drive reads
// discs without the licence wobble (burned discs) until the lid is opened. Works on PAL/US drives.
int cd_unlocked;
int cd_unlock(int pal) {
    static const char *const s[] = {"Licensed by", "Sony", "Computer", "Entertainment"};
    cd_cmd(0x50, 0, 0); cd_wait(500000);
    for (int i = 0; i < 4; i++) { cd_cmd(0x51 + i, (const u8 *)s[i], strlen(s[i])); cd_wait(500000); }
    const char *region = pal ? "(Europe)" : "of America";
    cd_cmd(0x55, (const u8 *)region, strlen(region)); cd_wait(500000);
    cd_cmd(0x56, 0, 0); cd_wait(500000);
    cd_cmd(0x57, 0, 0);                 // ends the sequence
    if (cd_wait(500000) != 3) return -1;
    cd_unlocked = 1;
    return 0;
}

// ---------------------------------------------------------------- ISO9660

static u8 sec[2048];

static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

int iso_root(u32 *lba, u32 *size) {
    if (cd_read(16, 1, sec)) return -1;
    if (sec[1] != 'C' || sec[2] != 'D' || sec[3] != '0' || sec[4] != '0' || sec[5] != '1') return -2;
    *lba = le32(sec + 156 + 2);
    *size = le32(sec + 156 + 10);
    return 0;
}

// List a directory: entries without "." and "..", names without ";1". Returns the count.
int iso_list(u32 lba, u32 size, CdEntry *e, int max) {
    int n = 0;
    for (u32 off = 0; off < size && n < max; off += 2048) {
        if (cd_read(lba + off / 2048, 1, sec)) return n ? n : -1;
        u32 p = 0;
        while (p < 2048 && sec[p] && n < max) {
            const u8 *r = sec + p;
            int nl = r[32];
            if (!(nl == 1 && (r[33] == 0 || r[33] == 1))) {
                CdEntry *x = &e[n++];
                int k = 0;
                for (; k < nl && k < 31 && r[33 + k] != ';'; k++) x->name[k] = r[33 + k];
                x->name[k] = 0;
                x->lba = le32(r + 2);
                x->size = le32(r + 10);
                x->dir = (r[25] & 2) != 0;
            }
            p += r[0];
        }
    }
    return n;
}

// ---------------------------------------------------------------- CD audio (CD-DA)
static int unbcd(u8 v) { return (v >> 4) * 10 + (v & 15); }

// first/last track numbers (GetTN), -1 if no disc
int cda_tracks(int *first, int *last) {
    if (cd_simple(0x13, 0, 0) < 0 || nresp < 3) return -1;
    *first = unbcd(resp[1]);
    *last = unbcd(resp[2]);
    return 0;
}

// play a track (Setmode CD-DA + Play), volume full on both the drive mixer and the SPU
// CD audio (CD-DA and XA) to the speakers: drive mixer full, SPU CD input on
void cd_audio_on(void) {
    CD_STAT = 2; CD_REG2 = 0x80; CD_REG3 = 0x00;      // ATV0 L->L, ATV1 L->R
    CD_STAT = 3; CD_REG1 = 0x80; CD_REG2 = 0x00;      // ATV2 R->R, ATV3 R->L
    CD_REG3 = 0x20;                                   // apply volumes
    REG16(0x1F801DAA) = 0xC001;          // SPU on, unmuted, CD audio input on
    REG16(0x1F801D80) = 0x3FFF; REG16(0x1F801D82) = 0x3FFF;   // main volume
    REG16(0x1F801DB0) = 0x7FFF; REG16(0x1F801DB2) = 0x7FFF;   // CD volume
}

int cda_play(int track) {
    u8 mode = 0x01;                      // CD-DA on, normal speed
    if (cd_simple(0x0E, &mode, 1) < 0) return -1;
    cd_audio_on();
    u8 t = bcd(track);
    return cd_simple(0x03, &t, 1) < 0 ? -1 : 0;
}

void cda_pause(void) { cd_cmd(0x09, 0, 0); if (cd_wait(2000000) == 3) cd_wait(20000000); }

// position in the current track (GetlocP): track, minutes, seconds. -1 if unavailable.
int cda_pos(int *track, int *mm, int *ss) {
    if (cd_simple(0x11, 0, 0) < 0 || nresp < 8) return -1;
    *track = unbcd(resp[0]);
    *mm = unbcd(resp[2]);
    *ss = unbcd(resp[3]);
    return 0;
}

// ---------------------------------------------------------------- streaming (STR video + XA audio)
// ReadS at double speed with XA-ADPCM on: the drive plays the audio sectors itself (to the SPU
// CD input) and hands over the others. Polled: cd_stream_get() returns 0 when no sector is ready.
int cd_stream_start(u32 lba) {
    u8 mode = 0x80 | 0x40;              // double speed, XA-ADPCM (real time) on, 2048-byte sectors
    if (cd_simple(0x0E, &mode, 1) < 0) return -1;
    u32 a = lba + 150;
    u8 loc[3] = {bcd(a / 4500), bcd((a / 75) % 60), bcd(a % 75)};
    if (cd_simple(0x02, loc, 3) < 0) return -1;
    return cd_simple(0x1B, 0, 0) < 0 ? -1 : 0;          // ReadS
}

int cd_stream_get(u8 *dst) {
    CD_STAT = 1;
    int irq = CD_REG3 & 7;
    if (!irq) return 0;
    if (irq != 1) {                                       // error / other: acknowledge, skip
        while (CD_STAT & 0x20) (void)CD_REG1;
        CD_STAT = 1; CD_REG3 = 0x07;
        return -1;
    }
    while (CD_STAT & 0x20) (void)CD_REG1;
    CD_STAT = 0;
    CD_REG3 = 0x00;
    CD_REG3 = 0x80;                                       // sector into the data FIFO
    while (!(CD_STAT & 0x40));
    for (int i = 0; i < 2048; i++) dst[i] = CD_REG2;
    CD_STAT = 0;
    CD_REG3 = 0x00;
    CD_STAT = 1;
    CD_REG3 = 0x07;                                       // acknowledge INT1
    return 1;
}

void cd_stream_stop(void) {
    cd_cmd(0x09, 0, 0);                                   // Pause
    if (cd_wait(2000000) == 3) cd_wait(20000000);
    u8 mode = 0x80;
    cd_simple(0x0E, &mode, 1);
}
