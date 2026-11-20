"""Compress the RAM payload of the ROM image (OMC1, like the memory card backups).

The linked image is: header + ROM code (run in place) | payload (copied to RAM at boot).
The payload is replaced by its OMC1-packed form at the same place; the boot code unpacks it
(rom_unpack in src/romx.c).
  python tools/packrom.py build/openrom.elf build/openrom.raw build/openrom.packed [path/to/nm]
"""
import subprocess, sys, os
sys.path.insert(0, os.path.dirname(__file__))
import omc

elf, raw, out = sys.argv[1:4]
# nm of the toolchain: 4th argument (the Makefile passes $(TOOLS)nm), else the default location
nm = sys.argv[4] if len(sys.argv) > 4 else os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'gcc', 'bin', 'mipsel-none-elf-nm')
syms = {}
for line in subprocess.run([nm, elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16) & 0xFFFFFFFF
base = syms.get('__rom_base', syms['__payload_rom'] & 0xFFFE0000)   # image start: bank base, or __rom_base
off = syms['__payload_rom'] - base
size = syms['__payload_size']
data = open(raw, 'rb').read()
payload = data[off:off + size]
assert len(payload) == size, 'payload not found in the image'
packed = omc.pack(payload)
assert omc.unpack(packed) == payload
open(out, 'wb').write(data[:off] + packed)
print(f'payload {size} -> {len(packed)} bytes ({len(packed) * 100 // size}%), ROM {off + len(packed)} bytes')
