// State shared by the resident core (RAM, 8000C000h) and the code run from the cartridge
// (src/romx.c) while a game runs. Fixed address, inside the core's 8 KB area:
//   8000C000-8000CBFF  core code + data (3 KB max, core.ld)
//   8000CC00-8000D7FF  code finder candidates (384 x address + old value)
//   8000D800-8000DBFF  this block
//   8000DC00-8000DEFF  stack for the ROM code (menu, serial commands)
//   8000DF00-8000DFFF  EXE start trampoline (boot) / breakpoint stack (game)
#pragma once

#define SH_ADDR   0x8000D800u
#define SH_STACK  0x8000DEF0u
#define MAXC 96
#define CAND_ADDR 0x8000CC00u
#define CAND_MAX  384

enum { F_DEBUG = 1, F_TTY = 2, F_CRASH = 4, F_PAL = 8, F_NTSC = 16, F_CHEATS = 32, F_MENU = 64 };

typedef struct {
    unsigned int flags;
    unsigned int window;            // last 4 serial bytes
    unsigned int halted;
    unsigned int proto;             // NOPS protocol version of the current command
    unsigned int sw_last;           // Xplorer switch at the last interrupt
    unsigned int in_rom;            // ROM code running (no re-entry from the interrupt hook)
    unsigned int scan_fn;           // core: u32 scan(value, width, out, max) over the game RAM
    unsigned int cand_n;            // code finder: candidates kept
    unsigned int cand_width;        // 1 or 2 bytes
    unsigned int bp_entry;          // core's breakpoint handler (for the 80000040h vector)
    unsigned int baud_div;          // in-game serial divisor after BAUD/FAST/MEDI (0 or 18: 115200)
    unsigned int combo_last;        // pad combo held at the last check (rom_combo)
    unsigned int nc;                // cheat codes
    unsigned int c_addr[MAXC];
    unsigned short c_val[MAXC];
    unsigned char c_on[MAXC];
} Shared;

#define SH ((volatile Shared *)SH_ADDR)

// Where this build is linked: bank 1 (1F000000h), or bank 3 (1F040000h, make bank3: a second
// OpenPsxRom started by the switch OFF + "bank 3" boot option, for tests)
#ifdef BANK3
#define ROM_HDR 0x1F040000u
#else
#define ROM_HDR 0x1F000000u
#endif
// Entry points of the ROM code, pointers in the cartridge header (header.s)
#define ROM_COMBO   (*(unsigned int *)(ROM_HDR + 0xE8))  // void rom_combo(void): pad combo -> in-game menu
#define ROM_BREAK   (*(unsigned int *)(ROM_HDR + 0xEC))  // int  rom_break(u32 *tcb): break, 1 = resume
#define ROM_FEED    (*(unsigned int *)(ROM_HDR + 0xF0))   // int  rom_feed(void): handle SH->window
#define ROM_HALTED  (*(unsigned int *)(ROM_HDR + 0xF4))   // void rom_halted(void): breakpoint hit
#define ROM_CRASH   (*(unsigned int *)(ROM_HDR + 0xF8))   // void rom_crash(const u32 *tcb)
#define ROM_MENU    (*(unsigned int *)(ROM_HDR + 0xFC))   // void rom_menu(void): in-game menu
