"""生成 tripa 的程序图标 icons/tripa.png（不依赖任何图像库）。

画的是一个钢笔尖：深蓝圆角底 + 白色笔尖轮廓。
用法: python tools/make_icon.py
"""

import math
import os
import struct
import zlib

SIZE = 256
BG = (28, 63, 168)
BG2 = (17, 28, 75)
INK = (255, 255, 255)


def rounded_alpha(x, y, w, h, r):
    """圆角矩形的覆盖度（0..1），做 4x4 超采样。"""
    hits = 0
    for sy in range(4):
        for sx in range(4):
            px = x + (sx + 0.5) / 4.0
            py = y + (sy + 0.5) / 4.0
            dx = max(r - px, px - (w - r), 0.0)
            dy = max(r - py, py - (h - r), 0.0)
            if math.hypot(dx, dy) <= r:
                hits += 1
    return hits / 16.0


def nib_alpha(x, y):
    """笔尖形状：一个上宽下尖的五边形，中间一条缝。"""
    cx = SIZE / 2.0
    top = SIZE * 0.14
    bottom = SIZE * 0.90
    half_top = SIZE * 0.20
    half_mid = SIZE * 0.17
    mid = SIZE * 0.62

    if y < top or y > bottom:
        return 0.0

    if y <= mid:
        t = (y - top) / (mid - top)
        half = half_top + (half_mid - half_top) * t
    else:
        t = (y - mid) / (bottom - mid)
        half = half_mid * (1.0 - t)

    dx = abs(x - cx)
    if dx > half:
        return 0.0

    # 中间那条缝
    slit = SIZE * 0.022
    if y > SIZE * 0.30 and dx < slit:
        return 0.0

    # 边缘 1px 抗锯齿
    edge = half - dx
    return max(0.0, min(1.0, edge))


def pixel(x, y):
    a_bg = rounded_alpha(x, y, SIZE, SIZE, SIZE * 0.22)
    if a_bg <= 0.0:
        return (0, 0, 0, 0)

    # 斜向渐变底色
    t = (x + y) / (2.0 * SIZE)
    bg = tuple(int(BG[i] + (BG2[i] - BG[i]) * t) for i in range(3))

    a_nib = nib_alpha(x, y)
    col = tuple(int(bg[i] + (INK[i] - bg[i]) * a_nib) for i in range(3))
    return (col[0], col[1], col[2], int(255 * a_bg))


def write_png(path, rows):
    raw = b''.join(b'\x00' + bytes(row) for row in rows)

    def chunk(tag, data):
        payload = tag + data
        return struct.pack('>I', len(data)) + payload + struct.pack('>I', zlib.crc32(payload) & 0xFFFFFFFF)

    header = struct.pack('>IIBBBBB', SIZE, SIZE, 8, 6, 0, 0, 0)
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', header)
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))

    with open(path, 'wb') as f:
        f.write(png)


def main():
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'icons')
    out_dir = os.path.normpath(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, 'tripa.png')

    rows = []
    for y in range(SIZE):
        row = bytearray()
        for x in range(SIZE):
            row.extend(pixel(x + 0.5, y + 0.5))
        rows.append(row)

    write_png(path, rows)
    print('写出 %s' % path)


if __name__ == '__main__':
    main()
