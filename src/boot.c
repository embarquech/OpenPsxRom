// Game boot done by OpenROM itself (like Unirom: the BIOS shell is never resumed).
// SYSTEM.CNF (or PSX.EXE) -> SetConf(EVENT, TCB, STACK) -> resident core -> load the EXE -> start.
#include "rom.h"

u32 bios_a0(u32 fn, u32 a0, u32 a1, u32 a2);

static int upc(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int name_eq(const char *a, const char *b, int n) {
    int i = 0;
    for (; i < n; i++) if (!a[i] || upc(a[i]) != upc(b[i])) return 0;
    return a[i] == 0;
}

static CdEntry ents[128];

// Find a path such as "\DIR\FILE.EXE" (separators \ or /) from the root directory.
static int find_path(const char *path, u32 *lba, u32 *size) {
    u32 dl, ds;
    if (iso_root(&dl, &ds)) return -1;
    while (*path == '\\' || *path == '/') path++;
    for (;;) {
        int n = 0;
        while (path[n] && path[n] != '\\' && path[n] != '/' && path[n] != ';') n++;
        int cnt = iso_list(dl, ds, ents, 128);
        int hit = -1;
        for (int i = 0; i < cnt; i++) if (name_eq(ents[i].name, path, n)) { hit = i; break; }
        if (hit < 0) return -1;
        path += n;
        if (*path == '\\' || *path == '/') {
            path++;
            dl = ents[hit].lba; ds = ents[hit].size;
            continue;
        }
        *lba = ents[hit].lba; *size = ents[hit].size;
        return 0;
    }
}

static u32 parse_hex(const char *s) {
    u32 v = 0;
    while (*s == ' ' || *s == '\t' || *s == '=') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;; s++) {
        int c = upc(*s);
        if (c >= '0' && c <= '9') v = v * 16 + c - '0';
        else if (c >= 'A' && c <= 'F') v = v * 16 + c - 'A' + 10;
        else break;
    }
    return v;
}

static u8 cnf[2048 + 1];

// Returns only on error (message in err).
int cd_boot(char *err) {
    if (cd_init()) { fmt_str(err, "no disc / drive error"); return -1; }
    char boot[64] = "PSX.EXE";
    u32 tcb = 4, ev = 16, stack = 0x801FFF00;
    u32 lba, size;
    if (find_path("SYSTEM.CNF", &lba, &size) == 0 && cd_read(lba, 1, cnf) == 0) {
        cnf[2048] = 0;
        if (size < 2048) cnf[size] = 0;
        for (char *l = (char *)cnf; *l;) {
            char *e = l;
            while (*e && *e != '\n' && *e != '\r') e++;
            char keep = *e;
            *e = 0;
            while (*l == ' ' || *l == '\t') l++;
            if (upc(l[0]) == 'B' && upc(l[1]) == 'O' && upc(l[2]) == 'O' && upc(l[3]) == 'T') {
                char *v = l + 4;
                while (*v == ' ' || *v == '\t' || *v == '=') v++;
                if (upc(v[0]) == 'C' && upc(v[1]) == 'D' && upc(v[2]) == 'R' && upc(v[3]) == 'O' && upc(v[4]) == 'M') v += 5;
                while (*v == ':') v++;
                int k = 0;
                while (v[k] && v[k] != ' ' && v[k] != '\t' && k < 63) { boot[k] = v[k]; k++; }
                boot[k] = 0;
            } else if (upc(l[0]) == 'T' && upc(l[1]) == 'C' && upc(l[2]) == 'B') tcb = parse_hex(l + 3);
            else if (upc(l[0]) == 'E' && upc(l[1]) == 'V' && upc(l[2]) == 'E' && upc(l[3]) == 'N' && upc(l[4]) == 'T') ev = parse_hex(l + 5);
            else if (upc(l[0]) == 'S' && upc(l[1]) == 'T' && upc(l[2]) == 'A' && upc(l[3]) == 'C' && upc(l[4]) == 'K') stack = parse_hex(l + 5);
            *e = keep;
            l = *e ? e + 1 : e;
        }
    }
    if (find_path(boot, &lba, &size)) { fmt_str(fmt_str(err, "not found: "), boot); return -1; }
    sio_puts("OpenPsxRom: booting ");
    sio_puts(boot);
    sio_puts("\r\n");
    bios_a0(0x9C, ev, tcb, stack);              // SetConf: kernel events/threads as the game wants
    return exe_boot_from_cd(lba, size, stack, err);
}
