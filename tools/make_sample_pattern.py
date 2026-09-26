# -*- coding: utf-8 -*-
"""
生成 tripa 内置手写数据（qrc:/handwrite/sample_alphabet.csv）。

输出格式与 getpattern 项目导出的 CSV 完全一致：
  第 1 行  : 字符标签（本文件是 "abcdefghijklmnopqrstuvwxyz"）
  之后每行 : x,y,pressure
  1 个空行 : 笔画边界
  2 个空行 : 字符边界

坐标系与 getpattern 的 InkPoint 一致：y 向下，原点在第一个采样点。
本脚本用"笔画骨架"定义了一套示例手写字母，采样成折线导出，
方便程序一装上就有可用的手写数据；要更漂亮的字迹，
用自己的手写笔迹在 getpattern 里采集再导出一份 CSV 即可。

用法:  python tools/make_sample_pattern.py
"""

import math
import os

CHARS = "abcdefghijklmnopqrstuvwxyz"
LINE_GAP = 260          # 字符之间的水平间隔
ROW_HEIGHT = 1600       # 一个字符分到的高度带（含间隙）
STEP = 3.0              # 折线采样步长（曲线按弧长采样，之后再用 RDP 抽稀）
PRESSURE = 0.7

# 一个字符分到的宽度带（按 26 个字母摊开）
CH = 1000


# --------------------------------------------------------------- 采样工具

def line(p, q, step=STEP):
    d = math.hypot(q[0] - p[0], q[1] - p[1])
    n = max(2, int(d / step) + 1)
    return [(p[0] + (q[0] - p[0]) * i / (n - 1),
             p[1] + (q[1] - p[1]) * i / (n - 1)) for i in range(n)]


def poly(pts, step=STEP):
    out = []
    for a, b in zip(pts, pts[1:]):
        seg = line(a, b, step)
        out.extend(seg if not out else seg[1:])
    return out


def bez(p0, p1, p2, p3, step=STEP):
    approx = (math.hypot(p1[0] - p0[0], p1[1] - p0[1])
              + math.hypot(p2[0] - p1[0], p2[1] - p1[1])
              + math.hypot(p3[0] - p2[0], p3[1] - p2[1]))
    n = max(4, int(approx / step) + 1)
    out = []
    for i in range(n):
        t = i / (n - 1)
        mt = 1 - t
        x = (mt ** 3 * p0[0] + 3 * mt * mt * t * p1[0]
             + 3 * mt * t * t * p2[0] + t ** 3 * p3[0])
        y = (mt ** 3 * p0[1] + 3 * mt * mt * t * p1[1]
             + 3 * mt * t * t * p2[1] + t ** 3 * p3[1])
        out.append((x, y))
    return out


def arc(cx, cy, r, a0, a1, step=STEP):
    length = abs(a1 - a0) * r
    n = max(4, int(length / step) + 1)
    return [(cx + r * math.cos(a0 + (a1 - a0) * i / (n - 1)),
             cy + r * math.sin(a0 + (a1 - a0) * i / (n - 1))) for i in range(n)]


def join(*segments):
    out = []
    for seg in segments:
        out.extend(seg if not out else seg[1:])
    return out


def rdp(points, eps=0.9):
    """Ramer-Douglas-Peucker 抽稀：曲线仍是折线，但点数少很多。

    点少了文件才小；渲染时 tripa 会按弧长重采样，
    所以稀疏折线不会影响扭曲效果。
    """
    if len(points) < 3:
        return list(points)

    def perp(p, a, b):
        ax, ay = a
        bx, by = b
        px, py = p
        dx, dy = bx - ax, by - ay
        den = math.hypot(dx, dy)
        if den < 1e-9:
            return math.hypot(px - ax, py - ay)
        return abs(dy * px - dx * py + bx * ay - by * ax) / den

    dmax, index = 0.0, 0
    for i in range(1, len(points) - 1):
        d = perp(points[i], points[0], points[-1])
        if d > dmax:
            dmax, index = d, i

    if dmax > eps:
        left = rdp(points[:index + 1], eps)
        right = rdp(points[index:], eps)
        return left[:-1] + right
    return [points[0], points[-1]]


def simplify(segments, eps=0.9):
    return [rdp(seg, eps) for seg in segments]


# --------------------------------------------------------------- 字形定义
# y 向下：大写顶 0、小写顶 400、基线 1000、下伸 1400

TOP, XTOP, BASE, DESC = 0, 400, 1000, 1400
W = 620


def glyph_a():
    return [poly([(560, 430), (150, 430), (90, 520), (80, 780), (140, 940), (420, 990), (560, 950)]),
            line((560, 430), (560, 1000))]


def glyph_b():
    return [line((100, TOP), (100, BASE)), arc(370, 775, 275, math.radians(90), math.radians(450))]


def glyph_c():
    return [bez((560, 560), (400, 330), (120, 430), (100, 720)),
            bez((100, 720), (120, 1010), (400, 1080), (560, 880))]


def glyph_d():
    return [line((560, TOP), (560, BASE)), arc(330, 775, 275, math.radians(90), math.radians(-270))]


def glyph_e():
    return [join(line((110, 700), (560, 700)),
                 arc(335, 700, 225, 0, math.radians(340)))]


def glyph_f():
    return [bez((480, 140), (330, 60), (230, 220), (210, 1000)), line((90, 520), (500, 520))]


def glyph_g():
    return [bez((570, 430), (420, 330), (180, 460), (140, 720)),
            bez((140, 720), (170, 990), (470, 1020), (570, 880)),
            bez((570, 430), (570, 1240), (330, 1420), (110, 1300))]


def glyph_h():
    return [line((100, TOP), (100, BASE)), bez((100, 620), (300, 400), (560, 600), (560, 1000))]


def glyph_i():
    return [line((170, 430), (170, 1000)), line((170, 250), (170, 270))]


def glyph_j():
    return [line((330, 430), (330, 1260)),
            bez((330, 1260), (300, 1400), (200, 1420), (90, 1340))]


def glyph_k():
    return [line((100, TOP), (100, BASE)), line((520, 430), (100, 780)), line((250, 640), (540, 1000))]


def glyph_l():
    return [line((170, TOP), (170, 1000))]


def glyph_m():
    return [line((90, 430), (90, 1000)),
            bez((90, 600), (240, 400), (360, 620), (360, 1000)),
            bez((360, 600), (510, 400), (620, 620), (620, 1000))]


def glyph_n():
    return [line((100, 430), (100, 1000)), bez((100, 600), (300, 400), (560, 620), (560, 1000))]


def glyph_o():
    return [join(arc(330, 715, 285, 0, math.pi),
                 arc(330, 715, 285, math.pi, 2 * math.pi))]


def glyph_p():
    return [line((100, 430), (100, DESC)), arc(370, 775, 275, math.radians(-90), math.radians(90))]


def glyph_q():
    return [line((560, 430), (560, DESC)), arc(290, 775, 275, math.radians(90), math.radians(-90))]


def glyph_r():
    return [line((120, 430), (120, 1000)),
            bez((120, 640), (260, 440), (400, 420), (520, 520))]


def glyph_s():
    return [bez((540, 560), (420, 350), (120, 420), (150, 660)),
            bez((150, 660), (180, 860), (560, 860), (540, 1080)),
            bez((540, 1080), (490, 1240), (140, 1160), (100, 1050))]


def glyph_t():
    return [line((330, 200), (330, 900)),
            bez((330, 900), (340, 1010), (440, 1040), (540, 990)),
            line((130, 430), (540, 430))]


def glyph_u():
    return [bez((100, 430), (90, 880), (330, 1030), (520, 900)),
            line((520, 430), (520, 960))]


def glyph_v():
    return [poly([(100, 430), (330, 990), (560, 430)])]


def glyph_w():
    return [poly([(70, 430), (230, 990), (380, 520), (530, 990), (690, 430)])]


def glyph_x():
    return [line((100, 430), (560, 1000)), line((560, 430), (100, 1000))]


def glyph_y():
    return [poly([(100, 430), (330, 990)]),
            bez((560, 430), (520, 800), (330, 990), (110, 1290))]


def glyph_z():
    return [poly([(110, 440), (560, 440), (110, 990), (570, 990)])]


GLYPHS = {
    'a': glyph_a, 'b': glyph_b, 'c': glyph_c, 'd': glyph_d, 'e': glyph_e,
    'f': glyph_f, 'g': glyph_g, 'h': glyph_h, 'i': glyph_i, 'j': glyph_j,
    'k': glyph_k, 'l': glyph_l, 'm': glyph_m, 'n': glyph_n, 'o': glyph_o,
    'p': glyph_p, 'q': glyph_q, 'r': glyph_r, 's': glyph_s, 't': glyph_t,
    'u': glyph_u, 'v': glyph_v, 'w': glyph_w, 'x': glyph_x, 'y': glyph_y,
    'z': glyph_z,
}


# --------------------------------------------------------------- 导出

def fmt(v):
    return ("%.2f" % v).rstrip('0').rstrip('.')


def main():
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'handwrite')
    out_dir = os.path.normpath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    # 一个字符一个文件，和 getpattern 保存出来的 CSV 一致
    total_points = 0
    total_strokes = 0
    written = []

    for index, ch in enumerate(CHARS):
        lines = [ch]
        strokes = simplify(GLYPHS[ch]())
        dx = index * (W + LINE_GAP)
        dy = ROW_HEIGHT

        for s_index, stroke in enumerate(strokes):
            if s_index > 0:
                lines.append('')  # 笔画边界：一个空行
            for (x, y) in stroke:
                lines.append('%s,%s,%s' % (fmt(x + dx), fmt(y + dy), PRESSURE))
                total_points += 1
            total_strokes += 1

        path = os.path.join(out_dir, '%s.csv' % ch)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
        written.append(path)

    print('写出 %d 个 CSV 到 %s' % (len(written), out_dir))
    print('字符 %d 个，笔画 %d 条，点 %d 个' % (len(CHARS), total_strokes, total_points))


if __name__ == '__main__':
    main()
