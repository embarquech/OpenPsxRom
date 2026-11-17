"""Pad the raw ROM image with 0xFF (erased flash) to the chip size, and report the usage."""
import sys, zlib

src, dst, size = sys.argv[1], sys.argv[2], int(sys.argv[3])
data = open(src, 'rb').read()
if len(data) > 0x10000:
    sys.exit(f'ROM too big: {len(data)} bytes > 64 KB (the rest of the flash holds the saves)')
data += b'\xff' * (size - len(data))
open(dst, 'wb').write(data)
used = len(open(src, 'rb').read())
print(f'{dst}: {used} bytes used of {size} ({used * 100 // size}%), CRC32 {zlib.crc32(data):08X}')
