#!/usr/bin/env python3
"""OMC1: OpenROM compressed memory card images (Xplorer-style backups).

  python tools/omc.py pack   card.mcr card.omc     raw 128 KB image -> compressed
  python tools/omc.py unpack card.omc card.mcr     compressed -> raw image (PCSX/DuckStation .mcr)
  python tools/omc.py info   card.omc
  python tools/omc.py saves  store.bin [dir]      saves kept in the cartridge -> one .mcs per save

The saves kept in the cartridge (flash 10000h-1BFFFh, menu Tools > Memory cards) can be read with
  nops /dump 0x1F010000 0xC000 store.bin       then: python tools/omc.py saves store.bin

A backup kept in the cartridge (flash 10000h-1BFFFh) can be read with
  nops /dump 0x1F010000 0xC000 backup.bin      then: python tools/omc.py unpack backup.bin card.mcr

Format: 'OMC1', u32 raw size, u32 CRC32 of the raw data, u32 packed size (all LE), then LZSS:
flag byte (bit 0 first) 1 = literal, 0 = match (b0 = (dist-1) & 255, b1 = ((dist-1) >> 8) << 4 | (len-3)),
distance 1..4096, length 3..18. Same as src/lz.c.
"""
import struct, sys, zlib

WIN, MAXLEN = 4096, 18


def pack(raw):
    out = bytearray(16)
    head = {}
    i, n = 0, len(raw)
    while i < n:
        fpos = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= n:
                break
            best = dist = 0
            if i + 3 <= n:
                key = raw[i:i + 3]
                for c in reversed(head.get(key, [])[-8:]):
                    if i - c > WIN:
                        break
                    l = 0
                    lim = min(MAXLEN, n - i)
                    while l < lim and raw[c + l] == raw[i + l]:
                        l += 1
                    if l > best:
                        best, dist = l, i - c
                        if l == MAXLEN:
                            break
            if best >= 3:
                d = dist - 1
                out += bytes([d & 255, ((d >> 8) << 4) | (best - 3)])
                for k in range(best):
                    if i + k + 3 <= n:
                        head.setdefault(raw[i + k:i + k + 3], []).append(i + k)
                i += best
            else:
                if i + 3 <= n:
                    head.setdefault(raw[i:i + 3], []).append(i)
                flags |= 1 << bit
                out.append(raw[i])
                i += 1
        out[fpos] = flags
    struct.pack_into('<4sIII', out, 0, b'OMC1', n, zlib.crc32(raw), len(out))
    return bytes(out)


def unpack(data):
    magic, n, crc, end = struct.unpack_from('<4sIII', data, 0)
    if magic != b'OMC1':
        sys.exit('not an OMC1 file')
    out = bytearray()
    i = 16
    while len(out) < n and i < end:
        flags = data[i]
        i += 1
        for bit in range(8):
            if len(out) >= n or i >= end:
                break
            if flags >> bit & 1:
                out.append(data[i])
                i += 1
            else:
                d = (data[i] | (data[i + 1] >> 4) << 8) + 1
                l = (data[i + 1] & 15) + 3
                i += 2
                for _ in range(l):
                    out.append(out[-d])
    out = bytes(out[:n])
    if zlib.crc32(out) != crc:
        sys.exit('CRC error: damaged backup')
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    data = open(sys.argv[2], 'rb').read()
    if cmd == 'pack':
        p = pack(data)
        open(sys.argv[3], 'wb').write(p)
        print(f'{len(data)} -> {len(p)} bytes ({len(p) * 100 // len(data)}%)')
    elif cmd == 'unpack':
        r = unpack(data)
        open(sys.argv[3], 'wb').write(r)
        print(f'{len(r)} bytes, CRC ok')
    elif cmd == 'saves':
        # cartridge save store (OFS1): one .mcs file per save (128-byte directory frame + data)
        raw = unpack(data)
        if raw[:4] != b'OFS1':
            sys.exit('not a save store (OFS1)')
        out = sys.argv[3] if len(sys.argv) > 3 else '.'
        count = struct.unpack_from('<I', raw, 4)[0]
        o = 8
        for _ in range(count):
            dirf = raw[o:o + 128]
            size = struct.unpack_from('<I', dirf, 4)[0]
            n = (size + 8191) // 8192 * 8192
            name = dirf[10:31].split(b'\0')[0].decode('ascii', 'replace')
            path = f'{out}/{name}.mcs'
            open(path, 'wb').write(dirf + raw[o + 128:o + 128 + n])
            print(f'{path}: {n // 8192} block(s)')
            o += 128 + n
    elif cmd == 'info':
        magic, n, crc, end = struct.unpack_from('<4sIII', data, 0)
        print(f'{magic.decode()} raw {n} bytes, packed {end} bytes, CRC32 {crc:08X}')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
