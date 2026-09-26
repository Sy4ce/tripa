#include "handwriting.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
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
        for (const QPointF &p : stroke) {
            minX = qMin(minX, p.x());
            minY = qMin(minY, p.y());
            maxX = qMax(maxX, p.x());
            maxY = qMax(maxY, p.y());
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

// ---------------------------------------------------------------- CSV 解析

bool parseHandwritingCsv(const QString &csv,
                         QString *labelOut,
                         QVector<QVector<QPointF>> *strokesOut,
                         QString *error)
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

    QVector<QVector<QPointF>> strokes;
    QVector<QPointF> current;

    for (int i = 1; i < lines.size(); ++i) {
        const QString s = lines.at(i).trimmed();
        if (s.isEmpty()) {
            if (!current.isEmpty()) {
                strokes.append(current);
                current.clear();
            }
            continue;
        }

        const QStringList parts = s.split(QLatin1Char(','));
        if (parts.size() < 2)
            continue;

        bool okX = false;
        bool okY = false;
        const double x = parts.at(0).toDouble(&okX);
        const double y = parts.at(1).toDouble(&okY);
        if (!okX || !okY)
            continue;

        current.append(QPointF(x, y));
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
    return true;
}

bool splitStrokesByXGap(const QVector<QVector<QPointF>> &strokes,
                        int numChars,
                        QVector<QVector<QVector<QPointF>>> *groupsOut)
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
        for (const QPointF &p : strokes.at(i))
            prevMax = qMax(prevMax, p.x());
        double nextMin = std::numeric_limits<double>::max();
        for (const QPointF &p : strokes.at(i + 1))
            nextMin = qMin(nextMin, p.x());
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

    QVector<QVector<QVector<QPointF>>> groups;
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

static void normalizeSample(HandwritingSample *sample)
{
    const QRectF box = sample->bbox();
    if (!box.isValid())
        return;
    const QPointF origin = box.topLeft();
    if (origin.isNull())
        return;
    for (auto &stroke : sample->strokes) {
        for (QPointF &p : stroke)
            p -= origin;
    }
}

void HandwritingLibrary::clear()
{
    m_index.clear();
    m_loadedFiles.clear();
    m_problems.clear();
    m_sampleCount = 0;
}

void HandwritingLibrary::addSample(const HandwritingSample &sample)
{
    if (sample.ch.isEmpty() || sample.isEmpty())
        return;

    // 同一个 CSV 文件重复加载时去重
    auto &list = m_index[sample.ch];
    for (const HandwritingSample &existing : list) {
        if (existing.source == sample.source
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
    QVector<QVector<QPointF>> strokes;
    if (!parseHandwritingCsv(csv, &label, &strokes, error))
        return false;

    const int n = label.size();

    QVector<QVector<QVector<QPointF>>> groups;
    if (n == 1) {
        groups = {strokes};
    } else if (strokes.size() == n) {
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

    for (int i = 0; i < groups.size() && i < n; ++i) {
        HandwritingSample sample;
        sample.ch = QString(label.at(i));
        sample.strokes = groups.at(i);
        sample.source = sourceName;
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
    return true;
}

int HandwritingLibrary::loadResourceDir(const QString &resourceDir, QStringList *problems)
{
    QDir dir(resourceDir);
    if (!dir.exists())
        return 0;

    int count = 0;
    const QFileInfoList entries = dir.entryInfoList({QStringLiteral("*.csv")}, QDir::Files, QDir::Name);
    for (const QFileInfo &info : entries) {
        QString error;
        if (loadFile(info.absoluteFilePath(), &error))
            ++count;
        else
            m_problems.append(QStringLiteral("%1: %2").arg(info.fileName(), error));
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
    const QFileInfoList entries = dir.entryInfoList({QStringLiteral("*.csv")}, QDir::Files, QDir::Name);
    for (const QFileInfo &info : entries) {
        QString error;
        if (loadFile(info.absoluteFilePath(), &error))
            ++count;
        else
            m_problems.append(QStringLiteral("%1: %2").arg(info.fileName(), error));
    }

    // 目录本身没有 CSV 时，向上找几层（用户常常选到上一层）
    if (count == 0) {
        QDir up = dir;
        for (int depth = 0; depth < 3; ++depth) {
            if (!up.cdUp())
                break;
            const QFileInfoList upEntries =
                up.entryInfoList({QStringLiteral("*.csv")}, QDir::Files, QDir::Name);
            if (!upEntries.isEmpty()) {
                for (const QFileInfo &info : upEntries) {
                    QString error;
                    if (loadFile(info.absoluteFilePath(), &error))
                        ++count;
                    else
                        m_problems.append(QStringLiteral("%1: %2").arg(info.fileName(), error));
                }
                break;
            }
        }
    }

    if (problems)
        *problems = m_problems;
    return count;
}
