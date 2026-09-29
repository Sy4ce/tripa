/*
 * 纯逻辑自检（不需要控件）：CSV 解析、手写库、噪声波、笔画扭曲。
 * 直接运行 build/test_core.exe 即可，退出码 0 表示全过。
 */

#include "effect.h"
#include "handwriting.h"
#include "noise.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
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
    QVector<QVector<HandwritingPoint>> strokes;
    QString error;
    CHECK(parseHandwritingCsv(csv, &label, &strokes, &error), "解析成功");
    CHECK(label == QStringLiteral("a"), "标签为 a");
    CHECK(strokes.size() == 2, "空行切出 2 笔");
    CHECK(strokes.value(0).size() == 2, "第一笔 2 个点");
    CHECK(strokes.value(1).size() == 1, "第二笔 1 个点");
    // 第 3 列（笔压）必须留下来：以前整列被丢掉，深度信息全没了
    CHECK(qFuzzyCompare(strokes.value(0).at(0).pressure, 0.5), "笔压解析为 0.5");
    CHECK(qFuzzyCompare(strokes.value(1).at(0).pressure, 0.7), "第二笔笔压解析为 0.7");
    CHECK(qFuzzyCompare(strokes.value(0).at(0).pos.x(), 10.0), "坐标仍解析正确");

    // 坏行会被跳过；没有第 3 列时笔压记为 -1（"无数据"，不是 0，也不是 1）
    QString label2;
    QVector<QVector<HandwritingPoint>> strokes2;
    CHECK(parseHandwritingCsv(QStringLiteral("b\n1,2\n坏行\n3,4\n"), &label2, &strokes2, nullptr),
          "含坏行仍能解析");
    CHECK(strokes2.size() == 1 && strokes2.first().size() == 2, "坏行被跳过");
    CHECK(strokes2.first().first().pressure < 0.0, "缺第 3 列 -> 笔压为 -1");
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

    /*!
     * 笔压量纲宽容：getpattern 约定是 0~1，
     * 但万一导出的是 0~255 的原始量纲，必须按最大值归一化 ——
     * 直接当 0~1 用会让每个点都顶格，整字糊成一坨。
     */
    CHECK(library.addCsvText(QStringLiteral("p\n0,0,0\n10,0,255\n"), QStringLiteral("mem:p"),
                             &error),
          "加入 0~255 量纲的笔压数据");
    const HandwritingSample *scaled = library.pick(QStringLiteral("p"), 1u);
    CHECK(scaled != nullptr && scaled->hasPressure(), "该样本带笔压");
    if (scaled) {
        CHECK(qFuzzyCompare(scaled->strokes.first().at(1).pressure, 1.0),
              "255 -> 归一化成 1.0");
        CHECK(qFuzzyIsNull(scaled->strokes.first().at(0).pressure), "0 -> 仍是 0");
    }
}

/*!
 * 多字符文件怎么切字。
 *
 * getpattern 的约定是"1 个空行 = 笔画边界、2 个空行 = 字符边界"。
 * 以前的代码把这个信息丢了，一律按 x 间隙猜，猜错的后果是"别的字的笔画
 * 安到这个字头上"——用户看到的就是一团认不出的墨（实测一个"好"只分到 2 笔）。
 */
static void testCharacterGrouping()
{
    qInfo().noquote() << "多字符切分";

    HandwritingLibrary library;
    QString error;

    // 1) 两个空行 = 字符边界：a 有 2 笔、b 有 3 笔
    const QString csv = QStringLiteral(
        "ab\n"
        "0,0,0.3\n"
        "\n"
        "5,0,0.3\n"
        "\n"
        "\n"
        "20,0,0.3\n"
        "\n"
        "25,0,0.3\n"
        "\n"
        "30,0,0.3\n");
    CHECK(library.addCsvText(csv, QStringLiteral("mem:ab"), &error), "按字符边界切分成功");
    const HandwritingSample *a = library.pick(QStringLiteral("a"), 1u);
    const HandwritingSample *b = library.pick(QStringLiteral("b"), 2u);
    CHECK(a && a->strokeCount() == 2, "a 拿到自己的 2 笔");
    CHECK(b && b->strokeCount() == 3, "b 拿到自己的 3 笔");

    /*!
     * 2) 书写顺序 ≠ 阅读顺序：先写右边的 b（3 笔），再写左边的 a（2 笔），
     *    组顺序是反的，但按阅读顺序排完之后 a/b 必须各归各位。
     */
    HandwritingLibrary reordered;
    CHECK(reordered.addCsvText(QStringLiteral(
                                  "ab\n"
                                  "100,0,0.3\n"
                                  "\n"
                                  "105,0,0.3\n"
                                  "\n"
                                  "110,0,0.3\n"
                                  "\n"
                                  "\n"
                                  "0,0,0.3\n"
                                  "\n"
                                  "5,0,0.3\n"),
                              QStringLiteral("mem:ab2"), &error),
          "后写的字在前面的文件也能载入");
    const HandwritingSample *ra = reordered.pick(QStringLiteral("a"), 3u);
    CHECK(ra && ra->strokeCount() == 2, "左边的字拿到 2 笔（不是右边那组）");

    /*!
     * 3) 组数少于字数：写字时没抬笔，两个字的笔画挤在一组里
     *    —— 要按组内 x 空档切开。这里 a|b 挤在第一组（中间 40 单位的空档），
     *    c 自成一组。（笔画都带高度，跟真实数据一样，不是一条水平线。）
     */
    HandwritingLibrary compact;
    CHECK(compact.addCsvText(QStringLiteral(
                                 "abc\n"
                                 "0,0,0.3\n"
                                 "0,20,0.3\n"
                                 "\n"
                                 "8,0,0.3\n"
                                 "8,20,0.3\n"
                                 "\n"
                                 "50,0,0.3\n"
                                 "50,20,0.3\n"
                                 "\n"
                                 "\n"
                                 "100,0,0.3\n"
                                 "100,20,0.3\n"),
                             QStringLiteral("mem:abc"), &error),
          "组数偏少的文件能载入");
    CHECK(compact.characterCount() == 3, "被挤在一起的三个字都切出来了");
    const HandwritingSample *ca = compact.pick(QStringLiteral("a"), 4u);
    CHECK(ca && ca->strokeCount() == 2, "切出来的 a 只有属于它的 2 笔");
    const HandwritingSample *cb = compact.pick(QStringLiteral("b"), 6u);
    CHECK(cb && cb->strokeCount() == 1, "切出来的 b 只有属于它的 1 笔");

    /*!
     * 4) 抬笔跨页的连线（宽高都远大于一个字）必须丢掉：
     *    留着它，后面的字全体错位。
     */
    HandwritingLibrary noisy;
    CHECK(noisy.addCsvText(QStringLiteral(
                               "abc\n"
                               "0,0,0.3\n"
                               "0,20,0.3\n"
                               "\n"
                               "\n"
                               "100,0,0.3\n"
                               "100,20,0.3\n"
                               "\n"
                               "\n"
                               "0,0,0.3\n"
                               "500,300,0.3\n"
                               "\n"
                               "\n"
                               "200,0,0.3\n"
                               "200,20,0.3\n"),
                           QStringLiteral("mem:abc2"), &error),
          "带跨页连线的文件能载入");
    CHECK(noisy.characterCount() == 3, "跨页连线被丢掉后仍是 3 个字");
    const HandwritingSample *nc = noisy.pick(QStringLiteral("c"), 5u);
    CHECK(nc && nc->strokeCount() == 1, "c 没被连线挤掉");
}

/*!
 * 手写目录必须**递归**扫子目录。
 *
 * getpattern 导出的数据常常按人/按次分层（patterns/alice/、patterns/2024-05-01/），
 * 只扫一层的话用户选了上层目录等于什么都没载到。
 */
static void testRecursiveDir()
{
    qInfo().noquote() << "递归扫描子目录";

    QTemporaryDir tmp;
    CHECK(tmp.isValid(), "建立临时目录");
    if (!tmp.isValid())
        return;

    QDir root(tmp.path());
    CHECK(root.mkpath(QStringLiteral("alice/2024-05-01")), "建立两级子目录");

    auto writeCsv = [](const QString &path, const QString &text) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
        f.write(text.toUtf8());
        f.close();
        return true;
    };

    CHECK(writeCsv(root.filePath(QStringLiteral("top.csv")),
                   QStringLiteral("x\n0,0,0.5\n10,10,0.5\n")),
          "根目录写一个 CSV");
    CHECK(writeCsv(root.filePath(QStringLiteral("alice/deep.csv")),
                   QStringLiteral("y\n0,0,0.5\n10,10,0.5\n")),
          "一层子目录写一个 CSV");
    CHECK(writeCsv(root.filePath(QStringLiteral("alice/2024-05-01/deeper.csv")),
                   QStringLiteral("z\n0,0,0.5\n10,10,0.5\n")),
          "两层子目录写一个 CSV");

    HandwritingLibrary library;
    QStringList problems;
    const int loaded = library.loadDir(tmp.path(), &problems);
    CHECK(loaded == 3, "三个 CSV 全部载入（含两层子目录）");
    CHECK(library.contains(QStringLiteral("x")) && library.contains(QStringLiteral("y"))
              && library.contains(QStringLiteral("z")),
          "子目录里的字符都进了库");
}

static void testSplitByGap()
{
    qInfo().noquote() << "按 x 间隙切字";
    // 3 个字符、5 笔：间隙最大的是第 1|2 与第 3|4 笔之间
    QVector<QVector<HandwritingPoint>> strokes;
    const double data[5][2] = {{0, 0}, {8, 0}, {100, 0}, {108, 0}, {200, 0}};
    for (const auto &p : data) {
        QVector<HandwritingPoint> stroke;
        stroke.append({QPointF(p[0], p[1]), 0.4});
        stroke.append({QPointF(p[0] + 5, p[1] + 5), 0.4});
        strokes.append(stroke);
    }

    QVector<QVector<QVector<HandwritingPoint>>> groups;
    CHECK(splitStrokesByXGap(strokes, 3, &groups), "能切成 3 组");
    CHECK(groups.size() == 3, "组数 = 3");
    if (groups.size() == 3) {
        CHECK(groups[0].size() == 2 && groups[1].size() == 2 && groups[2].size() == 1,
              "分组大小 2/2/1");
    }
    QVector<QVector<QVector<HandwritingPoint>>> one;
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

/*!
 * 套扭曲时该用哪种效果。
 *
 * 这条判据决定了用户看到的字**是不是手写体**：已经有手写的字必须保留手写
 * （噪声加在笔迹上），否则 Ctrl+D 会把用户辛苦铺好的手写一次性换成机打字。
 * 逻辑只有一行，但它错了在界面上很难看出来（都是"带噪声的字"），所以要钉住。
 */
static void testDistortionKeepsHandwriting()
{
    qInfo().noquote() << "套扭曲时保留手写";

    QTextCharFormat handwriting;
    EffectStyle style;
    style.kind = EffectKind::Handwriting;
    style.seed = 42u;
    setEffectStyle(&handwriting, style);
    CHECK(distortionEffectKind(handwriting) == EffectKind::Handwriting,
          "已经有手写的字符：套扭曲时保留手写（噪声加在笔迹上）");

    // 手写的种子不影响判断
    QTextCharFormat handwriting2 = handwriting;
    EffectStyle otherSeed;
    otherSeed.kind = EffectKind::Handwriting;
    otherSeed.seed = 0u;
    setEffectStyle(&handwriting2, otherSeed);
    CHECK(distortionEffectKind(handwriting2) == EffectKind::Handwriting,
          "换一个手写种子也照样保留手写");

    QTextCharFormat distortion;
    style.kind = EffectKind::Distortion;
    setEffectStyle(&distortion, style);
    CHECK(distortionEffectKind(distortion) == EffectKind::Distortion,
          "已经是扭曲字形的字符：还是扭曲字形");

    QTextCharFormat none;
    CHECK(distortionEffectKind(none) == EffectKind::Distortion,
          "没效果的字符：套扭曲就是扭曲字形（不看手写库里有没有这个字）");

    QTextCharFormat randomFont;
    EffectStyle rf;
    rf.kind = EffectKind::RandomFont;
    setEffectStyle(&randomFont, rf);
    CHECK(distortionEffectKind(randomFont) == EffectKind::Distortion,
          "只有随机字体的字符：套扭曲换成扭曲字形");
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

/*!
 * 基线 / 大小校正：按 (文件, 字符下标) 存，落在与 CSV 同名的 xml 里，载入时自动读回。
 *
 * 这一组盯的是三件容易悄悄坏掉的事：
 *   1. xml 路径规则（同名去扩展名）与内置资源只读；
 *   2. 同一个 CSV 里**不同字符**的校正不能互相串（所以键是下标不是字符）；
 *   3. "调过" 与 "没调过" 要能区分开 —— 没调过的 csv 不该凭空多出一个 xml。
 */
static void testBaselineAdjustments()
{
    qInfo().noquote() << "基线/大小校正（xml 往返）";

    QTemporaryDir tmp;
    CHECK(tmp.isValid(), "建立临时目录");
    if (!tmp.isValid())
        return;

    QDir root(tmp.path());
    auto writeCsv = [](const QString &path, const QString &text) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
        f.write(text.toUtf8());
        f.close();
        return true;
    };

    const QString csvPath = root.filePath(QStringLiteral("handwrite23.csv"));
    // 两个字：第一个 2 笔，第二个 1 笔（中间两个空行 = 字符边界）
    CHECK(writeCsv(csvPath, QStringLiteral("好云\n0,0,0.5\n10,10,0.5\n\n\n100,0,0.5\n")),
          "写入两个字的 CSV");

    CHECK(HandwritingLibrary::adjustmentPathFor(csvPath)
              == root.filePath(QStringLiteral("handwrite23.xml")),
          "xml 路径 = 同名换 .xml 后缀");
    CHECK(HandwritingLibrary::adjustmentPathFor(QStringLiteral(":/handwrite/a.csv")).isEmpty(),
          "内置资源没有 xml 路径（只读）");

    HandwritingLibrary library;
    CHECK(library.loadFile(csvPath), "载入 CSV");

    const QVector<HandwritingFileEntry> entries = library.fileEntries(csvPath);
    CHECK(entries.size() == 2, "文件里有两个字符槽位");
    CHECK(entries.size() == 2 && entries.at(0).charIndex == 0 && entries.at(1).charIndex == 1,
          "槽位按 charIndex 排好");
    CHECK(entries.size() == 2 && entries.at(0).ch == QStringLiteral("好")
              && entries.at(1).ch == QStringLiteral("云"),
          "槽位的字符与标签一致");

    const HandwritingSample *hao = library.sampleAt(csvPath, 0);
    const HandwritingSample *yun = library.sampleAt(csvPath, 1);
    CHECK(hao && yun, "两个槽位都能取到样本");
    CHECK(hao && hao->charIndex == 0 && yun && yun->charIndex == 1,
          "样本记住了自己在标签里的下标");
    if (!hao || !yun)
        return;

    CHECK(library.adjustment(*hao).isDefault(), "默认没有校正");
    CHECK(library.dirtyAdjustmentFiles().isEmpty(), "只是载入不算脏");

    // 只调第一个字：第二个字必须一点都不受影响
    HandwritingAdjustment adj;
    adj.baseline = 0.035;
    adj.size = 0.86;
    library.setAdjustment(csvPath, 0, adj);
    CHECK(qFuzzyCompare(library.adjustment(*hao).baseline, 0.035), "好 的基线偏移已生效");
    CHECK(qFuzzyCompare(library.adjustment(*hao).size, 0.86), "好 的大小倍率已生效");
    CHECK(library.adjustment(*yun).isDefault(), "同一个文件里 云 不受影响");
    CHECK(library.dirtyAdjustmentFiles() == QStringList{csvPath}, "改动被标记为待保存");
    CHECK(library.adjustedEntryCount() == 1, "统计到 1 个字符调过");

    QStringList problems;
    CHECK(library.saveAdjustments(&problems), "保存成功");
    CHECK(problems.isEmpty(), "保存没有报问题");
    CHECK(library.dirtyAdjustmentFiles().isEmpty(), "保存后不再是脏的");

    const QString xmlPath = root.filePath(QStringLiteral("handwrite23.xml"));
    CHECK(QFile::exists(xmlPath), "写出了一个手写同名 xml");

    // 重新载入：校正值必须自己回来
    HandwritingLibrary reloaded;
    CHECK(reloaded.loadFile(csvPath), "重新载入 CSV");
    const HandwritingSample *hao2 = reloaded.sampleAt(csvPath, 0);
    const HandwritingSample *yun2 = reloaded.sampleAt(csvPath, 1);
    CHECK(hao2 && qFuzzyCompare(reloaded.adjustment(*hao2).baseline, 0.035),
          "重开后 好 的基线偏移还在");
    CHECK(hao2 && qFuzzyCompare(reloaded.adjustment(*hao2).size, 0.86),
          "重开后 好 的大小倍率还在");
    CHECK(yun2 && reloaded.adjustment(*yun2).isDefault(), "重开后 云 仍然是默认值");
    CHECK(reloaded.adjustedFileCount() == 1, "统计到 1 个文件带校正");

    // 改回默认 -> 保存 -> 重开：应当回到没校正的状态
    HandwritingAdjustment none;
    reloaded.setAdjustment(csvPath, 0, none);
    CHECK(reloaded.dirtyAdjustmentFiles() == QStringList{csvPath}, "改回默认也要标脏（xml 里那条得抹掉）");
    CHECK(reloaded.saveAdjustments(nullptr), "把校正改回默认后保存");
    HandwritingLibrary cleared;
    CHECK(cleared.loadFile(csvPath), "再次载入");
    const HandwritingSample *hao3 = cleared.sampleAt(csvPath, 0);
    CHECK(hao3 && cleared.adjustment(*hao3).isDefault(), "改回默认后重开确实是默认值");
    CHECK(cleared.adjustedEntryCount() == 0, "没有非默认校正了");

    // 没调过的 csv 不该产生 xml
    const QString plainPath = root.filePath(QStringLiteral("plain.csv"));
    CHECK(writeCsv(plainPath, QStringLiteral("z\n0,0,0.5\n5,5,0.5\n")), "写入另一个 CSV");
    HandwritingLibrary plain;
    CHECK(plain.loadFile(plainPath), "载入 plain.csv");
    CHECK(plain.saveAdjustments(nullptr), "没改过时保存（应当什么都不做）");
    CHECK(!QFile::exists(root.filePath(QStringLiteral("plain.xml"))),
          "没校正过的 CSV 不会凭空多出一个 xml");

    // 坏 xml：内容不合法时不许崩，也不该把内存里已有的校正清掉
    const QString brokenPath = root.filePath(QStringLiteral("broken.csv"));
    CHECK(writeCsv(brokenPath, QStringLiteral("w\n0,0,0.5\n5,5,0.5\n")), "写入第三个 CSV");
    CHECK(writeCsv(root.filePath(QStringLiteral("broken.xml")),
                   QStringLiteral("<tripaHandwriting><char index=\"0\" baseline=\"0.1\"")),
          "写一个截断的 xml");
    HandwritingLibrary broken;
    CHECK(broken.loadFile(brokenPath), "坏 xml 不影响 CSV 载入");
    const HandwritingSample *brokenSample = broken.sampleAt(brokenPath, 0);
    CHECK(brokenSample && broken.adjustment(*brokenSample).isDefault(),
          "截断的 xml 不会被当成有效校正");
    CHECK(!broken.problems().isEmpty(), "坏 xml 记进了问题列表（会在校对表上提示）");

    // 越界的值要钳住：xml 是文本文件，谁都能手改
    const QString wildPath = root.filePath(QStringLiteral("wild.csv"));
    CHECK(writeCsv(wildPath, QStringLiteral("v\n0,0,0.5\n5,5,0.5\n")), "写入第四个 CSV");
    CHECK(writeCsv(root.filePath(QStringLiteral("wild.xml")),
                   QStringLiteral("<?xml version=\"1.0\"?>\n<tripaHandwriting version=\"1\">\n"
                                  "  <char index=\"0\" label=\"v\" baseline=\"9\" size=\"0\"/>\n"
                                  "</tripaHandwriting>\n")),
          "写入一个数值离谱的 xml");
    HandwritingLibrary wild;
    wild.loadFile(wildPath);
    const HandwritingSample *wildSample = wild.sampleAt(wildPath, 0);
    CHECK(wildSample && wild.adjustment(*wildSample).baseline <= 1.0,
          "离谱的 baseline 被钳到上限");
    CHECK(wildSample && wild.adjustment(*wildSample).size >= 0.05,
          "size=\"0\" 不会把字缩成看不见（钳到下限）");
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testCsvParsing();
    testLibrary();
    testSplitByGap();
    testCharacterGrouping();
    testRecursiveDir();
    testBaselineAdjustments();
    testNoise();
    testDistort();
    testEffectFormat();
    testDistortionKeepsHandwriting();
    testFormatMergeSemantics();

    if (g_failures == 0) {
        qInfo().noquote() << "\n全部通过";
        return 0;
    }
    qWarning().noquote() << "\n失败项:" << g_failures;
    return 1;
}
