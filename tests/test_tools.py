#!/usr/bin/env python3
"""Level 1 tests: PC tools and the built ROM image. No hardware, no emulator.

  python tests/test_tools.py            (or: python -m unittest discover tests)

The ROM image tests run when build/openrom.rom exists (make first).
"""
import os, random, struct, subprocess, sys, tempfile, unittest, zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import omc            # noqa: E402
import psxshot        # noqa: E402

ROM = os.path.join(ROOT, 'build', 'openrom.rom')


class OMC1(unittest.TestCase):
    """LZSS + CRC container used for the ROM payload and the memory card saves (same as src/lz.c)."""

    def roundtrip(self, raw):
        packed = omc.pack(raw)
        magic, n, crc, end = struct.unpack_from('<4sIII', packed, 0)
        self.assertEqual(magic, b'OMC1')
        self.assertEqual(n, len(raw))
        self.assertEqual(crc, zlib.crc32(raw))
        self.assertEqual(end, len(packed))
        self.assertEqual(omc.unpack(packed), raw)
        return packed

    def test_empty_card_compresses(self):
        # the format's limit: a match is at most 18 bytes for 2 bytes + a flag bit (~8.5x on zeros)
        packed = self.roundtrip(bytes(131072))
        self.assertLess(len(packed), 131072 // 8)

    def test_random_data(self):
        rnd = random.Random(1)
        self.roundtrip(bytes(rnd.getrandbits(8) for _ in range(20000)))

    def test_repetitions_at_the_window_limits(self):
        rnd = random.Random(2)
        block = bytes(rnd.getrandbits(8) for _ in range(4096))
        # matches at distance exactly 4096 (window size) and runs longer than the max length 18
        self.roundtrip(block + block + b'A' * 1000 + block[:17] + block[:18] + block[:19])

    def test_typical_save_shape(self):
        rnd = random.Random(3)
        frame = bytearray(8192)
        frame[0:2] = b'SC'
        frame[0x80:0x80 + 512] = bytes(rnd.getrandbits(4) for _ in range(512))   # icon-like data
        packed = self.roundtrip(bytes(frame) * 3)
        self.assertLess(len(packed), len(frame) * 3 // 3)

    def test_damaged_data_is_rejected(self):
        packed = bytearray(omc.pack(b'hello world ' * 200))
        packed[30] ^= 0xFF
        with self.assertRaises(SystemExit):
            omc.unpack(bytes(packed))


class Elf2Exe(unittest.TestCase):
    def test_header(self):
        with tempfile.TemporaryDirectory() as d:
            src, out = os.path.join(d, 'a.bin'), os.path.join(d, 'a.exe')
            open(src, 'wb').write(b'\x11' * 3000)
            subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'elf2exe.py'), src, out,
                            '0x80010000', '0x80010000'], check=True, capture_output=True)
            e = open(out, 'rb').read()
        self.assertEqual(e[:8], b'PS-X EXE')
        pc, gp, taddr, tsize = struct.unpack_from('<4I', e, 0x10)
        self.assertEqual((pc, taddr), (0x80010000, 0x80010000))
        self.assertEqual(tsize % 2048, 0)
        self.assertEqual(len(e), 2048 + tsize)
        self.assertEqual(struct.unpack_from('<I', e, 0x30)[0], 0x801FFF00)        # stack


class BootDisc(unittest.TestCase):
    def test_iso_layout(self):
        with tempfile.TemporaryDirectory() as d:
            exe, rom = os.path.join(d, 'm.exe'), os.path.join(d, 'm.rom')
            open(exe, 'wb').write(b'PS-X EXE' + bytes(4096 - 8))
            open(rom, 'wb').write(bytes(131072))
            subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'mkdisc.py'), '--exe', exe,
                            '--rom', rom, '--out', os.path.join(d, 'disc')], check=True, capture_output=True)
            b = open(os.path.join(d, 'disc.bin'), 'rb').read()
            cue = open(os.path.join(d, 'disc.cue')).read()
        self.assertEqual(len(b) % 2352, 0)
        self.assertIn('MODE2/2352', cue)
        sync = b'\x00' + b'\xff' * 10 + b'\x00'
        self.assertEqual(b[:12], sync)
        pvd = b[16 * 2352 + 24:16 * 2352 + 24 + 2048]                           # Mode 2 Form 1 data
        self.assertEqual(pvd[0:6], b'\x01CD001')
        self.assertIn(b'PLAYSTATION', pvd[8:40])
        self.assertIn(b'BOOT = cdrom:\\OPENROM.EXE;1', b)


class Screenshot(unittest.TestCase):
    def test_15bit_colours(self):
        self.assertEqual(psxshot.to_rgb(0x7FFF), (255, 255, 255))
        self.assertEqual(psxshot.to_rgb(0x0000), (0, 0, 0))
        self.assertEqual(psxshot.to_rgb(0x001F), (255, 0, 0))
        self.assertEqual(psxshot.to_rgb(0x03E0), (0, 255, 0))
        self.assertEqual(psxshot.to_rgb(0x7C00), (0, 0, 255))

    def test_image(self):
        try:
            import PIL  # noqa: F401
        except ImportError:
            self.skipTest('Pillow not installed')
        data = struct.pack('<4H', 0x001F, 0x03E0, 0x7C00, 0x7FFF)
        img = psxshot.to_image(data, 2, 2, scale=2)
        self.assertEqual(img.size, (4, 4))
        self.assertEqual(img.getpixel((0, 0)), (255, 0, 0))
        self.assertEqual(img.getpixel((3, 3)), (255, 255, 255))


@unittest.skipUnless(os.path.exists(ROM), 'build/openrom.rom not built (run make)')
class RomImage(unittest.TestCase):
    """The cartridge image: what the BIOS and the flashing code rely on."""

    @classmethod
    def setUpClass(cls):
        cls.rom = open(ROM, 'rb').read()

    def word(self, off):
        return struct.unpack_from('<I', self.rom, off)[0]

    def test_size_and_padding(self):
        self.assertEqual(len(self.rom), 131072)
        used = len(self.rom.rstrip(b'\xff'))
        self.assertLessEqual(used, 0x10000, 'code must fit in the first 64 KB (saves start at 10000h)')
        self.assertEqual(self.rom[0x10000:], b'\xff' * 0x10000, 'saves/cheats/settings area must be blank')

    def test_bios_headers(self):
        for base in (0x04, 0x84):                      # mid-boot and pre-boot license strings
            self.assertEqual(self.rom[base:base + 44], b'Licensed by Sony Computer Entertainment Inc.')
        self.assertEqual(self.rom[0x30:0x30 + 24], b'OpenPsxRom - open source')   # image detection id
        for off in (0x00, 0x80):                       # entry points inside the ROM code
            self.assertTrue(0x1F000100 <= self.word(off) < 0x1F010000, hex(self.word(off)))

    def test_rom_entry_table(self):
        for off in (0xEC, 0xF0, 0xF4, 0xF8, 0xFC):     # rom_break/feed/halted/crash/menu (src/shared.h)
            self.assertTrue(0x1F000100 <= self.word(off) < 0x1F010000, '%x: %08x' % (off, self.word(off)))

    def test_packed_payload(self):
        i = self.rom.find(b'OMC1')
        self.assertGreater(i, 0, 'no OMC1 payload in the ROM')
        n = struct.unpack_from('<I', self.rom, i + 4)[0]
        payload = omc.unpack(self.rom[i:])            # CRC checked inside
        self.assertEqual(len(payload), n)
        self.assertIn(b'OpenPsxRom', payload)

    def test_switch_bank_is_not_rom2(self):
        # an OpenPsxRom image must be written to bank 1, never mistaken for a ROM 2 image
        self.assertNotEqual(self.rom[0x200:0x204], b'ROM2')


if __name__ == '__main__':
    unittest.main(verbosity=2)
