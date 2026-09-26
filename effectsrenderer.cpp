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

/*!
 * 一个字符的定位结果：(基线 y, 磅 -> 文档坐标的缩放)。
 * 缩放一律用行内比例，不做任何 DPI 换算 —— 布局坐标和字体度量本来就是同一个体系。
 *
 * 注意基线必须和 charRectAt 用同一套原点：**块顶（块在文档里的位置）也要加**。
 * 少了 blockRect.top() 的话第一行碰巧是对的（块顶为 0），
 * 从第二行起笔迹就会整体下移一个行高 —— 这正是"第二行开始错位"的 bug。
 */
struct BaselineScale
{
    double baselineY = 0.0;
    double scale = 1.0;
    bool valid = false;
};

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

} // namespace

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
    double minGap = std::numeric_limits<double>::max();
    for (int i = 0; i < outlines.size(); ++i) {
        for (int j = i + 1; j < outlines.size(); ++j) {
            const QVector<QPointF> &a = outlines.at(i);
            const QVector<QPointF> &b = outlines.at(j);
            for (const QPointF &p : a) {
                for (const QPointF &q : b) {
                    const double d = std::hypot(p.x() - q.x(), p.y() - q.y());
                    minGap = qMin(minGap, d);
                }
            }
        }
    }

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

void drawHandwritingSample(QPainter *painter,
                           const HandwritingSample &sample,
                           const QRectF &target,
                           const QColor &color,
                           const NoiseWave *wave,
                           double amplitude,
                           double waveScale,
                           bool anchorLeftTop)
{
    if (!painter || sample.isEmpty())
        return;

    const QRectF box = sample.bbox();
    const double sampleH = box.height();
    const double sampleW = box.width();

    const double targetH = target.height();
    if (targetH <= 1e-6)
        return;

    // 按高度等比缩放：手写 pattern 保留采集时的宽高比
    double scale = 1.0;
    if (sampleH > 1e-6)
        scale = targetH / sampleH;
    else if (sampleW > 1e-6)
        scale = target.width() / sampleW;

    double originX = target.left();
    double originY = target.top();
    if (!anchorLeftTop && sampleW > 1e-6) {
        const double scaledW = sampleW * scale;
        if (scaledW < target.width())
            originX = target.left() + (target.width() - scaledW) / 2.0;
    }

    QPen pen(color);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    // 笔画粗细约等于字高的 7%，并限制在合理范围内
    pen.setWidthF(qBound(0.6, targetH * 0.07 * qMax(1.0, scale), 5.0));

    painter->save();
    for (const auto &stroke : sample.strokes) {
        if (stroke.isEmpty())
            continue;

        QVector<QPointF> points;
        points.reserve(stroke.size());
        for (const QPointF &p : stroke)
            points.append(QPointF(originX + p.x() * scale, originY + p.y() * scale));

        if (wave && wave->isValid() && amplitude > 0.0) {
            points = distortPolyline(points, *wave, amplitude, waveScale, true);
            points = smoothPolyline(points, 1);
        }

        if (points.size() == 1) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(color);
            const double r = pen.widthF() / 2.0;
            painter->drawEllipse(points.first(), r, r);
            continue;
        }

        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawPolyline(points.constData(), points.size());
    }
    painter->restore();
}

// ---------------------------------------------------------------- 主渲染

void renderEffects(QPainter *painter,
                   const QTextDocument *document,
                   const EffectRenderOptions &options)
{
    if (!painter || !document || !options.anyLayer())
        return;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
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

                const QString one(ch);
                const int pos = basePos + i;

                const QRectF charRect = charRectAt(document, pos);
                if (!charRect.isValid() || charRect.height() <= 1.0)
                    continue;

                const QFont font = effectiveFont(document, block, fmt);
                const BaselineScale fit = baselineScaleAt(document, pos, font);
                const QFontMetricsF fm(font);
                const quint32 seed = mixSeed(style.seed, quint32(ch.unicode()));

                // ---- 手写层 ----
                if (options.showHandwriting && style.kind == EffectKind::Handwriting) {
                    if (!options.library || !options.library->contains(one)) {
                        options.missing.insert(one);
                    } else {
                        const HandwritingSample *sample = options.library->pick(one, seed);
                        if (!sample) {
                            options.missing.insert(one);
                        } else if (fit.valid) {
                            /*!
                             * 手写 pattern 的包围盒正好填满字身的 ascent..descent，
                             * 所以按"行内比例"缩放、把它的下边缘压在基线上就能对齐正文。
                             * 缩放用的是 fit.scale（行高 / 自然字高），
                             * 和扭曲层同一个口径 —— 换字号、改行距时两层不会各走各的。
                             */
                            const double targetH = (fm.ascent() + fm.descent()) * fit.scale;
                            const QRectF target(charRect.left(), fit.baselineY - targetH,
                                                charRect.width(), targetH);

                            NoiseWave localWave;
                            const NoiseWave *wave = nullptr;
                            double amplitude = 0.0;
                            if (options.wave && options.amplitudePt > 0.0) {
                                localWave.reseed(mixSeed(seed, 0x5F356495u));
                                wave = &localWave;
                                amplitude = options.amplitudePt * fit.scale * 0.6;
                            }
                            drawHandwritingSample(painter, *sample, target,
                                                  options.handwritingColor,
                                                  wave, amplitude, options.waveScale,
                                                  options.handwritingReplaceText);
                        }
                    }
                }

                // ---- 扭曲层 ----
                if (options.showDistortion && style.kind == EffectKind::Distortion
                    && options.wave && options.wave->isValid() && fit.valid) {
                    // 幅度按行高缩放：换字号之后"抖多少"看起来是一样的。
                    // 真正的上限由 deformGlyph 按字形的最小特征尺寸再钳一次
                    // （钳不住就会自交，字形会碎）。
                    const double amplitude = qMax(0.05, options.amplitudePt * fit.scale);

                    const QVector<QVector<QPointF>> outlines =
                        distortedGlyphOutlines(one, font, *options.wave, seed, fit.scale,
                                               amplitude, options.waveScale);
                    if (outlines.isEmpty())
                        continue;

                    // 颜色：优先用字符自己的前景色（富文本颜色），没设过才用默认色
                    QColor fill = fmt.foreground().color();
                    if (!fmt.foreground().style() || !fill.isValid())
                        fill = options.distortionColor;
                    /*!
                     * 替换模式：先把这一格刷成纸色，原字就看不见了 ——
                     * "导出后还留着原字体"的问题就此消失，屏幕上什么样纸上就什么样。
                     * 叠加模式则把扭曲层降一点透明度，方便对照"改了哪些笔画"。
                     */
                    if (!options.distortionReplaceText && fill.alpha() == 255)
                        fill.setAlpha(205);

                    painter->save();
                    /*!
                     * 定位铁律（扭曲层）：字形轮廓已经是"基线在原点、y 向下"，
                     * 所以绘制原点就是**基线**，一项都不用再加。
                     * 墨迹自然落在 [基线 - ascent, 基线 + descent]，
                     * 正好是行框里 ascent..descent 那一段 —— 和正文完全重合。
                     *
                     * 以前这里又加过 ascent、又加过 descent，还配了一个实测的魔法常量
                     * kLineTopCalibration，全是绕着"轮廓被翻了 y 又没翻回来"打的补丁。
                     */
                    painter->translate(charRect.left(), fit.baselineY);
                    // 裁剪防溢：扭曲后的笔画不许爬进旁边的字
                    painter->setClipRect(QRectF(-1.0, -charRect.height(),
                                                charRect.width() + 2.0,
                                                charRect.height() * 3.0));

                    if (options.distortionReplaceText) {
                        // 先把这一格刷成纸色：原字被盖掉，剩下的就是扭曲后的笔画
                        const double padY = fm.ascent() * 0.2;
                        painter->setPen(Qt::NoPen);
                        painter->setBrush(options.paperColor);
                        painter->drawRect(QRectF(0.0, -fm.ascent() - padY, charRect.width(),
                                                 fm.ascent() + fm.descent() + padY * 2.0));
                    }

                    painter->setPen(Qt::NoPen);
                    painter->setBrush(fill);
                    // 用整字拼一个填充路径：外形和"洞"一起交给奇偶规则，
                    // 逐条 drawPolygon 填不出洞（详见 buildGlyphPath 的注释）
                    painter->drawPath(buildGlyphPath(outlines));
                    painter->restore();
                }
            }
        }
    }
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
