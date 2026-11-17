# Convert a flat binary (objcopy -O binary of the ELF) into a PS-EXE.
import sys, struct
binf, outf, load, entry = sys.argv[1], sys.argv[2], int(sys.argv[3], 0) & 0xFFFFFFFF, int(sys.argv[4], 0) & 0xFFFFFFFF
data = open(binf, 'rb').read()
data += b'\0' * ((-len(data)) % 2048)
hdr = bytearray(2048)
hdr[0:8] = b'PS-X EXE'
struct.pack_into('<IIIIIIIIIIII', hdr, 0x10, entry, 0, load, len(data), 0, 0, 0, 0, 0x801FFF00, 0, 0, 0)
hdr[0x4C:0x4C+46] = b'Sony Computer Entertainment Inc. for Europe area'[:46]
open(outf, 'wb').write(bytes(hdr) + data)
print(f"{outf}: {len(data)} bytes payload, entry {entry:#x}")
