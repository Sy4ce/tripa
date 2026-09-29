#include "tripadocument.h"

#include "effect.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QSizeF>
#include <QStringConverter>

#include <algorithm>
#include <cmath>

namespace tripadoc {
namespace {

//! 保存时写进文件的字节数上限（防止误选一个巨大的文件）；读的时候不设限
constexpr int kMaxHtmlBytes = 64 * 1024 * 1024;

QString num(double value)
{
    return QString::number(value, 'g', 12);
}

QString effectKindName(EffectKind kind)
{
    switch (kind) {
    case EffectKind::RandomFont:
        return QStringLiteral("randomFont");
    case EffectKind::Handwriting:
        return QStringLiteral("handwriting");
    case EffectKind::Distortion:
        return QStringLiteral("distortion");
    case EffectKind::None:
        break;
    }
    return QStringLiteral("none");
}

EffectKind effectKindFromName(const QString &name)
{
    if (name == QLatin1String("randomFont"))
        return EffectKind::RandomFont;
    if (name == QLatin1String("handwriting"))
        return EffectKind::Handwriting;
    if (name == QLatin1String("distortion"))
        return EffectKind::Distortion;
    return EffectKind::None;
}

bool isFinite(double value)
{
    return std::isfinite(value);
}

//! 越界钳住：xml 是文本文件，谁都能手改
double clampedAttr(const QXmlStreamAttributes &attrs, const QString &name,
                   double fallback, double lo, double hi)
{
    if (!attrs.hasAttribute(name))
        return fallback;
    bool ok = false;
    const double value = attrs.value(name).toDouble(&ok);
    if (!ok || !isFinite(value))
        return fallback;
    return qBound(lo, value, hi);
}

// ---------------------------------------------------------------- 正文 / 效果区间

/*!
 * 取文档正文的 HTML（含逐字格式与段落格式）。
 *
 * Qt6 只留了无参的 `toHtml()`：写进来的 `<body>` 上带着默认字体，
 * 读回来时 Qt 会把它装回文档的默认字体 —— 往返之后连"没显式设过字体的字"
 * 都算得对，所以这里不需要额外带什么 css。
 */
QString documentHtml(const QTextDocument *document)
{
    return document ? document->toHtml() : QString();
}

/*!
 * \brief 从文档里扫出"带效果的连续字符区间"。
 *
 * 逐 block 逐 fragment 扫，因为效果属性就是存在 fragment 的 charFormat 上的：
 * 相邻 fragment 的效果一样就并成一段，免得每个字写一个 `<run>`。
 * block 之间的分隔符（换行）不会有 fragment 覆盖，自然被跳过 ——
 * 效果本来也不该落在换行上。
 */
void scanEffectRuns(const QTextDocument *document,
                    QVector<EffectRun> *out,
                    int *charCount)
{
    if (out)
        out->clear();
    if (charCount)
        *charCount = 0;
    if (!document)
        return;

    QVector<EffectRun> runs;
    int total = 0;
    int offset = 0;

    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        // Qt6 的 QTextBlock::begin() 返回迭代器，要 .fragment() 才拿到 QTextFragment
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QString text = fragment.text();
            const int start = offset + fragment.position() - block.position();
            const int end = start + text.size();
            const EffectStyle style = effectStyle(fragment.charFormat());
            if (style.kind != EffectKind::None) {
                total += text.size();
                if (!runs.isEmpty() && runs.last().end == start
                    && runs.last().style == style) {
                    runs.last().end = end;      // 和上一段连成一片
                } else {
                    EffectRun run;
                    run.start = start;
                    run.end = end;
                    run.style = style;
                    runs.append(run);
                }
            }
        }
        offset += block.length(); // length() 含 block 末尾那一个换行
    }

    if (out)
        *out = runs;
    if (charCount)
        *charCount = total;
}

//! 正文的指纹：用来判断"读回来的文字和存进去的是不是同一份"
QString contentHash(const QString &plainText)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(plainText.toUtf8(), QCryptographicHash::Sha256).toHex());
}

//! 位置 \a pos 上那一个字符是不是真的字符（不是 block 之间的分隔符）
bool positionIsContent(const QTextDocument *document, int pos)
{
    const QTextBlock block = document->findBlock(pos);
    if (!block.isValid())
        return false;
    const int rel = pos - block.position();
    return rel >= 0 && rel < block.length() - 1;
}

/*!
 * 把效果区间套回文档。
 *
 * 位置是写文件时记下的字符下标；装了 HTML 之后如果正文**一模一样**，
 * 下标就还是准的。凡是越界、落在块分隔符上、或者区间里混进了不同内容的，
 * 一律跳过并记一条问题 —— 宁可不套，也不能把效果糊到别的字上。
 */
void applyEffectRuns(QTextDocument *document,
                     const QVector<EffectRun> &runs,
                     QStringList *problems,
                     int *applied)
{
    if (applied)
        *applied = 0;
    if (!document)
        return;

    QTextCursor work(document);
    work.beginEditBlock();

    for (const EffectRun &run : runs) {
        if (run.style.kind == EffectKind::None)
            continue;
        for (int pos = run.start; pos < run.end; ++pos) {
            if (!positionIsContent(document, pos)) {
                if (problems)
                    problems->append(QStringLiteral("效果区间 [%1, %2) 越出了正文，已跳过")
                                         .arg(run.start).arg(run.end));
                break;
            }
            QTextCursor one(document);
            one.setPosition(pos);
            one.setPosition(pos + 1, QTextCursor::KeepAnchor);
            QTextCharFormat fmt;
            setEffectStyle(&fmt, run.style);
            one.mergeCharFormat(fmt);
            if (applied)
                ++*applied;
        }
    }

    work.endEditBlock();
}

// ---------------------------------------------------------------- 段落格式

/*!
 * 段落格式（对齐 / 缩进 / 段前段后 / 行距）单独存一份。
 *
 * 为什么不靠正文 HTML 里那些 `style="margin-top:9.5px"`：Qt 导 HTML 时把
 * 长度值**四舍五入到整像素**（实测 9.5px 出去、回来变成 10px），
 * 字号、行距、缩进这些排版参数因此会悄悄走样。HTML 那边管字体、颜色、
 * 粗斜下划线就够（那几样是精确往返的），段落格式这里精确存一份。
 * `para` 是段落序号（从 0 起，和文档里的 block 一一对应）。
 */
void writeBlockFormats(QXmlStreamWriter &xml, const QTextDocument *document)
{
    if (document->isEmpty() && document->blockCount() <= 1) {
        // 空文档：一个默认段落，写不写都一样，干脆不写
        return;
    }

    xml.writeStartElement(QStringLiteral("blocks"));
    xml.writeComment(QStringLiteral(
        " 段落格式：HTML 里那些 margin/line-height 会被 Qt 四舍五入到整像素，"
        "所以精确值单独记一份；para 是段落序号（从 0 起）。"));

    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        const QTextBlockFormat format = block.blockFormat();
        xml.writeStartElement(QStringLiteral("block"));
        xml.writeAttribute(QStringLiteral("para"), QString::number(block.blockNumber()));
        /*!
         * 对齐**一律显式写**：没设过对齐的段落，`blockFormat().alignment()`
         * 取出来是 AlignLeft（枚举的默认值），而这个段落在屏幕上的真实对齐
         * 可能来自别的属性（QTextBlockFormat 里对齐位和"绝对定位"位是同一个
         * 标志字：实测一段居中的文字，hasProperty(BlockAlignment) 为**假**、
         * 取出来的却是 AlignCenter|AlignAbsolute=132）。照着"属性存在才写"
         * 会把这个段落按左对齐钉死，重开文件就跑版了。
         */
        QString align = QStringLiteral("left");
        const Qt::Alignment alignment = format.alignment();
        if (alignment.testFlag(Qt::AlignHCenter))
            align = QStringLiteral("center");
        else if (alignment.testFlag(Qt::AlignRight))
            align = QStringLiteral("right");
        else if (alignment.testFlag(Qt::AlignJustify))
            align = QStringLiteral("justify");
        xml.writeAttribute(QStringLiteral("align"), align);

        xml.writeAttribute(QStringLiteral("indent"), QString::number(format.indent()));
        xml.writeAttribute(QStringLiteral("topMm"), num(format.topMargin()));
        xml.writeAttribute(QStringLiteral("bottomMm"), num(format.bottomMargin()));
        xml.writeAttribute(QStringLiteral("leftMm"), num(format.leftMargin()));
        xml.writeAttribute(QStringLiteral("rightMm"), num(format.rightMargin()));
        xml.writeAttribute(QStringLiteral("lineHeight"), num(format.lineHeight()));
        /*!
         * 行距类型存名字（single / fixed / proportional / minimum / lineDistance），
         * 和 Qt 的枚举一一对应；认不出来的名字按"单倍"处理。
         */
        const char *typeName = "single";
        switch (format.lineHeightType()) {
        case QTextBlockFormat::FixedHeight:
            typeName = "fixed";
            break;
        case QTextBlockFormat::ProportionalHeight:
            typeName = "proportional";
            break;
        case QTextBlockFormat::MinimumHeight:
            typeName = "minimum";
            break;
        case QTextBlockFormat::LineDistanceHeight:
            typeName = "lineDistance";
            break;
        case QTextBlockFormat::SingleHeight:
            break;
        }
        xml.writeAttribute(QStringLiteral("lineType"), QLatin1String(typeName));
        xml.writeEndElement();
    }
    xml.writeEndElement();
}

void readBlockFormat(const QXmlStreamAttributes &attrs, QTextBlockFormat *format)
{
    if (attrs.hasAttribute(QStringLiteral("align"))) {
        const QString align = attrs.value(QStringLiteral("align")).toString();
        /*!
         * 用 Qt::AlignCenter / AlignRight 这些**现成的常量**，别自己拼位：
         * Qt 的居中常量是 AlignHCenter|AlignVCenter|AlignAbsolute = 132，
         * 手拼一个 AlignHCenter|AlignAbsolute 只有 20 —— 渲染上看不出差别，
         * 但"存了再读、格式一模一样"这条就断了（自检里逐段落比对会报差异）。
         */
        if (align == QLatin1String("center"))
            format->setAlignment(Qt::AlignCenter);
        else if (align == QLatin1String("right"))
            format->setAlignment(Qt::AlignRight);
        else if (align == QLatin1String("justify"))
            format->setAlignment(Qt::AlignJustify);
        else
            format->setAlignment(Qt::AlignLeft);
    }
    if (attrs.hasAttribute(QStringLiteral("indent"))) {
        bool ok = false;
        const int indent = attrs.value(QStringLiteral("indent")).toInt(&ok);
        if (ok)
            format->setIndent(qBound(0, indent, 1000));
    }
    format->setTopMargin(clampedAttr(attrs, QStringLiteral("topMm"), format->topMargin(),
                                     0.0, 2000.0));
    format->setBottomMargin(clampedAttr(attrs, QStringLiteral("bottomMm"), format->bottomMargin(),
                                        0.0, 2000.0));
    format->setLeftMargin(clampedAttr(attrs, QStringLiteral("leftMm"), format->leftMargin(),
                                      0.0, 2000.0));
    format->setRightMargin(clampedAttr(attrs, QStringLiteral("rightMm"), format->rightMargin(),
                                       0.0, 2000.0));

    const double lineHeight = clampedAttr(attrs, QStringLiteral("lineHeight"),
                                          format->lineHeight(), 0.0, 100000.0);
    const QString type = attrs.value(QStringLiteral("lineType")).toString();
    QTextBlockFormat::LineHeightTypes typeValue = QTextBlockFormat::SingleHeight;
    if (type == QLatin1String("fixed"))
        typeValue = QTextBlockFormat::FixedHeight;
    else if (type == QLatin1String("proportional"))
        typeValue = QTextBlockFormat::ProportionalHeight;
    else if (type == QLatin1String("minimum"))
        typeValue = QTextBlockFormat::MinimumHeight;
    else if (type == QLatin1String("lineDistance"))
        typeValue = QTextBlockFormat::LineDistanceHeight;
    format->setLineHeight(lineHeight, typeValue);
}

/*!
 * 把 `<blocks>` 里读到的段落格式套回文档。
 *
 * 按**段落序号**对齐（HTML 往返之后段落数、段落顺序是稳的），
 * 越界的序号直接跳过 —— 文件被改过时宁可少套，也不能把格式糊到别的段上。
 */
void applyBlockFormats(QTextDocument *document,
                       const QVector<QPair<int, QTextBlockFormat>> &formats,
                       QStringList *problems,
                       bool *applied)
{
    if (applied)
        *applied = false;

    QTextCursor work(document);
    work.beginEditBlock();
    for (const auto &entry : formats) {
        const QTextBlock block = document->findBlockByNumber(entry.first);
        if (!block.isValid()) {
            if (problems)
                problems->append(QStringLiteral("段落格式里有第 %1 段，但文档只有 %2 段，已跳过")
                                     .arg(entry.first)
                                     .arg(document->blockCount()));
            continue;
        }
        QTextCursor one(block);
        // setBlockFormat 是**合并**：写进去的属性会覆盖，没写的保持原样
        one.setBlockFormat(entry.second);
        if (applied)
            *applied = true;
    }
    work.endEditBlock();
}

// ---------------------------------------------------------------- 默认字体

/*!
 * 文档默认字体单独记一份。
 *
 * **不能指望 HTML 里那个 `<body style="font-family:...;font-size:14pt">`**：
 * Qt 的 HTML 导入器只在"文档默认字体还是应用默认字体"时才去读 body 上的字体，
 * 而且实测在**新建的** QTextDocument 上 `setHtml()` 之后默认字体仍是
 * `QApplication` 的默认值（Microsoft YaHei UI 9pt）—— 于是整篇 14pt 的文章
 * 重开之后看起来只影响"没显式设过字号"的字（字号在 charFormat 里记着，
 * 显示上可能看不出来），可只要有一段文字没设过字号，行高、断行就全变了。
 * 与其猜 Qt 什么时候认 body，不如自己记一份、读回来直接 setDefaultFont()。
 */
void writeDefaultFont(QXmlStreamWriter &xml, const QTextDocument *document)
{
    const QFont font = document->defaultFont();
    if (font.family().isEmpty() && font.pointSizeF() <= 0.0)
        return;
    xml.writeStartElement(QStringLiteral("font"));
    xml.writeAttribute(QStringLiteral("family"), font.family());
    if (font.pointSizeF() > 0.0)
        xml.writeAttribute(QStringLiteral("pointSize"), num(font.pointSizeF()));
    else if (font.pixelSize() > 0)
        xml.writeAttribute(QStringLiteral("pixelSize"), QString::number(font.pixelSize()));
    xml.writeEndElement();
}

void readDefaultFont(const QXmlStreamAttributes &attrs, QFont *font)
{
    if (attrs.hasAttribute(QStringLiteral("family")))
        font->setFamily(attrs.value(QStringLiteral("family")).toString());
    if (attrs.hasAttribute(QStringLiteral("pointSize"))) {
        bool ok = false;
        const double size = attrs.value(QStringLiteral("pointSize")).toDouble(&ok);
        if (ok && isFinite(size) && size > 0.0 && size < 1000.0)
            font->setPointSizeF(size);
    } else if (attrs.hasAttribute(QStringLiteral("pixelSize"))) {
        bool ok = false;
        const int size = attrs.value(QStringLiteral("pixelSize")).toInt(&ok);
        if (ok && size > 0 && size < 2000)
            font->setPixelSize(size);
    }
}

// ---------------------------------------------------------------- 页面设置
void writePageSetup(QXmlStreamWriter &xml, const PageSetup &setup)
{
    xml.writeStartElement(QStringLiteral("page"));
    xml.writeAttribute(QStringLiteral("preset"), setup.presetName);
    xml.writeAttribute(QStringLiteral("widthMm"), num(setup.widthMm));
    xml.writeAttribute(QStringLiteral("heightMm"), num(setup.heightMm));
    xml.writeAttribute(QStringLiteral("orientation"),
                       setup.landscape ? QStringLiteral("landscape")
                                       : QStringLiteral("portrait"));
    xml.writeStartElement(QStringLiteral("margins"));
    xml.writeAttribute(QStringLiteral("leftMm"), num(setup.marginLeftMm));
    xml.writeAttribute(QStringLiteral("topMm"), num(setup.marginTopMm));
    xml.writeAttribute(QStringLiteral("rightMm"), num(setup.marginRightMm));
    xml.writeAttribute(QStringLiteral("bottomMm"), num(setup.marginBottomMm));
    xml.writeAttribute(QStringLiteral("gutterMm"), num(setup.gutterMm));
    xml.writeEndElement();
    xml.writeEndElement();
}

void readPageSetup(const QXmlStreamAttributes &attrs, PageSetup *setup)
{
    if (attrs.hasAttribute(QStringLiteral("preset")))
        setup->presetName = attrs.value(QStringLiteral("preset")).toString();
    setup->widthMm = clampedAttr(attrs, QStringLiteral("widthMm"), setup->widthMm, 1.0, 5000.0);
    setup->heightMm = clampedAttr(attrs, QStringLiteral("heightMm"), setup->heightMm, 1.0, 5000.0);
    if (attrs.hasAttribute(QStringLiteral("orientation"))) {
        setup->landscape =
            attrs.value(QStringLiteral("orientation")).toString() == QLatin1String("landscape");
    }
}

void readMargins(const QXmlStreamAttributes &attrs, PageSetup *setup)
{
    setup->marginLeftMm = clampedAttr(attrs, QStringLiteral("leftMm"), setup->marginLeftMm, 0.0, 500.0);
    setup->marginTopMm = clampedAttr(attrs, QStringLiteral("topMm"), setup->marginTopMm, 0.0, 500.0);
    setup->marginRightMm = clampedAttr(attrs, QStringLiteral("rightMm"), setup->marginRightMm, 0.0, 500.0);
    setup->marginBottomMm = clampedAttr(attrs, QStringLiteral("bottomMm"), setup->marginBottomMm, 0.0, 500.0);
    setup->gutterMm = clampedAttr(attrs, QStringLiteral("gutterMm"), setup->gutterMm, 0.0, 500.0);
}

// ---------------------------------------------------------------- 渲染参数

/*!
 * 写渲染参数。
 *
 * 只写"影响画面"的那几项：颜色是从 `replaceText` 开关推出来的
 * （见 MainWindow::buildRenderOptions），所以存开关就够；
 * `library` 是运行时的手写库、`missing` 是每轮绘制的临时统计，都不该进文件。
 */
void writeEffectsConfig(QXmlStreamWriter &xml, const EffectRenderOptions &options)
{
    xml.writeStartElement(QStringLiteral("effects"));
    xml.writeStartElement(QStringLiteral("handwriting"));
    xml.writeAttribute(QStringLiteral("visible"), options.showHandwriting ? QStringLiteral("true")
                                                                         : QStringLiteral("false"));
    xml.writeAttribute(QStringLiteral("replaceText"),
                       options.handwritingReplaceText ? QStringLiteral("true")
                                                      : QStringLiteral("false"));
    xml.writeAttribute(QStringLiteral("pressureToWidth"), num(options.pressureToWidth));
    xml.writeEndElement();

    xml.writeStartElement(QStringLiteral("distortion"));
    xml.writeAttribute(QStringLiteral("visible"), options.showDistortion ? QStringLiteral("true")
                                                                        : QStringLiteral("false"));
    xml.writeAttribute(QStringLiteral("replaceText"),
                       options.distortionReplaceText ? QStringLiteral("true")
                                                     : QStringLiteral("false"));
    xml.writeAttribute(QStringLiteral("amplitudePt"), num(options.amplitudePt));
    xml.writeAttribute(QStringLiteral("waveScale"), num(options.waveScale));
    xml.writeAttribute(QStringLiteral("lineWidth"), num(options.distortionWidth));
    xml.writeEndElement();
    xml.writeEndElement();
}

void readEffectsConfig(const QString &name,
                       const QXmlStreamAttributes &attrs,
                       EffectRenderOptions *options)
{
    const auto flag = [&attrs](const QString &key, bool fallback) {
        if (!attrs.hasAttribute(key))
            return fallback;
        const QString value = attrs.value(key).toString();
        return value == QLatin1String("true") || value == QLatin1String("1");
    };

    if (name == QLatin1String("handwriting")) {
        options->showHandwriting = flag(QStringLiteral("visible"), options->showHandwriting);
        options->handwritingReplaceText =
            flag(QStringLiteral("replaceText"), options->handwritingReplaceText);
        options->pressureToWidth = clampedAttr(attrs, QStringLiteral("pressureToWidth"),
                                               options->pressureToWidth, 0.0, 2.0);
    } else if (name == QLatin1String("distortion")) {
        options->showDistortion = flag(QStringLiteral("visible"), options->showDistortion);
        options->distortionReplaceText =
            flag(QStringLiteral("replaceText"), options->distortionReplaceText);
        options->amplitudePt = clampedAttr(attrs, QStringLiteral("amplitudePt"),
                                           options->amplitudePt, 0.0, 200.0);
        options->waveScale = clampedAttr(attrs, QStringLiteral("waveScale"),
                                         options->waveScale, 0.05, 100.0);
        options->distortionWidth = clampedAttr(attrs, QStringLiteral("lineWidth"),
                                               options->distortionWidth, 0.01, 20.0);
    }
}

// ---------------------------------------------------------------- 噪声波

void writeWave(QXmlStreamWriter &xml, const NoiseWave &wave)
{
    xml.writeStartElement(QStringLiteral("wave"));
    xml.writeAttribute(QStringLiteral("terms"), QString::number(wave.termCount()));

    /*!
     * 归一化系数不单独写：它恒等于各分量 amplitude 之和（见 NoiseWave::reseed），
     * 存两份迟早会不一致，读的时候现算一遍就够。
     */
    double norm = 0.0;
    const QVector<NoiseWave::Term> &terms = wave.terms();
    for (const NoiseWave::Term &term : terms) {
        norm += term.amplitude;
        xml.writeStartElement(QStringLiteral("term"));
        xml.writeAttribute(QStringLiteral("amplitude"), num(term.amplitude));
        xml.writeAttribute(QStringLiteral("frequency"), num(term.frequency));
        xml.writeAttribute(QStringLiteral("phase"), num(term.phase));
        xml.writeEndElement();
    }
    xml.writeEndElement();
}

//! 返回是否读到了可用的波形（分量一个都没有时返回 false，调用方按 seed 重建）
bool readWaveTerm(const QXmlStreamAttributes &attrs,
                  QVector<NoiseWave::Term> *terms,
                  double *norm)
{
    bool okA = false;
    bool okF = false;
    bool okP = false;
    NoiseWave::Term term;
    term.amplitude = attrs.value(QStringLiteral("amplitude")).toDouble(&okA);
    term.frequency = attrs.value(QStringLiteral("frequency")).toDouble(&okF);
    term.phase = attrs.value(QStringLiteral("phase")).toDouble(&okP);
    if (!okA || !okF || !okP)
        return false;
    if (!isFinite(term.amplitude) || term.amplitude <= 0.0)
        return false;
    if (!isFinite(term.frequency) || term.frequency <= 0.0)
        return false;
    if (!isFinite(term.phase))
        return false;

    terms->append(term);
    *norm += term.amplitude;
    return true;
}

// ---------------------------------------------------------------- 读文件

//! 状态栏用的一句话："手写 12 个字符 / 扭曲 340 个字符"
QString summarizeRuns(const QVector<EffectRun> &runs)
{
    int handwriting = 0;
    int distortion = 0;
    int randomFont = 0;
    for (const EffectRun &run : runs) {
        switch (run.style.kind) {
        case EffectKind::Handwriting:
            handwriting += run.length();
            break;
        case EffectKind::Distortion:
            distortion += run.length();
            break;
        case EffectKind::RandomFont:
            randomFont += run.length();
            break;
        case EffectKind::None:
            break;
        }
    }

    QStringList parts;
    if (handwriting > 0)
        parts.append(QStringLiteral("手写 %1 个字符").arg(handwriting));
    if (distortion > 0)
        parts.append(QStringLiteral("扭曲 %1 个字符").arg(distortion));
    if (randomFont > 0)
        parts.append(QStringLiteral("特殊字体 %1 个字符").arg(randomFont));
    return parts.join(QStringLiteral(" / "));
}

//! 读文件时攒的临时状态
struct ReadState
{
    DocumentData *data = nullptr;
    QString html;
    QVector<EffectRun> runs;
    QVector<QPair<int, QTextBlockFormat>> blocks;  //!< 段落序号 -> 段落格式
    QVector<NoiseWave::Term> waveTerms;
    QFont defaultFont;                             //!< <font> 里读到的文档默认字体
    bool hasDefaultFont = false;
    QSizeF pageSize;                               //!< <content> 里读到的正文栏尺寸
    bool hasPageSize = false;
    double waveNorm = 0.0;
    quint32 seed = 0;
    QString savedHash;
    bool sawContent = false;
};

void handleStartElement(const QXmlStreamReader &reader, ReadState *state)
{
    const QString name = reader.name().toString();
    const QXmlStreamAttributes attrs = reader.attributes();

    if (name == QLatin1String("page")) {
        readPageSetup(attrs, &state->data->pageSetup);
        state->data->pageSetupExplicit = true;
    } else if (name == QLatin1String("margins")) {
        readMargins(attrs, &state->data->pageSetup);
        state->data->pageSetupExplicit = true;
    } else if (name == QLatin1String("effects")) {
        // 种子是 <effects> 自己的属性；两个渲染参数块是它的子元素
        if (attrs.hasAttribute(QStringLiteral("seed")))
            state->seed = attrs.value(QStringLiteral("seed")).toUInt();
    } else if (name == QLatin1String("handwriting") || name == QLatin1String("distortion")) {
        readEffectsConfig(name, attrs, &state->data->options);
    } else if (name == QLatin1String("term")) {
        readWaveTerm(attrs, &state->waveTerms, &state->waveNorm);
    } else if (name == QLatin1String("block")) {
        bool ok = false;
        const int para = attrs.value(QStringLiteral("para")).toInt(&ok);
        if (ok && para >= 0) {
            QTextBlockFormat format;
            readBlockFormat(attrs, &format);
            state->blocks.append({para, format});
        }
    } else if (name == QLatin1String("font")) {
        readDefaultFont(attrs, &state->defaultFont);
        state->hasDefaultFont = true;
    } else if (name == QLatin1String("run")) {
        EffectRun run;
        run.style.kind =
            effectKindFromName(attrs.value(QStringLiteral("kind")).toString());
        if (run.style.kind != EffectKind::None) {
            run.start = attrs.value(QStringLiteral("start")).toInt();
            run.end = attrs.value(QStringLiteral("end")).toInt();
            run.style.seed = attrs.value(QStringLiteral("seed")).toUInt();
            if (run.start < 0 || run.end <= run.start)
                run.style.kind = EffectKind::None; // 坏区间直接丢掉
            else
                state->runs.append(run);
        }
    } else if (name == QLatin1String("content")) {
        if (attrs.hasAttribute(QStringLiteral("hash")))
            state->savedHash = attrs.value(QStringLiteral("hash")).toString().toLower();
        // 正文栏尺寸：有就按它设，没有就保留调用方给文档设好的（见 finishContent）
        bool okW = false;
        bool okH = false;
        const double w = attrs.value(QStringLiteral("pageWidth")).toDouble(&okW);
        const double h = attrs.value(QStringLiteral("pageHeight")).toDouble(&okH);
        if (okW && okH && isFinite(w) && isFinite(h) && w > 0.0 && h > 0.0) {
            state->pageSize = QSizeF(w, h);
            state->hasPageSize = true;
        }
    }
}

//! 内容读回来之后：核对指纹、套默认字体/段落格式/效果区间、重建波形
void finishContent(const QTextDocument *document, ReadState *state)
{
    DocumentData *data = state->data;
    data->effectCharCount = 0;
    // 种子是从 <effects seed=...> 里读的，这里交回给调用方（主窗口要用它）
    data->seed = state->seed;

    // 默认字体得在装完正文之后设：setHtml() 会把它按 HTML 里那套重算
    if (state->hasDefaultFont)
        const_cast<QTextDocument *>(document)->setDefaultFont(state->defaultFont);

    /*!
     * 段落格式先套（它按段落序号对齐，和正文字符无关），
     * 效果区间后套 —— 后者要按字符位置，指纹对不上就整批丢掉。
     */
    if (!state->blocks.isEmpty()) {
        bool applied = false;
        applyBlockFormats(const_cast<QTextDocument *>(document), state->blocks,
                          &data->problems, &applied);
        if (!applied)
            data->problems.append(QStringLiteral("段落格式一条都没套上（段落序号对不上）"));
    }

    const QString hash = contentHash(document->toPlainText());
    if (!state->savedHash.isEmpty() && state->savedHash != hash) {
        data->contentHashMatched = false;
        data->problems.append(QStringLiteral(
            "正文和保存时对不上（内容被改过？），%1 段效果没有套回去 —— "
            "重新应用一次效果即可").arg(state->runs.size()));
        state->runs.clear();
    }

    if (!state->runs.isEmpty()) {
        int applied = 0;
        applyEffectRuns(const_cast<QTextDocument *>(document), state->runs,
                        &data->problems, &applied);
        data->effectCharCount = applied;
    }
    data->effectsSummary = summarizeRuns(state->runs);

    // 波形优先用文件里的分量（逐字节同一条），没有/坏了才退回按 seed 重建
    if (!state->waveTerms.isEmpty()
        && data->wave.setTerms(state->waveTerms, state->waveNorm)) {
        // 文件里那一条已经装好了
    } else if (state->seed != 0) {
        data->wave.reseed(state->seed);
    }
}

} // namespace

const char *const kRootElement = "tripaDocument";
const char *const kFileExtension = "tripa";

QString fileDialogFilter()
{
    return QStringLiteral("tripa 文档 (*.tripa)");
}

QString tripaSuffixWithExtension(const QString &path)
{
    const QFileInfo info(path);
    if (info.suffix().compare(QLatin1String(kFileExtension), Qt::CaseInsensitive) == 0)
        return path;
    const QString base = info.completeBaseName().isEmpty() ? info.fileName()
                                                           : info.completeBaseName();
    return info.dir().filePath(base + QLatin1Char('.') + QLatin1String(kFileExtension));
}

bool tripaLooksLikeDocument(const QString &path)
{
    const QFileInfo info(path);
    if (info.suffix().compare(QLatin1String(kFileExtension), Qt::CaseInsensitive) == 0)
        return true;

    // 扩展名不认识时看内容开头：别人把 .tripa 改名成 .xml 也照样能打开
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = file.read(256);
    file.close();
    const int start = head.indexOf("<tripaDocument");
    return start >= 0 && start < 64;
}

QString tripaDocumentToXml(const QTextDocument *document,
                           const PageSetup &pageSetup,
                           const EffectRenderOptions &options,
                           const NoiseWave *wave,
                           quint32 seed,
                           QString *error)
{
    if (error)
        error->clear();
    if (!document) {
        if (error)
            *error = QStringLiteral("没有可保存的文档");
        return QString();
    }

    const QString html = documentHtml(document);
    if (html.toUtf8().size() > kMaxHtmlBytes) {
        if (error)
            *error = QStringLiteral("文档太大（正文 HTML 超过 %1 MB）")
                         .arg(kMaxHtmlBytes / (1024 * 1024));
        return QString();
    }

    QVector<EffectRun> runs;
    int effectChars = 0;
    scanEffectRuns(document, &runs, &effectChars);

    const NoiseWave *source = wave ? wave : options.wave;

    QString out;
    QXmlStreamWriter xml(&out);
    xml.setAutoFormatting(true);
    xml.setAutoFormattingIndent(4);
    xml.writeStartDocument();
    xml.writeStartElement(QLatin1String(kRootElement));
    xml.writeAttribute(QStringLiteral("version"), QString::number(kFormatVersion));
    xml.writeAttribute(QStringLiteral("generator"), QStringLiteral("tripa"));
    xml.writeAttribute(QStringLiteral("saved"),
                       QDateTime::currentDateTime().toString(Qt::ISODate));
    xml.writeComment(QStringLiteral(
        " tripa 排版文档：正文与逐字格式（含段落）在这个文件的 content 元素里，"
        "用的是 Qt 的 HTML；手写 / 扭曲这类效果不进排版，单独记成字符区间。"
        " 详见 README 第 2.9 节。"));

    /*!
     * `<effects>` 只有一个：种子是它的属性，两个渲染参数块是它的子元素。
     * （早先这里写成了两个并列的 `<effects>`，读的时候倒是不影响，
     * 但文件看着就像打错了。）
     */
    xml.writeStartElement(QStringLiteral("effects"));
    xml.writeAttribute(QStringLiteral("seed"), QString::number(seed));
    writeEffectsConfig(xml, options);   // 子元素：handwriting / distortion
    xml.writeEndElement();

    writePageSetup(xml, pageSetup);
    writeDefaultFont(xml, document);
    writeWave(xml, source ? *source : NoiseWave());
    writeBlockFormats(xml, document);

    if (!runs.isEmpty()) {
        xml.writeStartElement(QStringLiteral("runs"));
        xml.writeAttribute(QStringLiteral("count"), QString::number(runs.size()));
        for (const EffectRun &run : std::as_const(runs)) {
            xml.writeStartElement(QStringLiteral("run"));
            xml.writeAttribute(QStringLiteral("start"), QString::number(run.start));
            xml.writeAttribute(QStringLiteral("end"), QString::number(run.end));
            xml.writeAttribute(QStringLiteral("kind"), effectKindName(run.style.kind));
            xml.writeAttribute(QStringLiteral("seed"), QString::number(run.style.seed));
            xml.writeEndElement();
        }
        xml.writeEndElement();
    }

    /*
     * 正文放最后：用 CDATA 原样嵌 HTML —— 转义成 &lt;p&gt; 的话文件就没法看了，
     * 而 Qt 的 toHtml() 本身就是自洽的 xml，CDATA 里的 & 和 < 都不成问题。
     */
    xml.writeStartElement(QStringLiteral("content"));
    xml.writeAttribute(QStringLiteral("hash"), contentHash(document->toPlainText()));
    xml.writeAttribute(QStringLiteral("chars"), QString::number(document->characterCount() - 1));
    /*!
     * 正文栏尺寸（QTextDocument::pageSize）：定断行的那个宽度。
     * 必须和正文一起存 —— 读回来装正文之后要把它**写回文档**，
     * 否则文档的栏宽是"没设过"（-1×-1），一行能排到天边，
     * 重开文件断行位置就全变了（屏幕上千真万确看得出来）。
     */
    if (document->pageSize().width() > 0.0 && document->pageSize().height() > 0.0) {
        xml.writeAttribute(QStringLiteral("pageWidth"), num(document->pageSize().width()));
        xml.writeAttribute(QStringLiteral("pageHeight"), num(document->pageSize().height()));
    }
    xml.writeCDATA(html);
    xml.writeEndElement();

    xml.writeEndElement(); // tripaDocument
    xml.writeEndDocument();

    if (xml.hasError()) {
        if (error)
            *error = QStringLiteral("生成 xml 时出错：%1").arg(xml.errorString());
        return QString();
    }
    return out;
}

bool tripaDocumentFromXml(QTextDocument *document,
                          const QString &xmlText,
                          DocumentData *data,
                          QString *error)
{
    if (error)
        error->clear();
    if (!document || !data) {
        if (error)
            *error = QStringLiteral("内部错误：没有目标文档");
        return false;
    }

    ReadState state;
    state.data = data;
    state.data->problems.clear();
    state.data->pageSetupExplicit = false;
    state.data->contentHashMatched = true;
    state.data->effectCharCount = 0;
    state.data->effectsSummary.clear();

    QXmlStreamReader xml(xmlText);
    bool rootSeen = false;
    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isStartElement()) {
            if (!rootSeen) {
                rootSeen = true;
                if (xml.name() != QLatin1String(kRootElement)) {
                    if (error)
                        *error = QStringLiteral("这不是 tripa 文档：根元素是 <%1>，应该是 <%2>")
                                     .arg(xml.name().toString(), QLatin1String(kRootElement));
                    return false;
                }
                continue;
            }
            handleStartElement(xml, &state);
        } else if (xml.isCharacters() && !xml.isWhitespace()) {
            // 只可能是 <content> 里的 CDATA（其余元素的文本都是空白）
            state.sawContent = true;
            state.html += xml.text().toString();
        } else if (xml.hasError()) {
            break;
        }
    }

    if (xml.hasError()) {
        if (error)
            *error = QStringLiteral("xml 解析失败：%1").arg(xml.errorString());
        return false;
    }
    if (!state.sawContent) {
        if (error)
            *error = QStringLiteral("文件里没有正文（<content> 缺了）");
        return false;
    }

    /*!
     * 正文栏尺寸。文件里写了就用文件里的（那是保存时真正在用的断行宽度）；
     * 没写（旧文件）就保留调用方给文档设好的。
     *
     * 判断"有没有设过"必须看宽高 > 0，**不能用 QSizeF::isValid()**：
     * 没设过的文档 pageSize 是 (-1, -1)，而 isValid() 对负值返回的仍然是 true
     * （它只排 NaN），照那个条件写回去等于把 -1 又设了一遍。
     */
    const QSizeF keepPageSize = document->pageSize();
    document->setHtml(state.html);
    if (state.hasPageSize)
        document->setPageSize(state.pageSize);
    else if (keepPageSize.width() > 0.0 && keepPageSize.height() > 0.0)
        document->setPageSize(keepPageSize);

    finishContent(document, &state);

    return true;
}

bool tripaSaveDocument(const QTextDocument *document,
                       const QString &path,
                       const PageSetup &pageSetup,
                       const EffectRenderOptions &options,
                       const NoiseWave *wave,
                       quint32 seed,
                       QString *error)
{
    if (error)
        error->clear();
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("没有给出保存路径");
        return false;
    }

    const QString text = tripaDocumentToXml(document, pageSetup, options, wave, seed, error);
    if (text.isEmpty())
        return false;   // tripaDocumentToXml 已经填了原因

    QSaveFile file(path);
    // 原子写：写一半被打断时，磁盘上要么是旧的完整文件、要么是新的
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入 %1（目录只读？）：%2").arg(path, file.errorString());
        return false;
    }
    if (file.write(text.toUtf8()) < 0) {
        if (error)
            *error = QStringLiteral("写入 %1 时出错：%2").arg(path, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("写入 %1 时出错：%2").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool tripaLoadDocument(QTextDocument *document,
                       const QString &path,
                       DocumentData *data,
                       QString *error)
{
    if (error)
        error->clear();
    if (!data) {
        if (error)
            *error = QStringLiteral("内部错误：没有接收结果的地方");
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取 %1：%2").arg(path, file.errorString());
        return false;
    }
    const QString text = QString::fromUtf8(file.readAll());
    file.close();

    QString inner;
    if (!tripaDocumentFromXml(document, text, data, &inner)) {
        if (error) {
            *error = QStringLiteral("%1：%2").arg(QFileInfo(path).fileName(),
                                                  inner.isEmpty() ? QStringLiteral("读不出来") : inner);
        }
        return false;
    }
    return true;
}

} // namespace tripadoc
