#!/usr/bin/env python3
"""Build the OpenROM boot disc (BIN/CUE, ISO9660, Mode 2 Form 1): SYSTEM.CNF, OPENROM.EXE (the menu,
which can install itself on the cartridge) and OPENROM.ROM (the cartridge image).
  python tools/mkdisc.py --exe build/openrom.exe --rom build/openrom.rom --out release/openrom
Sector/ISO code from pGbX tools/mkiso.py.
"""
import argparse, os, re, struct, sys

SECTOR = 2048

# ---------------------------------------------------------------- EDC / ECC (CD-ROM Mode 2 Form 1)
EDC_LUT = []
for i in range(256):
    e = i
    for _ in range(8):
        e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
    EDC_LUT.append(e)
ECC_F = [0] * 256
ECC_B = [0] * 256
for i in range(256):
    j = ((i << 1) ^ (0x11D if i & 0x80 else 0)) & 0xFF
    ECC_F[i] = j
    ECC_B[i ^ j] = i

def edc(data):
    e = 0
    for b in data:
        e = (e >> 8) ^ EDC_LUT[(e ^ b) & 0xFF]
    return e

def ecc_block(sec, major_count, minor_count, major_mult, minor_inc, dest):
    src = 0x0C
    size = major_count * minor_count
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minor_count):
            t = sec[src + index]
            index += minor_inc
            if index >= size:
                index -= size
            a ^= t
            b ^= t
            a = ECC_F[a]
        a = ECC_B[ECC_F[a] ^ b]
        sec[dest + major] = a
        sec[dest + major + major_count] = a ^ b

def bcd(v):
    return ((v // 10) << 4) | (v % 10)

def make_sector(lba, data, submode):
    s = bytearray(2352)
    s[1:11] = b'\xff' * 10
    a = lba + 150
    s[12], s[13], s[14], s[15] = bcd(a // 4500), bcd((a // 75) % 60), bcd(a % 75), 2
    sub = bytes([0, 0, submode, 0])
    s[16:20] = sub
    s[20:24] = sub
    s[24:24 + len(data)] = data
    s[2072:2076] = struct.pack('<I', edc(s[16:2072]))
    hdr = bytes(s[12:16])
    s[12:16] = b'\0\0\0\0'          # Mode 2: the header is not covered by the ECC
    ecc_block(s, 86, 24, 2, 86, 0x81C)
    ecc_block(s, 52, 43, 86, 88, 0x8C8)
    s[12:16] = hdr
    return bytes(s)

# ---------------------------------------------------------------- ISO9660 helpers
def both16(v): return struct.pack('<H', v) + struct.pack('>H', v)
def both32(v): return struct.pack('<I', v) + struct.pack('>I', v)
DATE7 = bytes([126, 11, 15, 12, 0, 0, 0])   # 2026-11-15 12:00
DATE17 = b'2026111512000000\0'

def dirrec(name, lba, size, flags):
    n = len(name)
    ln = 33 + n + (1 if n % 2 == 0 else 0)
    r = bytes([ln, 0]) + both32(lba) + both32(size) + DATE7 + bytes([flags, 0, 0]) + both16(1) + bytes([n]) + name
    return r + b'\0' * (ln - len(r))

def pad(s, n):
    return s.encode('ascii')[:n].ljust(n, b' ')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--rom', required=True)
    ap.add_argument('--label', default='OPENROM')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    files = [('SYSTEM.CNF', b'BOOT = cdrom:\\OPENROM.EXE;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n'),
             ('OPENROM.EXE', open(a.exe, 'rb').read()),
             ('OPENROM.ROM', open(a.rom, 'rb').read())]
    files.sort(key=lambda f: f[0])
    # layout
    lba = 21
    entries = []
    for name, data in files:
        n = (len(data) + SECTOR - 1) // SECTOR
        entries.append((name, lba, len(data), data))
        lba += max(n, 1)
    total = lba
    root = dirrec(b'\0', 20, SECTOR, 2) + dirrec(b'\1', 20, SECTOR, 2)
    for name, l, size, _ in entries:
        root += dirrec((name + ';1').encode('ascii'), l, size, 0)
    assert len(root) <= SECTOR, 'too many files for one directory sector'

    pt_l = bytes([1, 0]) + struct.pack('<I', 20) + struct.pack('<H', 1) + b'\0\0'
    pt_m = bytes([1, 0]) + struct.pack('>I', 20) + struct.pack('>H', 1) + b'\0\0'
    pvd = bytearray(SECTOR)
    pvd[0:8] = b'\x01CD001\x01\x00'
    pvd[8:40] = pad('PLAYSTATION', 32)
    pvd[40:72] = pad(a.label, 32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SECTOR)
    pvd[132:140] = both32(len(pt_l))
    pvd[140:144] = struct.pack('<I', 18)
    pvd[148:152] = struct.pack('>I', 19)
    pvd[156:190] = dirrec(b'\0', 20, SECTOR, 2)
    pvd[190:318] = pad(a.label, 128)
    pvd[318:446] = pad('OPENROM', 128)
    pvd[446:574] = pad('OPENROM', 128)
    pvd[574:702] = pad('PLAYSTATION', 128)
    pvd[702:813] = b' ' * 111
    for off in (813, 830, 847, 864):
        pvd[off:off + 17] = DATE17
    pvd[881] = 1
    term = bytearray(SECTOR)
    term[0:7] = b'\xffCD001\x01'

    out_bin = a.out + '.bin'
    with open(out_bin, 'wb') as f:
        for i in range(16):
            f.write(make_sector(i, b'', 0x00))
        f.write(make_sector(16, bytes(pvd), 0x09))
        f.write(make_sector(17, bytes(term), 0x89))
        f.write(make_sector(18, pt_l, 0x89))
        f.write(make_sector(19, pt_m, 0x89))
        f.write(make_sector(20, root, 0x89))
        for name, l, size, data in entries:
            n = max((size + SECTOR - 1) // SECTOR, 1)
            for k in range(n):
                chunk = data[k * SECTOR:(k + 1) * SECTOR]
                f.write(make_sector(l + k, chunk, 0x89 if k == n - 1 else 0x08))
    with open(a.out + '.cue', 'w') as f:
        f.write('FILE "%s" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % os.path.basename(out_bin))
    print('%s: %d sectors' % (out_bin, total))

if __name__ == '__main__':
    main()
