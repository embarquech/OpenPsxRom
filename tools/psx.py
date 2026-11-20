#!/usr/bin/env python3
"""psx.py: OpenPsxRom's own serial client (OPX1 extended link). nops keeps working as before.

  python tools/psx.py [-p COMx] [--fast] COMMAND ...
    info                       what the console reports (version, flash chip, banks...)
    ping                       is OpenPsxRom there?
    flash FILE [--full]        write a 128 KB ROM (bank from its entry point, as nops /rom): only
                               the 1 KB pieces that differ from the cartridge are sent (--full: all)
    exe FILE                   load a PS-X EXE and start it
    load FILE ADDR             load a file to RAM
    dump ADDR LEN FILE         read memory (RAM, flash 1F000000h, scratchpad...) to a file
    peek ADDR [LEN]            hex dump of memory
    reset                      restart the console (BIOS reset vector)
The port defaults to the one in tools/comport.txt. Needs pyserial (pip install pyserial).

Link (src/protocol.c, "OPX1 extended link"): the PC sends the token OPX1 at 115200 baud, the
console answers OPXA, then every request is a frame
    A5h, cmd, u16 len, payload, u32 CRC32 (zlib) of cmd..payload      (little endian)
answered by one frame with the same cmd and payload = status byte + data. A damaged frame either
way is sent again. The link stays at 115200 baud; --fast raises it (divisor 4, PC at 510000
baud, as nops /fast), which needs an adapter able to follow the console's real rates. Data is
sent LZ-compressed (OMC1, tools/omc.py) when that is smaller, and the console draws the progress bar the PC asks for. Exit code 0 only when the command really worked."""
import os, struct, sys, time, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import omc

try:
    import serial
except ImportError:
    sys.exit('psx.py needs pyserial: pip install pyserial')

PING, INFO, BAUD, READ, WRITE, LZWR, CRCS, COPY, FLASH, EXEC, RESET, PROG, END = range(13)
NAMES = ['ok', 'damaged frame', 'bad arguments', 'refused', 'failed', 'unknown command']
FLASH_BASE, BANK = 0x1F000000, 0x20000
# The link has bit errors (more with many 0/1 transitions, the console's 117600 baud against the
# PC's 115200 does not help): pieces of 1 KB, each sent again until its CRC is right.
PIECE = 1024
PC_BAUD = {4: 510000, 5: 414720, 18: 115200}            # as nops: /fast is 510000 on the PC side
DEBUG = os.environ.get('PSX_DEBUG')


class LinkError(Exception):
    pass


class Link:
    def __init__(self, port, fast=False):
        self.port = port
        self.s = serial.Serial(port, 115200, timeout=0.5, stopbits=serial.STOPBITS_ONE)
        self.open(fast)

    def open(self, fast):
        for attempt in range(4):
            self.s.baudrate = 115200
            self.s.reset_input_buffer()
            self.s.write(b'OPX1')
            got, t = b'', time.time()
            while b'OPXA' not in got and time.time() - t < 0.6:
                got += self.s.read(max(1, self.s.in_waiting))
            if b'OPXA' in got:
                break
            time.sleep(0.3 if attempt < 2 else 1.5)    # a link left at another speed times out (~1 s)
        else:
            raise LinkError('no answer to OPX1 on %s: OpenPsxRom 0.7+ in its menu? (nops still works '
                            'with older versions)' % self.port)
        self.info = dict(l.split('=', 1) for l in self.req(INFO)[1].decode().split('\n') if '=' in l)
        if fast:
            self.speed(4)

    def speed(self, div):
        self.req(BAUD, struct.pack('<I', div))
        self.s.flush()
        time.sleep(0.01)
        self.s.baudrate = PC_BAUD.get(div, 2073600 // div)
        try:
            self.req(PING, tries=3)
        except LinkError:                                  # this adapter cannot: back to 115200
            self.s.baudrate = 115200
            time.sleep(1.5)
            self.open(False)

    def frame(self, cmd, payload):
        body = struct.pack('<BH', cmd, len(payload)) + payload
        return b'\xa5' + body + struct.pack('<I', zlib.crc32(body))

    def answer(self, timeout):
        self.s.timeout = timeout
        while True:
            c = self.s.read(1)
            if not c:
                return None
            if c == b'\xa5':
                break
        self.s.timeout = 1
        h = self.s.read(3)
        if len(h) < 3:
            return None
        cmd, n = struct.unpack('<BH', h)
        rest = self.s.read(n + 4)
        if len(rest) < n + 4 or zlib.crc32(h + rest[:n]) != struct.unpack('<I', rest[n:])[0]:
            return None
        return cmd, rest[0], rest[1:n]

    def req(self, cmd, payload=b'', timeout=3, tries=10):
        for _ in range(tries):
            self.s.write(self.frame(cmd, payload))
            a = self.answer(timeout)
            if DEBUG and (a is None or a[0] == 0xFF):
                print('debug: cmd %d answer %r, pending %r' % (cmd, a, self.s.read(self.s.in_waiting)))
            if a is None or a[0] == 0xFF:                  # lost or damaged: send it again
                time.sleep(0.05)
                self.s.reset_input_buffer()
                continue
            if a[0] != cmd:
                continue
            return a[1], a[2]
        raise LinkError('no valid answer to command %d' % cmd)

    def ok(self, cmd, payload=b'', what='', **kw):
        st, data = self.req(cmd, payload, **kw)
        if st:
            raise LinkError('%s: %s %s' % (what or 'command %d' % cmd, NAMES[st] if st < 6 else st,
                                           data.decode(errors='replace')))
        return data

    def close(self):
        try:
            self.req(END, tries=3)
        except LinkError:
            pass
        self.s.close()

    # ---- helpers
    def read(self, addr, n, show=None):
        out = bytearray()
        while len(out) < n:
            k = min(PIECE, n - len(out))
            out += self.ok(READ, struct.pack('<II', addr + len(out), k), 'read')
            if show:
                show(len(out), n)
        return bytes(out)

    def crcs(self, addr, step, count):
        return struct.unpack('<%dI' % count, self.ok(CRCS, struct.pack('<III', addr, step, count), 'crc'))

    def put(self, addr, data):
        """One piece: compressed when that is smaller."""
        z = omc.pack(data)
        if len(z) < len(data):
            self.ok(LZWR, struct.pack('<I', addr) + z, 'write (lz) %08X' % addr)
        else:
            self.ok(WRITE, struct.pack('<I', addr) + data, 'write %08X' % addr)
        return min(len(z), len(data))

    def prog(self, done, total, title, *lines):
        txt = b'\0'.join(s.encode()[:43] for s in (title,) + lines)
        self.ok(PROG, struct.pack('<II', done, total) + txt, 'progress')


def bar(done, total, extra=''):
    w = 30
    f = w * done // max(total, 1)
    sys.stdout.write('\r  [%s%s] %3d%% %s' % ('#' * f, '.' * (w - f), 100 * done // max(total, 1), extra))
    sys.stdout.flush()


def upload(link, addr, data, title):
    """Send data to RAM at addr in pieces (PIECE), progress on both sides. Returns bytes on the wire."""
    sent, t0 = 0, time.time()
    for i in range(0, len(data), PIECE):
        sent += link.put(addr + i, data[i:i + PIECE])
        done = min(i + PIECE, len(data))
        bar(done, len(data))
        if (i // PIECE) % 16 == 15 or done == len(data):
            rate = done / max(time.time() - t0, 1e-3) / 1024
            link.prog(done, len(data), title, '%d KB / %d KB' % (done >> 10, (len(data) + 1023) >> 10),
                      '%.1f KB/s (%d KB sent, compressed)' % (rate, sent >> 10), 'to %08X' % addr)
    print()
    return sent


def image_bank(img):
    entry = struct.unpack_from('<I', img, 0x80)[0]
    if 0x1F020000 <= entry < 0x1F040000:
        return 2
    if 0x1F040000 <= entry < 0x1F060000:
        return 3
    return 1


def cmd_flash(link, path, full):
    img = open(path, 'rb').read()
    if len(img) > BANK:
        raise LinkError('%s: bigger than 128 KB' % path)
    img = img + b'\xff' * (BANK - len(img))
    if img[0x84:0x94] != b'Licensed by Sony':
        raise LinkError('%s: no "Licensed by Sony" at 84h, the BIOS would not start it' % path)
    bank = image_bank(img)
    if bank > 1 and link.info.get('bank%d' % bank) != '1':
        raise LinkError('image for bank %d: no such bank on this cartridge' % bank)
    off = (bank - 1) * BANK
    openrom = bank == 1 and any(img[0x30:0x30 + len(t)] == t
                                for t in (b'OpenPsxRom - open source', b'OpenROM - open source'))
    size = int(link.info.get('openrom_size', '10000'), 16) if openrom else BANK
    rombuf = int(link.info['rombuf'], 16)
    print('%s -> bank %d (%08X), %d KB written%s' % (path, bank, FLASH_BASE + off, size >> 10,
          ', saves after it kept' if size < BANK else ''))
    n = size // PIECE
    have = link.crcs(FLASH_BASE + off, PIECE, n)
    diff = [k for k in range(n) if full or have[k] != zlib.crc32(img[k * PIECE:(k + 1) * PIECE])]
    if not diff:
        print('already on the cartridge: nothing to write')
        return
    print('%d of %d pieces of 1 KB differ' % (len(diff), n))
    link.ok(COPY, struct.pack('<III', rombuf, FLASH_BASE + off, BANK), 'copy of the bank')
    sent, t0 = 0, time.time()
    for j, k in enumerate(diff):
        sent += link.put(rombuf + k * PIECE, img[k * PIECE:(k + 1) * PIECE])
        bar(j + 1, len(diff))
        if j % 16 == 15 or j + 1 == len(diff):
            link.prog(j + 1, len(diff), 'receiving the ROM', '%d / %d pieces of 1 KB changed' % (j + 1, len(diff)),
                      '%d KB sent (compressed)' % (sent >> 10), 'for bank %d' % bank)
    print()
    if link.crcs(rombuf, size, 1)[0] != zlib.crc32(img[:size]):
        raise LinkError('the image in the console RAM differs: nothing written')
    whole = link.crcs(rombuf, BANK, 1)[0]
    print('sent %d KB in %.1f s, flashing...' % (sent >> 10, time.time() - t0))
    # A damaged FLASH request is answered FFh and not run: send it again. Once it runs, the session
    # ends on the flash screen: if its answer is lost, a new session tells what is in the flash.
    req = link.frame(FLASH, struct.pack('<I', whole))
    for _ in range(10):
        link.s.reset_input_buffer()
        link.s.write(req)
        a = link.answer(120)
        if a is None or a[0] != 0xFF:
            break
        time.sleep(0.05)
    if a is None or a[0] != FLASH:
        link.s.close()
        time.sleep(0.5)
        check = Link(link.port)
        now = check.crcs(FLASH_BASE + off, PIECE, n)
        check.close()
        bad = sum(now[k] != zlib.crc32(img[k * PIECE:(k + 1) * PIECE]) for k in range(n))
        if bad:
            raise LinkError('no answer to FLASH, and %d piece(s) of 1 KB still differ' % bad)
        print('FLASH OK  (bank %d, answer lost, flash read back identical)' % bank)
        return
    st, data = a[1], a[2]
    if st == 0:
        print('FLASH OK  (bank %d, CRC32 %08X)' % (bank, whole))
    elif len(data) == 8 and st == 4:
        raise LinkError('FLASH FAILED: %d block(s)' % struct.unpack('<I', data[:4]))
    else:
        raise LinkError('flash %s: %s' % (NAMES[st] if st < 6 else st, data.decode(errors='replace')))


def cmd_exe(link, path):
    exe = open(path, 'rb').read()
    if exe[:8] != b'PS-X EXE':
        raise LinkError('%s: not a PS-X EXE' % path)
    pc, gp, addr, size = struct.unpack_from('<IIII', exe, 0x10)
    data = exe[2048:2048 + size]
    data += b'\0' * (size - len(data))
    print('%s: %d KB at %08X, entry %08X' % (path, size >> 10, addr, pc))
    upload(link, addr, data, 'loading the program')
    link.ok(EXEC, exe[:2048], 'start')
    print('started')


def num(s):
    return int(s, 0)


def main():
    a = sys.argv[1:]
    port, fast = None, False
    while a and a[0].startswith('-'):
        o = a.pop(0)
        if o in ('-p', '--port'):
            port = a.pop(0)
        elif o == '--fast':
            fast = True
        else:
            sys.exit(__doc__)
    if not a:
        sys.exit(__doc__)
    if not port:
        f = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'comport.txt')
        port = open(f).read().strip() if os.path.exists(f) else 'COM1'
    cmd, args = a[0], a[1:]
    t0 = time.time()
    try:
        link = Link(port, fast and cmd not in ('ping', 'info', 'reset', 'peek'))
        try:
            if cmd == 'info':
                for k, v in link.info.items():
                    print('%-13s %s' % (k, v))
            elif cmd == 'ping':
                link.ok(PING)
                print('OpenPsxRom %s on %s' % (link.info.get('version'), port))
            elif cmd == 'flash':
                cmd_flash(link, args[0], '--full' in args)
                link = None                                # the console shows the result screen
            elif cmd == 'exe':
                cmd_exe(link, args[0])
                link = None
            elif cmd == 'load':
                data = open(args[0], 'rb').read()
                upload(link, num(args[1]), data, 'loading the file')
            elif cmd == 'dump':
                addr, n = num(args[0]), num(args[1])
                data = link.read(addr, n, bar)
                print()
                open(args[2], 'wb').write(data)
                print('%s: %d bytes from %08X, CRC32 %08X' % (args[2], n, addr, zlib.crc32(data)))
            elif cmd == 'peek':
                addr = num(args[0])
                data = link.read(addr, num(args[1]) if len(args) > 1 else 64)
                for i in range(0, len(data), 16):
                    print('%08X  %s' % (addr + i, ' '.join('%02X' % b for b in data[i:i + 16])))
            elif cmd == 'reset':
                link.ok(RESET)
                link = None
                print('reset')
            else:
                sys.exit(__doc__)
        finally:
            if link:
                link.close()
    except (LinkError, serial.SerialException, OSError) as e:
        print('\nerror:', e)
        sys.exit(1)
    if cmd in ('flash', 'exe', 'load', 'dump'):
        print('%.1f s' % (time.time() - t0))


if __name__ == '__main__':
    main()
