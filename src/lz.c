// OMC1 compressed memory card format (Xplorer-style backups), LZSS:
//   header  'O','M','C','1', u32 raw size, u32 CRC32 of the raw data, u32 packed size (LE)
//   stream  flag byte (bit 0 first): 1 = literal byte, 0 = match of 2 bytes:
//           b0 = distance-1 low 8 bits, b1 = (distance-1 high 4 bits) << 4 | (length - 3)
//           distance 1..4096, length 3..18
// Same format in tools/omc.py (PC side).
#include "rom.h"

#define WIN 4096
#define MAXLEN 18
static u16 head[4096];          // last position + 1 of each 3-byte hash

static u32 hash3(const u8 *p) { return ((p[0] << 4) ^ (p[1] << 2) ^ p[2] ^ (p[0] >> 4)) & 4095; }

// Returns the packed size (header included), 0 if it does not fit in max.
u32 omc_pack(const u8 *src, u32 n, u8 *dst, u32 max) {
    if (max < 16) return 0;
    memset(head, 0, sizeof head);
    u32 o = 16, i = 0;
    while (i < n) {
        if (o + 1 + 16 > max) return 0;
        u32 fpos = o++;
        u8 flags = 0;
        for (int bit = 0; bit < 8 && i < n; bit++) {
            u32 best = 0, dist = 0;
            if (i + 3 <= n) {
                u32 h = hash3(src + i);
                u32 cand = head[h];             // positions are stored +1 (0 = none)
                // the hash table only keeps 16 bits: rebuild the full position near i
                if (cand) {
                    u32 c = (i & ~0xFFFFu) | (cand - 1);
                    if (c >= i) c -= 0x10000;
                    if (c < i && i - c <= WIN) {
                        u32 l = 0, lim = n - i < MAXLEN ? n - i : MAXLEN;
                        while (l < lim && src[c + l] == src[i + l]) l++;
                        if (l >= 3) { best = l; dist = i - c; }
                    }
                }
                head[h] = (u16)((i & 0xFFFF) + 1);
            }
            if (best >= 3) {
                u32 d = dist - 1;
                dst[o++] = (u8)d;
                dst[o++] = (u8)(((d >> 8) << 4) | (best - 3));
                // index the skipped positions too (better ratio on long runs)
                for (u32 k = 1; k < best && i + k + 3 <= n; k++) head[hash3(src + i + k)] = (u16)(((i + k) & 0xFFFF) + 1);
                i += best;
            } else {
                flags |= 1 << bit;
                dst[o++] = src[i++];
            }
        }
        dst[fpos] = flags;
    }
    u32 crc = crc32(src, n);
    dst[0] = 'O'; dst[1] = 'M'; dst[2] = 'C'; dst[3] = '1';
    for (int k = 0; k < 4; k++) {
        dst[4 + k] = (u8)(n >> (8 * k));
        dst[8 + k] = (u8)(crc >> (8 * k));
        dst[12 + k] = (u8)(o >> (8 * k));
    }
    return o;
}

static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

// Returns the raw size, 0 on a bad header / overflow / CRC error.
u32 omc_unpack(const u8 *src, u8 *dst, u32 max) {
    if (src[0] != 'O' || src[1] != 'M' || src[2] != 'C' || src[3] != '1') return 0;
    u32 n = rd32(src + 4), crc = rd32(src + 8), end = rd32(src + 12);
    if (n > max || end < 16) return 0;
    u32 i = 16, o = 0;
    while (o < n && i < end) {
        u8 flags = src[i++];
        for (int bit = 0; bit < 8 && o < n && i < end; bit++) {
            if (flags & (1 << bit)) dst[o++] = src[i++];
            else {
                u32 d = (src[i] | ((src[i + 1] >> 4) << 8)) + 1, l = (src[i + 1] & 15) + 3;
                i += 2;
                if (d > o) return 0;
                while (l-- && o < n) { dst[o] = dst[o - d]; o++; }
            }
        }
    }
    if (o != n || crc32(dst, n) != crc) return 0;
    return n;
}

int omc_valid(const u8 *src) {
    return src[0] == 'O' && src[1] == 'M' && src[2] == 'C' && src[3] == '1';
}
u32 omc_raw_size(const u8 *src) { return rd32(src + 4); }
u32 omc_packed_size(const u8 *src) { return rd32(src + 12); }
