// OpenROM - shared declarations
#pragma once
#include "hw.h"
#include "pad.h"

#define OPENROM_VERSION "0.6"

// colours (0xBBGGRR, 0x80 = neutral for text)
#define C_TEXT   0x808080
#define C_DIM    0x505050
#define C_TITLE  0x30A0FF
#define C_SEL    0x40FFFF
#define C_OK     0x40FF40
#define C_ERR    0x4040FF

// debug: characters on the EXP2 DUART (shown as TTY by emulators)
static inline void dbg(const char *s) {
    while (*s) *(volatile u8 *)0x1F802023 = (u8)*s++;
    *(volatile u8 *)0x1F802023 = '\n';
}

// pad button / arrow icons inside strings (drawn in colour): G_CROSS " open"
#define G_CROSS    "\x01"
#define G_CIRCLE   "\x02"
#define G_TRIANGLE "\x03"
#define G_SQUARE   "\x04"
#define G_LEFT     "\x05"
#define G_RIGHT    "\x06"
#define G_UP       "\x07"
#define G_DOWN     "\x08"

// gfx.c
extern int gfx_pal;
void gfx_init(void);
void gfx_rect(int x, int y, int w, int h, u32 rgb);
void gfx_shade(int x, int y, int w, int h, u32 rgb);
void gfx_grad(int x, int y, int w, int h, u32 top, u32 bottom);
int gfx_text(int x, int y, u32 rgb, const char *s);
int gfx_text2(int x, int y, u32 rgb, const char *s);
int gfx_text_w(const char *s);
void gfx_flip(void);
void gfx_shutdown(void);
void gfx_raw_display(int x, int y, u32 mode);
u32 gfx_shot(u16 *dst, int *x, int *y, int *w, int *h);   // VRAM rectangle -> RAM (w 0: screen)
void gfx_restore(void);
void gfx_icon_load(int slot, const u8 (*icon)[128], int frames, const u16 *clut);   // slot 0..79
void gfx_icon(int slot, int frame, int x, int y);

// sio.c (SIO1 serial, NOPS protocol)
void sio_init(u32 baud_div);
void sio_set_div(u32 baud_div);   // speed change without resetting the port
int sio_rx_ready(void);
u8 sio_getc(void);
void sio_putc(u8 c);
void sio_puts(const char *s);
void sio_write(const void *p, u32 n);
void sio_poll(void);            // called every frame from the menu: handles NOPS commands
extern char sio_status[48];     // last serial event, shown in the menu

// flash.c (AM29F010 and JEDEC-compatible 128 KB chips at 1F000000h)
#define FLASH_BASE 0x1F000000u
#define FLASH_SIZE 0x20000u
#define FLASH_SECT 0x4000u
int flash_identify(u8 *mfr, u8 *dev);      // 1 if a known chip answered
void flash_protect(u8 prot[8]);
int flash_write(const u8 *img, u32 size, void (*progress)(u32 sector, const char *what));
int flash_write_at(u32 off, const u8 *img, u32 size, void (*progress)(u32 sector, const char *what));
// flash layout: ROM 0-64 KB, compressed memory card backup 64-112 KB, cheat list 112-128 KB
#define FL_ROM_SIZE   0x10000u
#define FL_CARD_OFF   0x10000u
#define FL_CARD_SIZE  0xC000u
#define FL_CHEAT_OFF  0x1C000u
#define FL_CFG_OFF    0x1FF80u   // last 128 bytes of the cheat area: 'OCF1' + flags (read by preboot)
#define CFG_MAGIC     0x3146434Fu
// flags bits 0-7: what the Xplorer switch OFF starts at power-on: 0 bypass (normal boot),
// 2 the ROM in bank 2 (1F020000h, 1 = same: older setting), 3 the ROM in bank 3 (1F040000h)
#define CFG_BANK_MASK 0xFFu
int rom_bank_ok(int bank);       // a startable ROM is in that bank (header + entry inside it)
const char *flash_chip_name(u8 mfr, u8 dev);
u32 flash_chip_kb(u8 mfr, u8 dev);
int flash_identify_at(u32 off, u8 *mfr, u8 *dev);
#define ROM2_OFF      0x20000u   // bank 2: second firmware (Unirom "ROM2" on Xplorers)
#define ROM3_OFF      0x40000u   // bank 3: second chip of double-bank Xplorer FX
int rom_image_bank(const u8 *img);   // 1: image for 1F000000h, 2: for 1F020000h ("ROM2"), 3: for 1F040000h

// cd.c
typedef struct { char name[32]; u32 lba, size; u8 dir; } CdEntry;
int cd_init(void);
int cd_lid_open(void);
int cd_read(u32 lba, u32 n, u8 *dst);
int cd_unlock(int pal);
int iso_root(u32 *lba, u32 *size);
int iso_list(u32 lba, u32 size, CdEntry *e, int max);
int cda_tracks(int *first, int *last);
int cda_play(int track);
void cda_pause(void);
int cda_pos(int *track, int *mm, int *ss);
void cd_audio_on(void);
int cd_stream_start(u32 lba);
int cd_stream_get(u8 *dst);
void cd_stream_stop(void);
int str_play(u32 lba, u32 size, char *err);          // strplay.c

// mcard.c
// has_icon: number of icon frames (0 = none, 1..3; 2-3 = animated)
typedef struct { char name[21]; char title[33]; u8 blocks, first, has_icon; u32 size; u16 clut[16]; u8 icon[3][128]; } McEntry;
int mc_read_frame(int port, int f, u8 *buf);
int mc_write_frame(int port, int f, const u8 *buf);
int mc_put_frame(int port, int f, const u8 *buf);
int mc_read_card(int port, u8 *dst, void (*progress)(int));
int mc_write_card(int port, const u8 *src, void (*progress)(int));
int mc_list(int port, McEntry *e);
int mc_free_blocks(void);
void mc_parse_header(const u8 *hdr, const u8 *icons, McEntry *e);   // icons: the next 1..3 frames
int mc_read_file(int port, const McEntry *e, u8 *dst, u8 *dirf);
int mc_write_file(int port, const u8 *dirf, const u8 *src, int blocks);
int mc_delete_file(int port, const McEntry *e);

// store.c (saves kept in the cartridge flash)
int store_load(void);
int store_save(int (*writer)(u32 off, const u8 *data, u32 size));
int store_count(void);
u32 store_packed_kb(void);
void store_entry(int i, McEntry *e);
const u8 *store_dirf(int i);
const u8 *store_data(int i);
int store_add(const u8 *dirf, const u8 *data);
void store_delete(int i);

// exe.c
int exe_run_from_cd(u32 lba, u32 size, char *err);   // only returns on error
int exe_boot_from_cd(u32 lba, u32 size, u32 sp, char *err);
int cd_boot(char *err);                               // boot the disc (only returns on error)
int exe_overlaps_rom(u32 addr, u32 len);
void exe_start(const u8 *hdr) __attribute__((noreturn));

// main.c
void ui_progress(const char *what, u32 done, u32 total);

// util.c
void *memcpy(void *d, const void *s, u32 n);
void *memset(void *d, int c, u32 n);
int strlen(const char *s);
char *fmt_hex(char *p, u32 v, int digits);
char *fmt_dec(char *p, u32 v);
char *fmt_str(char *p, const char *s);
u32 crc32(const void *p, u32 n);

// crt.s
void bios_return(void) __attribute__((noreturn));
void exec_jump(u32 pc, u32 gp, u32 sp) __attribute__((noreturn));
u32 call_addr(u32 addr);
void flush_cache(void);
