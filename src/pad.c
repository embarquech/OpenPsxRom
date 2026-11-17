// Direct SIO0 controller polling (no BIOS).
#include "hw.h"
#include "pad.h"

static void delay(int n) { for (volatile int i = 0; i < n; i++); }

void pad_init(void) {
    SIO0_CTRL = 0x40;          // reset
    SIO0_BAUD = 0x88;          // ~250 kHz
    SIO0_MODE = 0x0D;          // 8 bit, MUL1
    SIO0_CTRL = 0;
}

static int xfer(u8 out, u8 *in, int want_ack) {
    I_STAT = ~0x80u;
    SIO0_CTRL |= 0x10;         // ack
    while (!(SIO0_STAT & 1));
    SIO0_DATA = out;
    int t = 0;
    while (!(SIO0_STAT & 2)) if (++t > 5000) return 0;
    *in = SIO0_DATA;
    if (!want_ack) return 1;
    t = 0;
    while (!(I_STAT & 0x80)) if (++t > 3000) return 0;
    return 1;
}

u16 pad_read(void) {
    u8 b[5] = {0, 0, 0xFF, 0xFF, 0xFF};
    for (int i = 0; i < 16 && (SIO0_STAT & 2); i++) (void)SIO0_DATA;   // stale bytes (memory card)
    SIO0_CTRL = 0x1003;        // TX enable, select port 1, ack irq enable
    delay(300);
    int ok = xfer(0x01, &b[0], 1) && xfer(0x42, &b[1], 1) && xfer(0x00, &b[0], 1) &&
             xfer(0x00, &b[2], 1) && xfer(0x00, &b[3], 0);
    SIO0_CTRL = 0;
    I_STAT = ~0x80u;
    if (!ok) return 0;
    return (u16)~(b[2] | (b[3] << 8));
}

// Full poll of a port (0/1) for the controller test: returns the number of bytes received after
// the header (0 = nothing connected); out[0] = ID byte (high nibble type, low nibble = half-words
// of data), out[1..] = data (buttons, then analog sticks / mouse movement).
int pad_read_full(int port, u8 *out, int max) {
    for (int i = 0; i < 16 && (SIO0_STAT & 2); i++) (void)SIO0_DATA;
    SIO0_CTRL = 0x1003 | ((port & 1) << 13);
    delay(300);
    u8 id = 0xFF, z;
    int n = 0;
    if (xfer(0x01, &z, 1) && xfer(0x42, &id, 1) && xfer(0x00, &z, 1) && id != 0xFF) {
        int words = id & 15 ? id & 15 : 16;
        out[0] = id;
        for (int i = 0; i < words * 2 && i + 1 < max; i++) {
            if (!xfer(0x00, &out[1 + i], i < words * 2 - 1)) break;
            n = i + 1;
        }
    }
    SIO0_CTRL = 0;
    I_STAT = ~0x80u;
    for (int i = 0; i < 16 && (SIO0_STAT & 2); i++) (void)SIO0_DATA;
    return n;
}
