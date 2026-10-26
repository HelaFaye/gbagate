#!/usr/bin/env python3
"""ppm2png.py IN.ppm OUT.png -- a binary PPM (P6) to PNG, standard library only."""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos)
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b'P6' or fields[3] != b'255':
        sys.exit(f'{path}: not an 8-bit P6 file')
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def chunk(kind, body):
    return (struct.pack('>I', len(body)) + kind + body +
            struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    w, h, pixels = read_ppm(sys.argv[1])
    rows = b''.join(b'\0' + pixels[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))
    with open(sys.argv[2], 'wb') as f:
        f.write(png)


if __name__ == '__main__':
    main()
