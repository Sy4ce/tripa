/*
 * 纯逻辑自检（不需要控件）：CSV 解析、手写库、噪声波、笔画扭曲。
 * 直接运行 build/test_core.exe 即可，退出码 0 表示全过。
 */

#include "effect.h"
#include "handwriting.h"
#include "noise.h"

#include <QCoreApplication>
#include <QDebug>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextCharFormat>

#include <cmath>

static int g_failures = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        if (cond) {                                                              \
            qInfo().noquote() << "  ok   " << (what);                            \
        } else {                                                                 \
            ++g_failures;                                                        \
            qWarning().noquote() << "  FAIL " << (what);                         \
        }                                                                        \
    } while (false)

static void testCsvParsing()
{
    qInfo().noquote() << "CSV 解析";
    const QString csv = QStringLiteral(
        "a\n"
        "10,20,0.5\n"
        "12,22,0.5\n"
        "\n"
        "30,40,0.7\n");

    QString label;
    QVector<QVector<QPointF>> strokes;
    QString error;
    CHECK(parseHandwritingCsv(csv, &label, &strokes, &error), "解析成功");
    CHECK(label == QStringLiteral("a"), "标签为 a");
    CHECK(strokes.size() == 2, "空行切出 2 笔");
    CHECK(strokes.value(0).size() == 2, "第一笔 2 个点");
    CHECK(strokes.value(1).size() == 1, "第二笔 1 个点");

    // 坏行会被跳过
    QString label2;
    QVector<QVector<QPointF>> strokes2;
    CHECK(parseHandwritingCsv(QStringLiteral("b\n1,2\n坏行\n3,4\n"), &label2, &strokes2, nullptr),
          "含坏行仍能解析");
    CHECK(strokes2.size() == 1 && strokes2.first().size() == 2, "坏行被跳过");

    CHECK(!parseHandwritingCsv(QStringLiteral("x\n"), &label, &strokes, nullptr),
          "只有标签、没有点 -> 失败");
}

static void testLibrary()
{
    qInfo().noquote() << "手写库";
    HandwritingLibrary library;

    QString error;
    CHECK(library.addCsvText(QStringLiteral("a\n0,0\n10,0\n\n10,20\n"), QStringLiteral("mem:a"),
                             &error),
          "加入单字符数据");

    // 多字符标签：3 笔 -> 3 个字（每笔一个字）
    CHECK(library.addCsvText(QStringLiteral("xyz\n0,0\n5,5\n\n100,0\n105,5\n\n200,0\n205,5\n"),
                             QStringLiteral("mem:xyz"), &error),
          "加入多字符标签数据");
    CHECK(library.characterCount() == 4, "共 4 个字符（a/x/y/z）");
    CHECK(library.contains(QStringLiteral("y")), "包含 y");
    CHECK(!library.contains(QStringLiteral("q")), "不含 q");

    const HandwritingSample *sample = library.pick(QStringLiteral("a"), 7u);
    CHECK(sample != nullptr, "能取到样本");
    if (sample) {
        const QRectF box = sample->bbox();
        CHECK(qFuzzyIsNull(box.left()) && qFuzzyIsNull(box.top()), "包围盒已归一化到 (0,0)");
        CHECK(qFuzzyCompare(box.width(), 10.0) && qFuzzyCompare(box.height(), 20.0),
              "包围盒尺寸正确");
    }

    // 同一 seed 必须稳定（否则每次重绘字迹都在跳）
    CHECK(library.pick(QStringLiteral("a"), 7u) == library.pick(QStringLiteral("a"), 7u),
          "同一 seed 取到同一样本");
}

static void testSplitByGap()
{
    qInfo().noquote() << "按 x 间隙切字";
    // 3 个字符、5 笔：间隙最大的是第 1|2 与第 3|4 笔之间
    QVector<QVector<QPointF>> strokes;
    const double data[5][2] = {{0, 0}, {8, 0}, {100, 0}, {108, 0}, {200, 0}};
    for (const auto &p : data)
        strokes.append({QPointF(p[0], p[1]), QPointF(p[0] + 5, p[1] + 5)});

    QVector<QVector<QVector<QPointF>>> groups;
    CHECK(splitStrokesByXGap(strokes, 3, &groups), "能切成 3 组");
    CHECK(groups.size() == 3, "组数 = 3");
    if (groups.size() == 3) {
        CHECK(groups[0].size() == 2 && groups[1].size() == 2 && groups[2].size() == 1,
              "分组大小 2/2/1");
    }
    QVector<QVector<QVector<QPointF>>> one;
    CHECK(splitStrokesByXGap(strokes, 1, &one) && one.size() == 1, "numChars=1 时原样一组");
}

static void testNoise()
{
    qInfo().noquote() << "噪声波";
    NoiseWave wave;
    wave.reseed(42u);
    CHECK(wave.isValid(), "波形有效（有分量）");

    bool inRange = true;
    bool varies = false;
    const double first = wave.value(0.0);
    for (int i = 0; i <= 200; ++i) {
        const double v = wave.value(i / 200.0 * 4.0);
        if (v < -1.0001 || v > 1.0001)
            inRange = false;
        if (std::fabs(v - first) > 1e-6)
            varies = true;
    }
    CHECK(inRange, "值域在 [-1,1] 之内");
    CHECK(varies, "不是常值");

    NoiseWave same;
    same.reseed(42u);
    CHECK(qFuzzyCompare(wave.value(0.37), same.value(0.37)), "同 seed 波形可复现");

    NoiseWave other;
    other.reseed(43u);
    CHECK(!qFuzzyCompare(wave.value(0.37) + 1.0, other.value(0.37) + 1.0), "不同 seed 波形不同");
}

static void testDistort()
{
    qInfo().noquote() << "笔画扭曲";
    QVector<QPointF> line;
    for (int i = 0; i <= 40; ++i)
        line.append(QPointF(i * 5.0, 0.0));

    NoiseWave wave;
    wave.reseed(9u);

    const QVector<QPointF> distorted = distortPolyline(line, wave, 6.0, 2.0, true);
    CHECK(distorted.size() == line.size(), "点数不变");

    // 端点保持不动（preserveEndpoints）
    CHECK((distorted.first() - line.first()).manhattanLength() < 1e-9, "起点不动");
    CHECK((distorted.last() - line.last()).manhattanLength() < 1e-9, "终点不动");

    double maxOffset = 0.0;
    for (int i = 0; i < line.size(); ++i)
        maxOffset = qMax(maxOffset, std::fabs(distorted[i].y()));
    CHECK(maxOffset > 0.5, "中间确实被推离了原线");
    CHECK(maxOffset <= 6.0 + 1e-9, "位移没有超过给定幅度");

    // 幅度为 0 时原样返回
    const QVector<QPointF> untouched = distortPolyline(line, wave, 0.0, 2.0, true);
    CHECK(untouched.size() == line.size() && (untouched.first() - line.first()).manhattanLength() < 1e-9,
          "幅度 0 时不改动");

    // 重采样 / 平滑不炸
    const QVector<QPointF> resampled = resamplePolyline(line, 3.0);
    CHECK(resampled.size() > 10, "重采样得到合理点数");
    CHECK((resampled.first() - line.first()).manhattanLength() < 1e-6, "重采样保持起点");
    CHECK((resampled.last() - line.last()).manhattanLength() < 0.5, "重采样保持终点附近");
    CHECK(smoothPolyline(resampled, 1).size() == resampled.size(), "平滑不改变点数");
}

static void testEffectFormat()
{
    qInfo().noquote() << "效果格式属性";
    QTextDocument document;
    QTextCursor cursor(&document);
    cursor.insertText(QStringLiteral("中文English123"));

    cursor.setPosition(0);
    cursor.setPosition(2, QTextCursor::KeepAnchor);
    QTextCharFormat fmt;
    EffectStyle style;
    style.kind = EffectKind::Handwriting;
    style.seed = 777u;
    setEffectStyle(&fmt, style);
    cursor.mergeCharFormat(fmt);

    cursor.setPosition(0);
    cursor.setPosition(1, QTextCursor::KeepAnchor);
    const EffectStyle read = effectStyle(cursor.charFormat());
    CHECK(read.kind == EffectKind::Handwriting && read.seed == 777u, "写入后能读回同样效果");

    cursor.setPosition(5);
    cursor.setPosition(6, QTextCursor::KeepAnchor);
    CHECK(!hasEffect(cursor.charFormat()), "没设过的字符没有效果");

    // 清除
    cursor.setPosition(0);
    cursor.setPosition(2, QTextCursor::KeepAnchor);
    clearEffects(&cursor);
    cursor.setPosition(0);
    cursor.setPosition(1, QTextCursor::KeepAnchor);
    CHECK(!hasEffect(cursor.charFormat()), "清除后效果消失（字符 0）");
    cursor.setPosition(1);
    cursor.setPosition(2, QTextCursor::KeepAnchor);
    CHECK(!hasEffect(cursor.charFormat()), "清除后效果消失（字符 1）");

    // 字符分类
    const QString mix = QStringLiteral("你好abc，世界x1");
    CHECK(chineseCharsIn(mix).size() == 4, "数出 4 个不同的汉字");
    CHECK(chineseCharsIn(mix, false).size() == 4, "汉字实例数 = 4");
    CHECK(englishCharsIn(mix).size() == 4, "数出 4 个不同的英文字母");
    CHECK(englishCharsIn(QStringLiteral("abcabc"), false).size() == 6, "英文实例数 = 6");

    // 缺字检查
    QSet<QString> available;
    available.insert(QStringLiteral("a"));
    available.insert(QStringLiteral("b"));
    const QStringList missing = missingHandwritingChars(QStringLiteral("abc ab"), available, false);
    CHECK(missing.size() == 1 && missing.first() == QStringLiteral("c"),
          "缺字检查只报 c（空格不计）");
}

static void testFormatMergeSemantics()
{
    qInfo().noquote() << "格式合并语义（决定清除效果该用哪个 API）";
    QTextDocument document;
    QTextCursor cursor(&document);
    cursor.insertText(QStringLiteral("abc"));

    QTextCharFormat fmt;
    EffectStyle style;
    style.kind = EffectKind::Distortion;
    style.seed = 5u;
    setEffectStyle(&fmt, style);
    fmt.setFontPointSize(40.0);
    cursor.setPosition(0);
    cursor.setPosition(3, QTextCursor::KeepAnchor);
    cursor.mergeCharFormat(fmt);

    // 补丁里"没有"某属性时，mergeCharFormat 不会删掉原有属性
    QTextCharFormat emptyPatch;
    emptyPatch.clearProperty(QTextFormat::UserProperty + 0x5710);
    emptyPatch.clearProperty(QTextFormat::UserProperty + 0x5711);
    cursor.setPosition(0);
    cursor.setPosition(3, QTextCursor::KeepAnchor);
    cursor.mergeCharFormat(emptyPatch);
    cursor.setPosition(0);
    cursor.setPosition(1, QTextCursor::KeepAnchor);
    CHECK(hasEffect(cursor.charFormat()),
          "merge 空补丁不会清掉已有属性（所以清除必须用 setCharFormat）");

    // 用原格式去掉属性之后整体写回 —— 这才是可靠的清除
    cursor.setPosition(0);
    cursor.setPosition(1, QTextCursor::KeepAnchor);
    QTextCharFormat original = cursor.charFormat();
    setEffectStyle(&original, EffectStyle());
    cursor.setCharFormat(original);
    cursor.setPosition(0);
    cursor.setPosition(1, QTextCursor::KeepAnchor);
    CHECK(!hasEffect(cursor.charFormat()), "setCharFormat 能清掉效果属性");
    CHECK(qFuzzyCompare(cursor.charFormat().fontPointSize(), 40.0),
          "setCharFormat 保留效果之外的其他格式（字号 40）");
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testCsvParsing();
    testLibrary();
    testSplitByGap();
    testNoise();
    testDistort();
    testEffectFormat();
    testFormatMergeSemantics();

    if (g_failures == 0) {
        qInfo().noquote() << "\n全部通过";
        return 0;
    }
    qWarning().noquote() << "\n失败项:" << g_failures;
    return 1;
}
