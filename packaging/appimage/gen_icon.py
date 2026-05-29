#!/usr/bin/env python3
"""Generate a minimal 256x256 placeholder PNG for the AOE3DEHarness AppImage icon."""
import struct, zlib, os, sys

def png_chunk(chunk_type, data):
    c = chunk_type + data
    return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)

def make_png(w, h, r, g, b):
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr_data = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    ihdr = png_chunk(b'IHDR', ihdr_data)
    raw = b''
    for _ in range(h):
        row = bytes([0])  # filter type None
        for _ in range(w):
            row += bytes([r, g, b])
        raw += row
    idat = png_chunk(b'IDAT', zlib.compress(raw, 9))
    iend = png_chunk(b'IEND', b'')
    return sig + ihdr + idat + iend

def main():
    appdir = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('APPDIR', '.')
    paths = [
        os.path.join(appdir, 'AOE3DEHarness.png'),
        os.path.join(appdir, 'usr', 'share', 'icons', 'hicolor', '256x256', 'apps', 'AOE3DEHarness.png'),
    ]
    data = make_png(256, 256, 42, 86, 153)  # AOE-ish dark blue
    for p in paths:
        os.makedirs(os.path.dirname(p) or '.', exist_ok=True)
        with open(p, 'wb') as f:
            f.write(data)
        print(f"[appimage]   icon written to {p}")

if __name__ == '__main__':
    main()
