// STR video player (MDEC video + XA audio), polled, no interrupts.
//
// Pipeline (one loop): CD sectors are taken as soon as they are ready and reassembled into frames
// (two bitstream buffers); a complete frame is decompressed into MDEC run-length codes by the
// PSn00bSDK decoder (src/str/vlc.s, GTE assisted, bitstream v2 and v3), in pieces so the CD is
// checked in between; the MDEC decodes it one 16-pixel-wide column at a time, each column going
// to the back framebuffer by DMA; then the display flips. XA audio sectors are played by the drive.
// Based on the PSn00bSDK strvideo example (MPL-2.0, (C) 2022-2023 spicyjpeg).
#include "rom.h"
#include "str/psxpress.h"

#define MDEC0 REG32(0x1F801820)
#define MDEC1 REG32(0x1F801824)
#define DMA0_MADR REG32(0x1F801080)
#define DMA0_BCR REG32(0x1F801084)
#define DMA0_CHCR REG32(0x1F801088)
#define DMA1_MADR REG32(0x1F801090)
#define DMA1_BCR REG32(0x1F801094)
#define DMA1_CHCR REG32(0x1F801098)

#define BS0 ((u32 *)0x80100000)          // bitstream of the frame being read (32 KB each)
#define BS1 ((u32 *)0x80108000)
#define RLE ((u32 *)0x80110000)          // run-length codes for the MDEC (128 KB)
#define RLE_WORDS 0x8000
#define SLICE ((u32 *)0x80130000)        // one decoded 16-pixel column (16 x 240 x 2 bytes)
#define SECT ((u8 *)0x80132000)          // sector buffer

typedef struct {
    u16 magic, type, sector_id, sector_count;
    u32 frame_id, bs_length;
    u16 width, height;
    u8 bs_header[8];
    u32 reserved;
} StrHeader;

// MDEC tables (psx-spx / PSn00bSDK defaults): quantization (zigzag order) and IDCT matrix
static const u8 iq[64] = {
     2, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27,
    27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29,
    29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34,
    35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83,
};
#define S0 0x5a82
#define S1 0x7d8a
#define S2 0x7641
#define S3 0x6a6d
#define S4 0x5a82
#define S5 0x471c
#define S6 0x30fb
#define S7 0x18f8
static const s16 dct[64] = {
    S0, S0, S0, S0, S0, S0, S0, S0,  S1, S3, S5, S7, -S7, -S5, -S3, -S1,
    S2, S6, -S6, -S2, -S2, -S6, S6, S2,  S3, -S7, -S1, -S5, S5, S1, S7, -S3,
    S4, -S4, -S4, S4, S4, -S4, -S4, S4,  S5, -S1, S7, S3, -S3, -S7, S1, -S5,
    S6, -S2, S2, -S6, -S6, S2, -S2, S6,  S7, -S5, S3, -S1, S1, -S3, S5, -S7,
};
static u32 tables[16 + 32 + 32];        // word-aligned copies for DMA

static void mdec_in(const u32 *data, u32 words) {        // DMA0, blocks of 32 words
    while (DMA0_CHCR & 0x01000000);
    DMA0_MADR = (u32)data;
    DMA0_BCR = words >= 32 ? (32 | ((words + 31) / 32) << 16) : (0x10000 | words);
    DMA0_CHCR = 0x01000201;
}
static void mdec_init(void) {
    DMA_DPCR |= 0x88;                    // DMA0 (MDEC in), DMA1 (MDEC out) on
    DMA0_CHCR = 0x201; DMA1_CHCR = 0x200;
    MDEC1 = 0x80000000;                  // reset
    MDEC1 = 0x60000000;                  // DMA requests on
    memcpy(tables, dct, 128);
    MDEC0 = 0x60000000;                  // IDCT matrix
    mdec_in(tables, 32);
    while (DMA0_CHCR & 0x01000000);
    memcpy(tables, iq, 64);
    memcpy((u8 *)tables + 64, iq, 64);
    MDEC0 = 0x40000001;                  // luma + chroma quantization tables
    mdec_in(tables, 32);
    while (DMA0_CHCR & 0x01000000);
}

static void gte_on(void) {
    u32 sr;
    __asm__ volatile("mfc0 %0, $12" : "=r"(sr));
    sr |= 0x40000000;                    // COP2 (GTE) usable: the decoder uses its LZCS/LZCR
    __asm__ volatile("mtc0 %0, $12\n nop\n nop" :: "r"(sr));
}

static void upload(int x, int y, int w, int h, const u32 *data) {   // CPU -> VRAM by DMA2
    while (!(GPUSTAT & (1u << 26)));
    GP0 = 0x01000000;
    while (!(GPUSTAT & (1u << 26)));
    GP0 = 0xA0000000;
    GP0 = (y << 16) | x;
    GP0 = (h << 16) | w;
    u32 words = (u32)(w * h) / 2;
    GP1 = 0x04000002;
    DMA2_MADR = (u32)data;
    DMA2_BCR = ((words / 16) << 16) | 16;
    DMA2_CHCR = 0x01000201;
    while (DMA2_CHCR & 0x01000000);
}

// ---------------------------------------------------------------- stream state
static u32 *bs[2] = {BS0, BS1};
static int cur, ready, frame_id, missing, ended, frame_w[2], frame_h[2];
static u32 sectors_left;

static void take_sectors(void) {
    for (int k = 0; k < 8; k++) {
        int r = cd_stream_get(SECT);
        if (r == 0) return;
        if (r < 0) continue;
        if (sectors_left) sectors_left--;
        if (!sectors_left) ended = 1;
        const StrHeader *h = (const StrHeader *)SECT;
        if (h->magic != 0x0160) { ended = 1; return; }
        if (h->type != 0x8001) continue;                 // not video
        if ((int)h->frame_id < frame_id) { ended = 1; return; }
        if ((int)h->frame_id > frame_id) {
            if (frame_id >= 0 && !missing) ready = cur;   // previous frame complete
            frame_id = h->frame_id;
            missing = h->sector_count;
            cur ^= 1;
            if (ready == cur) ready = -1;                 // not decoded in time: dropped
            frame_w[cur] = (h->width + 15) & ~15;
            frame_h[cur] = (h->height + 15) & ~15;
        }
        if (h->sector_id < 16) {
            memcpy((u8 *)bs[cur] + h->sector_id * 2016, SECT + 32, 2016);
            if (missing) missing--;
        }
    }
}

int str_frames;
int str_play(u32 lba, u32 size, char *err) {
    gfx_shutdown();
    GP1 = 0x08000001 | (gfx_pal ? 8 : 0);               // 320x240 15-bit
    for (int b = 0; b < 2; b++) {                        // both framebuffers black
        while (!(GPUSTAT & (1u << 26)));
        GP0 = 0x02000000; GP0 = (b * 256) << 16; GP0 = (240 << 16) | 320;
    }
    GP1 = 0x05000000;
    GP1 = 0x03000000;
    mdec_init();
    gte_on();
    DecDCTvlcCopyTableV3((VLC_TableV3 *)0x1F800000);     // decoder table in the scratchpad
    cd_audio_on();
    cur = 0; ready = -1; frame_id = -1; missing = 0; ended = 0;
    sectors_left = (size + 2047) / 2048;
    if (cd_stream_start(lba)) { fmt_str(err, "CD read error"); gfx_restore(); return -1; }
    int back = 1, frames = 0;
    u16 old = 0xFFFF;
    while (!ended) {
        take_sectors();
        u16 now = pad_read();
        if ((now & ~old) & (PAD_CIRCLE | PAD_START)) break;
        old = now;
        if (ready < 0) continue;
        int f = ready, w = frame_w[f], h = frame_h[f];
        ready = -1;
        if (h > 240 || w > 320) continue;                // unsupported size
        // bitstream -> MDEC codes, in pieces of PIECE words so the CD can be served in between.
        // Each piece starts with its own length word, written over the last code of the previous
        // piece: that word is saved and put back, so the codes end up in one continuous stream.
        #define PIECE 0x800
        VLC_Context ctx;
        u32 *out = RLE;
        int r = DecDCTvlcStart(&ctx, out, PIECE, bs[f]);
        while (r == 1) {
            take_sectors();
            out += PIECE - 1;
            if (out + PIECE > RLE + RLE_WORDS) { r = -1; break; }
            u32 keep = *out;
            r = DecDCTvlcContinue(&ctx, out, PIECE);
            *out = keep;
        }
        if (r < 0) continue;                             // damaged frame
        u32 total = ((const u32 *)bs[f])[0] & 0xFFFF;    // BS header: number of MDEC words
        // MDEC: one command for the whole frame, then one 16-pixel column at a time
        while (DMA0_CHCR & 0x01000000) take_sectors();
        MDEC0 = 0x38000000 | total;                      // 15-bit output
        mdec_in(RLE + 1, total);
        int x0 = (320 - w) / 2, y0 = back * 256 + (240 - h) / 2;
        for (int col = 0; col < w / 16; col++) {
            DMA1_MADR = (u32)SLICE;
            DMA1_BCR = 32 | ((16 * h / 2 / 32) << 16);
            DMA1_CHCR = 0x01000200;
            u32 t = 0;
            while ((DMA1_CHCR & 0x01000000) && ++t < 3000000) take_sectors();
            if (DMA1_CHCR & 0x01000000) { DMA1_CHCR = 0x200; break; }       // MDEC stalled
            upload(x0 + col * 16, y0, 16, h, SLICE);
        }
        while (!(GPUSTAT & (1u << 26)));
        GP1 = 0x05000000 | ((back * 256) << 10);         // show it
        back ^= 1;
        frames++;
    }
    str_frames = frames;
    cd_stream_stop();
    MDEC1 = 0x80000000;
    gfx_restore();
    if (!frames) { fmt_str(err, "no video frame (not an STR file?)"); return -1; }
    return 0;
}
