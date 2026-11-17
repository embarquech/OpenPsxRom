#!/usr/bin/env python3
"""Screenshot of the PlayStation screen over the serial port (OpenPsxRom menu running).

  python tools/psxshot.py shot.png [--port COM3] [--nops path/to/nops.exe] [--rect X Y W H] [--scale 2]

OpenPsxRom copies the picture on screen (or any VRAM rectangle with --rect, e.g. --rect 0 0 1024 512
for the whole VRAM) to RAM with its SHOT command, the pixels are fetched with "nops /dump", then
converted from the PS1's 15-bit format to PNG. Needs pyserial and Pillow.
"""
import argparse, os, struct, subprocess, sys, tempfile, time


def ask_shot(port, rect):
    import serial
    s = serial.Serial(port, 115200, timeout=2)
    s.rts = True
    s.dtr = True
    s.reset_input_buffer()
    s.write(b"SHOT")
    win, t0 = b"", time.time()
    while not win.endswith(b"OKAY"):                   # OKV2 then OKAY (no protocol upgrade asked)
        c = s.read(1)
        if not c:
            if time.time() - t0 > 5:
                sys.exit("no answer: is the OpenPsxRom menu running on " + port + "?")
            continue
        win = (win + c)[-8:]
    s.write(struct.pack("<4I", *rect))
    r = s.read(12)
    s.close()
    if len(r) != 12:
        sys.exit("incomplete answer to SHOT")
    return struct.unpack("<3I", r)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", help="PNG file to write")
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--nops", default="nops", help="nops executable (v18)")
    ap.add_argument("--rect", type=int, nargs=4, metavar=("X", "Y", "W", "H"), default=[0, 0, 0, 0],
                    help="VRAM rectangle (default: the picture on screen)")
    ap.add_argument("--scale", type=int, default=2, help="pixel scale of the PNG (default 2)")
    a = ap.parse_args()

    addr, w, h = ask_shot(a.port, a.rect)
    size = w * h * 2
    print("VRAM picture %dx%d at %08X, fetching %d bytes..." % (w, h, addr, size))
    raw = os.path.join(tempfile.gettempdir(), "psxshot.bin")
    if os.path.exists(raw):
        os.remove(raw)
    subprocess.run([a.nops, "/dump", "0x%08X" % addr, "0x%X" % size, raw, "/dest", a.port],
                   stdout=subprocess.DEVNULL, check=False)
    if not os.path.exists(raw) or os.path.getsize(raw) < size:
        sys.exit("dump failed")
    data = open(raw, "rb").read()[:size]

    from PIL import Image
    img = Image.new("RGB", (w, h))
    px = img.load()
    for i in range(w * h):
        v = data[2 * i] | data[2 * i + 1] << 8          # 0BBBBBGGGGGRRRRR
        r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
        px[i % w, i // w] = (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2)
    if a.scale > 1:
        img = img.resize((w * a.scale, h * a.scale), Image.NEAREST)
    img.save(a.out)
    print("written", a.out)


if __name__ == "__main__":
    main()
