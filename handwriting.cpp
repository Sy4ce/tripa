#include "handwriting.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <limits>

// ---------------------------------------------------------------- Sample

QRectF HandwritingSample::bbox() const
{
    if (strokes.isEmpty())
        return QRectF();

    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();

    for (const auto &stroke : strokes) {
        for (const HandwritingPoint &p : stroke) {
            minX = qMin(minX, p.pos.x());
            minY = qMin(minY, p.pos.y());
            maxX = qMax(maxX, p.pos.x());
            maxY = qMax(maxY, p.pos.y());
        }
    }
    if (minX > maxX)
        return QRectF();
    return QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
}

int HandwritingSample::pointCount() const
{
    int n = 0;
    for (const auto &stroke : strokes)
        n += stroke.size();
    return n;
}

bool HandwritingSample::hasPressure() const
{
    for (const auto &stroke : strokes) {
        for (const HandwritingPoint &p : stroke) {
            if (p.pressure >= 0.0)
                return true;
        }
    }
    return false;
}

bool HandwritingAdjustment::isDefault() const
{
    return qAbs(baseline) < 1e-6 && qAbs(size - 1.0) < 1e-6;
}

// ---------------------------------------------------------------- CSV 解析

bool parseHandwritingCsv(const QString &csv,
                         QString *labelOut,
                         QVector<QVector<HandwritingPoint>> *strokesOut,
                         QString *error,
                         QVector<int> *charStartsOut)
{
    const QStringList lines = csv.split(QRegularExpression(QStringLiteral("\r\n|\n|\r")),
                                        Qt::KeepEmptyParts);
    if (lines.isEmpty()) {
        if (error)
            *error = QStringLiteral("空文件");
        return false;
    }

    QString label = lines.first().trimmed();
    // 去掉 BOM 和可能存在的 UTF-8 BOM 残留
    if (label.startsWith(QChar(0xFEFF)))
        label.remove(0, 1);

    QVector<QVector<HandwritingPoint>> strokes;
    QVector<HandwritingPoint> current;
    QVector<int> charStarts;   // strokes 里"从这一笔起是新字符"的下标
    int blankRun = 0;          // 当前连续空行数

    for (int i = 1; i < lines.size(); ++i) {
        const QString s = lines.at(i).trimmed();
        if (s.isEmpty()) {
            ++blankRun;
            if (!current.isEmpty()) {
                strokes.append(current);
                current.clear();
            }
            continue;
        }

        /*!
         * 一笔的开头：看它前面隔了几个空行。
         *   1 个空行 = 同一字内的下一笔
         *   ≥2 个空行 = 新字符（getpattern 的导出约定，见 handwrite/README.txt）
         * 这个信息以前被丢掉了 —— 解析器把所有空行一视同仁，
         * 于是多字符文件只能靠"x 方向间隙最大处切开"去猜，
         * 猜错就把别的字的笔画安到这个字头上（写一行排不下、折行写的文件必错）。
         */
        if (blankRun >= 2 && !strokes.isEmpty())
            charStarts.append(strokes.size());
        blankRun = 0;

        const QStringList parts = s.split(QLatin1Char(','));
        if (parts.size() < 2)
            continue;

        bool okX = false;
        bool okY = false;
        const double x = parts.at(0).toDouble(&okX);
        const double y = parts.at(1).toDouble(&okY);
        if (!okX || !okY)
            continue;

        HandwritingPoint point;
        point.pos = QPointF(x, y);
        /*!
         * 第 3 列是笔压（getpattern 的 depth）。
         * 缺列、非数字、负数都当成"这一份数据没有笔压"（-1），
         * 渲染时回退到固定线宽 —— 不能猜成 1.0，那会画成一坨实心墨。
         */
        if (parts.size() >= 3) {
            bool okP = false;
            const double pressure = parts.at(2).trimmed().toDouble(&okP);
            if (okP && pressure >= 0.0)
                point.pressure = pressure;
        }

        current.append(point);
    }
    if (!current.isEmpty())
        strokes.append(current);

    if (label.isEmpty()) {
        if (error)
            *error = QStringLiteral("第 1 行没有字符标签");
        return false;
    }
    if (strokes.isEmpty()) {
        if (error)
            *error = QStringLiteral("没有解析到笔画点");
        return false;
    }

    if (labelOut)
        *labelOut = label;
    if (strokesOut)
        *strokesOut = strokes;
    if (charStartsOut)
        *charStartsOut = charStarts;
    return true;
}

bool splitStrokesByXGap(const QVector<QVector<HandwritingPoint>> &strokes,
                        int numChars,
                        QVector<QVector<QVector<HandwritingPoint>>> *groupsOut)
{
    if (numChars <= 0)
        return false;
    if (numChars == 1) {
        if (groupsOut)
            *groupsOut = {strokes};
        return true;
    }
    if (strokes.size() < numChars)
        return false;

    // 计算相邻笔画之间的 x 间隙：后一笔 minX - 前一笔 maxX
    QVector<double> gaps;
    gaps.reserve(strokes.size() - 1);
    for (int i = 0; i + 1 < strokes.size(); ++i) {
        double prevMax = std::numeric_limits<double>::lowest();
        for (const HandwritingPoint &p : strokes.at(i))
            prevMax = qMax(prevMax, p.pos.x());
        double nextMin = std::numeric_limits<double>::max();
        for (const HandwritingPoint &p : strokes.at(i + 1))
            nextMin = qMin(nextMin, p.pos.x());
        gaps.append(qMax(0.0, nextMin - prevMax));
    }

    // 取最大的 numChars-1 个间隙作为切分点
    QVector<int> order(gaps.size());
    for (int i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&gaps](int a, int b) { return gaps[a] > gaps[b]; });

    QVector<int> cuts;
    for (int i = 0; i < numChars - 1 && i < order.size(); ++i)
        cuts.append(order.at(i));
    std::sort(cuts.begin(), cuts.end());

    QVector<QVector<QVector<HandwritingPoint>>> groups;
    int start = 0;
    for (int c : cuts) {
        groups.append(strokes.mid(start, c + 1 - start));
        start = c + 1;
    }
    groups.append(strokes.mid(start));

    if (groups.size() != numChars)
        return false;
    if (groupsOut)
        *groupsOut = groups;
    return true;
}

// ---------------------------------------------------------------- Library

namespace {

//! 一组笔画的包围盒
QRectF groupBBox(const QVector<QVector<HandwritingPoint>> &group)
{
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    bool any = false;
    for (const auto &stroke : group) {
        for (const HandwritingPoint &p : stroke) {
            minX = qMin(minX, p.pos.x());
            minY = qMin(minY, p.pos.y());
            maxX = qMax(maxX, p.pos.x());
            maxY = qMax(maxY, p.pos.y());
            any = true;
        }
    }
    if (!any)
        return QRectF();
    return QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
}

//! 一组笔画在 x 方向投影出来的空档（宽 >= minGapProgress 的那些），用于切字
QVector<QPair<double, double>> xValleys(const QVector<QVector<HandwritingPoint>> &group,
                                       double minGap)
{
    const QRectF box = groupBBox(group);
    if (!box.isValid())
        return {};

    const int n = int(box.width()) + 3;
    QVector<bool> occupied(n, false);
    for (const auto &stroke : group) {
        for (const HandwritingPoint &p : stroke) {
            const int i = int(p.pos.x() - box.left());
            if (i >= 0 && i < n)
                occupied[i] = true;
        }
    }

    QVector<QPair<double, double>> valleys; // (起始 x, 宽度)
    int start = -1;
    for (int i = 0; i < n; ++i) {
        if (!occupied[i]) {
            if (start < 0)
                start = i;
        } else if (start >= 0) {
            if (i - start >= minGap)
                valleys.append({box.left() + start, double(i - start)});
            start = -1;
        }
    }
    return valleys;
}

//! 按 x 位置把一组笔画切成两组（按每笔的 x 中心归属）
bool splitGroupAtX(const QVector<QVector<HandwritingPoint>> &group, double xcut,
                   QVector<QVector<HandwritingPoint>> *leftOut,
                   QVector<QVector<HandwritingPoint>> *rightOut)
{
    QVector<QVector<HandwritingPoint>> left;
    QVector<QVector<HandwritingPoint>> right;
    for (const auto &stroke : group) {
        if (stroke.isEmpty())
            continue;
        double sum = 0.0;
        for (const HandwritingPoint &p : stroke)
            sum += p.pos.x();
        const double cx = sum / double(stroke.size());
        (cx < xcut ? left : right).append(stroke);
    }
    if (left.isEmpty() || right.isEmpty())
        return false;
    if (leftOut)
        *leftOut = left;
    if (rightOut)
        *rightOut = right;
    return true;
}

double medianOf(QVector<double> values)
{
    if (values.isEmpty())
        return 0.0;
    std::sort(values.begin(), values.end());
    return values.at(values.size() / 2);
}

} // namespace

/*!
 * 把"字符组"按阅读顺序排好：先上后下分行，行内再左到右。
 *
 * 为什么要重排：CSV 里的组顺序是**书写顺序**，不是阅读顺序 ——
 * 手写时抬笔换行、跳回去补一笔都很正常，这份数据的导出甚至会把几个字
 * 提前写（实测一份 7 行 84 字的文件里，前 6 组是"营/建/着/换/整"这些
 * 行尾字，正文才从第 7 组开始）。要按标签一个字一个字地对上，
 * 就必须先把它们摆回阅读顺序。
 */
static void sortGroupsInReadingOrder(QVector<QVector<QVector<HandwritingPoint>>> *groups)
{
    if (!groups || groups->size() < 2)
        return;

    QVector<double> heights;
    heights.reserve(groups->size());
    for (const auto &g : *groups)
        heights.append(groupBBox(g).height());
    const double medianH = medianOf(heights);
    const double rowTol = qMax(1e-6, medianH * 0.5); // 行间距的判据

    struct Item {
        int index;
        double cx;
        double cy;
        int row;
    };
    QVector<Item> items;
    items.reserve(groups->size());
    for (int i = 0; i < groups->size(); ++i) {
        const QRectF box = groupBBox(groups->at(i));
        items.append({i, box.center().x(), box.center().y(), 0});
    }

    // 按 y 排序后贪心分行：与当前行的 y 中心差超过阈值就另起一行
    std::sort(items.begin(), items.end(),
              [](const Item &a, const Item &b) { return a.cy < b.cy; });
    int row = 0;
    double rowY = items.first().cy;
    for (Item &item : items) {
        if (item.cy - rowY > rowTol) {
            ++row;
            rowY = item.cy;
        }
        item.row = row;
    }

    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        if (a.row != b.row)
            return a.row < b.row;
        return a.cx < b.cx;
    });

    QVector<QVector<QVector<HandwritingPoint>>> sorted;
    sorted.reserve(items.size());
    for (const Item &item : items)
        sorted.append(groups->at(item.index));
    *groups = sorted;
}

/*!
 * 组数多于标签字数时，把相邻的组并起来，直到组数正好等于字数。
 *
 * 优先并"x 重叠最多"的一对：同一个字被切成两组时，两组在 x 上必然大范围重叠
 * （比如"冫"和"马"上下叠着）；两个相邻的字则几乎不重叠。没有重叠对时再退而
 * 求其次并接缝最紧的一对。跨行的接缝给大惩罚，免得把行尾和下一行行首并起来。
 * 合并是保序的：第 i 组永远还是第 i 个字。
 */
static void mergeGroupsDownTo(QVector<QVector<QVector<HandwritingPoint>>> *groups, int target)
{
    if (!groups || target < 1)
        return;

    while (groups->size() > target) {
        int bestSeam = -1;
        QPair<double, double> bestKey(std::numeric_limits<double>::max(),
                                      std::numeric_limits<double>::max());
        for (int i = 0; i + 1 < groups->size(); ++i) {
            const QRectF a = groupBBox(groups->at(i));
            const QRectF b = groupBBox(groups->at(i + 1));
            const bool sameRow = a.bottom() > b.top() && b.bottom() > a.top();
            const double overlap = qMin(a.right(), b.right()) - qMax(a.left(), b.left());
            const double smaller = qMax(1e-6, qMin(a.width(), b.width()));
            const double ratio = sameRow ? overlap / smaller : -1.0;
            const double gap = sameRow ? b.left() - a.right()
                                       : std::numeric_limits<double>::max();
            const QPair<double, double> key(-ratio, gap);
            if (key < bestKey) {
                bestKey = key;
                bestSeam = i;
            }
        }
        if (bestSeam < 0)
            break;
        (*groups)[bestSeam] += groups->at(bestSeam + 1);
        groups->removeAt(bestSeam + 1);
    }
}

/*!
 * 组数少于标签字数时，把过宽的组切开，直到组数正好等于字数。
 *
 * 组比字少只有一个原因：写字时有的字之间没抬笔，边界没写出来，
 * 于是一个"组"里塞了两三个字。切的位置取组内最宽的 x 空档
 * （字与字之间的空档总比字内部的大），并要求切完两边都还有料。
 * 实在切不动就停手，让调用方退回别的策略。
 */
static void splitWideGroupsUpTo(QVector<QVector<QVector<HandwritingPoint>>> *groups,
                                int target, double medianW, double medianH)
{
    if (!groups)
        return;

    int guard = 0;
    while (groups->size() < target && guard++ < 500) {
        // 先挑最宽的那一组来切
        int widest = 0;
        double widestW = -1.0;
        for (int i = 0; i < groups->size(); ++i) {
            const double w = groupBBox(groups->at(i)).width();
            if (w > widestW) {
                widestW = w;
                widest = i;
            }
        }
        const QRectF box = groupBBox(groups->at(widest));
        if (!box.isValid())
            break;

        double cutX = 0.0;
        double bestGap = 0.0;
        /*!
         * 候选空档要够宽：至少"半个字"的宽度量级，或者字高的 10%。
         * 太窄的空档多半是字内部的笔画间隙（"测"里"氵"和"马"之间就有），
         * 在那儿切等于把字劈成两半。
         */
        const double minGap = qMax(2.0, qMax(medianH * 0.10, medianW * 0.20));
        const auto valleys = xValleys(groups->at(widest), minGap);
        for (const auto &valley : valleys) {
            const double gap = valley.second;
            if (gap > bestGap) {
                bestGap = gap;
                cutX = valley.first + gap / 2.0;
            }
        }
        if (bestGap <= 0.0)
            break; // 这一组切不动

        QVector<QVector<HandwritingPoint>> left;
        QVector<QVector<HandwritingPoint>> right;
        if (!splitGroupAtX(groups->at(widest), cutX, &left, &right))
            break;
        /*!
         * 切完两边都要"有料"：各占这一组笔画的至少 20%。
         * 只把一笔甩出去不算切字，那更像把某个笔画认错了位置。
         */
        const int total = groups->at(widest).size();
        const int atLeast = qMax(1, int(total * 0.20));
        if (left.size() < atLeast || right.size() < atLeast)
            break;
        (*groups)[widest] = left;
        groups->insert(widest + 1, right);
    }
}

/*!
 * 丢掉"大得不像一个字"的组：宽高都远大于中位数的，是抬笔跨页的连线
 * （导出时笔在纸上空走的那一段），不是字。留着一个就会把后面所有字挤错位。
 */
static void dropOversizedGroups(QVector<QVector<QVector<HandwritingPoint>>> *groups)
{
    if (!groups || groups->size() < 4)
        return;

    QVector<double> widths;
    QVector<double> heights;
    for (const auto &g : *groups) {
        const QRectF box = groupBBox(g);
        widths.append(box.width());
        heights.append(box.height());
    }
    const double medianW = medianOf(widths);
    const double medianH = medianOf(heights);
    if (medianW <= 0.0 && medianH <= 0.0)
        return;

    QVector<QVector<QVector<HandwritingPoint>>> kept;
    kept.reserve(groups->size());
    for (const auto &g : *groups) {
        const QRectF box = groupBBox(g);
        const bool oversized = box.width() > 2.5 * medianW && box.height() > 1.8 * medianH;
        if (!oversized)
            kept.append(g);
    }
    if (kept.size() >= 2)
        *groups = kept;
}

/*!
 * 把样本挪到原点、并把笔压归一化到 0~1。
 *
 * 笔压这一步是宽容处理：约定就是 0~1（getpattern 的 depth），
 * 但万一导出的是 0~255 / 0~1024 那种原始量纲，直接当 0~1 用会全部顶格
 * —— 整字糊死。所以只要最大值超过 1，就按最大值线性归一化。
 * 全都是 -1（没有笔压列）时原样留着，渲染层会回退到固定线宽。
 */
static void normalizeSample(HandwritingSample *sample)
{
    const QRectF box = sample->bbox();
    if (box.isValid()) {
        const QPointF origin = box.topLeft();
        if (!origin.isNull()) {
            for (auto &stroke : sample->strokes) {
                for (HandwritingPoint &p : stroke)
                    p.pos -= origin;
            }
        }
    }

    double maxPressure = 0.0;
    for (const auto &stroke : sample->strokes) {
        for (const HandwritingPoint &p : stroke)
            maxPressure = qMax(maxPressure, p.pressure);
    }
    if (maxPressure > 1.0) {
        for (auto &stroke : sample->strokes) {
            for (HandwritingPoint &p : stroke) {
                if (p.pressure >= 0.0)
                    p.pressure /= maxPressure;
            }
        }
    }
}

void HandwritingLibrary::clear()
{
    m_index.clear();
    m_loadedFiles.clear();
    m_problems.clear();
    m_sampleCount = 0;
    /*!
     * 校正值也一起清掉：重新载入会从各自的 xml 再读回来。
     * 留着的话，"换了一个数据目录"会把上一个目录的校正错认成新目录的。
     */
    m_adjust.clear();
    m_dirtyAdjust.clear();
}

void HandwritingLibrary::addSample(const HandwritingSample &sample)
{
    if (sample.ch.isEmpty() || sample.isEmpty())
        return;

    /*!
     * 同一个 CSV 文件重复加载时去重。
     *
     * charIndex 必须一起比：同一个文件里两个字完全可能笔画数、点数都一样
     * （实测一个"日"一个"曰"就是），只按笔画数比会把后一个当成重复丢掉，
     * 那个字就永远没有手写数据了。
     */
    auto &list = m_index[sample.ch];
    for (const HandwritingSample &existing : list) {
        if (existing.source == sample.source
            && existing.charIndex == sample.charIndex
            && existing.strokeCount() == sample.strokeCount()
            && existing.pointCount() == sample.pointCount()) {
            return;
        }
    }

    list.append(sample);
    ++m_sampleCount;
}

QSet<QString> HandwritingLibrary::characters() const
{
    QSet<QString> set;
    for (auto it = m_index.constBegin(); it != m_index.constEnd(); ++it)
        set.insert(it.key());
    return set;
}

QStringList HandwritingLibrary::sortedCharacters() const
{
    QStringList list = m_index.keys();
    std::sort(list.begin(), list.end());
    return list;
}

QVector<HandwritingSample> HandwritingLibrary::samples(const QString &ch) const
{
    return m_index.value(ch);
}

const HandwritingSample *HandwritingLibrary::pick(const QString &ch, quint32 seed) const
{
    auto it = m_index.constFind(ch);
    if (it == m_index.constEnd() || it->isEmpty())
        return nullptr;

    const int n = it->size();
    if (n == 1)
        return &it->at(0);

    // 简单整数哈希，保证同一 seed 稳定
    quint32 h = seed * 2654435761u;
    h ^= h >> 13;
    h *= 2246822519u;
    h ^= h >> 16;
    return &it->at(int(h % quint32(n)));
}

bool HandwritingLibrary::addCsvText(const QString &csv, const QString &sourceName, QString *error)
{
    QString label;
    QVector<QVector<HandwritingPoint>> strokes;
    QVector<int> charStarts;
    if (!parseHandwritingCsv(csv, &label, &strokes, error, &charStarts))
        return false;

    /*!
     * 标签里的空格不算字：有的导出会把识别结果按词断行/加空格，
     * 但画布上并没有"空格"这个笔画组，不去掉的话组数永远对不上。
     */
    QString effectiveLabel;
    effectiveLabel.reserve(label.size());
    for (const QChar &c : label) {
        if (!c.isSpace())
            effectiveLabel.append(c);
    }
    if (effectiveLabel.isEmpty())
        effectiveLabel = label;

    const int n = effectiveLabel.size();

    QVector<QVector<QVector<HandwritingPoint>>> groups;
    if (n == 1) {
        groups = {strokes};
    } else if (!charStarts.isEmpty()) {
        /*!
         * 用 CSV 自带的字符边界（两个空行）切 —— 这是唯一可靠的一路。
         *
         * 以前不管有没有边界都按 x 间隙猜，猜出来的"字"可能由别的字的笔画拼成：
         * 一行写不下、折行写的文件必然张冠李戴（实测一个"好"只分到 2 笔，
         * 这字要 5~6 笔，渲染出来就是一团认不出的墨）。
         *
         * 边界本身也不总准，所以再按几何修三道：
         *   1. 丢掉抬笔跨页的连线（大得不像一个字）；
         *   2. 摆回阅读顺序（书写顺序和阅读顺序不一定一致）；
         *   3. 组多了并、组少了切，直到组数正好等于标签字数。
         * 修完还对不上，就交给下面的兜底策略，绝不硬凑。
         */
        int start = 0;
        for (int cut : charStarts) {
            groups.append(strokes.mid(start, cut - start));
            start = cut;
        }
        groups.append(strokes.mid(start));

        dropOversizedGroups(&groups);
        sortGroupsInReadingOrder(&groups);

        if (groups.size() != n) {
            QVector<double> widths;
            QVector<double> heights;
            for (const auto &g : groups) {
                const QRectF box = groupBBox(g);
                widths.append(box.width());
                heights.append(box.height());
            }
            const double medianW = medianOf(widths);
            const double medianH = medianOf(heights);
            if (groups.size() < n)
                splitWideGroupsUpTo(&groups, n, medianW, medianH);
            if (groups.size() > n)
                mergeGroupsDownTo(&groups, n);
        }

        if (groups.size() != n)
            groups.clear();
    }

    if (groups.isEmpty()) {
        if (strokes.size() == n) {
            // 每笔正好是一个字
            for (int i = 0; i < n; ++i)
                groups.append({strokes.at(i)});
        } else if (!splitStrokesByXGap(strokes, n, &groups)) {
            if (error) {
                *error = QStringLiteral("标签有 %1 个字符，但只有 %2 笔，无法切分")
                             .arg(n)
                             .arg(strokes.size());
            }
            return false;
        }
    }

    for (int i = 0; i < groups.size() && i < n; ++i) {
        HandwritingSample sample;
        sample.ch = QString(effectiveLabel.at(i));
        sample.strokes = groups.at(i);
        sample.source = sourceName;
        sample.charIndex = i;
        if (sample.isEmpty())
            continue;
        normalizeSample(&sample);
        addSample(sample);
    }
    return true;
}

bool HandwritingLibrary::loadFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("打不开文件");
        return false;
    }
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    const QString csv = in.readAll();
    file.close();

    QString localError;
    if (!addCsvText(csv, path, &localError)) {
        if (error)
            *error = localError;
        return false;
    }
    if (!m_loadedFiles.contains(path))
        m_loadedFiles.append(path);

    /*!
     * 顺手读一下伴生 xml（基线 / 大小校正）。
     *
     * 放在这里而不是让调用方自己记得调：校正值和 CSV 是一体的 ——
     * 少读一次，用户调了半天的基线就"没生效"，而且屏幕上不会有任何提示。
     * 读不到 xml 是完全正常的（大部分 CSV 没调过），所以不报 problem。
     */
    QString adjustError;
    if (!loadAdjustmentFile(path, &adjustError) && !adjustError.isEmpty())
        m_problems.append(QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), adjustError));
    return true;
}

/*!
 * 递归收集 \a root 下的所有 *.csv（含子目录、子子目录……）。
 *
 * 遍历顺序按路径排序固定下来：加载顺序会决定"同一个字符的多个样本"
 * 的入库次序，进而影响 pick() 的取用结果 —— 让它随文件系统枚举顺序
 * 变来变去的话，同一个文档每次启动的字迹都不一样。
 * 符号链接不入栈（QDirIterator 默认不跟随目录符号链接），避免自引用死循环。
 */
static QStringList collectCsvFiles(const QString &root)
{
    QStringList files;
    QDirIterator it(root, {QStringLiteral("*.csv")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        files.append(it.next());
    files.sort();
    return files;
}

int HandwritingLibrary::loadResourceDir(const QString &resourceDir, QStringList *problems)
{
    QDir dir(resourceDir);
    if (!dir.exists())
        return 0;

    int count = 0;
    const QStringList files = collectCsvFiles(resourceDir);
    for (const QString &path : files) {
        QString error;
        if (loadFile(path, &error))
            ++count;
        else
            m_problems.append(QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), error));
    }
    if (problems)
        *problems = m_problems;
    return count;
}

int HandwritingLibrary::loadDir(const QString &dirPath, QStringList *problems)
{
    QDir dir(dirPath);
    if (!dir.exists()) {
        m_problems.append(QStringLiteral("目录不存在: %1").arg(dirPath));
        if (problems)
            *problems = m_problems;
        return 0;
    }

    int count = 0;
    const QStringList files = collectCsvFiles(dirPath);
    for (const QString &path : files) {
        QString error;
        if (loadFile(path, &error))
            ++count;
        else
            m_problems.append(QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), error));
    }

    if (problems)
        *problems = m_problems;
    return count;
}

// ---------------------------------------------------------------- 基线校正

QString HandwritingLibrary::adjustmentPathFor(const QString &csvPath)
{
    if (csvPath.isEmpty() || csvPath.startsWith(QLatin1Char(':')))
        return QString();

    const QFileInfo info(csvPath);
    const QString base = info.completeBaseName();
    if (base.isEmpty())
        return QString();
    /*!
     * 用 info.dir() 而不是 absolutePath()：相对路径保持相对，
     * 免得载入的是相对路径、写出去却变成了另一个绝对位置。
     */
    return info.dir().filePath(base + QStringLiteral(".xml"));
}

bool HandwritingLibrary::loadAdjustmentFile(const QString &csvPath, QString *error)
{
    if (error)
        error->clear();

    const QString xmlPath = adjustmentPathFor(csvPath);
    if (xmlPath.isEmpty())
        return false; // 内置资源：本来就没有伴生 xml

    QFile file(xmlPath);
    if (!file.exists())
        return false; // 没调过 = 正常情况，不是错误
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("打不开校正文件 %1").arg(xmlPath);
        return false;
    }

    QHash<int, HandwritingAdjustment> map;
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("char"))
            continue;

        const QXmlStreamAttributes attrs = xml.attributes();
        bool okIndex = false;
        const int index = attrs.value(QLatin1String("index")).toInt(&okIndex);
        if (!okIndex || index < 0)
            continue;

        HandwritingAdjustment adj;
        /*!
         * 属性缺失和"属性是 0"必须分开：size 少了就当 1.0，
         * 用 toDouble() 的默认 0 会让整个字缩成看不见的一个点。
         * 超出范围的值直接钳住 —— xml 是文本文件，谁都能手改。
         */
        if (attrs.hasAttribute(QLatin1String("baseline")))
            adj.baseline = qBound(-1.0, attrs.value(QLatin1String("baseline")).toDouble(), 1.0);
        if (attrs.hasAttribute(QLatin1String("size")))
            adj.size = qBound(0.05, attrs.value(QLatin1String("size")).toDouble(), 10.0);
        if (!adj.isDefault())
            map.insert(index, adj);
    }

    const bool malformed = xml.hasError();
    const QString parseError = xml.errorString();
    file.close();

    if (malformed) {
        if (error)
            *error = QStringLiteral("校正文件 %1 解析失败：%2")
                         .arg(QFileInfo(xmlPath).fileName(), parseError);
        return false;
    }

    // 读成功才替换：xml 坏了不该把内存里已有的校正也一起清掉
    m_adjust.insert(csvPath, map);
    m_dirtyAdjust.remove(csvPath);
    return true;
}

HandwritingAdjustment HandwritingLibrary::adjustment(const HandwritingSample &sample) const
{
    if (sample.charIndex < 0)
        return HandwritingAdjustment();

    const auto fileIt = m_adjust.constFind(sample.source);
    if (fileIt == m_adjust.constEnd())
        return HandwritingAdjustment();

    const auto entryIt = fileIt->constFind(sample.charIndex);
    return entryIt == fileIt->constEnd() ? HandwritingAdjustment() : entryIt.value();
}

void HandwritingLibrary::setAdjustment(const QString &csvPath, int charIndex,
                                       const HandwritingAdjustment &adj)
{
    if (csvPath.isEmpty() || charIndex < 0)
        return;

    HandwritingAdjustment clamped;
    clamped.baseline = qBound(-1.0, adj.baseline, 1.0);
    clamped.size = qBound(0.05, adj.size, 10.0);

    auto &file = m_adjust[csvPath];
    const HandwritingAdjustment before = file.value(charIndex);
    if (clamped.isDefault())
        file.remove(charIndex);
    else
        file.insert(charIndex, clamped);

    /*!
     * 真的变了才标脏。
     *
     * 对话框把 spinbox 初始化成当前值也会走到这里（不算改动），
     * 不比较的话"只是打开看了一眼"也会给每个文件写一份 xml。
     */
    const HandwritingAdjustment after = file.value(charIndex);
    if (qAbs(before.baseline - after.baseline) > 1e-9 || qAbs(before.size - after.size) > 1e-9)
        m_dirtyAdjust.insert(csvPath);
}

QStringList HandwritingLibrary::dirtyAdjustmentFiles() const
{
    QStringList list = m_dirtyAdjust.values();
    list.sort();
    return list;
}

int HandwritingLibrary::adjustedFileCount() const
{
    int n = 0;
    for (auto it = m_adjust.constBegin(); it != m_adjust.constEnd(); ++it) {
        if (!it.value().isEmpty())
            ++n;
    }
    return n;
}

int HandwritingLibrary::adjustedEntryCount() const
{
    int n = 0;
    for (auto it = m_adjust.constBegin(); it != m_adjust.constEnd(); ++it)
        n += it.value().size();
    return n;
}

//! 写一份校正 xml；\a labels 用来把字符下标还原成字符（纯粹给人看的）
static bool writeAdjustmentXml(const QString &csvPath,
                               const QString &xmlPath,
                               const QHash<int, HandwritingAdjustment> &map,
                               const QHash<int, QString> &labels,
                               QString *error)
{
    QSaveFile file(xmlPath);
    // 原子写：写一半被打断时，磁盘上要么是旧的完整 xml，要么是新的
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("无法写入 %1（目录只读？）").arg(xmlPath);
        return false;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("tripaHandwriting"));
    xml.writeAttribute(QStringLiteral("version"), QStringLiteral("1"));
    xml.writeAttribute(QStringLiteral("csv"), QFileInfo(csvPath).fileName());
    xml.writeComment(QStringLiteral(" 手写笔迹的基线/大小校正。baseline 单位 = 字身高"
                                    "（行内 ascent+descent）的比例，正值往下沉；"
                                    "size = 字形缩放倍率。index 是字符在 CSV 标签里的下标（从 0 起）。"));

    QList<int> keys = map.keys();
    std::sort(keys.begin(), keys.end());
    for (int index : keys) {
        const HandwritingAdjustment &adj = map.value(index);
        xml.writeStartElement(QStringLiteral("char"));
        xml.writeAttribute(QStringLiteral("index"), QString::number(index));
        const QString label = labels.value(index);
        if (!label.isEmpty())
            xml.writeAttribute(QStringLiteral("label"), label);
        xml.writeAttribute(QStringLiteral("baseline"), QString::number(adj.baseline, 'f', 4));
        xml.writeAttribute(QStringLiteral("size"), QString::number(adj.size, 'f', 4));
        xml.writeEndElement();
    }

    xml.writeEndElement();
    xml.writeEndDocument();

    if (xml.hasError()) {
        if (error)
            *error = QStringLiteral("写 %1 时出错").arg(xmlPath);
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("保存 %1 失败：%2").arg(xmlPath, file.errorString());
        return false;
    }
    return true;
}

bool HandwritingLibrary::saveAdjustments(QStringList *problems)
{
    const QStringList dirty = dirtyAdjustmentFiles();
    bool allOk = true;

    for (const QString &csvPath : dirty) {
        const QString xmlPath = adjustmentPathFor(csvPath);
        if (xmlPath.isEmpty()) {
            /*!
             * 内置资源（qrc:/handwrite/...）只读：调了也存不下来。
             * 这里明确报出来并把它从脏表里摘掉 —— 否则每次保存都重复失败一次，
             * 用户只会看到"又报错了"，却不知道是哪份数据的问题。
             */
            allOk = false;
            m_dirtyAdjust.remove(csvPath);
            if (problems) {
                problems->append(QStringLiteral("内置数据只读，无法保存校正：%1")
                                     .arg(QFileInfo(csvPath).fileName()));
            }
            continue;
        }

        QHash<int, QString> labels;
        for (const HandwritingFileEntry &e : fileEntries(csvPath))
            labels.insert(e.charIndex, e.ch);

        QString error;
        if (!writeAdjustmentXml(csvPath, xmlPath, m_adjust.value(csvPath), labels, &error)) {
            allOk = false;
            if (problems)
                problems->append(error);
            continue;
        }
        m_dirtyAdjust.remove(csvPath);
    }
    return allOk;
}

QStringList HandwritingLibrary::sourceFiles() const
{
    QStringList files = m_loadedFiles;
    files.sort();
    // 先按路径排、再按文件名稳定排：同名文件（不同目录）的顺序也是确定的
    std::stable_sort(files.begin(), files.end(), [](const QString &a, const QString &b) {
        return QFileInfo(a).fileName().localeAwareCompare(QFileInfo(b).fileName()) < 0;
    });
    return files;
}

QVector<HandwritingFileEntry> HandwritingLibrary::fileEntries(const QString &csvPath) const
{
    QVector<HandwritingFileEntry> entries;
    const bool readOnly = csvPath.startsWith(QLatin1Char(':'));

    for (auto it = m_index.constBegin(); it != m_index.constEnd(); ++it) {
        for (const HandwritingSample &sample : it.value()) {
            if (sample.source != csvPath || sample.charIndex < 0)
                continue;
            HandwritingFileEntry entry;
            entry.ch = sample.ch;
            entry.charIndex = sample.charIndex;
            entry.strokeCount = sample.strokeCount();
            entry.pointCount = sample.pointCount();
            entry.readOnly = readOnly;
            entries.append(entry);
        }
    }

    std::stable_sort(entries.begin(), entries.end(),
                     [](const HandwritingFileEntry &a, const HandwritingFileEntry &b) {
                         return a.charIndex < b.charIndex;
                     });
    return entries;
}

const HandwritingSample *HandwritingLibrary::sampleAt(const QString &csvPath, int charIndex) const
{
    if (charIndex < 0)
        return nullptr;

    for (auto it = m_index.constBegin(); it != m_index.constEnd(); ++it) {
        for (const HandwritingSample &sample : it.value()) {
            if (sample.source == csvPath && sample.charIndex == charIndex)
                return &sample;
        }
    }
    return nullptr;
}
