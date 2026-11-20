#!/usr/bin/env python3
"""Relocate a Caetla ROM (K-comm, versions 0.34 / 0.37 / 0.38 tested) to bank 2 or 3 of an
Xplorer FX cart, to be started by OpenPsxRom (switch OFF + Boot options "ROM bank 2/3").

  python tools/caetla_reloc.py caetla.bin 2 build/caetla_bank2.rom
  python tools/caetla_reloc.py caetla.bin 3 build/caetla_bank3.rom

Then: bank 2 -> nops /rom build/caetla_bank2.rom (from OpenPsxRom, which sends it to bank 2);
      bank 3 -> make flash3 IMG=build/caetla_bank3.rom, then nops /exe build/flash3.exe /m.

What is changed (Caetla is built for 1F000000h):
- code run from the ROM: jal/j into the ROM, lui 1F00/1F01 (+ first use) into Caetla's 128 KB,
  pointers into it; the jump tables and the header entries (+00h mid-boot, +80h pre-boot)
- compressed RAM modules (LZ, decoder in the ROM, its constant derived from the
  "caetla ver.0.3x" text): unpacked, their references (service dispatcher 1F000100h, settings
  page 1F01FF80h) moved, packed again in place (optimal parse: smaller than the original)
- Action Replay ports (1F020010h/18h): on an Xplorer those addresses are bank 2 itself, whose
  bytes made Caetla wait forever; moved to the erased area at the end of Caetla's own bank
- left alone: the flash routines' chip addresses (Caetla's own ROM flasher) and other I/O.
  Caution: Caetla's "save settings" then writes through those routines: do not save its
  configuration on the cart (it would land in bank 1); change the language without saving.
Only the input file is read; nothing is written but the output.
"""
import struct, sys

# ------------------------------------------------------------------ LZ format (ROM decoder)
# Flag bits interleaved with data. First byte: flag byte with 7 bits (6..0), then the first
# literal; later flag bytes are fetched when needed, 8 bits each (7..0).
#  0 literal | 1 0 b: match, distance 256-b (b=0: end) | 1 1 b nnnn: distance 4351-(b<<4|nnnn)
#  length: 0 -> 2, else 1 then the bits of n-1 after its leading 1, each followed by 1/0 (more)

def unpack(d, pos):
    p = pos; out = bytearray(); org = []
    t0 = d[p]; p += 1; t1 = 7
    def bit():
        nonlocal t0, t1, p
        t1 -= 1; t0 = (t0 << 1) & 0xFFFFFFFF
        if t1 < 0: t0 = d[p]; p += 1; t1 = 7
        return 1 if t0 & 0x80 else 0
    out.append(d[p]); org.append(p); p += 1
    while True:
        if not bit():
            out.append(d[p]); org.append(p); p += 1; continue
        if not bit():
            b = d[p]; p += 1
            if b == 0: return bytes(out), p
            off = -256 + b
        else:
            v = d[p]; p += 1
            for _ in range(4): v = (v << 1) | bit()
            off = (v | -4096) - 255
        src = len(out) + off
        if src < 0: raise ValueError('bad stream')
        if not bit(): n = 2
        else:
            v = 1
            while True:
                v = (v << 1) | bit()
                if not bit(): break
            n = v + 1
        for _ in range(n): out.append(out[src]); src += 1

class _W:
    def __init__(self): self.out = bytearray([0]); self.fpos = 0; self.bit = 6
    def b(self, v):
        if self.bit < 0: self.fpos = len(self.out); self.out.append(0); self.bit = 7
        if v: self.out[self.fpos] |= 1 << self.bit
        self.bit -= 1
    def byte(self, v): self.out.append(v)

def _put_len(w, n):
    if n == 2: w.b(0); return
    w.b(1); bits = bin(n - 1)[3:]
    for i, c in enumerate(bits): w.b(int(c)); w.b(1 if i < len(bits) - 1 else 0)

def _len_bits(n): return 1 if n == 2 else 1 + 2 * (len(bin(n - 1)) - 3)

def pack(data, cands=48):
    N = len(data); heads = {}; matches = [[] for _ in range(N)]
    for i in range(N - 1):
        lst = heads.setdefault(data[i:i + 2], []); res = []
        for c in reversed(lst[-cands:]):
            dist = i - c
            if dist > 4351: continue
            n = 2
            while i + n < N and n < 4096 and data[c + n] == data[i + n]: n += 1
            res.append((n, dist))
        lst.append(i); matches[i] = res
    cost = [0] * (N + 1); choice = [None] * (N + 1)
    for i in range(N - 1, 0, -1):
        best = 9 + cost[i + 1]; ch = None
        for L, dist in matches[i]:
            db = 10 if dist <= 255 else 14
            lens = {L}; k = 2
            while k <= L: lens.add(k); lens.add(min(L, k + 1)); k *= 2
            for n in lens:
                if n >= 2:
                    c = db + _len_bits(n) + cost[i + n]
                    if c < best: best = c; ch = (n, dist)
        cost[i] = best; choice[i] = ch
    w = _W(); w.byte(data[0]); i = 1
    while i < N:
        ch = choice[i]
        if ch is None: w.b(0); w.byte(data[i]); i += 1; continue
        n, dist = ch; w.b(1)
        if dist <= 255: w.b(0); w.byte(256 - dist)
        else:
            v = 4351 - dist; w.b(1); w.byte(v >> 4)
            for k in (3, 2, 1, 0): w.b((v >> k) & 1)
        _put_len(w, n); i += n
    w.b(1); w.b(0); w.byte(0)
    return bytes(w.out)

# ------------------------------------------------------------------ analysis
LOADS = (0x08, 0x09, 0x0D, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B)
def sx(v): return v - 0x10000 if v & 0x8000 else v
def words(b): return list(struct.unpack('<%dI' % (len(b) // 4), b[:len(b) // 4 * 4]))

def first_use(w, i, end):
    x = w[i]; imm, rt = x & 0xFFFF, (x >> 16) & 31
    for j in range(i + 1, min(i + 10, end)):
        y = w[j]; yo = y >> 26
        if ((y >> 21) & 31) == rt and yo in LOADS:
            return (imm << 16) | (y & 0xFFFF) if yo == 0x0D else (imm << 16) + sx(y & 0xFFFF)
    return None

def find_modules(d):
    """every ROM address referenced by the code that unpacks into a MIPS routine"""
    w = words(d); addrs = set()
    for i, x in enumerate(w):
        if x >> 26 == 0x0F and (x & 0xFFFF) in (0x1F00, 0x1F01, 0x1F02):   # 1F02 - x: end of the ROM
            f = first_use(w, i, len(w))
            if f is not None: addrs.add(f - 0x1F000000)
        if 0x1F000000 <= x < 0x1F000000 + len(d): addrs.add(x - 0x1F000000)
    mods = []
    for a in sorted(addrs):
        if not 0x100 <= a < len(d) - 16: continue
        try:
            o, end = unpack(d + b'\0' * 16, a)
        except Exception:
            continue
        if len(o) >= 512 and o[2:4] == b'\xbd\x27' and end <= len(d):
            mods.append((a, end))
    return mods

def redirect_ports(w, start, end, newhi):
    n = 0
    for i in range(start, end):
        x = w[i]
        if x >> 26 != 0x0F or (x & 0xFFFF) != 0x1F02: continue
        rt = (x >> 16) & 31; loads = []
        for j in range(i + 1, min(i + 16, end)):
            y = w[j]; yo = y >> 26
            if yo in (0x20, 0x23, 0x24) and ((y >> 21) & 31) == rt and (y & 0xFFFF) in (0x10, 0x18): loads.append(j)
            elif yo in (0x09, 0x0D, 0x0F) and ((y >> 16) & 31) == rt: break
            elif yo == 0 and ((y >> 11) & 31) == rt and y: break
        if not loads: continue
        w[i] = (x & 0xFFFF0000) | newhi
        for j in loads: w[j] = (w[j] & 0xFFFF0000) | (((w[j] & 0xFFFF) - 0x800) & 0xFFFF)   # bank + 1F810h
        n += 1
    return n

def relocate(src, bank):
    d = bytearray(src); N = len(d.rstrip(b'\xff'))
    d = d[:N + (-N % 4)]; N = len(d)
    assert d[0x84:0x94] == b'Licensed by Sony', 'not a cartridge ROM'
    assert b'caetla ver.' in d, 'not Caetla'
    assert N <= 0x1F800, 'image too big: no erased area left at the end of the bank'
    delta = (bank - 1) * 0x20000
    newhi = 0x1F00 + ((delta + 0x20000) >> 16)
    mods = find_modules(bytes(d))
    assert mods, 'no compressed module found'
    code_end = min(a for a, e in mods if a >= 0x4000)
    ranges = []; lo = 0
    for a, e in sorted(mods):
        if a >= code_end: break
        ranges.append((lo, a)); lo = (e + 3) & ~3
    ranges.append((lo, code_end))
    def in_bank(a): return a is not None and 0x1F000000 <= a < 0x1F020000
    unpacked = {a: unpack(bytes(d), a) for a, e in mods}
    w = words(d); st = dict(jal=0, lui=0, ptr=0, ports=0, kept=0)
    for lo, hi in ranges:
        for i in range(lo // 4, hi // 4):
            x = w[i]; op = x >> 26; pc = 0x1F000000 + i * 4
            if op in (2, 3):
                t = (pc & 0xF0000000) | ((x & 0x3FFFFFF) << 2)
                if in_bank(t): w[i] = (x & 0xFC000000) | (((t + delta) >> 2) & 0x3FFFFFF); st['jal'] += 1
                continue
            if op == 0x0F and (x & 0xFFFF) in (0x1F00, 0x1F01, 0x1F02):
                f = first_use(w, i, hi // 4)
                if in_bank(f if f is not None else (x & 0xFFFF) << 16):
                    w[i] = (x & 0xFFFF0000) | ((x & 0xFFFF) + (delta >> 16)); st['lui'] += 1
                else: st['kept'] += 1
                continue
            if in_bank(x) and (x - 0x1F000000 < N or x >= 0x1F01FF80): w[i] = x + delta; st['ptr'] += 1
        st['ports'] += redirect_ports(w, lo // 4, hi // 4, newhi)
    struct.pack_into('<%dI' % len(w), d, 0, *w)
    for a, e in mods:
        o, end = unpacked[a]
        mw = words(o)
        for i, x in enumerate(mw):
            if x >> 26 == 0x0F and (x & 0xFFFF) in (0x1F00, 0x1F01) and in_bank(first_use(mw, i, len(mw))):
                mw[i] = (x & 0xFFFF0000) | ((x & 0xFFFF) + (delta >> 16)); st['lui'] += 1
        st['ports'] += redirect_ports(mw, 0, len(mw), newhi)
        o2 = bytearray(o); struct.pack_into('<%dI' % len(mw), o2, 0, *mw)
        p = pack(bytes(o2))
        assert len(p) <= end - a, 'module %x does not fit back (%d > %d)' % (a, len(p), end - a)
        d[a:a + len(p)] = p
        assert unpack(bytes(d), a)[0] == bytes(o2), 'module %x: round trip failed' % a
    img = bytes(d) + b'\xff' * (0x20000 - N)
    return img, mods, st

if __name__ == '__main__':
    if len(sys.argv) != 4 or sys.argv[2] not in ('2', '3'):
        sys.exit(__doc__)
    src = open(sys.argv[1], 'rb').read()
    img, mods, st = relocate(src, int(sys.argv[2]))
    open(sys.argv[3], 'wb').write(img)
    ver = src[src.find(b'caetla ver.'):][:15].decode('latin1')
    e0, e80 = struct.unpack('<I', img[0:4])[0], struct.unpack('<I', img[0x80:0x84])[0]
    print('%s -> bank %s: %d modules, %s; entries %08X / %08X -> %s'
          % (ver, sys.argv[2], len(mods), st, e0, e80, sys.argv[3]))
