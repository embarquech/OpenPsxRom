// Save store in the cartridge flash (64..112 KB): a list of memory card saves, compressed as a whole.
//   raw container: 'O','F','S','1', u32 count, then per save: 128-byte directory frame (name at
//   10..30, size at 4..7) + its data (size rounded up to 8 KB blocks)
//   in the flash: the container packed with OMC1 (src/lz.c), 48 KB max
// A legacy OMC1 whole-card image (first OpenROM 0.4 backups) is converted into saves on load.
#include "rom.h"

u32 omc_pack(const u8 *src, u32 n, u8 *dst, u32 max);
u32 omc_unpack(const u8 *src, u8 *dst, u32 max);
int omc_valid(const u8 *src);

#define RAW ((u8 *)0x80100000)          // container in RAM (320 KB max)
#define RAW_MAX 0x50000u
#define PACK ((u8 *)0x80150000)         // packed copy for the flash (48 KB)

static u32 raw_len;
static int count;
static u32 offs[64];                    // offset of each save in RAW

static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 save_blocks(const u8 *dirf) { return (rd32(dirf + 4) + 8191) / 8192; }

static void index_saves(void) {
    u32 o = 8;
    for (int i = 0; i < count; i++) {
        offs[i] = o;
        o += 128 + save_blocks(RAW + o) * 8192;
    }
    raw_len = o;
}

static void empty(void) {
    RAW[0] = 'O'; RAW[1] = 'F'; RAW[2] = 'S'; RAW[3] = '1';
    wr32(RAW + 4, 0);
    count = 0;
    raw_len = 8;
}

// Convert a raw 128 KB card image (at src, not RAW) into saves.
static void import_card(const u8 *card) {
    empty();
    for (int b = 1; b <= 15; b++) {
        const u8 *d = card + b * 128;
        if (d[0] != 0x51) continue;
        u32 nb = (rd32(d + 4) + 8191) / 8192;
        u8 *o = RAW + raw_len;
        memcpy(o, d, 128);
        int blk = b;
        for (u32 k = 0; k < nb && blk >= 1 && blk <= 15; k++) {
            memcpy(o + 128 + k * 8192, card + blk * 8192, 8192);
            const u8 *dd = card + blk * 128;
            u16 nx = dd[8] | (dd[9] << 8);
            blk = nx == 0xFFFF ? -1 : nx + 1;
        }
        raw_len += 128 + nb * 8192;
        count++;
    }
    wr32(RAW + 4, count);
    index_saves();
}

// Load the store from the flash. Returns the number of saves (0 if empty or damaged).
int store_load(void) {
    const u8 *f = (const u8 *)(FLASH_BASE + FL_CARD_OFF);
    empty();
    if (!omc_valid(f)) return 0;
    u32 n = omc_unpack(f, RAW, RAW_MAX);
    if (n == 131072 && RAW[0] == 'M' && RAW[1] == 'C') {          // legacy whole-card backup
        u8 *card = (u8 *)0x80160000;
        memcpy(card, RAW, 131072);
        import_card(card);
        return count;
    }
    if (n < 8 || RAW[0] != 'O' || RAW[1] != 'F' || RAW[2] != 'S' || RAW[3] != '1') { empty(); return 0; }
    count = rd32(RAW + 4);
    if (count > 64) { empty(); return 0; }
    index_saves();
    return count;
}

// Write the store to the flash. 0 ok, -1 flash error, -2 too big for the 48 KB area.
int store_save(int (*writer)(u32 off, const u8 *data, u32 size)) {
    wr32(RAW + 4, count);
    u32 n = omc_pack(RAW, raw_len, PACK, FL_CARD_SIZE);
    if (!n) return -2;
    memset(PACK + n, 0xFF, FL_CARD_SIZE - n);
    return writer(FL_CARD_OFF, PACK, FL_CARD_SIZE) ? -1 : 0;
}

int store_count(void) { return count; }

// Packed size of the current store (KB, rounded up), to show how full the cartridge is.
u32 store_packed_kb(void) {
    u32 n = omc_pack(RAW, raw_len, PACK, FL_CARD_SIZE);
    return n ? (n + 1023) / 1024 : 999;
}

void store_entry(int i, McEntry *e) {
    const u8 *d = RAW + offs[i];
    int k = 0;
    for (; k < 20 && d[10 + k]; k++) e->name[k] = d[10 + k];
    e->name[k] = 0;
    e->size = rd32(d + 4);
    e->blocks = save_blocks(d);
    e->first = 0;
    mc_parse_header(d + 128, d + 256, e);           // icon frames follow the header frame
}

const u8 *store_dirf(int i) { return RAW + offs[i]; }
const u8 *store_data(int i) { return RAW + offs[i] + 128; }

// Add a save (same name: replaced). -1 store full.
int store_add(const u8 *dirf, const u8 *data) {
    for (int i = 0; i < count; i++) {
        const u8 *d = RAW + offs[i];
        int same = 1;
        for (int k = 0; k < 21; k++) if (d[10 + k] != dirf[10 + k]) { same = 0; break; }
        if (same) { store_delete(i); break; }
    }
    u32 nb = save_blocks(dirf);
    if (count >= 64 || raw_len + 128 + nb * 8192 > RAW_MAX) return -1;
    memcpy(RAW + raw_len, dirf, 128);
    memcpy(RAW + raw_len + 128, data, nb * 8192);
    offs[count++] = raw_len;
    raw_len += 128 + nb * 8192;
    return 0;
}

void store_delete(int i) {
    if (i < 0 || i >= count) return;
    u32 a = offs[i], len = 128 + save_blocks(RAW + a) * 8192;
    memcpy(RAW + a, RAW + a + len, raw_len - a - len);
    count--;
    index_saves();
}
