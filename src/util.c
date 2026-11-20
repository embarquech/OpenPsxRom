#include "rom.h"

void *memcpy(void *d, const void *s, u32 n) {
    u8 *a = d; const u8 *b = s;
    while (n--) *a++ = *b++;
    return d;
}
void *memset(void *d, int c, u32 n) {
    u8 *a = d;
    while (n--) *a++ = (u8)c;
    return d;
}
int strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

char *fmt_hex(char *p, u32 v, int digits) {
    for (int i = digits - 1; i >= 0; i--) *p++ = "0123456789ABCDEF"[(v >> (i * 4)) & 15];
    *p = 0;
    return p;
}
char *fmt_dec(char *p, u32 v) {
    char t[12];
    int n = 0;
    do { t[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}
char *fmt_str(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }

static u32 crc_tab[256];
// zlib's crc32(): crc32_upd(0, a) then crc32_upd(that, b) = crc32 of a and b one after the other
u32 crc32_upd(u32 crc, const void *data, u32 n) {
    if (!crc_tab[1])
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_tab[i] = c;
        }
    const volatile u8 *p = data;
    u32 c = ~crc;
    while (n--) c = crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
u32 crc32(const void *data, u32 n) { return crc32_upd(0, data, n); }
