#!/usr/bin/env python3
"""Pick GameShark codes on the PC and send them to the cartridge (OpenPsxRom's Cheats list).

  python tools/cheats.py --db DB search TEXT          games whose title or serial matches
  python tools/cheats.py --db DB list SERIAL          the cheats of a game, numbered
  python tools/cheats.py --db DB send SERIAL N...     those cheats to the cartridge (or: all)
                                [--append] [--off]     keep the codes already there / send disabled
  python tools/cheats.py show                         the codes on the cartridge now
  python tools/cheats.py clear                        remove them all
Options: -p COMx (default: tools/comport.txt), --db: a cheat database in DuckStation's format
(its resources/cheats.zip, a folder of SERIAL.cht files, or one .cht file); the environment
variable PSX_CHEATDB can hold it instead.

The cartridge keeps one list (flash 1C000h, 'OCH2': the game's title, each cheat's name and code
lines, up to 96 lines; see src/main.c), shown by name in the Cheats menu: the resident core
applies every enabled line while a game runs, whatever the game. So send the codes of the game you are about to play (--append to add
another cheat of the same game). Codes that need a value chosen ("00??") and code types the core
does not handle are listed but cannot be sent. Written over the OPX1 link (tools/psx.py), only
the 16 KB cheat sector; the settings at its end are kept."""
import os, re, struct, sys, zipfile, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psx

SUPPORTED = set('80 30 10 11 20 21 D0 D1 D2 D3 E0 E1 E2 E3 50'.split())
MAXC = 96
AREA, AREA_SIZE, CFG = 0x1C000, 0x4000, 0x1FF80


class Db:
    def __init__(self, path):
        if not path:
            sys.exit('no cheat database: --db PATH (DuckStation resources/cheats.zip, a folder of '
                     '.cht files or one .cht), or PSX_CHEATDB')
        self.path = path
        if zipfile.is_zipfile(path):
            self.zip = zipfile.ZipFile(path)
            self.names = [n for n in self.zip.namelist() if n.lower().endswith('.cht')]
        elif os.path.isdir(path):
            self.zip = None
            self.names = [n for n in os.listdir(path) if n.lower().endswith('.cht')]
        else:
            self.zip = None
            self.names = [path]

    def text(self, name):
        if self.zip:
            return self.zip.read(name).decode('utf-8', 'replace')
        p = name if os.path.isabs(name) or os.path.exists(name) else os.path.join(self.path, name)
        return open(p, encoding='utf-8', errors='replace').read()

    def find(self, serial):
        want = serial.upper().replace('_', '-').replace('.', '')
        for n in self.names:
            if os.path.basename(n)[:-4].upper().replace('.', '') == want:
                return n
        return None


def title_of(text):
    m = re.search(r'\[\s*([^\[\]]+?)\s*\]\s*$', text.split('\n', 1)[0])
    return m.group(1) if m else ''


def cheats_of(text):
    """[(name, [(addr, val)], why_not)]: why_not is '' when the cheat can be sent."""
    out = []
    parts = re.split(r'^\[(.+?)\]\s*$', text, flags=re.M)
    for i in range(1, len(parts), 2):
        name, body = parts[i].strip(), parts[i + 1]
        lines = re.findall(r'^\s*([0-9A-Fa-f]{8})\s+([0-9A-Fa-f?]{4})\s*$', body, flags=re.M)
        why = ''
        if not re.search(r'^\s*Type\s*=\s*Gameshark', body, flags=re.M | re.I):
            why = 'not a GameShark code'
        elif not lines:
            why = 'no code line'
        elif any('?' in v for a, v in lines):
            why = 'needs a value'
        else:
            bad = sorted({a[:2].upper() for a, v in lines} - SUPPORTED)
            if bad:
                why = 'code type ' + '/'.join(bad) + ' not handled'
        out.append((name, [(int(a, 16), v) for a, v in lines], why))
    return out


def read_cart(link):
    """(area, game, [(name, [(addr, val, on)])]) of the cheat sector ('OCH2', or 'OCH1': no names)."""
    area = link.read(psx.FLASH_BASE + AREA, AREA_SIZE)
    magic, n = struct.unpack_from('<II', area)
    game, cheats = '', []
    if magic == 0x3248434F and n <= MAXC:
        g = struct.unpack_from('<I', area, 8)[0]
        game = area[12:52].split(b'\0')[0].decode('ascii', 'replace')
        cheats = [(area[0x400 + k * 32:0x420 + k * 32].split(b'\0')[0].decode('ascii', 'replace'), [])
                  for k in range(min(g, MAXC))]
        for i in range(n):
            a, v = struct.unpack_from('<II', area, 52 + 8 * i)
            if v >> 24 < len(cheats):
                cheats[v >> 24][1].append((a, v & 0xFFFF, (v >> 16) & 1))
    elif magic == 0x3148434F and n <= MAXC:
        for i in range(n):
            a, v = struct.unpack_from('<II', area, 8 + 8 * i)
            cheats.append(('', [(a, v & 0xFFFF, (v >> 16) & 1)]))
    return area, game, cheats


def ascii_name(s, n):
    # "Infinite Lives-Codes\N. Sanity Beach" -> "N. Sanity Beach - Infinite Lives": the part that
    # tells the cheats apart first, as the cartridge keeps 31 characters
    parts = [re.sub(r'[- ]?Codes?$', '', p.strip()) for p in s.split('\\')]
    s = ' - '.join([parts[-1]] + parts[:-1][::-1]) if len(parts) > 1 else parts[0]
    return ''.join(c if 32 <= ord(c) < 127 else '?' for c in s)[:n]


def write_cart(link, area, game, cheats):
    lines = sum(len(c[1]) for c in cheats)
    if lines > MAXC or len(cheats) > MAXC:
        raise psx.LinkError('%d code lines: the cartridge takes %d at most' % (lines, MAXC))
    b = bytearray(b'\xff' * AREA_SIZE)
    struct.pack_into('<III', b, 0, 0x3248434F, lines, len(cheats))
    b[12:52] = ascii_name(game, 39).encode().ljust(40, b'\0')
    i = 0
    for k, (name, codes) in enumerate(cheats):
        b[0x400 + k * 32:0x420 + k * 32] = ascii_name(name, 31).encode().ljust(32, b'\0')
        for a, v, on in codes:
            struct.pack_into('<II', b, 52 + 8 * i, a, v | (on << 16) | (k << 24))
            i += 1
    b[CFG - AREA:] = area[CFG - AREA:]                      # settings (switch OFF choice...) kept
    if bytes(b) == area:
        print('the cartridge already holds exactly these codes')
        return
    rombuf = int(link.info['rombuf'], 16)
    for i in range(0, AREA_SIZE, psx.PIECE):
        link.put(rombuf + i, bytes(b[i:i + psx.PIECE]))
    st, data = link.req(psx.FLAREA, struct.pack('<III', AREA, AREA_SIZE, zlib.crc32(bytes(b))), timeout=60)
    if st:
        raise psx.LinkError('writing the cheat sector: %s %s' % (psx.NAMES[st] if st < 6 else st,
                                                                  data.decode(errors='replace')))
    print('written: %d cheat(s), %d code line(s) on the cartridge' % (len(cheats), lines))


def show(game, cheats):
    if not cheats:
        print('no code on the cartridge')
        return
    print(game or '(no game named)')
    for name, codes in cheats:
        on = codes and codes[0][2]
        print('  %-3s %s' % ('on' if on else 'off', name or 'code'))
        for a, v, _ in codes:
            print('        %08X %04X' % (a, v))


def main():
    a = sys.argv[1:]
    port, db, append, off = None, os.environ.get('PSX_CHEATDB'), False, False
    rest = []
    while a:
        o = a.pop(0)
        if o in ('-p', '--port'):
            port = a.pop(0)
        elif o == '--db':
            db = a.pop(0)
        elif o == '--append':
            append = True
        elif o == '--off':
            off = True
        else:
            rest.append(o)
    if not rest:
        sys.exit(__doc__)
    cmd, args = rest[0], rest[1:]
    if not port:
        f = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'comport.txt')
        port = open(f).read().strip() if os.path.exists(f) else 'COM1'

    if cmd == 'search':
        d, q = Db(db), ' '.join(args).lower()
        hits = 0
        for n in sorted(d.names):
            serial = os.path.basename(n)[:-4]
            if q in serial.lower():
                t = title_of(d.text(n))
            else:
                t = title_of(d.text(n))
                if q not in t.lower():
                    continue
            print('%-12s %s' % (serial, t))
            hits += 1
        print('%d game(s)' % hits)
        return
    if cmd == 'list':
        d = Db(db)
        n = d.find(args[0])
        if not n:
            sys.exit('%s: not in the database' % args[0])
        t = d.text(n)
        print(title_of(t) or args[0])
        for i, (name, lines, why) in enumerate(cheats_of(t), 1):
            print('%4d  %-40s %2d line(s)%s' % (i, name[:40], len(lines), '   (' + why + ')' if why else ''))
        return

    try:
        link = psx.Link(port)
        try:
            area, game, have = read_cart(link)
            if cmd == 'show':
                show(game, have)
            elif cmd == 'clear':
                write_cart(link, area, '', [])
            elif cmd == 'send':
                d = Db(db)
                n = d.find(args[0])
                if not n:
                    sys.exit('%s: not in the database' % args[0])
                text = d.text(n)
                cl = cheats_of(text)
                title = re.sub(r'\s*\{.*$', '', title_of(text)) or args[0]
                picks = range(1, len(cl) + 1) if args[1:] == ['all'] else [int(x) for x in args[1:]]
                if not picks:
                    sys.exit('which cheats? numbers from "list", or all')
                new = list(have) if append else []
                for k in picks:
                    if not 1 <= k <= len(cl):
                        sys.exit('no cheat %d (1-%d)' % (k, len(cl)))
                    name, lines, why = cl[k - 1]
                    if why:
                        if args[1:] != ['all']:
                            print('skipped %d %s: %s' % (k, name, why))
                        continue
                    new.append((name, [(addr, int(v, 16), 0 if off else 1) for addr, v in lines]))
                    print('+ %s (%d line(s))' % (name, len(lines)))
                if sum(len(c[1]) for c in new) > MAXC:
                    sys.exit('%d code lines: the cartridge takes %d at most, pick fewer cheats'
                             % (sum(len(c[1]) for c in new), MAXC))
                write_cart(link, area, title if not (append and game) else game, new)
            else:
                sys.exit(__doc__)
        finally:
            link.close()
    except (psx.LinkError, OSError) as e:
        print('error:', e)
        sys.exit(1)


if __name__ == '__main__':
    main()
