// PS-EXE loading and start (from the CD, and the start routine shared with the serial /exe).
#include "rom.h"

extern u8 __payload_ram[], __bss_end[];

int exe_overlaps_rom(u32 addr, u32 len) {
    return addr < (u32)__bss_end && addr + len > (u32)__payload_ram;
}

// Standard start-up from the 2048-byte header: BSS cleared, gp, stack, jump (never returns).
static u32 sp_override;
void exe_start(const u8 *hdr) {
    const u32 *h = (const u32 *)hdr;
    u32 pc = h[0x10 / 4], gp = h[0x14 / 4], b_addr = h[0x28 / 4], b_size = h[0x2C / 4];
    u32 s_addr = h[0x30 / 4], s_size = h[0x34 / 4];
    u32 sp = s_addr ? s_addr + s_size : sp_override ? sp_override : 0x801FFF00;
    sio_puts("OpenPsxRom: starting the exe\r\n");
    gfx_shutdown();
    // the BSS may cover OpenROM: clear it and jump from a trampoline outside our RAM
    extern u8 exe_tramp[], exe_tramp_end[];
    u8 *t = (u8 *)0x8000DF00;
    memcpy(t, exe_tramp, exe_tramp_end - exe_tramp);
    *(u32 *)0x8000DFF0 = gp;
    *(u32 *)0x8000DFF4 = sp;
    flush_cache();
    ((void (*)(u32, u32, u32, u32 *))t)(b_addr, b_size, pc, (u32 *)0x8000DFF0);
    for (;;);
}

static u8 hdr[2048];

int exe_run_from_cd(u32 lba, u32 size, char *err) { return exe_boot_from_cd(lba, size, 0, err); }

int exe_boot_from_cd(u32 lba, u32 size, u32 sp, char *err) {
    sp_override = sp;
    if (cd_read(lba, 1, hdr)) { fmt_str(err, "CD read error"); return -1; }
    static const char magic[] = "PS-X EXE";
    for (int i = 0; i < 8; i++)
        if (hdr[i] != (u8)magic[i]) { fmt_str(err, "not a PS-X EXE"); return -1; }
    const u32 *h = (const u32 *)hdr;
    u32 addr = h[0x18 / 4], len = h[0x1C / 4];
    if (len > size) len = size - 2048;
    len = (len + 2047) & ~2047u;
    if (exe_overlaps_rom(addr, len)) { fmt_str(err, "exe overlaps OpenPsxRom (801A0000-)"); return -1; }
    if (cd_read(lba + 1, len / 2048, (u8 *)addr)) { fmt_str(err, "CD read error"); return -1; }
    extern void core_boot(void);
    core_boot();
    exe_start(hdr);
    return 0;
}
