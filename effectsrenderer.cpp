#include "effectsrenderer.h"

#include "effect.h"
#include "handwriting.h"
#include "noise.h"

#include <QAbstractTextDocumentLayout>
#include <QCache>
#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRandomGenerator>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QTextStream>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

//! 稳定的整数哈希，用来给每个字符派生"固定但看着随机"的种子
quint32 mixSeed(quint32 seed, quint32 value)
{
    quint32 h = seed ^ (value + 0x9E3779B9u + (seed << 6) + (seed >> 2));
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

//! 字形轮廓缓存：同一个 (字符, 字体, 字号) 只取一次 outline
QCache<QString, QVector<QVector<QPointF>>> &glyphCache()
{
    static QCache<QString, QVector<QVector<QPointF>>> cache(512);
    return cache;
}

//! 扭曲结果缓存：同一个字形 + 同一条噪声波 + 同样的幅度，几何完全一样
QCache<QString, QVector<QVector<QPointF>>> &distortCache()
{
    static QCache<QString, QVector<QVector<QPointF>>> cache(4096);
    return cache;
}

QString glyphCacheKey(const QString &text, const QFont &font)
{
    return QStringLiteral("%1|%2|%3").arg(text, font.toString()).arg(font.pointSizeF());
}

/*!
 * 把一个 QPainterPath 拆成"闭合子路径点列"，并按 \a tolerance 自适应细分曲线。
 *
 * Qt6 的 toSubpathPolygons() 没有容差参数（内部固定按比较粗的步长细分），
 * 而轮廓点太稀的话，一条直线段会被整段平移，字形就塌成几块多边形
 * —— 看着就像"从原字体对称折出来的"。所以这里自己细分：
 * 控制点离弦的垂距超过 tolerance 就继续二分。
 *
 * 深度上限给 12（最多 4096 段）：上限太小会让容差形同虚设 ——
 * 一条 40pt 的竖笔画弧长就有 36pt，深度 6 只能切出 0.6pt 的一段，
 * 结果整条竖画还是一根直线，噪声一叠字形就散了。
 */
constexpr int kMaxFlattenDepth = 12;

void flattenSubpath(const QPainterPath &path, int from, int to, double tolerance,
                    QVector<QPointF> *out);

void flattenCurve(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3,
                  double tolerance, int depth, QVector<QPointF> *out)
{
    if (depth >= kMaxFlattenDepth) {
        out->append(p3);
        return;
    }

    /*!
     * 用曲线上两个采样点到弦的距离当误差。
     *
     * 只拿"控制点中点到弦中点"当判据是不准的：三次曲线退化成二次时
     * （TrueType 里很常见）控制点会偏在一头，算出来的值比真实误差小得多，
     * 于是一条圆角被一步过掉，拐角处变成硬折线。
     */
    auto onCurve = [&](double u) {
        const double v = 1.0 - u;
        return p0 * (v * v * v) + p1 * (3.0 * v * v * u) + p2 * (3.0 * v * u * u)
               + p3 * (u * u * u);
    };
    auto distToChord = [&](const QPointF &q) {
        const QPointF d = p3 - p0;
        const double len = std::hypot(d.x(), d.y());
        if (len < 1e-9)
            return std::hypot(q.x() - p0.x(), q.y() - p0.y());
        return std::abs(d.x() * (p0.y() - q.y()) - (p0.x() - q.x()) * d.y()) / len;
    };

    const double error = qMax(distToChord(onCurve(0.25)), distToChord(onCurve(0.75)));
    if (error <= tolerance) {
        out->append(p3);
        return;
    }

    const QPointF p01 = (p0 + p1) * 0.5;
    const QPointF p12 = (p1 + p2) * 0.5;
    const QPointF p23 = (p2 + p3) * 0.5;
    const QPointF p012 = (p01 + p12) * 0.5;
    const QPointF p123 = (p12 + p23) * 0.5;
    const QPointF p0123 = (p012 + p123) * 0.5;

    flattenCurve(p0, p01, p012, p0123, tolerance, depth + 1, out);
    flattenCurve(p0123, p123, p23, p3, tolerance, depth + 1, out);
}

void flattenSubpath(const QPainterPath &path, int from, int to, double tolerance,
                    QVector<QPointF> *out)
{
    out->clear();
    if (to <= from)
        return;

    auto point = [&path](int index) {
        const QPainterPath::Element e = path.elementAt(index);
        return QPointF(e.x, e.y);
    };

    QPointF current = point(from);
    out->append(current);

    for (int i = from + 1; i <= to; ++i) {
        const QPainterPath::Element e = path.elementAt(i);
        switch (e.type) {
        case QPainterPath::LineToElement:
            current = point(i);
            out->append(current);
            break;
        case QPainterPath::CurveToElement: {
            // 三次曲线是三个连续元素：CurveToElement + 两个 CurveToDataElement
            if (i + 2 > to)
                break;
            const QPointF c1 = point(i);
            const QPointF c2 = point(i + 1);
            const QPointF end = point(i + 2);
            flattenCurve(current, c1, c2, end, tolerance, 0, out);
            current = end;
            i += 2;
            break;
        }
        case QPainterPath::CurveToDataElement:
            break; // 已经在上面一起处理了
        case QPainterPath::MoveToElement:
            break;
        }
    }
}

//! 把字形 path 拆成若干条闭合点列（每个子路径一条）
QVector<QVector<QPointF>> splitSubpaths(const QPainterPath &path, const QTransform &transform,
                                        double tolerance)
{
    QVector<QVector<QPointF>> result;

    int start = 0;
    const int count = path.elementCount();
    for (int i = 1; i <= count; ++i) {
        const bool isBreak = (i == count)
                             || (path.elementAt(i).type == QPainterPath::MoveToElement);
        if (!isBreak)
            continue;

        QVector<QPointF> pts;
        flattenSubpath(path, start, i - 1, tolerance, &pts);
        for (QPointF &p : pts)
            p = transform.map(p);
        if (pts.size() >= 2)
            result.append(pts);
        start = i;
    }
    return result;
}

/*!
 * 取字符的字形轮廓，输出"基线在原点、y 向下、单位磅"的闭合点列。
 *
 * 关于坐标系，一句话说清（之前在这里反复栽跟头）：
 *   QPainterPath::addText() 给出的坐标**已经是** y 向下、基线在原点、
 *   单位为磅了 —— 一个 18pt 的 'l' 轮廓是 (x, 0) .. (x, 15.2)，
 *   0 就是基线，负 y 在上方（'g' 的尾巴）。
 *   所以：
 *     - 不要再乘 1/64 之类的定点转换（会整体缩小 64 倍）；
 *     - 也**不要**再翻一次 y（会把字上下颠倒，而且把它挪到基线下面去）。
 *   这两件事以前都干过。
 */
const QVector<QVector<QPointF>> *glyphOutlines(const QString &text, const QFont &font)
{
    const QString key = glyphCacheKey(text, font);
    if (const auto *cached = glyphCache().object(key))
        return cached;

    QPainterPath path;
    path.addText(QPointF(0.0, 0.0), font, text);

    // 0.02pt ≈ 40pt 字号的千分之五，肉眼看不出折线；
    // 字形轮廓会被缓存，多切几段不影响后续绘制速度
    auto *outlines = new QVector<QVector<QPointF>>(splitSubpaths(path, QTransform(), 0.02));

    glyphCache().insert(key, outlines);
    return outlines;
}

/*!
 * 一行的基线在文档坐标里的 y 值。
 *
 * QTextLine::ascent() 是"行顶到基线"的距离，和 line.y() / line.height() 同一体系。
 * 不要再拿 QFontMetricsF::ascent() 自己拼：
 *   - 它的单位和布局单位一致，但值不一定等于 QTextLine::ascent()
 *     （行距、字体回退、上下标都会改）；
 *   - 更不能乘 DPI 系数 —— 布局坐标和字体度量本来就是同一个体系，
 *     乘了之后笔迹会整体偏大半个字（曾经就是这个 bug）。
 */
double lineBaselineY(const QTextLine &line)
{
    return line.y() + line.ascent();
}

/*!
 * 一个字符在文档坐标里的占位矩形（不含基线校正，交给 lineBaselineY 处理）。
 *
 * 不用 QTextCursor 的 cursorRect（Qt6 已没有这个公开接口），直接问 QTextLayout：
 *   - blockBoundingRect() 给出块在文档里的位置（已含 documentMargin）；
 *   - QTextLine::x() / y() 是行在块内 layout 里的偏移；
 *   - Qt6 里 cursorToX() 返回的是**行内**偏移（相对行首，不是相对块首！），
 *     所以还要加上 line.x()。
 * 少了 line.x() 这一步，所有笔迹都会整体左移一个正文左边距 —— 这正是之前的 bug。
 */
QRectF charRectAt(const QTextDocument *document, int position)
{
    if (!document)
        return QRectF();

    const QTextBlock block = document->findBlock(position);
    if (!block.isValid() || !block.layout())
        return QRectF();

    const int linePos = position - block.position();
    const QTextLine line = block.layout()->lineForTextPosition(linePos);
    if (!line.isValid())
        return QRectF();

    const QRectF blockRect = document->documentLayout()->blockBoundingRect(block);
    const double x0 = line.x() + line.cursorToX(linePos);
    const double x1 = line.x() + line.cursorToX(linePos + 1);

    return QRectF(blockRect.left() + qMin(x0, x1),
                  blockRect.top() + line.y(),
                  qMax(1.0, qAbs(x1 - x0)),
                  qMax(1.0, line.height()));
}

} // namespace

/*!
 * 一个字符的定位结果：(基线 y, 磅 -> 文档坐标的缩放)。
 * 结构体本身在头文件里（调用方要用它判断"这一格能不能画"）。
 *
 * 注意基线必须和 charRectAt 用同一套原点：**块顶（块在文档里的位置）也要加**。
 * 少了 blockRect.top() 的话第一行碰巧是对的（块顶为 0），
 * 从第二行起笔迹就会整体下移一个行高 —— 这正是"第二行开始错位"的 bug。
 */
BaselineScale baselineScaleAt(const QTextDocument *document, int position, const QFont &font)
{
    BaselineScale result;
    if (!document)
        return result;

    const QTextBlock block = document->findBlock(position);
    if (!block.isValid() || !block.layout())
        return result;
    const QTextLine line = block.layout()->lineForTextPosition(position - block.position());
    if (!line.isValid())
        return result;

    const QFontMetricsF fm(font);
    const double naturalH = fm.ascent() + fm.descent();
    if (naturalH <= 1e-6 || line.height() <= 1e-6)
        return result;

    const QRectF blockRect = document->documentLayout()->blockBoundingRect(block);
    result.baselineY = blockRect.top() + lineBaselineY(line);
    // 行高 / 自然字高：字号变了、行距变了，这个比例自动跟着变
    result.scale = line.height() / naturalH;
    result.valid = true;
    return result;
}


/*!
 * 把一个字符的字形轮廓用"平滑二维位移场"变形。
 *
 * 为什么不用"沿法线位移"（更直觉的那种做法）：
 *   法线位移是按**弧长**参数化的，两条相邻线段的法线可以差 90°，
 *   于是拐角两侧的点被推向完全不同的方向；位移量又和笔画宽度是同一个数量级时，
 *   轮廓必然自交 —— 用 Winding 填充后字形就碎成几块、中段整段消失
 *   （这正是"4504504"变成蓝色碎块的原因）。
 *
 * 改用二维位移场 d(x, y)：
 *   - 位移是位置的连续函数，相邻点位移几乎相同，笔画之间的相对关系不会断裂；
 *   - 幅度上限取"不同轮廓之间的最小距离的一半"：位移把任意两点最多拉近
 *     2×amp，只要 amp 小于这个上限，轮廓之间就不可能相交，
 *     映射保持单射 —— 字形有几个洞就还是几个洞
 *     （"0" 被填成一个实心椭圆，就是这个上限没算对造成的）。
 *
 * \param cycles    横竖两个方向各走几个噪声周期（2 附近就是"平缓的波"）
 * \param amplitude 期望的最大位移（文档单位），会被安全上限再钳一次
 */
QVector<QVector<QPointF>> deformGlyph(const QVector<QVector<QPointF>> &outlines,
                                      quint32 seed,
                                      double cycles,
                                      double amplitude)
{
    /*!
     * 安全幅度：找"不同轮廓之间的最小距离"。
     *
     * 直觉上会想用最小包围盒尺寸，但那是错的：
     * 一个 40pt 的"0"，包围盒 26x41，可它的笔画只有 2.3pt 宽 ——
     * 按包围盒算出来的上限（12pt）足以把洞整个推穿，外形和洞一交叠就被填实了。
     * 轮廓间距才是真正决定"能不能推"的量。
     */
    /*!
     * "不同轮廓之间的最近距离"是整个函数里最贵的一步（子路径两两配对 × 点两两配对），
     * 而且它的结果只被用作"幅度的上限"。所以做了三件事：
     *
     *   1. **用平方距离比较**，只在最后开一次根 —— `std::hypot` 一次几十纳秒，
     *      一个复杂字要调上千万次（实测：这一项就能差出十倍以上）；
     *   2. 先给每条轮廓算一个包围盒，两块盒子离得比当前最小距离还远就整对跳过；
     *   3. 盒子里最坏情况下才退化成原来的全量比较。
     *
     * 结果与逐点开根的老写法**完全相同**（只换算法，不换公式）。
     */
    const int count = outlines.size();
    QVector<QRectF> boxes;
    boxes.reserve(count);
    for (const QVector<QPointF> &o : outlines) {
        double minX = std::numeric_limits<double>::max();
        double maxX = std::numeric_limits<double>::lowest();
        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        for (const QPointF &p : o) {
            minX = qMin(minX, p.x());
            maxX = qMax(maxX, p.x());
            minY = qMin(minY, p.y());
            maxY = qMax(maxY, p.y());
        }
        boxes.append(minX > maxX ? QRectF() : QRectF(QPointF(minX, minY), QPointF(maxX, maxY)));
    }

    //! 两个矩形之间的最小距离（相交/挨着 = 0）；用平方值比较，不开根
    auto boxGapSq = [](const QRectF &a, const QRectF &b) {
        const double dx = qMax(0.0, qMax(b.left() - a.right(), a.left() - b.right()));
        const double dy = qMax(0.0, qMax(b.top() - a.bottom(), a.top() - b.bottom()));
        return dx * dx + dy * dy;
    };

    double minGapSq = std::numeric_limits<double>::max();
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (!boxes.at(i).isNull() && !boxes.at(j).isNull()
                && boxGapSq(boxes.at(i), boxes.at(j)) >= minGapSq)
                continue;
            const QVector<QPointF> &a = outlines.at(i);
            const QVector<QPointF> &b = outlines.at(j);
            for (const QPointF &p : a) {
                for (const QPointF &q : b) {
                    const double dx = p.x() - q.x();
                    const double dy = p.y() - q.y();
                    const double d = dx * dx + dy * dy;
                    minGapSq = qMin(minGapSq, d);
                }
            }
        }
    }
    double minGap = minGapSq == std::numeric_limits<double>::max() ? minGapSq
                                                                  : std::sqrt(minGapSq);

    /*!
     * 逐点求包围盒。
     *
     * 千万别写成 `box = box.isNull() ? QRectF(p, QSizeF(0,0)) : box.united(...)`：
     * 用 0 尺寸矩形初始化出来的 QRectF 是**空矩形**（isNull() 为真、isValid() 为假），
     * 而空矩形和任何矩形 united() 都还是空矩形 —— 包围盒永远是 0x0，
     * 于是下面那个 `box.width() < 1e-6` 的守卫直接 return，
     * 字形根本没被变形，画面上只剩一条被裁剪框切出来的横带。
     */
    QRectF box;
    {
        double minX = std::numeric_limits<double>::max();
        double maxX = std::numeric_limits<double>::lowest();
        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        for (const QVector<QPointF> &o : outlines) {
            for (const QPointF &p : o) {
                minX = qMin(minX, p.x());
                maxX = qMax(maxX, p.x());
                minY = qMin(minY, p.y());
                maxY = qMax(maxY, p.y());
            }
        }
        if (minX > maxX || minY > maxY)
            return outlines;
        box = QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
    }
    if (box.width() < 1e-6 || box.height() < 1e-6)
        return outlines;

    // 只有一个轮廓（H、L 这类）时没有"间距"可算，退回到最小包围盒尺寸
    if (minGap == std::numeric_limits<double>::max())
        minGap = qMin(box.width(), box.height()) * 0.6;

    const double safe = qMax(0.02, minGap * 0.45);
    const double amp = qBound(0.0, amplitude, safe);
    if (amp < 1e-4)
        return outlines;

    QRandomGenerator rng(seed);
    const double phaseX = rng.generateDouble() * 2.0 * M_PI;
    const double phaseY = rng.generateDouble() * 2.0 * M_PI;
    const double phaseC = rng.generateDouble() * 2.0 * M_PI;
    // 横竖用同一个空间频率：频率不一致会让一个字在某个方向上被拉歪
    const double freq = qMax(0.4, cycles) / qMax(box.width(), box.height());

    const QPointF center = box.center();

    auto wave = [&](double u, double v, double phase) {
        return 0.62 * std::sin(2.0 * M_PI * freq * u + phase)
               + 0.38 * std::sin(2.0 * M_PI * freq * v + phase + phaseC);
    };

    auto field = [&](const QPointF &p) {
        const double u = p.x() - box.left();
        const double v = p.y() - box.top();
        // 减去中心处的值：整个字不会被平移，基线和字宽保持稳定
        const double cu = center.x() - box.left();
        const double cv = center.y() - box.top();
        const double dx = wave(u, v, phaseX) - wave(cu, cv, phaseX);
        const double dy = wave(v, u, phaseY) - wave(cv, cu, phaseY);
        return QPointF(dx * amp, dy * amp);
    };

    QVector<QVector<QPointF>> result;
    result.reserve(outlines.size());
    for (const QVector<QPointF> &o : outlines) {
        QVector<QPointF> moved;
        moved.reserve(o.size());
        for (const QPointF &p : o)
            moved.append(p + field(p));
        // 闭合轮廓首尾必须一致，否则填充会留缝
        if (moved.size() >= 2 && (o.first() - o.last()).manhattanLength() < 1e-6)
            moved.last() = moved.first();
        result.append(moved);
    }
    return result;
}

/*!
 * 把若干条轮廓拼成一个填充路径。
 *
 * 为什么不用逐条 drawPolygon(..., Qt::WindingFill)：
 * 实测那样填不出字形的"洞" —— "0"、"回"、"4" 会变成实心块
 * （同一份数据换 QPainterPath + OddEvenFill 就是对的，见 --uitest 的对照图）。
 *
 * 这里的做法是图形学里的标准套路：按包围盒面积从大到小加入子路径，
 * 用奇偶规则填充。外形先加、洞后加，任一射线穿过"外形 + 洞"两次即为空。
 */
QPainterPath buildGlyphPath(const QVector<QVector<QPointF>> &outlines)
{
    QVector<const QVector<QPointF> *> sorted;
    sorted.reserve(outlines.size());
    for (const QVector<QPointF> &o : outlines) {
        if (o.size() >= 3)
            sorted.append(&o);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const QVector<QPointF> *a, const QVector<QPointF> *b) {
                  const QRectF ra = QPolygonF(*a).boundingRect();
                  const QRectF rb = QPolygonF(*b).boundingRect();
                  return ra.width() * ra.height() > rb.width() * rb.height();
              });

    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    for (const QVector<QPointF> *o : sorted) {
        path.addPolygon(QPolygonF(*o));
        path.closeSubpath();
    }
    return path;
}

// ---------------------------------------------------------------- 字体族兼容层

/*!
 * 求一个字符真正被排版时用的字体。
 *
 * 铁律：**不能直接用 QTextCharFormat::font()**。
 * 它返回的是"格式里显式设过的属性"，没设的属性会落到 QFont 的默认值
 * （字号 0 → QApplication 的默认字号），而不是 QTextDocument 的默认字体。
 * 文档里只有一部分字显式设过字号时（比如"整篇 28pt、其中几个字被单独改过"），
 * fmt.font() 拿到的字号就是错的 —— 由此算出的"行高 / 自然字高"比例会差一倍，
 * 笔迹要么缩成一半、要么涨成两倍（屏幕上就是"每行都错位、字还翻了个个儿"的观感）。
 *
 * 正确做法是按 QTextDocument 自己的规则合并：默认字体 + 块格式 + 片段格式。
 */
QFont effectiveFont(const QTextDocument *document, const QTextBlock &block,
                    const QTextCharFormat &format)
{
    QFont font = document ? document->defaultFont() : QFont();
    font = block.charFormat().font().resolve(font);
    return format.font().resolve(font);
}

QFont effectiveFont(const QTextDocument *document, const QTextCharFormat &format)
{
    QFont font = document ? document->defaultFont() : QFont();
    return format.font().resolve(font);
}

/*!
 * 生成被噪声扭曲过的字形轮廓（对外接口，屏幕 / 打印 / 导出共用）。
 *
 * \param scale     磅 -> 文档坐标的缩放（基线原点仍在 (0,0)）
 * \param amplitude 噪声最大位移（文档单位）
 * \param cycles    横竖方向各走几个噪声周期
 *
 * \a wave 只用来提供"这条噪声是随机的"这件事的种子；真正的波形由
 * deformGlyph 里的二维位移场生成（原因见 deformGlyph 的注释）。
 */
QVector<QVector<QPointF>> distortedGlyphOutlines(const QString &text,
                                                 const QFont &font,
                                                 const NoiseWave &wave,
                                                 quint32 seed,
                                                 double scale,
                                                 double amplitude,
                                                 double cycles)
{
    Q_UNUSED(wave);
    const QString key = QStringLiteral("%1|%2|%3|%4|%5")
                            .arg(glyphCacheKey(text, font))
                            .arg(seed)
                            .arg(scale, 0, 'g', 10)
                            .arg(amplitude, 0, 'g', 10)
                            .arg(cycles, 0, 'g', 10);
    if (const auto *cached = distortCache().object(key))
        return *cached;

    QVector<QVector<QPointF>> result;
    const QVector<QVector<QPointF>> *outlines = glyphOutlines(text, font);
    if (!outlines)
        return result;

    QVector<QVector<QPointF>> scaled;
    scaled.reserve(outlines->size());
    for (const QVector<QPointF> &outline : *outlines) {
        if (outline.size() < 3)
            continue;
        QVector<QPointF> s;
        s.reserve(outline.size());
        for (const QPointF &p : outline)
            s.append(p * scale);
        scaled.append(s);
    }

    if (!scaled.isEmpty())
        result = deformGlyph(scaled, seed, cycles, amplitude);

    distortCache().insert(key, new QVector<QVector<QPointF>>(result));
    return result;
}

void setFormatFontFamily(QTextCharFormat *format, const QString &family)
{
    if (!format)
        return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 1, 0)
    if (family.isEmpty())
        format->clearProperty(QTextFormat::FontFamilies);
    else
        format->setFontFamilies(QStringList{family});
#else
    if (family.isEmpty())
        format->clearProperty(QTextFormat::FontFamily);
    else
        format->setFontFamily(family);
#endif
}

QString formatFontFamily(const QTextCharFormat &format)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 1, 0)
    const QStringList families = format.fontFamilies().toStringList();
    return families.value(0);
#else
    return format.fontFamily();
#endif
}

// ---------------------------------------------------------------- 手写笔迹

/*!
 * 按每点的线宽逐段画一条折线（笔压就是逐点变的，一条线一个笔宽画不出来）。
 *
 * 做法：把线宽量化成 0.1 单位一档，**同一档的连续点合成一条折线**，
 * 换档处把上一条收在换档的那个点上、下一条从同一个点起笔 ——
 * 圆头圆角笔帽会让接缝连成一体，看不出断口。
 * 不量化的话就是"每两个点画一条线"，一条笔画几百次 drawLine，白费。
 *
 * \param widths 与 \a points 等长；某个点为负表示"这个点没有笔压"，用 \a fallbackWidth
 */
void drawPressurePolyline(QPainter *painter,
                          const QVector<QPointF> &points,
                          const QVector<double> &widths,
                          const QColor &color,
                          double fallbackWidth)
{
    if (!painter || points.isEmpty())
        return;

    const int n = points.size();
    const double fallback = qMax(0.05, fallbackWidth);

    auto widthAt = [&widths, fallback](int i) {
        const double w = (i >= 0 && i < widths.size()) ? widths.at(i) : -1.0;
        return w >= 0.0 ? w : fallback;
    };

    if (n == 1) {
        const double r = qMax(0.05, widthAt(0)) / 2.0;
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawEllipse(points.first(), r, r);
        return;
    }

    constexpr double kBucket = 0.1; // 线宽量化档距（文档单位）
    auto bucketOf = [&widthAt](int i) {
        return int(std::lround(widthAt(i) / kBucket));
    };

    QPen pen(color);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setBrush(Qt::NoBrush);

    auto drawRun = [&](int from, int to, int bucket) {
        if (to <= from)
            return;
        /*!
         * 直接用 `constData() + from` 画，不做 `points.mid()`：
         * 每换一次线宽档就复制一遍点列，全篇两千个字下来是一堆白白的分配。
         */
        pen.setWidthF(qMax(0.05, bucket * kBucket));
        painter->setPen(pen);
        painter->drawPolyline(points.constData() + from, to - from + 1);
    };

    int start = 0;
    int bucket = bucketOf(0);
    for (int i = 1; i < n; ++i) {
        const int b = bucketOf(i);
        if (b == bucket)
            continue;
        /*!
         * 换档发生在点 i 上：这一段画到 i-1 为止（用旧档），
         * 下一段从 i-1 起笔（用新档）—— 两段共点 i-1，接缝不断，
         * 而且 i 自己的线宽不会被旧档吞掉。
         */
        drawRun(start, i - 1, bucket);
        start = i - 1;
        bucket = b;
    }
    drawRun(start, n - 1, bucket);
}

QRectF handwritingTargetRect(const QRectF &cell, const HandwritingAdjustment &adjust)
{
    // xml 是文本文件，手改出来的离谱值在这里统一钳住，别让它把界面搞崩
    const double size = qBound(0.05, adjust.size, 10.0);
    const double shift = qBound(-1.0, adjust.baseline, 1.0);

    const double w = cell.width() * size;
    const double h = cell.height() * size;
    const double bottom = cell.bottom() + shift * cell.height();

    /*!
     * 宽度和高度一起乘 size，但**以格子中线为准**摆回去。
     *
     * 高度不乘的话，"宽度受限"的样本（一横、'一' 这种宽而扁的）放不大：
     * drawHandwritingSample 取的是 min(高比例, 宽比例)，宽那一头没变，整体就不变。
     * 而普通的"格子中心对齐"写法会随着 size 把字形越推越右，
     * 所以这里必须自己按中线摆，不能直接用 cell.left() + cell.width()*size。
     */
    return QRectF(cell.center().x() - w / 2.0, bottom - h, w, h);
}

/*!
 * 把一个手写样本画到 \a target 里。
 *
 * 两条规矩：
 *   1. **等比缩放到完全塞进 target**（宽、高两个方向取更小的那个比例）。
 *      以前只按高度缩放，pattern 一宽就横向溢出格子：字压到邻居身上，
 *      靠正文区边缘的那些还会被裁掉（"手写字被砍掉一半"）。
 *   2. 字形**水平居中、底边压在 target 底边上** —— 调用方给的 target
 *      底边就是基线，按宽度缩小之后字形会变矮，这时若还按顶边对齐，
 *      墨迹就会浮到基线上面去。
 *
 * \param maxStrokeWidth 笔压 1.0 对应的线宽（文档单位，也就是"字号"）：
 *        每个点的线宽 = 该点笔压 × maxStrokeWidth，0~1 线性映射。
 *        传 <= 0 表示没有字号信息，整体回退到"字高的固定比例"。
 */
void drawHandwritingSample(QPainter *painter,
                           const HandwritingSample &sample,
                           const QRectF &target,
                           const QColor &color,
                           const NoiseWave *wave,
                           double amplitude,
                           double waveScale,
                           double maxStrokeWidth)
{
    if (!painter || sample.isEmpty())
        return;

    const QRectF box = sample.bbox();
    const double sampleH = box.height();
    const double sampleW = box.width();

    const double targetH = target.height();
    const double targetW = target.width();
    if (targetH <= 1e-6 || targetW <= 1e-6)
        return;

    /*!
     * 等比缩放：宽、高各自算一个"塞得进去"的比例，取小的那个。
     * 某一维为 0 的样本（'i' 的点、'一' 这种一笔横）只有另一维有意义，
     * 那一维给一个"无穷大"的比例，等于不参与比较。
     */
    const double infinite = std::numeric_limits<double>::max();
    const double fitH = sampleH > 1e-6 ? targetH / sampleH : infinite;
    const double fitW = sampleW > 1e-6 ? targetW / sampleW : infinite;
    double scale = qMin(fitH, fitW);
    if (!std::isfinite(scale) || scale <= 0.0)
        scale = 1.0;

    const double glyphW = sampleW * scale;
    const double glyphH = sampleH * scale;
    const double originX = target.left() + (targetW - glyphW) / 2.0;
    const double originY = target.bottom() - glyphH;

    // 没有笔压数据时的兜底线宽：字高的 7%（老行为）
    const double fallbackWidth = qBound(0.4, targetH * 0.07, 6.0);
    /*!
     * 笔压 0 不能真的画成 0 宽：圆头笔帽画不出宽度 0 的线，
     * 笔画中间会出现断口。给一个相对下限，视觉上仍是一条细线。
     */
    const double minWidth = fallbackWidth * 0.35;

    painter->save();
    for (const auto &stroke : sample.strokes) {
        if (stroke.isEmpty())
            continue;

        QVector<QPointF> points;
        QVector<double> widths;
        points.reserve(stroke.size());
        widths.reserve(stroke.size());
        for (const HandwritingPoint &p : stroke) {
            points.append(QPointF(originX + p.pos.x() * scale, originY + p.pos.y() * scale));
            if (p.pressure < 0.0 || maxStrokeWidth <= 0.0)
                widths.append(-1.0); // 没有笔压：交给兜底线宽
            else
                widths.append(qMax(p.pressure * maxStrokeWidth, minWidth));
        }

        if (wave && wave->isValid() && amplitude > 0.0) {
            points = distortPolyline(points, *wave, amplitude, waveScale, true);
            points = smoothPolyline(points, 1);
        }

        drawPressurePolyline(painter, points, widths, color, fallbackWidth);
    }
    painter->restore();
}

// ---------------------------------------------------------------- 主渲染

/*!
 * 把一个段落里带效果的字符收集成绘制计划。
 *
 * 单独抽出来是因为"隐藏原字"也要用同一份信息：绘制正文的那一步必须先知道
 * 哪些格子会被变形后的字形替换掉（见 EffectDrawItem 的注释）。
 * 两边各走一遍文档遍历的话，迟早会不一致 —— 一处改了、另一处忘了。
 *
 * 按段落为单位（而不是全篇一次）是给计算层用的：见 planEffectsForBlock 的声明。
 */
QVector<EffectDrawItem> planEffectsForBlock(const QTextDocument *document,
                                            const QTextBlock &block,
                                            const EffectRenderOptions &options,
                                            QVector<QPair<int, int>> *hiddenRanges)
{
    QVector<EffectDrawItem> items;
    if (!document || !block.isValid() || !options.anyLayer())
        return items;

    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid())
            continue;
        const QTextCharFormat fmt = fragment.charFormat();
        const EffectStyle style = effectStyle(fmt);
        if (!style.isValid())
            continue;

        const QString text = fragment.text();
        const int basePos = fragment.position();

        for (int i = 0; i < text.size(); ++i) {
            const QChar ch = text.at(i);
            if (ch.isSpace() || ch == QChar::ParagraphSeparator
                || ch == QChar::LineSeparator)
                continue;

            const int pos = basePos + i;
            const QRectF charRect = charRectAt(document, pos);
            if (!charRect.isValid() || charRect.height() <= 1.0)
                continue;

            EffectDrawItem item;
            item.character = ch;
            item.position = pos;
            item.charRect = charRect;
            item.font = effectiveFont(document, block, fmt);
            item.fit = baselineScaleAt(document, pos, item.font);
            item.seed = mixSeed(style.seed, quint32(ch.unicode()));
            item.style = style;
            item.foreground = fmt.foreground().color();
            item.hasForeground = (fmt.foreground().style() != Qt::NoBrush)
                                 && item.foreground.isValid();

            /*!
             * 只有"会把这一格整格替换掉"的层才需要隐藏原字：
             *   - 扭曲层开启替换模式时，字形轮廓已经完全顶替原字；
             *   - 手写层开启"笔迹盖住正文"时同理。
             * 叠加模式（distortionReplaceText = false）是故意让原字露出来的，
             * 那种情况下一格都不能隐藏。
             *
             * 注意必须写进 item **之后**才 append（追加的是副本）。
             */
            item.replaceIntent =
                (options.showDistortion && style.kind == EffectKind::Distortion
                 && options.distortionReplaceText && item.fit.valid && options.wave
                 && options.wave->isValid())
                || (options.showHandwriting && style.kind == EffectKind::Handwriting
                    && options.handwritingReplaceText);
            /*!
             * 真正"挖洞"要等几何算好（见 prepareEffectItem）：
             * 几何没好就先挖洞，屏幕上会出现"原字没了、新字还没来"的空窗。
             */
            item.hidden = false;
            items.append(item);

            if (item.replaceIntent && hiddenRanges)
                hiddenRanges->append(qMakePair(pos, 1));
        }
    }

    // 相邻的位置合并成区间，调用方可以一次设一大段格式
    if (hiddenRanges) {
        std::sort(hiddenRanges->begin(), hiddenRanges->end());
        QVector<QPair<int, int>> merged;
        for (const QPair<int, int> &r : *hiddenRanges) {
            if (!merged.isEmpty() && merged.last().first + merged.last().second == r.first)
                merged.last().second += r.second;
            else
                merged.append(r);
        }
        *hiddenRanges = merged;
    }
    return items;
}

/*!
 * 全篇的计划 = 逐段计划拼起来（给打印 / 导出 / 自检这类"一次性"场合用）。
 */
QVector<EffectDrawItem> planEffects(const QTextDocument *document,
                                    const EffectRenderOptions &options,
                                    QVector<QPair<int, int>> *hiddenRanges)
{
    QVector<EffectDrawItem> all;
    if (!document || !options.anyLayer())
        return all;

    QVector<QPair<int, int>> hidden;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next())
        all += planEffectsForBlock(document, block, options, &hidden);

    if (hiddenRanges)
        *hiddenRanges = hidden;
    return all;
}

/*!
 * 把整批项算成可绘制几何（**同步**）。
 *
 * 交互式绘制**不要**用它 —— 它会把这一批全算完才返回（实测 2000 字 585ms，
 * 界面就僵在那里）。它只给这些场合用：打印 / 导出（本来就是模态操作）、
 * 校对表这类一次性绘制、以及自检。
 *
 * 交互式绘制走 effectplanner.h 里那个"分片算 + 进度条"的路子。
 */
void prepareEffects(QVector<EffectDrawItem> *items, const EffectRenderOptions &options)
{
    if (!items)
        return;
    for (EffectDrawItem &item : *items)
        prepareEffectItem(&item, options);
}
/*!
 * 把手写样本换算成"文档坐标下的折线 + 逐点笔压"。
 *
 * 这段计算原来是 `drawHandwritingSample()` 里每帧都做一遍的：逐点坐标换算、
 * （有噪声时）重采样 + 滑动平均平滑。全篇两千个字就是几十毫秒一帧，
 * 现在挪到准备阶段一次性算完，绘制时只剩"按档位落笔"。
 *
 * 落点公式与 `drawHandwritingSample()` 逐项一致（就是从这里搬的）。
 */
static void prepareHandwritingGeometry(const HandwritingSample &sample,
                                       const QRectF &target,
                                       const NoiseWave *wave,
                                       double amplitude,
                                       double waveScale,
                                       PreparedChar *out)
{
    if (!out || sample.isEmpty())
        return;

    const QRectF box = sample.bbox();
    const double sampleH = box.height();
    const double sampleW = box.width();
    const double targetH = target.height();
    const double targetW = target.width();
    if (targetH <= 1e-6 || targetW <= 1e-6)
        return;

    /*!
     * 等比缩放：宽、高各自算一个"塞得进去"的比例，取小的那个。
     * 某一维为 0 的样本（'i' 的点、'一' 这种一笔横）只有另一维有意义。
     */
    const double infinite = std::numeric_limits<double>::max();
    const double fitH = sampleH > 1e-6 ? targetH / sampleH : infinite;
    const double fitW = sampleW > 1e-6 ? targetW / sampleW : infinite;
    double scale = qMin(fitH, fitW);
    if (!std::isfinite(scale) || scale <= 0.0)
        scale = 1.0;

    const double glyphW = sampleW * scale;
    const double glyphH = sampleH * scale;
    const double originX = target.left() + (targetW - glyphW) / 2.0;
    const double originY = target.bottom() - glyphH;

    // 没有笔压数据时的兜底线宽：字高的 7%（老行为）
    out->fallbackStrokeWidth = qBound(0.4, targetH * 0.07, 6.0);
    out->minStrokeWidth = out->fallbackStrokeWidth * 0.35;

    out->strokes.reserve(sample.strokes.size());
    out->strokePressure.reserve(sample.strokes.size());
    for (const auto &stroke : sample.strokes) {
        if (stroke.isEmpty())
            continue;

        QVector<QPointF> points;
        QVector<double> pressure;
        points.reserve(stroke.size());
        pressure.reserve(stroke.size());
        for (const HandwritingPoint &p : stroke) {
            points.append(QPointF(originX + p.pos.x() * scale, originY + p.pos.y() * scale));
            pressure.append(p.pressure);
        }

        if (wave && wave->isValid() && amplitude > 0.0) {
            points = distortPolyline(points, *wave, amplitude, waveScale, true);
            points = smoothPolyline(points, 1);
        }

        out->strokes.append(points);
        out->strokePressure.append(pressure);
    }

    out->ready = !out->strokes.isEmpty();
}

/*!
 * 把一个计划项算成可绘制几何。
 * 把一个计划项算成可绘制几何。
 *
 * 这里是原来 `renderEffects()` 里那份"每帧都重算"的代码，原样搬过来 ——
 * 一字未改的公式是故意的：搬家的目的是**换时间**，不是改画面。
 * 屏幕、打印、导出画的还是同一份几何。
 */
void prepareEffectItem(EffectDrawItem *item, const EffectRenderOptions &options)
{
    if (!item || item->geometryDone)
        return;
    item->geometryDone = true;
    item->art = PreparedChar();
    item->hidden = false;

    const QChar ch = item->character;
    if (ch.isNull() || !item->fit.valid)
        return;

    const QString one(ch);
    const QFont &font = item->font;
    const QRectF &charRect = item->charRect;
    const BaselineScale &fit = item->fit;
    const quint32 seed = item->seed;

    // ---- 手写层：把样本换算成"文档坐标下的折线 + 逐点笔压" ----
    if (options.showHandwriting && item->style.kind == EffectKind::Handwriting) {
        const HandwritingSample *sample =
            options.library ? options.library->pick(one, seed) : nullptr;
        if (!sample) {
            item->art.missingData = true;
            return;
        }

        const QFontMetricsF fm(font);
        /*!
         * 手写 pattern 的包围盒正好填满字身的 ascent..descent，
         * 所以按"行内比例"缩放、把它的下边缘压在基线上就能对齐正文。
         * 缩放用 fit.scale（行高 / 自然字高），和扭曲层同一个口径 ——
         * 换字号、改行距时两层不会各走各的。
         */
        const double targetH = (fm.ascent() + fm.descent()) * fit.scale;
        const QRectF cell(charRect.left(), fit.baselineY - targetH, charRect.width(), targetH);
        const QRectF target = handwritingTargetRect(cell, options.library->adjustment(*sample));

        NoiseWave localWave;
        const NoiseWave *wave = nullptr;
        double amplitude = 0.0;
        if (options.wave && options.amplitudePt > 0.0) {
            localWave.reseed(mixSeed(seed, 0x5F356495u));
            wave = &localWave;
            amplitude = options.amplitudePt * fit.scale * 0.6;
        }

        prepareHandwritingGeometry(*sample, target, wave, amplitude, options.waveScale, &item->art);
    }

    // ---- 扭曲层：字形轮廓 -> 噪声变形 -> 一个填充路径 ----
    if (options.showDistortion && item->style.kind == EffectKind::Distortion && options.wave
        && options.wave->isValid()) {
        // 幅度按行高缩放：换字号之后"抖多少"看起来是一样的。
        // 真正的上限由 deformGlyph 按字形的最小特征尺寸再钳一次。
        const double amplitude = qMax(0.05, options.amplitudePt * fit.scale);
        const QVector<QVector<QPointF>> outlines =
            distortedGlyphOutlines(one, font, *options.wave, seed, fit.scale, amplitude,
                                   options.waveScale);
        if (!outlines.isEmpty()) {
            /*!
             * 用整字拼一个填充路径：外形和"洞"一起交给奇偶规则，
             * 逐条 drawPolygon 填不出洞（详见 buildGlyphPath 的注释）。
             */
            item->art.fillPath = buildGlyphPath(outlines);
            /*!
             * 定位铁律（扭曲层）：字形轮廓已经是"基线在原点、y 向下"，
             * 所以绘制原点就是**基线**，一项都不用再加。
             * 墨迹自然落在 [基线 - ascent, 基线 + descent]，正好是行框里那一段。
             */
            item->art.origin = QPointF(charRect.left(), fit.baselineY);
            item->art.ready = !item->art.fillPath.isEmpty();
        }
    }

    /*!
     * 几何真的算好了才挖洞。
     *
     * 反过来说：几何还没算好的格子，正文照原样画着 ——
     * 用户看到的是一页字"一个一个"变成手写/扭曲，而不是先空一片再长出来。
     */
    item->hidden = item->replaceIntent && item->art.ready;
}

void drawEffectItem(QPainter *painter, const EffectDrawItem &item, const EffectRenderOptions &options)
{
    if (!painter)
        return;

    const PreparedChar &art = item.art;
    if (art.missingData) {
        // 缺数据的字符记下来（调用方拿去报"哪些字没铺上手写"）
        options.missing.insert(QString(item.character));
        return;
    }
    if (!art.ready)
        return;

    // ---- 手写层 ----
    if (options.showHandwriting && !art.strokes.isEmpty()) {
        /*!
         * 笔压映射：线宽 = 笔压 × 字号 × pressureToWidth。
         * 笔压是在准备阶段存下来的，这里只乘系数 —— 所以拖"笔宽"滑块
         * 不需要把几何重算一遍。
         */
        const double emDoc = qMax(1.0, item.font.pointSizeF()) * item.fit.scale;
        const double maxStrokeWidth = emDoc * qMax(0.0, options.pressureToWidth);
        const double fallback = qMax(0.05, art.fallbackStrokeWidth);

        for (int s = 0; s < art.strokes.size(); ++s) {
            const QVector<QPointF> &points = art.strokes.at(s);
            const QVector<double> &pressure = art.strokePressure.at(s);
            QVector<double> widths;
            widths.reserve(pressure.size());
            for (double p : pressure) {
                if (p < 0.0 || maxStrokeWidth <= 0.0)
                    widths.append(-1.0); // 没有笔压：交给兜底线宽
                else
                    widths.append(qMax(p * maxStrokeWidth, art.minStrokeWidth));
            }
            drawPressurePolyline(painter, points, widths, options.handwritingColor, fallback);
        }
    }

    // ---- 扭曲层 ----
    if (options.showDistortion && !art.fillPath.isEmpty()) {
        // 颜色：优先用字符自己的前景色（富文本颜色），没设过才用默认色
        QColor fill = item.hasForeground ? item.foreground : options.distortionColor;
        if (!fill.isValid())
            fill = options.distortionColor;
        /*!
         * 替换模式：**原字根本不会被画出来**（正文那一步把这一格挖掉了），
         * 所以这里绝对不能再刷一块纸色当底 —— 那块底色盖在选区高亮之上，
         * 会把选框啃掉一圈。
         *
         * 叠加模式则把扭曲层降一点透明度，方便对照"改了哪些笔画"。
         */
        if (!options.distortionReplaceText && fill.alpha() == 255)
            fill.setAlpha(205);

        const QRectF &charRect = item.charRect;
        painter->save();
        painter->translate(art.origin);
        // 裁剪防溢：扭曲后的笔画不许爬进旁边的字
        painter->setClipRect(QRectF(-1.0, -charRect.height(), charRect.width() + 2.0,
                                    charRect.height() * 3.0));
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawPath(art.fillPath);
        painter->restore();
    }
}

void renderPreparedEffects(QPainter *painter,
                           const QVector<EffectDrawItem> &items,
                           const EffectRenderOptions &options)
{
    if (!painter || !options.anyLayer())
        return;
    for (const EffectDrawItem &item : items)
        drawEffectItem(painter, item, options);
}

/*!
 * 同步版：计划 + 算几何 + 绘制一次干完。
 *
 * 打印、导出、自检、校对表这类"一次性"场合用它（本来就是模态操作）。
 * **交互式绘制不要用**：2000 字的稿子这里要 585ms，界面会僵在那里 ——
 * 那正是这次重构要治的病（见 jobrunner.h / effectplanner.h）。
 */
void renderEffects(QPainter *painter,
                   const QTextDocument *document,
                   const EffectRenderOptions &options)
{
    if (!painter || !document || !options.anyLayer())
        return;

    QVector<EffectDrawItem> items = planEffects(document, options);
    prepareEffects(&items, options);
    renderPreparedEffects(painter, items, options);
}

// ---------------------------------------------------------------- 调试探针

QString tripaDumpGlyphOutline(const QString &text, const QFont &font,
                              const NoiseWave &wave, quint32 seed,
                              double scale, double amplitude, double waveScale,
                              const QString &filePath)
{
    QString out;
    QTextStream stream(&out);

    const QVector<QVector<QPointF>> *raw = glyphOutlines(text, font);
    stream << "glyph " << text << " font=" << font.family() << " " << font.pointSizeF()
           << "pt  子路径=" << (raw ? raw->size() : -1) << "\n";
    if (raw) {
        for (int i = 0; i < raw->size(); ++i) {
            const QVector<QPointF> &o = raw->at(i);
            QRectF box;
            for (const QPointF &p : o)
                box = box.isNull() ? QRectF(p, QSizeF(0, 0)) : box.united(QRectF(p, QSizeF(0, 0)));
            const double arc = [&o] {
                double sum = 0;
                for (int k = 1; k < o.size(); ++k)
                    sum += std::hypot(o[k].x() - o[k - 1].x(), o[k].y() - o[k - 1].y());
                return sum;
            }();
            stream << "  raw[" << i << "] 点数=" << o.size() << " 弧长=" << QString::number(arc, 'f', 1)
                   << " 包围盒=" << QString::number(box.left(), 'f', 1) << ","
                   << QString::number(box.top(), 'f', 1) << " "
                   << QString::number(box.width(), 'f', 1) << "x"
                   << QString::number(box.height(), 'f', 1) << "\n";
        }
    }

    const QVector<QVector<QPointF>> distorted =
        distortedGlyphOutlines(text, font, wave, seed, scale, amplitude, waveScale);
    stream << "扭曲后 子路径=" << distorted.size() << "\n";
    for (int i = 0; i < distorted.size(); ++i) {
        const QVector<QPointF> &o = distorted.at(i);
        QRectF box;
        for (const QPointF &p : o)
            box = box.isNull() ? QRectF(p, QSizeF(0, 0)) : box.united(QRectF(p, QSizeF(0, 0)));
        // 相邻点距：如果中段点距突然变成 0 或者巨大，说明重采样/平滑出了问题
        double minStep = 1e9, maxStep = 0, sumStep = 0;
        int zeroSteps = 0;
        for (int k = 1; k < o.size(); ++k) {
            const double d = std::hypot(o[k].x() - o[k - 1].x(), o[k].y() - o[k - 1].y());
            minStep = qMin(minStep, d);
            maxStep = qMax(maxStep, d);
            sumStep += d;
            if (d < 1e-9)
                ++zeroSteps;
        }
        stream << "  dis[" << i << "] 点数=" << o.size()
               << " 包围盒=" << QString::number(box.left(), 'f', 1) << ","
               << QString::number(box.top(), 'f', 1) << " "
               << QString::number(box.width(), 'f', 1) << "x"
               << QString::number(box.height(), 'f', 1)
               << " 点距 min/max/avg=" << QString::number(o.size() > 1 ? minStep : 0, 'f', 2) << "/"
               << QString::number(maxStep, 'f', 2) << "/"
               << QString::number(o.size() > 1 ? sumStep / (o.size() - 1) : 0, 'f', 2)
               << " 零长段=" << zeroSteps << "\n";
    }

    QFile f(filePath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream fs(&f);
        fs << out;
    }
    return out;
}

QStringList findMissingHandwriting(const QTextDocument *document,
                                   const HandwritingLibrary *library)
{
    QStringList missing;
    QSet<QString> seen;
    if (!document)
        return missing;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            const EffectStyle style = effectStyle(fragment.charFormat());
            if (style.kind != EffectKind::Handwriting)
                continue;
            const QString text = fragment.text();
            for (const QChar &ch : text) {
                if (ch.isSpace())
                    continue;
                const QString one(ch);
                if (library && library->contains(one))
                    continue;
                if (!seen.contains(one)) {
                    seen.insert(one);
                    missing.append(one);
                }
            }
        }
    }
    return missing;
}
