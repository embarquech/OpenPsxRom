#!/usr/bin/env python3
"""Level 3 tests: a real PlayStation running OpenPsxRom (menu), over the serial port.

  python tests/hw_test.py --port COM3 --nops path/to/nops.exe [--no-fast] [--card]
                          [--flash build/openrom.rom] [--mcup card.mcr]

Default: read-only for the cartridge and the memory card; only RAM (80100000h, free while the
menu runs) is written. Destructive tests only with their option:
  --flash FILE   writes the cartridge (nops /rom), then reads it back and compares
  --mcup FILE    ERASES the memory card in slot 1 with FILE (128 KB), then reads it back
Needs pyserial; the SHOT test needs Pillow. The console must be in the OpenPsxRom menu.
"""
import argparse, os, random, struct, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
RAM = 0x80100000
results = []


def report(name, ok, detail=''):
    results.append((name, ok))
    print('%s  %s%s' % ('PASS' if ok else 'FAIL', name, ('  (' + detail + ')') if detail else ''), flush=True)


class Console:
    def __init__(self, port, nops):
        self.port, self.nops = port, nops

    def raw(self, send, wait=1.2, nbytes=64):
        """Send raw bytes on the line, return what comes back within `wait` seconds."""
        import serial
        s = serial.Serial(self.port, 115200, timeout=0.1)
        s.rts = True
        s.dtr = True
        s.reset_input_buffer()
        s.write(send)
        got, t0 = b'', time.time()
        while time.time() - t0 < wait and len(got) < nbytes:
            got += s.read(nbytes)
        s.close()
        return got

    def run(self, *args, timeout=120):
        cmd = [self.nops] + [str(a) for a in args] + ['/dest', self.port]
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
            return p.stdout + p.stderr
        except subprocess.TimeoutExpired:
            return 'TIMEOUT'

    def dump(self, addr, size, extra=(), timeout=180):
        f = os.path.join(tempfile.gettempdir(), 'hwtest_dump.bin')
        if os.path.exists(f):
            os.remove(f)
        self.run(*extra, '/dump', '0x%08X' % addr, '0x%X' % size, f, timeout=timeout)
        if not os.path.exists(f):
            return None
        data = open(f, 'rb').read()
        return data if len(data) == size else None

    def upload(self, addr, data, extra=()):
        f = os.path.join(tempfile.gettempdir(), 'hwtest_up.bin')
        open(f, 'wb').write(data)
        return self.run(*extra, '/bin', f, '0x%08X' % addr)     # nops v18: file THEN address


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='COM3')
    ap.add_argument('--nops', default='nops', help='nops v18 executable')
    ap.add_argument('--no-fast', action='store_true', help='skip the 230400 baud test')
    ap.add_argument('--card', action='store_true', help='also read the memory card in slot 1')
    ap.add_argument('--flash', metavar='ROM', help='DESTRUCTIVE: flash this image and verify it')
    ap.add_argument('--mcup', metavar='MCR', help='DESTRUCTIVE: write this card image to slot 1 and verify')
    a = ap.parse_args()
    c = Console(a.port, a.nops)
    rnd = random.Random(1234)

    # serial basics
    got = c.raw(b'PING')
    report('raw PING answered PONG (no echo)', got.endswith(b'PONG') and not got.startswith(b'PING'), repr(got[-8:]))
    report('nops /ping', 'Response: True' in c.run('/ping', timeout=15))
    got = c.raw(b'HALT')
    report('debug-only command refused with ONLY in the menu', b'ONLY' in got, repr(got[-8:]))

    # cartridge header (read-only)
    h = c.dump(0x1F000000, 0x100)
    name = h[0x30:0x40].split(b' ')[0].decode('ascii', 'replace') if h else '-'
    ok = h is not None and h[0x84:0x84 + 16] == b'Licensed by Sony' and name in ('OpenPsxRom', 'OpenROM')
    report('cartridge header readable (Sony license + firmware name)', ok, 'flashed: ' + name)

    # RAM write / read back at 115200
    blob = bytes(rnd.getrandbits(8) for _ in range(0x10000))
    c.upload(RAM, blob)
    report('64 KB /bin then /dump at 115200', c.dump(RAM, len(blob)) == blob)

    # poke: 32-bit write through SBIN
    c.run('/poke32', '0x%08X' % (RAM + 0x100), '0x12345678')
    d = c.dump(RAM + 0x100, 4)
    report('/poke32', d == struct.pack('<I', 0x12345678), d.hex() if d else 'no data')

    # faster link for one command, then automatic return to 115200
    if not a.no_fast:
        blob2 = bytes(rnd.getrandbits(8) for _ in range(0x8000))
        c.upload(RAM, blob2)
        # /psxbaud switches the PS1, /pcbaud the PC: both are needed
        back = c.dump(RAM, len(blob2), extra=('/psxbaud', '230400', '/pcbaud', '230400'))
        report('/psxbaud 230400 dump (32 KB)', back == blob2)
        time.sleep(0.5)
        report('back to 115200 after the fast command', 'Response: True' in c.run('/ping', timeout=15))

    # screenshot of the menu
    try:
        import psxshot
        addr, w, hgt = psxshot.ask_shot(a.port, (0, 0, 0, 0))
        pix = c.dump(addr, w * hgt * 2)
        varied = pix is not None and len(set(pix[i:i + 2] for i in range(0, len(pix), 2))) > 4
        report('SHOT: %dx%d screen picture, not blank' % (w, hgt), (w, hgt) == (320, 240) and varied)
    except SystemExit as e:
        report('SHOT command', False, str(e))

    # memory card (read-only unless --mcup)
    if a.card or a.mcup:
        f = os.path.join(tempfile.gettempdir(), 'hwtest_card.mcr')
        if os.path.exists(f):
            os.remove(f)
        c.run('/mcdown', '0', f, timeout=200)
        card = open(f, 'rb').read() if os.path.exists(f) else b''
        report('/mcdown slot 1 (128 KB)', len(card) == 131072)
    if a.mcup:
        img = open(a.mcup, 'rb').read()
        c.run('/mcup', '0', a.mcup, timeout=300)
        time.sleep(90)                                  # the console writes and verifies 1024 frames
        f = os.path.join(tempfile.gettempdir(), 'hwtest_card2.mcr')
        if os.path.exists(f):
            os.remove(f)
        c.run('/mcdown', '0', f, timeout=200)
        back = open(f, 'rb').read() if os.path.exists(f) else b''
        report('/mcup then /mcdown: card identical', back == img)

    # flash (only with --flash)
    if a.flash:
        img = open(a.flash, 'rb').read()
        out = c.run('/rom', a.flash, timeout=300)
        time.sleep(20)                                  # programming + verification on the console
        back = c.dump(0x1F000000, 0x10000)
        report('/rom then dump: first 64 KB identical', back == img[:0x10000], out.strip().splitlines()[-1] if out.strip() else '')

    bad = [n for n, ok in results if not ok]
    print('\n%d passed, %d failed' % (len(results) - len(bad), len(bad)))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
