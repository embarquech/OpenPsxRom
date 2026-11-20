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

// Console model from the BIOS (psx-spx "BIOS versions"): "System ROM Version 4.1 12/16/97 E" at
// BFC7FF32h from v2.0 on, J/A/E = Japan/America/Europe; v1.x (Japan only) by its date at BFC00100h.
// The same BIOS went into several models (7002/7502/9002...): all of them are given, the BIOS
// cannot tell them apart.
// Table: version digits + region, then the model, ';' between entries.
static const char models[] =
    "20A1001;20E1002;21J3500;21A1001;21E1002;22J5000;22A1001;22E1002;30J5500;30A5501/5503/7003;"
    "30E5502/5552;40J7000/7500/9000;41A7001/7501/7503/9001/9003;41E7002/7502/9002;43J100;"
    "44A101;44E102;45A101;45E102;";
// The BIOS version string, or "v1.x" when there is none (before v2.0).
char *fmt_bios(char *p) {
    const char *v = (const char *)0xBFC7FF32;
    if (v[0] != 'S' || v[1] != 'y') return fmt_str(p, "v1.x (no version string)");
    for (int i = 0; i < 40 && v[i] >= 32 && v[i] < 127; i++) *p++ = v[i];
    while (p[-1] == ' ') p--;
    *p = 0;
    return p;
}
const char *bios_model(void) {
    static char out[40] = "SCPH-";
    char ver[48], *e = fmt_bios(ver);
    if (ver[0] == 'v') {
        u32 date = *(const volatile u32 *)0xBFC00100;    // BCD yyyymmdd
        return date == 0x19940922 ? "SCPH-1000" : date == 0x19950122 ? "SCPH-3000" : "unknown";
    }
    for (const char *t = models; *t; t++) {             // "System ROM Version X.Y ... R"
        int hit = t[0] == ver[19] && t[1] == ver[21] && t[2] == e[-1];
        t += 3;
        char *o = out + 5;
        while (*t != ';') *o++ = *t++;
        *o = 0;
        if (hit) return out;
    }
    return "unknown";
}
