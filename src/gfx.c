// Minimal polled GPU driver: 320x240 double buffer, 6x8 font + pad button icons (pGbX style),
// gradients, semi-transparent panels, 2x title text.
#include "rom.h"

static const u8 font8[][8] = {
#include "font.inc"
};
// Icons (control codes 1..8 in strings, see G_* in rom.h): pad buttons and arrows, 8x8.
static const u8 icons8[8][8] = {
    {0x82, 0x44, 0x28, 0x10, 0x28, 0x44, 0x82, 0x00},   // cross
    {0x38, 0x44, 0x82, 0x82, 0x82, 0x44, 0x38, 0x00},   // circle
    {0x10, 0x28, 0x28, 0x44, 0x44, 0x82, 0xFE, 0x00},   // triangle
    {0xFE, 0x82, 0x82, 0x82, 0x82, 0x82, 0xFE, 0x00},   // square
    {0x08, 0x18, 0x38, 0x78, 0x38, 0x18, 0x08, 0x00},   // left
    {0x40, 0x60, 0x70, 0x78, 0x70, 0x60, 0x40, 0x00},   // right
    {0x00, 0x10, 0x38, 0x7C, 0xFE, 0x00, 0x00, 0x00},   // up
    {0x00, 0xFE, 0x7C, 0x38, 0x10, 0x00, 0x00, 0x00},   // down
};
// button colours (modulation, 0xBBGGRR): cross blue, circle red, triangle green, square pink
static const u32 icon_col[8] = {0xF0A060, 0x5050F0, 0x70E050, 0xE090F0, 0, 0, 0, 0};

#define FONT_X 640              // font texture in VRAM (15-bit texels): 16 glyphs per row, 8x8 cells
#define NASCII 95
#define ICON0 96
#define NGLYPH 104
#define TPAGE (0x400 | (2 << 7) | (FONT_X / 64))    // 15-bit, draw to display, x base 640

static int buf;                 // buffer being drawn (0/1); the other one is displayed
int gfx_pal;

// wait for 'ready for a command word' before a command, parameters follow directly (FIFO)
// bounded: a GPU that never says "ready" must not freeze the menu (seen once after a flash)
static void gp0(u32 w) { for (u32 t = 0; t < 2000000 && !(GPUSTAT & (1u << 26)); t++); GP0 = w; }
#define gp0p(w) (GP0 = (w))

// PAL if the BIOS version string ends with 'E' ("System ROM Version 4.1 12/16/97 E").
static int bios_is_pal(void) {
    const char *p = (const char *)0xBFC7FF32;
    for (int i = 0; i < 40 && p[i]; i++)
        if (p[i] == 'E' && (p[i + 1] == 0 || p[i + 1] == ' ') && i > 10 && p[i - 1] == ' ') return 1;
    return 0;
}

// 128 x 56 texels, sent by DMA2 (block mode, 16 words per block)
static u32 font_tex[128 * 64 / 2];     // 7 rows of 16 glyphs, 9 lines apart (1 blank line)
static void upload_font(void) {
    u32 *w = font_tex;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 128; x += 2) {
            u32 v = 0;
            for (int k = 0; k < 2; k++) {
                int g = (y / 9) * 16 + ((x + k) >> 3), r = y % 9;
                const u8 *gl = r == 8 ? 0 : g < NASCII ? font8[g] : (g >= ICON0 && g < NGLYPH) ? icons8[g - ICON0] : 0;
                if (gl && (gl[r] & (0x80 >> ((x + k) & 7)))) v |= 0x7FFFu << (16 * k);
            }
            *w++ = v;
        }
    gp0(0x01000000);                                    // clear texture cache
    gp0(0xA0000000);                                    // CPU -> VRAM
    gp0p((0 << 16) | FONT_X);
    gp0p((64 << 16) | 128);
    GP1 = 0x04000002;                                   // DMA direction CPU -> GP0
    DMA_DPCR |= 0x800;
    DMA2_MADR = (u32)font_tex;
    DMA2_BCR = ((sizeof font_tex / 64) << 16) | 16;
    DMA2_CHCR = 0x01000201;
    while (DMA2_CHCR & 0x01000000);
    GP1 = 0x04000000;
}

static void set_draw_area(void) {
    int y0 = buf * 256;
    gp0(0xE3000000 | (y0 << 10));
    gp0(0xE4000000 | ((y0 + 239) << 10) | 319);
    gp0(0xE5000000 | (y0 << 11));
    gp0(0xE1000000 | TPAGE);                            // semi-transparency mode 0: 50/50
}

void gfx_init(void) {
    gfx_pal = bios_is_pal();
    GP1 = 0x00000000;
    GP1 = 0x03000001;
    GP1 = 0x06C60260;
    GP1 = gfx_pal ? 0x07046C2B : 0x07040010;
    GP1 = 0x08000001 | (gfx_pal ? 8 : 0);
    upload_font();
    buf = 0;
    set_draw_area();
    gfx_rect(0, 0, 320, 240, 0);
    gfx_flip();
    GP1 = 0x03000000;
}

// opaque filled rectangle (drawn as a primitive, so it follows the drawing offset)
void gfx_rect(int x, int y, int w, int h, u32 rgb) {
    gp0(0x60000000 | (rgb & 0xFFFFFF));
    gp0p((y << 16) | (x & 0xFFFF));
    gp0p((h << 16) | w);
}

// semi-transparent rectangle (50% blend)
void gfx_shade(int x, int y, int w, int h, u32 rgb) {
    gp0(0x62000000 | (rgb & 0xFFFFFF));
    gp0p((y << 16) | (x & 0xFFFF));
    gp0p((h << 16) | w);
}

// vertical gradient (Gouraud quad)
void gfx_grad(int x, int y, int w, int h, u32 top, u32 bottom) {
    gp0(0x38000000 | (top & 0xFFFFFF));
    gp0p((y << 16) | (x & 0xFFFF));
    gp0p(top & 0xFFFFFF);
    gp0p((y << 16) | ((x + w) & 0xFFFF));
    gp0p(bottom & 0xFFFFFF);
    gp0p(((y + h) << 16) | (x & 0xFFFF));
    gp0p(bottom & 0xFFFFFF);
    gp0p(((y + h) << 16) | ((x + w) & 0xFFFF));
}

static int glyph_of(u8 c) {
    if (c >= 1 && c <= 8) return ICON0 + c - 1;
    if (c >= 32 && c < 127) return c - 32;
    return -1;
}
static int adv(int g) { return g >= ICON0 && g < ICON0 + 4 ? 8 : 6; }

int gfx_text_w(const char *s) {
    int w = 0;
    for (; *s; s++) { int g = glyph_of((u8)*s); w += g < 0 ? 0 : adv(g); }
    return w;
}

static int glyphs(int x, int y, u32 rgb, const char *s, int shadow) {
    for (; *s; s++) {
        int g = glyph_of((u8)*s);
        if (g < 0) continue;
        if (g > 0) {
            u32 c = shadow ? 0x010101 : (g >= ICON0 && icon_col[g - ICON0] ? icon_col[g - ICON0] : rgb);
            gp0(0x64000000 | (c & 0xFFFFFF));
            gp0p((y << 16) | (x & 0xFFFF));
            gp0p((((g >> 4) * 9) << 8) | ((g & 15) * 8));
            gp0p((8 << 16) | 8);
        }
        x += adv(g);
    }
    return x;
}

int gfx_text(int x, int y, u32 rgb, const char *s) {
    glyphs(x + 1, y + 1, 0, s, 1);
    return glyphs(x, y, rgb, s, 0);
}

// 2x text for titles: every font pixel drawn as a 2x2 block (crisp, no texture scaling), with a
// 1-pixel shadow and a vertical colour fade (rgb at the top, rgb2 at the bottom).
static void text2_pass(int x, int y, const char *s, u32 rgb, u32 rgb2, int shadow) {
    for (; *s; s++) {
        int g = glyph_of((u8)*s);
        if (g < 0) continue;
        const u8 *gl = g < NASCII ? font8[g] : 0;
        if (gl)
            for (int r = 0; r < 8; r++) {
                u32 c = shadow ? 0x000000 : r < 4 ? rgb : rgb2;
                for (int b = 0; b < 8; b++)
                    if (gl[r] & (0x80 >> b)) gfx_rect(x + b * 2, y + r * 2, 2, 2, c);
            }
        x += adv(g) * 2;
    }
}
int gfx_text2(int x, int y, u32 rgb, const char *s) {
    u32 rgb2 = ((rgb >> 1) & 0x7F7F7F) + ((rgb >> 2) & 0x3F3F3F);    // 3/4 brightness
    text2_pass(x + 1, y + 1, s, 0, 0, 1);
    text2_pass(x, y, s, rgb, rgb2, 0);
    return x + gfx_text_w(s) * 2;
}

static void wait_vblank(void) {
    I_STAT = ~1u;
    for (u32 t = 0; t < 4000000 && !(I_STAT & 1); t++);
    I_STAT = ~1u;
}

void gfx_flip(void) {
    for (u32 t = 0; t < 2000000 && !(GPUSTAT & (1u << 26)); t++);
    wait_vblank();
    GP1 = 0x05000000 | ((buf * 256) << 10);
    buf ^= 1;
    set_draw_area();
}

// Screenshot: copy a VRAM rectangle (15-bit pixels) to RAM through the GPU (GP0 C0h).
// w == 0: the picture currently displayed by the menu. Returns the pixel count, fills x/y/w/h.
u32 gfx_shot(u16 *dst, int *x, int *y, int *w, int *h) {
    if (*w <= 0 || *h <= 0) { *x = 0; *y = (buf ^ 1) * 256; *w = 320; *h = 240; }
    if (*w > 1024) *w = 1024;
    if (*h > 512) *h = 512;
    u32 n = (u32)*w * (u32)*h;
    gp0(0x01000000);                                   // flush the texture cache
    gp0(0xC0000000);
    gp0p(((u32)*y << 16) | ((u32)*x & 0xFFFF));
    gp0p(((u32)*h << 16) | ((u32)*w & 0xFFFF));
    while (!(GPUSTAT & (1u << 27)));                   // ready to send VRAM to the CPU
    u32 *d = (u32 *)dst;
    for (u32 i = 0; i < (n + 1) / 2; i++) d[i] = GPUREAD;
    return n;
}

// raw VRAM display (VRAM viewer): mode = GP1(08h) bits, display start x/y
void gfx_raw_display(int x, int y, u32 mode) {
    wait_vblank();
    GP1 = 0x08000000 | mode | (gfx_pal ? 8 : 0);
    GP1 = 0x05000000 | ((y & 511) << 10) | (x & 1023);
}
void gfx_restore(void) {
    GP1 = 0x08000001 | (gfx_pal ? 8 : 0);
    GP1 = 0x06C60260;
    GP1 = gfx_pal ? 0x07046C2B : 0x07040010;
    set_draw_area();
}

// ---- memory card save icons: 16x16 4-bit images at VRAM 896,256 (16 per row; frame f of slot s
// is image s + 80 f), one CLUT per slot at 768,496
static void upload(int x, int y, int w, int h, const void *data) {
    u32 words = (u32)(w * h) / 2, bs = words % 16 == 0 ? 16 : words % 8 == 0 ? 8 : 1;
    gp0(0x01000000);
    gp0(0xA0000000);
    gp0p((y << 16) | x);
    gp0p((h << 16) | w);
    GP1 = 0x04000002;
    DMA2_MADR = (u32)data;
    DMA2_BCR = ((words / bs) << 16) | bs;
    DMA2_CHCR = 0x01000201;
    while (DMA2_CHCR & 0x01000000);
    GP1 = 0x04000000;
}
void gfx_icon_load(int slot, const u8 (*icon)[128], int frames, const u16 *clut) {
    static u32 ib[32], cb[8];                   // DMA needs word-aligned buffers
    for (int f = 0; f < frames; f++) {
        int s = slot + f * 80;
        memcpy(ib, icon[f], 128);
        upload(896 + (s & 15) * 4, 256 + (s >> 4) * 16, 4, 16, ib);
    }
    memcpy(cb, clut, 32);
    upload(768 + (slot & 15) * 16, 496 + (slot >> 4), 16, 1, cb);
}
void gfx_icon(int slot, int frame, int x, int y) {
    u32 clut = ((496 + (slot >> 4)) << 6) | ((768 + (slot & 15) * 16) >> 4);
    slot += frame * 80;
    gp0(0xE1000000 | 0x400 | (0 << 7) | 0x10 | (896 / 64));   // 4-bit texpage at 896,256
    gp0(0x64808080);
    gp0p((y << 16) | (x & 0xFFFF));
    gp0p((clut << 16) | (((slot >> 4) * 16) << 8) | ((slot & 15) * 16));
    gp0p((16 << 16) | 16);
    gp0(0xE1000000 | TPAGE);
}

void gfx_shutdown(void) {
    GP1 = 0x03000001;
}
