#!/usr/bin/env python3
"""128 KB image for a Datel cart with locked sectors 0 and 5 ("PS Hacker (Version 1999)", Am29F010):
the dump of the cart, with OpenPsxRom (make arcart: linked at 1F005200h) in the writable sectors and
the two hooks (src/arcart_hooks.s) at the addresses Datel's locked boot code calls.
  python tools/arcart_image.py DUMP.bin build/openrom_ar.rom build/arcart_hooks.elf OUT.rom [nm]
The locked sectors stay byte for byte as in the dump (a flash write skips them as unchanged);
Datel's settings word 'CA' at 13F80h is kept (else its mid-boot takes another path)."""
import struct, subprocess, sys, os

dump, ar_rom, hooks_elf, out = sys.argv[1:5]
nm = sys.argv[5] if len(sys.argv) > 5 else os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'gcc', 'bin', 'mipsel-none-elf-nm')
objcopy = nm[:-2] + 'objcopy'
d = bytearray(open(dump, 'rb').read())
assert len(d) == 0x20000, 'the dump must be 128 KB'

def word(a): return struct.unpack('<I', d[a:a + 4])[0]
def jal_to(a): return (word(a) >> 26) == 3 and (((word(a) & 0x3FFFFFF) << 2) | 0x10000000)
# the firmware this was made for: entries and the two call sites
checks = [(word(0x80) == 0x1F000120, 'pre-boot entry 1F000120h'),
          (word(0x00) == 0x1F0001D0, 'mid-boot entry 1F0001D0h'),
          (jal_to(0x15C) == 0x1F004BCC, 'pre-boot call to 1F004BCCh at 1F00015Ch'),
          (jal_to(0x2C4) == 0x1F0051E0, 'mid-boot call to 1F0051E0h at 1F0002C4h'),
          (word(0x13F80) == 0x4143, "settings word 'CA' at 13F80h"),
          (b'PS Hacker  ( Version 1999 )' in d, 'PS Hacker (Version 1999) firmware')]
for ok, what in checks:
    if not ok: sys.exit('not the expected firmware: ' + what)

def section(elf, name):
    tmp = out + '.' + name.strip('.') + '.bin'
    subprocess.run([objcopy, '-O', 'binary', '-j', name, elf, tmp], check=True)
    b = open(tmp, 'rb').read(); os.remove(tmp)
    return b

img = bytearray(d)
for a in range(0x4000, 0x14000): img[a] = 0xFF                 # sectors 1-4: rebuilt
for a in range(0x18000, 0x20000): img[a] = 0xFF                # sectors 6-7: card save, cheats
img[0x13F80:0x14000] = d[0x13F80:0x14000]                       # Datel's settings word kept
ar = open(ar_rom, 'rb').read().rstrip(b'\xff')
assert 0x5200 + len(ar) <= 0x13F80, 'OpenPsxRom too big for 5200h-13F80h (%d bytes)' % len(ar)
img[0x5200:0x5200 + len(ar)] = ar
pre, mid, body = (section(hooks_elf, s) for s in ('.hook_pre', '.hook_mid', '.hook_body'))
assert 0x4BCC + len(pre) <= 0x5000 and 0x51E0 + len(mid) <= 0x5200 and 0x4000 + len(body) <= 0x4BCC
img[0x4BCC:0x4BCC + len(pre)] = pre
img[0x51E0:0x51E0 + len(mid)] = mid
img[0x4000:0x4000 + len(body)] = body
assert img[0:0x4000] == d[0:0x4000] and img[0x14000:0x18000] == d[0x14000:0x18000]
open(out, 'wb').write(img)
print('%s: OpenPsxRom %d bytes at 1F005200h (free up to 13F80h: %d), hooks %d + %d + %d bytes, '
      'locked sectors 0 and 5 unchanged' % (out, len(ar), 0x13F80 - 0x5200 - len(ar), len(pre), len(mid), len(body)))
