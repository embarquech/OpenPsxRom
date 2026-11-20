// 128 KB parallel flash at 1F000000h: identification, protection status, sector-verified writing.
// Same slow and careful method as the standalone flasher: per 16 KB block, skip if identical,
// erase, program byte by byte with read-back, verify by CRC, 3 attempts.
//
// Supported: byte-program/sector-erase JEDEC chips (AMD Am29F010 01/20, SST 39SF010A BF/B5 with
// 4 KB sectors, ST M29F010B 20/20, Fujitsu MBM29F010 04/20) and page-write chips (Atmel
// AT29C010A 1F/D5: 128-byte pages, no erase).
#include "rom.h"

// commands and data through KSEG1 (BF000000h, uncached, like Unirom): through 1F000000h the
// write buffer lets a read overtake the command writes and the chip does not answer
static u32 fbase = 0xBF000000u;      // where the chip takes its commands (see flash_identify)
#define F(o) REG8(fbase + (o))
#define EXP1_DELAY REG32(0x1F801008)
#define COM_DELAY REG32(0x1F801020)

static u32 saved_exp1, saved_com;
// The BIOS bus timings are kept: longer EXP1 timings (fine on a Datel cartridge) make the
// Xplorer's logic drop off the bus (every read gives FFh). Slowness comes from the pauses instead.
static void slow_bus(void) {
    saved_exp1 = EXP1_DELAY;
    saved_com = COM_DELAY;
}
static void restore_bus(void) { EXP1_DELAY = saved_exp1; COM_DELAY = saved_com; }
static void pause(u32 n) { for (volatile u32 i = 0; i < n; i++); }
static void wr(u32 o, u8 v) { F(o) = v; pause(20); }
static void cmd(u8 c) { wr(0x5555, 0xAA); wr(0x2AAA, 0x55); wr(0x5555, c); }
// Atmel/SST alternative ID entry: AA 55 80 AA 55 60
static void cmd_id2(void) { cmd(0x80); wr(0x5555, 0xAA); wr(0x2AAA, 0x55); wr(0x5555, 0x60); }
// exit ID mode / reset: full unlock sequence only (a lone F0h write could start an Atmel page
// write when its software protection is off), then 10 ms (Atmel tIDA)
static void reset(void) { cmd(0xF0); pause(150000); }

enum { T_NONE, T_SECTOR, T_PAGE };
// size: erase sector in KB (T_SECTOR) or page size in bytes (T_PAGE)
static const struct { u8 mfr, dev, type; u16 size; const char *name; } chips[] = {
    {0x01, 0x20, T_SECTOR, 16, "AMD Am29F010"},
    {0x20, 0x20, T_SECTOR, 16, "ST M29F010B"},
    {0x04, 0x20, T_SECTOR, 16, "Fujitsu MBM29F010"},
    {0xBF, 0xB5, T_SECTOR, 4, "SST 39SF010A"},
    {0xBF, 0xB6, T_SECTOR, 4, "SST 39SF020A"},
    {0xBF, 0xB7, T_SECTOR, 4, "SST 39SF040"},
    {0x1F, 0xD5, T_PAGE, 128, "Atmel AT29C010A"},
    {0x1F, 0xDA, T_PAGE, 256, "Atmel AT29C020"},
    {0x1F, 0xA4, T_PAGE, 256, "Atmel AT29C040A"},
    {0xBF, 0x07, T_PAGE, 128, "SST 29EE010"},
    {0xBF, 0x10, T_PAGE, 128, "SST 29EE020"},           // Xplorer
    {0xBF, 0x12, T_PAGE, 128, "SST 29LE020"},
    {0xDA, 0xC1, T_PAGE, 128, "Winbond W29EE011"},
};
#define NCHIPS (int)(sizeof chips / sizeof chips[0])

static int find(u8 mfr, u8 dev) {
    for (int i = 0; i < NCHIPS; i++) if (chips[i].mfr == mfr && chips[i].dev == dev) return i;
    return -1;
}
const char *flash_chip_name(u8 mfr, u8 dev) {
    int i = find(mfr, dev);
    return i < 0 ? "unknown chip" : chips[i].name;
}
// chip capacity in KB, from the part number (...010 = 1 Mbit, 020 = 2 Mbit, 040 = 4 Mbit); 0 if unknown
u32 flash_chip_kb(u8 mfr, u8 dev) {
    int i = find(mfr, dev);
    if (i < 0) return 0;
    const char *n = chips[i].name;
    for (int k = 0; n[k] && n[k + 1] && n[k + 2]; k++)
        if (n[k] == '0' && n[k + 2] >= '0' && n[k + 2] <= '9') {
            if (n[k + 1] == '1') return 128;
            if (n[k + 1] == '2') return 256;
            if (n[k + 1] == '4') return 512;
        }
    return 128;                                        // W29EE011: 1 Mbit
}

// Raw probe results for diagnosis (read over serial with /dump): for each try
// [base 0/1][sequence 0/1]: bytes at 0 and 1 before, then in ID mode.
u8 flash_dbg[16];

int flash_identify(u8 *mfr, u8 *dev) {
    static const u32 bases[2] = {0xBF000000u, 0xBF040000u};
    slow_bus();
    int found = -1;
    for (int b = 0; b < 2 && found < 0; b++)
        for (int q = 0; q < 2 && found < 0; q++) {
            fbase = bases[b];
            u8 o0 = F(0), o1 = F(1);
            if (q == 0) cmd(0x90); else cmd_id2();
            pause(150000);                             // Atmel needs ~10 ms before the ID reads
            u8 m = F(0), d = F(1);
            reset();
            u8 *g = flash_dbg + (b * 2 + q) * 4;
            g[0] = o0; g[1] = o1; g[2] = m; g[3] = d;
            if ((m != o0 || d != o1) && find(m, d) >= 0) { found = b; *mfr = m; *dev = d; }
            else if (b == 0 && q == 0) { *mfr = m; *dev = d; }
        }
    if (found < 0) fbase = bases[0];
    restore_bus();
    return found >= 0;
}

// ID of whatever answers at flash offset off (0 or 40000h: a second chip on double-bank Xplorer FX).
// 1 if the bytes changed in ID mode (a chip answered), mfr/dev = what was read.
int flash_identify_at(u32 off, u8 *mfr, u8 *dev) {
    u32 save = fbase;
    int ok = 0;
    slow_bus();
    for (int q = 0; q < 2 && !ok; q++) {
        fbase = 0xBF000000u + off;
        u8 o0 = F(0), o1 = F(1);
        if (q == 0) cmd(0x90); else cmd_id2();
        pause(150000);
        u8 m = F(0), d = F(1);
        reset();
        if (m != o0 || d != o1) { ok = 1; *mfr = m; *dev = d; }
        else if (q == 0) { *mfr = m; *dev = d; }
    }
    fbase = save;
    restore_bus();
    return ok;
}

void flash_protect(u8 prot[8]) {
    slow_bus();
    cmd(0x90);
    pause(150000);                                     // Atmel needs ~10 ms before the ID reads
    for (u32 s = 0; s < 8; s++) prot[s] = F(s * FLASH_SECT + 2) & 1;
    reset();
    restore_bus();
}

// DQ7 polling. 0 ok, 1 DQ5 error, 2 timeout.
static int poll(u32 a, u8 want, u32 loops) {
    while (loops--) {
        pause(4);
        u8 v = F(a);
        if (!((v ^ want) & 0x80)) return 0;
        if (v & 0x20) {
            v = F(a);
            if (!((v ^ want) & 0x80)) return 0;
            reset();
            return 1;
        }
    }
    reset();
    return 2;
}

static int erase(u32 a) {
    cmd(0x80);
    wr(0x5555, 0xAA); wr(0x2AAA, 0x55);
    wr(a, 0x30);
    pause(2000);
    return poll(a, 0xFF, 50000000u);
}

static int program_byte(u32 a, u8 v) {
    for (int k = 0; k < 3; k++) {
        cmd(0xA0);
        wr(a, v);
        if (poll(a, v, 200000u)) continue;
        pause(10);
        if (F(a) == v && F(a) == v) return 0;
    }
    return 1;
}

// Nothing may slow a page load down: wait (bounded) until no DMA runs (the progress screen just
// drawn leaves a GPU list transferring, which held the bus and cut page loads after 32 bytes on
// the Xplorer FX's second chip), and keep interrupts off during the load.
static void dma_idle(void) {
    for (u32 ch = 0; ch < 7; ch++)
        for (u32 t = 0; t < 2000000 && (REG32(0x1F801088 + ch * 0x10) & 0x01000000); t++);
}
static u32 irq_off(void) {
    u32 sr;
    __asm__ volatile("mfc0 %0, $12; nop" : "=r"(sr));
    __asm__ volatile("mtc0 %0, $12; nop; nop" ::"r"(sr & ~1u));
    return sr;
}
static void irq_restore(u32 sr) { __asm__ volatile("mtc0 %0, $12; nop; nop" ::"r"(sr)); }

// Atmel/SST/Winbond page chips: one page loaded after the unlock (bytes < 150 us apart), then
// programmed internally (~10 ms); no erase.
static int program_page(u32 a, const u8 *p, int n) {
    for (int k = 0; k < 3; k++) {
        dma_idle();
        u32 sr = irq_off();
        cmd(0xA0);
        for (int i = 0; i < n; i++) F(a + i) = p[i];
        irq_restore(sr);
        pause(2000);                                   // > 150 us: end of the page load
        poll(a + n - 1, p[n - 1], 2000000u);           // DATA polling until programmed (~5-10 ms)
        pause(200);
        int ok = 1;
        for (int i = 0; i < n; i++) if (F(a + i) != p[i]) { ok = 0; break; }
        if (ok) return 0;
    }
    return 1;
}

static int block_equal(u32 base, const u8 *img) {
    for (u32 i = 0; i < FLASH_SECT; i++) if (F(base + i) != img[i]) return 0;
    return 1;
}

// Returns the number of 16 KB blocks that failed (0 = success), -1 unknown chip.
int flash_write(const u8 *img, u32 size, void (*progress)(u32, const char *)) {
    return flash_write_at(0, img, size, progress);
}

// Write [off, off+size) of the flash (off and size multiples of 16 KB) from img.
int flash_write_at(u32 off, const u8 *img, u32 size, void (*progress)(u32, const char *)) {
    u8 mfr, dev;
    // double-bank carts (Xplorer FX Pro): from 40000h on, a second chip with its own commands
    u32 chip = off >= 0x40000u ? 0x40000u : 0;
    if (chip) { if (!flash_identify_at(chip, &mfr, &dev)) return -1; }
    else flash_identify(&mfr, &dev);
    int c = find(mfr, dev);
    if (c < 0) return -1;
    if (chip && off + size > 0x60000u) return -1;      // 60000h+: the Xplorer's registers
    if (!chip && off + size > 0x40000u) return -1;     // one chip per call
    slow_bus();
    int bad = 0;
    fbase = 0xBF000000u + chip;
    for (u32 s = off / FLASH_SECT; s * FLASH_SECT < off + size; s++) {
        u32 base = s * FLASH_SECT - chip;               // offset inside the chip being written
        const u8 *src = img + s * FLASH_SECT - off;
        if (block_equal(base, src)) { if (progress) progress(s, "same"); continue; }
        int ok = 0;
        for (int t = 0; t < 3 && !ok; t++) {
            if (progress) progress(s, t ? "retry" : "write");
            int fail = 0;
            if (chips[c].type == T_SECTOR) {
                u32 esz = chips[c].size * 1024u;
                for (u32 o = 0; o < FLASH_SECT && !fail; o += esz) fail = erase(base + o) != 0;
                for (u32 i = 0; i < FLASH_SECT && !fail; i++)
                    if (src[i] != 0xFF) fail = program_byte(base + i, src[i]);
            } else {
                for (u32 i = 0; i < FLASH_SECT && !fail; i += chips[c].size) {
                    int same = 1;
                    for (u32 k = 0; k < chips[c].size; k++) if (F(base + i + k) != src[i + k]) { same = 0; break; }
                    if (!same) fail = program_page(base + i, src + i, chips[c].size);
                    if (progress && !(i & 0xFFF)) progress(s | (i << 8), "page");
                }
            }
            ok = !fail && block_equal(base, src);
        }
        if (progress) progress(s, ok ? "ok" : "FAILED");
        if (!ok) bad++;
    }
    reset();
    fbase = 0xBF000000u;
    restore_bus();
    return bad;
}
