/*
 * .tripa 文档格式的自检：文档 -> xml -> 文档，逐项核对。
 *
 * 核心往返全走**内存里的字符串**接口（tripadoc::tripaDocumentToXml /
 * tripaDocumentFromXml），只有一项去真的写文件 —— 这样在"新建目录不可写"
 * 的受限环境里也能把格式本身测干净。
 * 跑法： cmake --build build --target test_document && ./build/test_document.exe
 */

#include "effect.h"
#include "noise.h"
#include "pagesetup.h"
#include "tripadocument.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QXmlStreamReader>

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

namespace {

const QString kPlain = QStringLiteral("普通文字在这里。");
const QString kSimSun = QStringLiteral("宋体二十八");
const QString kDecor = QStringLiteral("粗斜下划线红");

/*!
 * 造一份"什么都有"的文档：普通字、换字体字号的字、粗斜下划线带色的字、
 * 两个段落（第二个有对齐/缩进/段前距/行距）、三种效果各一段。
 */
void buildRichDocument(QTextDocument *doc)
{
    QFont base(QStringLiteral("Microsoft YaHei"));
    base.setPointSizeF(14.0);
    doc->setDefaultFont(base);
    doc->setPageSize(QSizeF(500, 700));

    QTextCursor c(doc);
    c.insertText(kPlain);

    QTextCharFormat simsun;
    simsun.setFontFamilies({QStringLiteral("SimSun")});
    simsun.setFontPointSize(28);
    c.insertText(kSimSun, simsun);

    QTextCharFormat decor;
    decor.setFontWeight(QFont::Bold);
    decor.setFontItalic(true);
    decor.setFontUnderline(true);
    decor.setForeground(QColor(200, 30, 40));
    c.insertText(kDecor, decor);

    // 换段：居中 + 缩进 + 段前距 + 1.5 倍行距
    c.insertBlock();
    QTextBlockFormat bf;
    bf.setAlignment(Qt::AlignCenter);
    bf.setIndent(2);
    bf.setTopMargin(9.5);
    bf.setLineHeight(150.0, QTextBlockFormat::ProportionalHeight);
    c.setBlockFormat(bf);
    c.insertText(QStringLiteral("第二段居中缩进"));

    // 效果：正文第 0..3 个字扭曲、第 6..8 个字手写、第二段随机字体
    const auto applyEffect = [doc](int start, int end, EffectKind kind, quint32 seed) {
        QTextCursor work(doc);
        work.beginEditBlock();
        for (int pos = start; pos < end; ++pos) {
            QTextCursor one(doc);
            one.setPosition(pos);
            one.setPosition(pos + 1, QTextCursor::KeepAnchor);
            EffectStyle style;
            style.kind = kind;
            style.seed = seed + quint32(pos) * 2654435761u;
            QTextCharFormat fmt;
            setEffectStyle(&fmt, style);
            one.mergeCharFormat(fmt);
        }
        work.endEditBlock();
    };
    applyEffect(0, 3, EffectKind::Distortion, 0x1111u);
    applyEffect(6, 8, EffectKind::Handwriting, 0x2222u);
    applyEffect(kPlain.size() + 2, kPlain.size() + 5, EffectKind::RandomFont, 0x3333u);

    doc->documentLayout(); // 逼一次布局，position 才都算准
    doc->setModified(false);
}

/*!
 * 逐字符比较两个文档"看得见的格式"。
 *
 * 一个坑：**没显式设过字体的字符**，回去一遭之后会带上等于文档默认字体的
 * 显式字体属性（Qt 导 HTML 时把默认字体写在 `<body style=...>` 上，
 * 读回来就落到每个字符头上）。渲染结果完全一样，但那不算"格式变了"。
 * 所以比较前先把原文里"没设过字体"的字符按文档默认字体显式化一遍 ——
 * 这里只用默认字体，不走 effectiveFont()（那是 effectsrenderer 里的，
 * 这个自检不该把整份渲染代码拖进来）。
 */
void materializeDefaultFont(QTextDocument *document)
{
    const QFont base = document->defaultFont();
    if (base.family().isEmpty() || base.pointSizeF() <= 0.0)
        return;

    QTextCursor cursor(document);
    cursor.beginEditBlock();
    for (int pos = 0; pos < document->characterCount() - 1; ++pos) {
        QTextCursor one(document);
        one.setPosition(pos);
        one.setPosition(pos + 1, QTextCursor::KeepAnchor);
        const QTextCharFormat current = one.charFormat();
        if (current.hasProperty(QTextFormat::FontFamilies)
            && current.hasProperty(QTextFormat::FontPointSize)) {
            continue;
        }
        QTextCharFormat fmt;
        fmt.setFontFamilies({base.family()});
        fmt.setFontPointSize(base.pointSizeF());
        one.mergeCharFormat(fmt);
    }
    cursor.endEditBlock();
}

int countFormatDiffs(const QTextDocument &a, const QTextDocument &b, QStringList *detail)
{
    const auto signature = [](const QTextCharFormat &f) {
        return QStringLiteral("%1|%2|%3|%4|%5|%6")
            .arg(f.fontFamilies().toStringList().join(QLatin1Char(',')))
            .arg(f.hasProperty(QTextFormat::FontPointSize) ? f.fontPointSize() : -1.0, 0, 'f', 4)
            .arg(f.fontWeight())
            .arg(f.fontItalic() ? 1 : 0)
            .arg(f.fontUnderline() ? 1 : 0)
            .arg(f.foreground().color().name(QColor::HexArgb));
    };

    int diffs = 0;
    const int limits = qMin(a.characterCount(), b.characterCount()) - 1;
    for (int pos = 0; pos < limits; ++pos) {
        QTextCursor ca(const_cast<QTextDocument *>(&a));
        QTextCursor cb(const_cast<QTextDocument *>(&b));
        ca.setPosition(pos);
        ca.setPosition(pos + 1, QTextCursor::KeepAnchor);
        cb.setPosition(pos);
        cb.setPosition(pos + 1, QTextCursor::KeepAnchor);
        if (signature(ca.charFormat()) == signature(cb.charFormat()))
            continue;
        ++diffs;
        if (detail && detail->size() < 8) {
            detail->append(QStringLiteral("位置 %1 的 %2：%3 vs %4")
                               .arg(pos)
                               .arg(ca.selectedText(), signature(ca.charFormat()),
                                    signature(cb.charFormat())));
        }
    }
    return diffs;
}

//! 逐字符比较两个文档的效果
int countEffectDiffs(const QTextDocument &a, const QTextDocument &b)
{
    int diffs = 0;
    const int limits = qMin(a.characterCount(), b.characterCount()) - 1;
    for (int pos = 0; pos < limits; ++pos) {
        QTextCursor ca(const_cast<QTextDocument *>(&a));
        QTextCursor cb(const_cast<QTextDocument *>(&b));
        ca.setPosition(pos);
        ca.setPosition(pos + 1, QTextCursor::KeepAnchor);
        cb.setPosition(pos);
        cb.setPosition(pos + 1, QTextCursor::KeepAnchor);
        if (effectStyle(ca.charFormat()) != effectStyle(cb.charFormat()))
            ++diffs;
    }
    return diffs;
}

//! 把文档画到一张图里（用同一份渲染路径：QTextDocument::drawContents）
QImage renderDocument(QTextDocument *doc)
{
    QImage image(500, 700, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    doc->drawContents(&painter);
    painter.end();
    return image;
}

int countPixelDiffs(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return a.width() * a.height();
    int diffs = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb pa = a.pixel(x, y);
            const QRgb pb = b.pixel(x, y);
            if (qAbs(qRed(pa) - qRed(pb)) > 8 || qAbs(qGreen(pa) - qGreen(pb)) > 8
                || qAbs(qBlue(pa) - qBlue(pb)) > 8) {
                ++diffs;
            }
        }
    }
    return diffs;
}

bool fileHasRootElement(const QString &path, const QString &root, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("打不开 %1").arg(path);
        return false;
    }
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const bool ok = xml.name() == root;
            if (!ok && error)
                *error = QStringLiteral("根元素是 %1，期望 %2").arg(xml.name().toString(), root);
            return ok;
        }
        if (xml.hasError())
            break;
    }
    if (error)
        *error = QStringLiteral("xml 解析失败：%1").arg(xml.errorString());
    return false;
}

//! 文档里带效果的字符数（逐字符数，和 tripadoc::DocumentData 的口径一致）
int countEffectChars(const QTextDocument &document)
{
    int n = 0;
    for (int pos = 0; pos < document.characterCount() - 1; ++pos) {
        QTextCursor one(const_cast<QTextDocument *>(&document));
        one.setPosition(pos);
        one.setPosition(pos + 1, QTextCursor::KeepAnchor);
        if (hasEffect(one.charFormat()))
            ++n;
    }
    return n;
}

} // namespace

// ---------------------------------------------------------------- 各项自检

//! 把对齐的位标志归一成"看起来是什么对齐"：0 和 AlignLeft 都算左对齐
static QString alignmentName(Qt::Alignment alignment)
{
    if (alignment.testFlag(Qt::AlignHCenter))
        return QStringLiteral("center");
    if (alignment.testFlag(Qt::AlignRight))
        return QStringLiteral("right");
    if (alignment.testFlag(Qt::AlignJustify))
        return QStringLiteral("justify");
    return QStringLiteral("left");
}

/*!
 * 造一份"什么都有"的文档 + 非默认的页面 / 渲染参数，下面几项自检共用
 */
static void buildSetup(PageSetup *setup, EffectRenderOptions *options, NoiseWave *wave)
{
    setup->presetName = QStringLiteral("B5");
    setup->widthMm = 176.0;
    setup->heightMm = 250.0;
    setup->landscape = true;
    setup->marginLeftMm = 12.5;
    setup->marginTopMm = 14.25;
    setup->marginRightMm = 15.0;
    setup->marginBottomMm = 16.75;
    setup->gutterMm = 5.5;

    options->showHandwriting = true;
    options->showDistortion = true;
    options->handwritingReplaceText = true;
    options->distortionReplaceText = false;
    options->amplitudePt = 2.25;
    options->waveScale = 3.5;
    options->pressureToWidth = 0.31;
    options->distortionWidth = 1.4;

    wave->reseed(0xC0FFEEu);
}

/*!
 * 核心往返：文档 -> xml -> 文档，逐项核对。
 *
 * 走的是**字符串**接口（tripadoc::tripaDocumentToXml / tripaDocumentFromXml）：
 * 文件读写只是薄薄一层包装（另一个自检管），要害全在这一层。
 */
static void testXmlRoundTrip()
{
    qInfo().noquote() << "文档 -> xml -> 文档";
    QTextDocument original;
    buildRichDocument(&original);
    PageSetup setup;
    EffectRenderOptions options;
    NoiseWave wave;
    buildSetup(&setup, &options, &wave);

    QString error;
    const QString xml =
        tripadoc::tripaDocumentToXml(&original, setup, options, &wave, 0x12345678u, &error);
    CHECK(!xml.isEmpty(), QStringLiteral("生成 xml：%1")
                              .arg(error.isEmpty() ? QStringLiteral("成功") : error));
    CHECK(xml.startsWith(QStringLiteral("<?xml")), "是 xml（带声明）");
    CHECK(xml.contains(QStringLiteral("<tripaDocument")), "有根元素 tripaDocument");
    CHECK(xml.contains(QStringLiteral("<![CDATA[")), "正文用 CDATA 原样嵌着（没被转义成 &lt;）");

    // xml 本身要能被解析器吃掉（自洽的 xml，不是"看起来像"）
    {
        QXmlStreamReader reader(xml);
        while (!reader.atEnd())
            reader.readNext();
        CHECK(!reader.hasError(), QStringLiteral("xml 语法正确（%1）").arg(reader.errorString()));
    }
    qInfo().noquote() << "       xml 里的 <blocks> 段："
                      << xml.section(QStringLiteral("<blocks"), 1, 1)
                             .section(QStringLiteral("</blocks>"), 0, 0).left(400);

    QTextDocument loaded;
    tripadoc::DocumentData data;
    data.pageSetup = PageSetup();
    data.options.showHandwriting = false;
    data.options.showDistortion = false;
    CHECK(tripadoc::tripaDocumentFromXml(&loaded, xml, &data, &error),
          QStringLiteral("读回 xml：%1").arg(error.isEmpty() ? QStringLiteral("成功") : error));

    // --- 文字
    CHECK(loaded.toPlainText() == original.toPlainText(), "正文一字不差");
    CHECK(loaded.blockCount() == original.blockCount(), "段落数一致");
    CHECK(loaded.defaultFont().family() == original.defaultFont().family()
              && qFuzzyCompare(loaded.defaultFont().pointSizeF(),
                               original.defaultFont().pointSizeF()),
          QStringLiteral("文档默认字体一致（%1 %2pt）")
              .arg(original.defaultFont().family())
              .arg(original.defaultFont().pointSizeF()));
    CHECK(qFuzzyCompare(loaded.pageSize().width(), original.pageSize().width())
              && qFuzzyCompare(loaded.pageSize().height(), original.pageSize().height()),
          QStringLiteral("正文栏尺寸一致（断行宽度 %1px）").arg(original.pageSize().width()));

    // --- 效果
    const int effectDiffs = countEffectDiffs(original, loaded);
    if (effectDiffs > 0) {
        // 效果是按字符位置存的，出问题时要能一眼看到是哪个位置错位了
        for (int pos = 0; pos < qMin(original.characterCount(), loaded.characterCount()) - 1; ++pos) {
            QTextCursor ca(&original);
            QTextCursor cb(&loaded);
            ca.setPosition(pos);
            ca.setPosition(pos + 1, QTextCursor::KeepAnchor);
            cb.setPosition(pos);
            cb.setPosition(pos + 1, QTextCursor::KeepAnchor);
            const EffectStyle sa = effectStyle(ca.charFormat());
            const EffectStyle sb = effectStyle(cb.charFormat());
            if (sa == sb)
                continue;
            qWarning().noquote() << "       位置" << pos
                                 << "原文 kind" << int(sa.kind) << "seed" << sa.seed
                                 << "／ 读回 kind" << int(sb.kind) << "seed" << sb.seed;
        }
    }
    CHECK(effectDiffs == 0, QStringLiteral("手写/扭曲/随机字体效果 %1 处差异").arg(effectDiffs));
    CHECK(data.effectCharCount == 8, QStringLiteral("读回 8 个带效果的字符（实际 %1）")
                                         .arg(data.effectCharCount));
    CHECK(data.effectsSummary.contains(QStringLiteral("扭曲"))
              && data.effectsSummary.contains(QStringLiteral("手写")),
          QStringLiteral("效果摘要：%1").arg(data.effectsSummary));

    // --- 段落格式
    {
        bool blockOk = true;
        QTextBlock a = original.begin();
        QTextBlock b = loaded.begin();
        int index = 0;
        while (a.isValid() && b.isValid()) {
            const QTextBlockFormat fa = a.blockFormat();
            const QTextBlockFormat fb = b.blockFormat();
            if (alignmentName(fa.alignment()) != alignmentName(fb.alignment())
                || fa.indent() != fb.indent()
                || !qFuzzyCompare(fa.topMargin() + 1.0, fb.topMargin() + 1.0)
                || !qFuzzyCompare(fa.lineHeight() + 1.0, fb.lineHeight() + 1.0)) {
                blockOk = false;
                qWarning().noquote() << "       段落" << index << "差异："
                                     << "align" << int(fa.alignment()) << int(fb.alignment())
                                     << "indent" << fa.indent() << fb.indent()
                                     << "topMargin" << fa.topMargin() << fb.topMargin()
                                     << "lineHeight" << fa.lineHeight() << fb.lineHeight();
            }
            a = a.next();
            b = b.next();
            ++index;
        }
        CHECK(blockOk, "段落格式（对齐/缩进/段前距/行距）一致");
    }

    // --- 页面设置
    CHECK(data.pageSetupExplicit, "文件里明确写了 <page>");
    CHECK(data.pageSetup.presetName == setup.presetName
              && qFuzzyCompare(data.pageSetup.widthMm, setup.widthMm)
              && qFuzzyCompare(data.pageSetup.heightMm, setup.heightMm)
              && data.pageSetup.landscape == setup.landscape,
          "纸张（预设/宽高/横向）一致");
    CHECK(qFuzzyCompare(data.pageSetup.marginLeftMm, setup.marginLeftMm)
              && qFuzzyCompare(data.pageSetup.marginTopMm, setup.marginTopMm)
              && qFuzzyCompare(data.pageSetup.marginRightMm, setup.marginRightMm)
              && qFuzzyCompare(data.pageSetup.marginBottomMm, setup.marginBottomMm)
              && qFuzzyCompare(data.pageSetup.gutterMm, setup.gutterMm),
          "页边距与装订线一致");

    // --- 渲染参数
    CHECK(data.options.showHandwriting && data.options.showDistortion, "显示开关读回来了");
    CHECK(data.options.handwritingReplaceText && !data.options.distortionReplaceText,
          "「替换正文」两个开关读回来了");
    CHECK(qFuzzyCompare(data.options.amplitudePt, options.amplitudePt)
              && qFuzzyCompare(data.options.waveScale, options.waveScale)
              && qFuzzyCompare(data.options.pressureToWidth, options.pressureToWidth)
              && qFuzzyCompare(data.options.distortionWidth, options.distortionWidth),
          "幅度 / 波数 / 笔宽 / 线宽一致");
    CHECK(data.seed == 0x12345678u, QStringLiteral("种子一致（%1）").arg(data.seed));

    // --- 噪声波：必须逐点相同，否则重开文件字迹就变了
    {
        CHECK(data.wave.termCount() == wave.termCount(),
              QStringLiteral("波形分量数一致（%1）").arg(data.wave.termCount()));
        double worst = 0.0;
        for (int i = 0; i <= 400; ++i) {
            const double t = i / 40.0;
            worst = qMax(worst, qAbs(data.wave.value(t) - wave.value(t)));
        }
        CHECK(worst < 1e-9, QStringLiteral("波形逐点相同（最大偏差 %1）").arg(worst, 0, 'g', 3));
    }

    /*!
     * --- 渲染结果逐像素比较
     *
     * 这是最硬的一条：文字、格式、断行、字体度量只要有一处对不上，
     * 画出来就一定有像素差。放在"显式化字体"之前 ——
     * `materializeDefaultFont()` 会**真的改动** original，改完再比就是拿两份
     * 不同的文档对拍（那次比较放在最后做）。
     */
    {
        QImage a = renderDocument(&original);
        QImage b = renderDocument(&loaded);
        const int pixelDiffs = countPixelDiffs(a, b);
        if (pixelDiffs > 0) {
            // 出错时留对照图 + 差异分布图，不然只看到一句"不一样"没法查
            a.save(QStringLiteral("tripa_roundtrip_original.png"));
            b.save(QStringLiteral("tripa_roundtrip_loaded.png"));
            QString map;
            for (int gy = 0; gy < a.height(); gy += 10) {
                for (int gx = 0; gx < a.width(); gx += 20) {
                    bool any = false;
                    for (int y = gy; y < qMin(gy + 10, a.height()) && !any; ++y)
                        for (int x = gx; x < qMin(gx + 20, a.width()); ++x)
                            if (a.pixel(x, y) != b.pixel(x, y)) {
                                any = true;
                                break;
                            }
                    map += any ? QLatin1Char('#') : QLatin1Char('.');
                }
                map += QLatin1Char('\n');
            }
            qInfo().noquote() << "       差异分布（每格 20x10px）：\n" << map
                              << "       对照图：tripa_roundtrip_original.png / "
                                 "tripa_roundtrip_loaded.png";
        }
        CHECK(pixelDiffs == 0, QStringLiteral("渲染结果逐像素相同（%1 个像素不同）").arg(pixelDiffs));
    }

    // --- 再存一遍：内容应该和第一份完全一样（存读都稳定）
    {
        const QString again = tripadoc::tripaDocumentToXml(&loaded, data.pageSetup, data.options,
                                                           &data.wave, data.seed, &error);
        CHECK(!again.isEmpty(), QStringLiteral("第二次生成 xml：%1")
                                    .arg(error.isEmpty() ? QStringLiteral("成功") : error));
        /*!
         * 只比"正文 + 效果 + 页面"这些实质内容：`saved` 时间戳每次都不一样，
         * 这里把它抹掉再比，剩下的部分必须逐字节相同。
         */
        const auto stripTimestamp = [](QString text) {
            return text.replace(QRegularExpression(QStringLiteral("saved=\"[^\"]*\"")),
                                QStringLiteral("saved=\"\""));
        };
        if (stripTimestamp(xml) != stripTimestamp(again)) {
            // 出错时把两份的差异位置指出来，不然只看到一句"不一样"没法查
            const QString a = stripTimestamp(xml);
            const QString b = stripTimestamp(again);
            int i = 0;
            while (i < a.size() && i < b.size() && a.at(i) == b.at(i))
                ++i;
            qWarning().noquote() << "       第一个不同处在" << i << "\n        A:"
                                 << a.mid(qMax(0, i - 80), 160) << "\n        B:"
                                 << b.mid(qMax(0, i - 80), 160);
        }
        CHECK(stripTimestamp(xml) == stripTimestamp(again),
              "存 -> 读 -> 再存，xml 逐字节相同（除时间戳）");
    }

    // --- 逐字格式（最后做：先把原文里"隐式默认字体"显式化，才和读回来的文档同一口径）
    QStringList details;
    materializeDefaultFont(&original);
    const int formatDiffs = countFormatDiffs(original, loaded, &details);
    for (const QString &line : details)
        qInfo().noquote() << "       " << line;
    CHECK(formatDiffs == 0, QStringLiteral("逐字符格式（字体/字号/粗斜/下划线/颜色）%1 处差异")
                                .arg(formatDiffs));
}

/*!
 * 文件这一层：真写一个 .tripa 再读回来。
 *
 * 受限沙箱里可能"新建的目录不可写"，那时这一项会被跳过并说明原因 ——
 * 格式本身的正确性由 testXmlRoundTrip 保证，这里只验证读写包装没写错。
 */
static void testFileRoundTrip()
{
    qInfo().noquote() << "写盘 / 读盘";

    QString dir = QDir::currentPath();
    const QString path = QDir(dir).filePath(QStringLiteral("tripa_doc_test.tripa"));

    QTextDocument original;
    buildRichDocument(&original);
    PageSetup setup;
    EffectRenderOptions options;
    NoiseWave wave;
    buildSetup(&setup, &options, &wave);

    QString error;
    if (!tripadoc::tripaSaveDocument(&original, path, setup, options, &wave, 0x12345678u, &error)) {
        qInfo().noquote() << "  skip  写不进 " << dir << "（" << error << "），跳过文件往返";
        return;
    }
    CHECK(QFile::exists(path), QStringLiteral("文件真的写出来了：%1").arg(path));

    QString rootError;
    CHECK(fileHasRootElement(path, QStringLiteral("tripaDocument"), &rootError),
          QStringLiteral("根元素是 tripaDocument（%1）").arg(rootError));

    QTextDocument loaded;
    tripadoc::DocumentData data;
    CHECK(tripadoc::tripaLoadDocument(&loaded, path, &data, &error),
          QStringLiteral("读回 .tripa：%1").arg(error.isEmpty() ? QStringLiteral("成功") : error));
    CHECK(loaded.toPlainText() == original.toPlainText(), "文字一致");
    CHECK(countEffectDiffs(original, loaded) == 0, "效果一致");
    CHECK(data.effectCharCount == 8, "效果字符数一致");

    QFile::remove(path);

    // 坏文件：不是 xml
    const QString junk = QDir(dir).filePath(QStringLiteral("tripa_doc_test_junk.tripa"));
    QFile file(junk);
    if (file.open(QIODevice::WriteOnly)) {
        file.write("这不是 xml\n");
        file.close();
        QTextDocument target;
        target.setPlainText(QStringLiteral("原来的内容"));
        tripadoc::DocumentData junkData;
        CHECK(!tripadoc::tripaLoadDocument(&target, junk, &junkData, &error),
              QStringLiteral("拒绝非 xml 文件：%1").arg(error));
        QFile::remove(junk);
    }
}

static void testEditedContentSkipsEffects()
{
    qInfo().noquote() << "正文被改过之后不硬套效果";

    QTextDocument original;
    buildRichDocument(&original);

    QString error;
    EffectRenderOptions options;
    NoiseWave wave;
    wave.reseed(7u);
    QString text = tripadoc::tripaDocumentToXml(&original, PageSetup(), options, &wave, 7u, &error);
    CHECK(!text.isEmpty(), "生成 xml 成功");

    // 像"用别的编辑器改过 xml 正文"那样：把正文换掉（含 hash），但效果区间还是老的
    // 注意用 lastIndexOf：文件头的注释里也提到了 "<content>"，那不算正文
    const int contentStart = text.lastIndexOf(QStringLiteral("<content"));
    CHECK(contentStart > 0, "找到 <content>");
    text.truncate(contentStart);
    text += QStringLiteral("<content hash=\"deadbeef\"><![CDATA[<html><body>"
                           "<p style=\"margin:0;\">完全不同的正文</p></body></html>]]></content>"
                           "</tripaDocument>");

    QTextDocument loaded;
    tripadoc::DocumentData data;
    CHECK(tripadoc::tripaDocumentFromXml(&loaded, text, &data, &error),
          QStringLiteral("仍然能读出来：%1").arg(error));
    qInfo().noquote() << "       读回来的正文：" << loaded.toPlainText()
                      << "／ 指纹匹配：" << data.contentHashMatched;
    CHECK(loaded.toPlainText().contains(QStringLiteral("完全不同的正文")), "正文是新写的那份");
    CHECK(!data.contentHashMatched, "指纹对不上被记下来了");
    CHECK(!data.problems.isEmpty(), "给了明确的提示（而不是静默套错位置）");
    for (const QString &line : data.problems)
        qInfo().noquote() << "       " << line;
    CHECK(data.effectCharCount == 0, "一个效果都没套上去");
    CHECK(countEffectChars(loaded) == 0, "文档里确实没有任何效果属性");
}

static void testBrokenInput()
{
    qInfo().noquote() << "坏输入 / 边界情况（全部在内存里做，不碰磁盘）";

    QString error;

    // 1) 根本不是 xml
    {
        QTextDocument target;
        target.setPlainText(QStringLiteral("原来的内容"));
        tripadoc::DocumentData data;
        CHECK(!tripadoc::tripaDocumentFromXml(&target, QStringLiteral("这不是 xml\n"), &data, &error),
              "拒绝非 xml 内容");
        CHECK(!error.isEmpty(), QStringLiteral("给了错误说明：%1").arg(error));
        CHECK(target.toPlainText() == QStringLiteral("原来的内容"), "失败时不动原文档");
    }

    // 2) 根元素不对（别的软件的 xml）
    {
        QTextDocument doc;
        tripadoc::DocumentData data;
        CHECK(!tripadoc::tripaDocumentFromXml(
                  &doc, QStringLiteral("<?xml version=\"1.0\"?><somethingElse><a/></somethingElse>"),
                  &data, &error),
              "拒绝根元素不对的 xml");
    }

    // 3) 有根元素但没有正文
    {
        QTextDocument doc;
        tripadoc::DocumentData data;
        CHECK(!tripadoc::tripaDocumentFromXml(
                  &doc, QStringLiteral("<?xml version=\"1.0\"?><tripaDocument version=\"1\"/>"),
                  &data, &error),
              "拒绝没有 <content> 的文件");
    }

    // 4) 合法但缺各种段落：页面 / 参数 / 波形全缺，也该读得出来
    {
        QTextDocument minimal;
        buildRichDocument(&minimal);
        EffectRenderOptions options;
        QString saveError;
        const QString xml =
            tripadoc::tripaDocumentToXml(&minimal, PageSetup(), options, nullptr, 0, &saveError);
        CHECK(!xml.isEmpty(), "生成最小文档的 xml");

        // 把 <page> 那一段整个删掉，模拟"旧文件没有页面设置"
        QString pruned = xml;
        const int pageStart = pruned.indexOf(QStringLiteral("<page"));
        const int pageEnd = pruned.indexOf(QStringLiteral("</page>"));
        if (pageStart > 0 && pageEnd > pageStart)
            pruned.remove(pageStart, pageEnd + 7 - pageStart);

        QTextDocument loadedMinimal;
        tripadoc::DocumentData minData;
        minData.pageSetup = PageSetup();
        minData.pageSetup.presetName = QStringLiteral("A5"); // 文件里没写，应该保留这个
        CHECK(tripadoc::tripaDocumentFromXml(&loadedMinimal, pruned, &minData, &error),
              QStringLiteral("读回最小文档：%1").arg(error));
        CHECK(loadedMinimal.toPlainText() == minimal.toPlainText(), "正文照样对");
        CHECK(!minData.pageSetupExplicit, "知道文件里没写页面设置");
        CHECK(minData.pageSetup.presetName == QStringLiteral("A5"), "没写页面设置时保留调用方的值");
    }

    // 5) 越界 / 反向的效果区间被丢掉，不会崩
    {
        QTextDocument odd;
        tripadoc::DocumentData oddData;
        const QString sample = QStringLiteral(
            "<?xml version=\"1.0\"?><tripaDocument version=\"1\">"
            "<runs><run start=\"900\" end=\"9999\" kind=\"distortion\" seed=\"5\"/>"
            "<run start=\"-4\" end=\"-1\" kind=\"handwriting\" seed=\"6\"/></runs>"
            "<content><![CDATA[<html><body><p>短</p></body></html>]]></content>"
            "</tripaDocument>");
        QXmlStreamReader probe(sample);
        while (!probe.atEnd())
            probe.readNext();
        CHECK(!probe.hasError(), QStringLiteral("样例 xml 本身合法：%1").arg(probe.errorString()));
        CHECK(tripadoc::tripaDocumentFromXml(&odd, sample, &oddData, &error),
              QStringLiteral("越界区间不影响读取：%1").arg(error));
        CHECK(odd.toPlainText() == QStringLiteral("短"), "正文读出来了");
        CHECK(oddData.effectCharCount == 0, "越界的效果区间被跳过");
    }

    // 6) 不认识的元素 / 属性直接跳过（前向兼容）
    {
        QTextDocument doc;
        tripadoc::DocumentData data;
        const QString sample = QStringLiteral(
            "<?xml version=\"1.0\"?><tripaDocument version=\"99\">"
            "<somethingNew a=\"1\"><inner/></somethingNew>"
            "<effects seed=\"42\"><handwriting visible=\"maybe\" bogus=\"x\"/></effects>"
            "<content><![CDATA[<html><body><p>兼容</p></body></html>]]></content>"
            "</tripaDocument>");
        CHECK(tripadoc::tripaDocumentFromXml(&doc, sample, &data, &error),
              QStringLiteral("不认识的元素跳过，照样能读：%1").arg(error));
        CHECK(doc.toPlainText() == QStringLiteral("兼容"), "正文读出来了");
        CHECK(data.seed == 42u, "认识的属性照常生效");
        CHECK(!data.options.showHandwriting, "看不懂的布尔值不会当成 true");
    }

    // 7) 页面设置的数值越界会被钳住（xml 谁都能手改）
    {
        QTextDocument doc;
        tripadoc::DocumentData data;
        const QString sample = QStringLiteral(
            "<?xml version=\"1.0\"?><tripaDocument version=\"1\">"
            "<page widthMm=\"-500\" heightMm=\"999999\" orientation=\"portrait\">"
            "<margins leftMm=\"-10\" rightMm=\"30\" bottomMm=\"40\" gutterMm=\"0\"/></page>"
            "<content><![CDATA[<html><body><p>x</p></body></html>]]></content>"
            "</tripaDocument>");
        CHECK(tripadoc::tripaDocumentFromXml(&doc, sample, &data, &error),
              QStringLiteral("越界的页面数值不报错（钳住）：%1").arg(error));
        CHECK(data.pageSetup.widthMm > 0.0 && data.pageSetup.widthMm <= 5000.0,
              QStringLiteral("宽度被钳到 %1").arg(data.pageSetup.widthMm));
        CHECK(data.pageSetup.heightMm <= 5000.0, "高度被钳住");
        CHECK(data.pageSetup.marginLeftMm >= 0.0, "负边距被钳成 0");
        CHECK(qFuzzyCompare(data.pageSetup.marginRightMm, 30.0), "正常值原样保留");
    }

    // 8) 扩展名 -> .tripa
    CHECK(tripadoc::tripaSuffixWithExtension(QStringLiteral("C:/x/未命名.txt"))
              == QStringLiteral("C:/x/未命名.tripa"),
          "另存为会把 .txt 换成 .tripa");
    CHECK(tripadoc::tripaSuffixWithExtension(QStringLiteral("C:/x/a.tripa"))
              == QStringLiteral("C:/x/a.tripa"),
          "已经是 .tripa 就不动");
    CHECK(tripadoc::tripaSuffixWithExtension(QStringLiteral("C:/x/abc"))
              == QStringLiteral("C:/x/abc.tripa"),
          "没有扩展名也会补上 .tripa");
}

static void testEmptyDocument()
{
    qInfo().noquote() << "空文档";

    QTextDocument empty;
    QString error;
    EffectRenderOptions options;
    NoiseWave wave;
    wave.reseed(3u);

    const QString xml = tripadoc::tripaDocumentToXml(&empty, PageSetup(), options, &wave, 3u,
                                                     &error);
    CHECK(!xml.isEmpty(), QStringLiteral("空文档也能生成 xml：%1").arg(error));

    QTextDocument loaded;
    tripadoc::DocumentData data;
    CHECK(tripadoc::tripaDocumentFromXml(&loaded, xml, &data, &error),
          QStringLiteral("空文档也能读：%1").arg(error));
    CHECK(loaded.toPlainText().isEmpty(), "读回来还是空的");
    CHECK(data.effectCharCount == 0, "没有效果");
    CHECK(data.wave.termCount() == wave.termCount(), "波形照样存下来了");
}

static void testNoiseWaveRoundTrip()
{
    qInfo().noquote() << "噪声波自身";

    NoiseWave original;
    original.reseed(20240925u);

    QVector<NoiseWave::Term> terms = original.terms();
    double norm = 0.0;
    for (const NoiseWave::Term &term : terms)
        norm += term.amplitude;

    NoiseWave copy;
    CHECK(copy.setTerms(terms, norm), "分量能装回去");
    double worst = 0.0;
    for (int i = 0; i <= 200; ++i)
        worst = qMax(worst, qAbs(copy.value(i / 20.0) - original.value(i / 20.0)));
    CHECK(worst < 1e-12, QStringLiteral("两个波形逐点相同（最大偏差 %1）").arg(worst, 0, 'g', 3));

    // 坏数据一律拒收，波形保持不变
    NoiseWave guard;
    guard.reseed(5u);
    const double before = guard.value(0.37);
    QVector<NoiseWave::Term> bad = terms;
    bad[0].amplitude = -1.0;
    CHECK(!guard.setTerms(bad, norm), "负幅度被拒收");
    CHECK(!guard.setTerms({}, 1.0), "空分量被拒收");
    CHECK(!guard.setTerms(terms, 0.0), "归一化系数为 0 被拒收");
    CHECK(qFuzzyCompare(guard.value(0.37), before), "拒收之后波形没被改坏");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("tripa"));
    QApplication::setOrganizationName(QStringLiteral("mywrite"));

    testNoiseWaveRoundTrip();
    testXmlRoundTrip();
    testFileRoundTrip();
    testEditedContentSkipsEffects();
    testBrokenInput();
    testEmptyDocument();

    if (g_failures == 0) {
        qInfo().noquote() << "\ntest_document: 全部通过";
        return 0;
    }
    qWarning().noquote() << "\ntest_document:" << g_failures << "项失败";
    return 1;
}
