"""生成 tripa 的程序图标 icons/tripa.png 和 icons/tripa.ico（不依赖任何图像库）。

画的是一个钢笔尖：深蓝圆角底 + 白色笔尖轮廓。

为什么要 .ico：**Windows 资源管理器读的是 exe 里嵌的图标资源**，
不认 QApplication::setWindowIcon() —— 那个只管窗口左上角和任务栏。
所以除了 png（Qt 运行时用）还得有一个 .ico 编进 exe（见 tripa.rc），
而且必须是**多尺寸**的：16/24/32 用在小图标列表里，256 用在超大图标视图，
只放一张大图的话小尺寸下会糊成一团。

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

#! .ico 里放这几档（Windows 会在这些尺寸里挑最合适的一张）
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)


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


def png_bytes(size, rows):
    """把像素行编成一张 PNG 的字节流（8 位 RGBA）。"""
    raw = b''.join(b'\x00' + bytes(row) for row in rows)

    def chunk(tag, data):
        payload = tag + data
        return struct.pack('>I', len(data)) + payload + struct.pack('>I', zlib.crc32(payload) & 0xFFFFFFFF)

    header = struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', header)
            + chunk(b'IDAT', zlib.compress(raw, 9))
            + chunk(b'IEND', b''))


def write_png(path, size, rows):
    with open(path, 'wb') as f:
        f.write(png_bytes(size, rows))


def srgb_to_linear(v):
    v = v / 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def linear_to_srgb(v):
    v = max(0.0, min(1.0, v))
    s = v * 12.92 if v <= 0.0031308 else 1.055 * (v ** (1 / 2.4)) - 0.055
    return int(round(s * 255.0))


def downscale(rows, src, dst):
    """面积平均缩小（在线性光里加权），只用于生成小尺寸图标。

    缩小必须**按面积平均**：直接最近邻采样会让笔画粗细随尺寸乱跳，
    16x16 那档会缺胳膊少腿。
    """
    if src == dst:
        return [bytearray(row) for row in rows]

    factor = src / dst
    out = []
    for dy in range(dst):
        row = bytearray()
        y0 = int(dy * factor)
        y1 = max(y0 + 1, int((dy + 1) * factor))
        for dx in range(dst):
            x0 = int(dx * factor)
            x1 = max(x0 + 1, int((dx + 1) * factor))
            a_sum = 0.0
            rgb = [0.0, 0.0, 0.0]
            count = 0
            for y in range(y0, min(y1, src)):
                for x in range(x0, min(x1, src)):
                    p = rows[y]
                    i = x * 4
                    a = p[i + 3] / 255.0
                    a_sum += a
                    for c in range(3):
                        rgb[c] += a * srgb_to_linear(p[i + c])
                    count += 1
            if count == 0 or a_sum <= 0.0:
                row.extend((0, 0, 0, 0))
                continue
            row.extend([
                linear_to_srgb(rgb[0] / a_sum),
                linear_to_srgb(rgb[1] / a_sum),
                linear_to_srgb(rgb[2] / a_sum),
                int(round(255.0 * a_sum / count)),
            ])
        out.append(row)
    return out


def write_ico(path, master):
    """写一个多尺寸 .ico：每档都是一张 PNG（Windows Vista 起支持）。

    目录项里的宽高字节用 0 表示 256（这是 .ico 格式的规定）。
    """
    images = [(n, downscale(master, SIZE, n)) for n in ICO_SIZES]
    blobs = [(n, png_bytes(n, rows)) for n, rows in images]

    header = struct.pack('<HHH', 0, 1, len(blobs))  # reserved, type=icon, count
    offset = len(header) + 16 * len(blobs)
    directory = b''
    for (n, blob) in blobs:
        b = 0 if n >= 256 else n
        directory += struct.pack('<BBBBHHII', b, b, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)

    with open(path, 'wb') as f:
        f.write(header + directory + b''.join(blob for _, blob in blobs))
    return [(n, len(blob)) for n, blob in blobs]


def main():
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'icons')
    out_dir = os.path.normpath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    master = []
    for y in range(SIZE):
        row = bytearray()
        for x in range(SIZE):
            row.extend(pixel(x + 0.5, y + 0.5))
        master.append(row)

    png_path = os.path.join(out_dir, 'tripa.png')
    write_png(png_path, SIZE, master)
    print('写出 %s' % png_path)

    ico_path = os.path.join(out_dir, 'tripa.ico')
    sizes = write_ico(ico_path, master)
    print('写出 %s（%s）' % (ico_path, '、'.join('%dx%d' % (n, n) for n, _ in sizes)))


if __name__ == '__main__':
    main()
