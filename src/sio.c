// SIO1 serial driver (polled) + NOPS protocol (filled in protocol.c)
#include "rom.h"

#define SIO_DATA REG8(0x1F801050)
#define SIO_STAT REG32(0x1F801054)
#define SIO_MODE REG16(0x1F801058)
#define SIO_CTRL REG16(0x1F80105A)
#define SIO_BAUD REG16(0x1F80105E)

char sio_status[48];

void sio_init(u32 div) {
    SIO_CTRL = 0x40;            // reset
    SIO_MODE = 0xCE;            // 8 bits, no parity, 2 stop bits (as Unirom: slack for /fast), x16
    SIO_BAUD = div;             // 0x12: 33868800 / 16 / 18 = 115200
    SIO_CTRL = 0x05;            // TX + RX enable (as Unirom)
    for (int i = 0; i < 16 && (SIO_STAT & 2); i++) (void)SIO_DATA;
}
int sio_rx_ready(void) { return (SIO_STAT & 2) != 0; }
u8 sio_getc(void) { while (!(SIO_STAT & 2)); return SIO_DATA; }
void sio_putc(u8 c) { while (!(SIO_STAT & 1)); SIO_DATA = c; }
void sio_puts(const char *s) { while (*s) sio_putc((u8)*s++); }
void sio_write(const void *p, u32 n) { const u8 *b = p; while (n--) sio_putc(*b++); }
