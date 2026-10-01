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

// ---------------------------------------------------------------- 正文（纯文本 + 属性）
/*
 * 「不要直接把 html 存进去」这件事的做法：
 *
 * 正文写成**层次化的 xml**，每个字能带什么格式就用属性写出来，
 * 文本本身原样放在 CDATA 里，一个字都不转义、不加工：
 *
 *   <content hash=... chars=... pageWidth=... pageHeight=...>
 *     <p para="0"><r>普通文字在这里。</r>
 *       <r family="SimSun" size="28">宋体二十八</r>
 *       <r weight="700" italic="1" underline="1" color="#ffc81e28">粗斜下划线红</r></p>
 *     <p para="1">…</p>
 *   </content>
 *
 * 为什么不再嵌 HTML：Qt 的 `toHtml()` 塞进来的是一整份 `<html><head><style>…`
 * 文档 —— 一个"排版工具自己的文档格式"里躺着一坨 HTML，既不好读也不好改，
 * 而且 Qt 导 HTML 时会把长度值**四舍五入到整像素**、把字号写成 css 的 pt，
 * 往返一趟格式就会悄悄走样（所以以前还得另写一份 `<blocks>` 来兜段落格式）。
 * 自己写就没有这回事：属性写多少就是多少。
 *
 * 兼容：老文件里 `<content>` 里是一段 HTML 的 CDATA，读的时候照样认
 * （见 finishContent 里的 state.html 分支），只是**不再**往里写。
 */

//! 单段文本的字节上限（防止误存一个巨大的文件）
constexpr int kMaxContentBytes = 64 * 1024 * 1024;

/*!
 * \brief 一段文本 + 它的字符格式。
 *
 * 粒度按"格式变了就切一刀"来定，和 `QTextFragment` 一一对应
 * （Qt 自己也把相邻同格式的字并成一个 fragment），所以合并之后
 * 一个 `<r>` 往往就是一大段文字，不会一个字一个元素。
 */
struct ContentRun
{
    QString text;
    QTextCharFormat format;
};

struct ContentPara
{
    int number = 0;
    QString text;
    QVector<ContentRun> runs;
};

/*!
 * \brief 一个字符格式里"看得见的那些属性"（排掉 Qt 的内部记账）。
 *
 * `QTextCharFormat::operator==` 比的是**整个属性表**，其中包含
 * `QTextFormat::ObjectIndex` —— 那是 Qt 给"注册过的格式对象"编的内部号，
 * 和显示效果毫无关系，可是同一个格式插进不同的 fragment 时它可能不一样。
 * 直接拿 operator== 去合并相邻文本，会出现"一个字一个 `<r>`"的惨状
 * （实测：一段普通文字被拆成 6 个元素，而另一段同样格式的却并成了一个）。
 * 所以按"属性表去掉 ObjectIndex / ObjectType"来比。
 */
QVector<QPair<int, QVariant>> visibleProperties(const QTextCharFormat &format)
{
    QVector<QPair<int, QVariant>> out;
    const QMap<int, QVariant> all = format.properties();
    for (auto it = all.constBegin(); it != all.constEnd(); ++it) {
        if (it.key() == QTextFormat::ObjectIndex || it.key() == QTextFormat::ObjectType)
            continue;
        out.append({it.key(), it.value()});
    }
    return out;
}

//! 只留"看得见的属性"的那一份格式（写文件用，免得把 Qt 的内部号也写出去）
QTextCharFormat visibleFormat(const QTextCharFormat &format)
{
    QTextCharFormat out;
    const QVector<QPair<int, QVariant>> props = visibleProperties(format);
    for (const auto &entry : props)
        out.setProperty(entry.first, entry.second);
    return out;
}

/*!
 * 把文档拆成"段落 -> 若干段同格式文本"。
 *
 * **必须逐 fragment 走，而且要按 fragment 的"有效字符格式"取**：
 * 一个字符没显式设过字体时，`charFormat()` 是空的，而它实际是按文档默认字体
 * （或者整段统一设过的那套格式）排的。只认 charFormat() 的话，
 * 重开文件就会把默认字体丢掉。
 */
void scanContent(const QTextDocument *document, QVector<ContentPara> *out)
{
    out->clear();
    if (!document)
        return;

    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        ContentPara para;
        para.number = block.blockNumber();
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.text().isEmpty())
                continue;
            const QTextCharFormat format = visibleFormat(fragment.charFormat());
            para.text += fragment.text();
            /*!
             * 相邻的、格式一样的两段要并起来 ——
             * 否则一次全选改字号会在文件里留下成百上千个 `<r>`。
             */
            if (!para.runs.isEmpty()
                && visibleProperties(para.runs.last().format) == visibleProperties(format)) {
                para.runs.last().text += fragment.text();
            } else {
                ContentRun run;
                run.text = fragment.text();
                run.format = format;
                para.runs.append(run);
            }
        }
        out->append(para);
    }
}

//! 格式里那三样"单独存、别塞进 HTML"的：字色、下划线类型、竖排偏移
void writeRunAttributes(QXmlStreamWriter &xml, const QTextCharFormat &format,
                        const QFont &documentFont)
{
    QStringList families = format.fontFamilies().toStringList();
    /*!
     * **没显式设过字体的字，也要把它实际用的字体写出来。**
     *
     * 一个字符的 `charFormat()` 里可能根本没有字体属性，它显示成什么样
     * 取决于文档的默认字体（`<font>` 那一块）。这种字如果属性一个都不写，
     * 读回来就成了一块"没有格式"的文字 —— 渲染时跟着**读入方**的默认字体走，
     * 换个环境打开就变样，往返比对（自检里那个逐字符签名）也对不上。
     * 所以这里把"文档默认字体"当成实际字体补上，让文件**自足**。
     */
    if (families.isEmpty() && !documentFont.family().isEmpty() && format.fontPointSize() <= 0.0
        && !format.hasProperty(QTextFormat::FontPointSize))
        families = QStringList{documentFont.family()};
    if (!families.isEmpty())
        xml.writeAttribute(QStringLiteral("family"), families.join(QLatin1Char(',')));

    const bool hasPointSize = format.hasProperty(QTextFormat::FontPointSize);
    const bool hasPixelSize = format.hasProperty(QTextFormat::FontPixelSize);
    if (hasPointSize)
        xml.writeAttribute(QStringLiteral("size"), num(format.fontPointSize()));
    else if (hasPixelSize)
        xml.writeAttribute(QStringLiteral("pixelSize"),
                           QString::number(format.intProperty(QTextFormat::FontPixelSize)));
    else if (documentFont.pointSizeF() > 0.0)
        xml.writeAttribute(QStringLiteral("size"), num(documentFont.pointSizeF()));
    else if (documentFont.pixelSize() > 0)
        xml.writeAttribute(QStringLiteral("pixelSize"),
                           QString::number(documentFont.pixelSize()));

    if (format.hasProperty(QTextFormat::FontWeight))
        xml.writeAttribute(QStringLiteral("weight"), QString::number(format.fontWeight()));
    if (format.hasProperty(QTextFormat::FontItalic) && format.fontItalic())
        xml.writeAttribute(QStringLiteral("italic"), QStringLiteral("1"));
    if (format.hasProperty(QTextFormat::FontStrikeOut) && format.fontStrikeOut())
        xml.writeAttribute(QStringLiteral("strikeOut"), QStringLiteral("1"));

    if (format.hasProperty(QTextFormat::TextUnderlineStyle)) {
        const char *name = "single";
        switch (format.underlineStyle()) {
        case QTextCharFormat::NoUnderline:
            name = "none";
            break;
        case QTextCharFormat::DashUnderline:
            name = "dash";
            break;
        case QTextCharFormat::DotLine:
            name = "dot";
            break;
        case QTextCharFormat::DashDotLine:
            name = "dashDot";
            break;
        case QTextCharFormat::DashDotDotLine:
            name = "dashDotDot";
            break;
        case QTextCharFormat::WaveUnderline:
            name = "wave";
            break;
        case QTextCharFormat::SpellCheckUnderline:
            name = "spellCheck";
            break;
        case QTextCharFormat::SingleUnderline:
            break;
        }
        xml.writeAttribute(QStringLiteral("underline"), QLatin1String(name));
    }
    if (format.hasProperty(QTextFormat::TextVerticalAlignment)
        && format.verticalAlignment() != QTextCharFormat::AlignNormal) {
        const char *name = "super";
        switch (format.verticalAlignment()) {
        case QTextCharFormat::AlignSubScript:
            name = "sub";
            break;
        case QTextCharFormat::AlignSuperScript:
            break;
        default:
            name = "normal";
            break;
        }
        xml.writeAttribute(QStringLiteral("valign"), QLatin1String(name));
    }

    /*!
     * 颜色一律写成 `#aarrggbb` **八位**（HexArgb）：
     * 带不带 alpha 都能一字不差地读回来，而 `#rrggbb` 会把半透明丢掉。
     */
    if (format.hasProperty(QTextFormat::ForegroundBrush))
        xml.writeAttribute(QStringLiteral("color"),
                           format.foreground().color().name(QColor::HexArgb));
    if (format.hasProperty(QTextFormat::BackgroundBrush))
        xml.writeAttribute(QStringLiteral("bg"),
                           format.background().color().name(QColor::HexArgb));
}

void writeContent(QXmlStreamWriter &xml, const QTextDocument *document)
{
    QVector<ContentPara> paras;
    scanContent(document, &paras);
    const QFont documentFont = document ? document->defaultFont() : QFont();

    for (const ContentPara &para : std::as_const(paras)) {
        xml.writeStartElement(QStringLiteral("p"));
        xml.writeAttribute(QStringLiteral("para"), QString::number(para.number));
        if (para.runs.isEmpty()) {
            // 空段落：没有 run，元素自己就是那个空行
            xml.writeEndElement();
            continue;
        }
        for (const ContentRun &run : std::as_const(para.runs)) {
            xml.writeStartElement(QStringLiteral("r"));
            writeRunAttributes(xml, run.format, documentFont);
            xml.writeCDATA(run.text);
            xml.writeEndElement();
        }
        xml.writeEndElement();
    }
}

//! `<r>` 上的属性 -> 字符格式（写多少设多少，没写的保持默认）
void readRunAttributes(const QXmlStreamAttributes &attrs, QTextCharFormat *format)
{
    if (!format)
        return;
    if (attrs.hasAttribute(QStringLiteral("family"))) {
        const QString family = attrs.value(QStringLiteral("family")).toString();
        const QStringList families = family.split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (families.size() > 1)
            format->setFontFamilies(families);
        else if (!family.isEmpty())
            format->setFontFamilies({family});
    }
    if (attrs.hasAttribute(QStringLiteral("size"))) {
        const double size = clampedAttr(attrs, QStringLiteral("size"), 0.0, 0.0, 1000.0);
        if (size > 0.0)
            format->setFontPointSize(size);
    } else if (attrs.hasAttribute(QStringLiteral("pixelSize"))) {
        bool ok = false;
        const int px = attrs.value(QStringLiteral("pixelSize")).toInt(&ok);
        if (ok && px > 0 && px < 2000)
            format->setProperty(QTextFormat::FontPixelSize, px);
    }
    if (attrs.hasAttribute(QStringLiteral("weight"))) {
        bool ok = false;
        const int weight = attrs.value(QStringLiteral("weight")).toInt(&ok);
        if (ok)
            format->setFontWeight(qBound(1, weight, 1000));
    }
    if (attrs.hasAttribute(QStringLiteral("italic")))
        format->setFontItalic(attrs.value(QStringLiteral("italic")) != QLatin1String("0"));
    if (attrs.hasAttribute(QStringLiteral("strikeOut")))
        format->setFontStrikeOut(attrs.value(QStringLiteral("strikeOut")) != QLatin1String("0"));
    if (attrs.hasAttribute(QStringLiteral("underline"))) {
        const QString style = attrs.value(QStringLiteral("underline")).toString();
        QTextCharFormat::UnderlineStyle value = QTextCharFormat::SingleUnderline;
        if (style == QLatin1String("none"))
            value = QTextCharFormat::NoUnderline;
        else if (style == QLatin1String("dash"))
            value = QTextCharFormat::DashUnderline;
        else if (style == QLatin1String("dot"))
            value = QTextCharFormat::DotLine;
        else if (style == QLatin1String("dashDot"))
            value = QTextCharFormat::DashDotLine;
        else if (style == QLatin1String("dashDotDot"))
            value = QTextCharFormat::DashDotDotLine;
        else if (style == QLatin1String("wave"))
            value = QTextCharFormat::WaveUnderline;
        else if (style == QLatin1String("spellCheck"))
            value = QTextCharFormat::SpellCheckUnderline;
        format->setUnderlineStyle(value);
    }
    if (attrs.hasAttribute(QStringLiteral("valign"))) {
        const QString value = attrs.value(QStringLiteral("valign")).toString();
        if (value == QLatin1String("sub"))
            format->setVerticalAlignment(QTextCharFormat::AlignSubScript);
        else if (value == QLatin1String("super"))
            format->setVerticalAlignment(QTextCharFormat::AlignSuperScript);
        else
            format->setVerticalAlignment(QTextCharFormat::AlignNormal);
    }
    auto readColor = [&](const QString &name, bool foreground) {
        if (!attrs.hasAttribute(name))
            return;
        QColor color(attrs.value(name).toString());
        if (!color.isValid())
            return;
        if (foreground)
            format->setForeground(color);
        else
            format->setBackground(color);
    };
    readColor(QStringLiteral("color"), true);
    readColor(QStringLiteral("bg"), false);
}

/*!
 * 把新格式的正文装进文档。
 *
 * 用 `QTextCursor::insertText(text, format)` 而不是拼一份 HTML 再 `setHtml()`：
 * 自己插的字，字符格式**一个属性都不多不少**，也不经过 HTML 解析器
 * 那套"四舍五入到整像素 / 字号写 pt"的换算。
 *
 * 段落一律用 `insertBlock()` 分开（哪怕前后两段是空的），
 * 段落序号（`<blocks>` 里的 `para`）才和文档里的 block 一一对应。
 */
void buildDocumentFromParagraphs(QTextDocument *document, const QVector<ContentPara> &paragraphs)
{
    if (!document)
        return;

    QTextCursor cursor(document);
    cursor.beginEditBlock();
    /*!
     * 先把原来的内容整段删掉。`setPlainText` 会**带着当前字符格式**插入
     * （Qt 有意这么设计），所以这里宁可走"清空 + 逐段插"，
     * 也不要先设一批文字再补格式 —— 那会在中途触发重排、还可能把
     * 当前格式漏进正文里。
     */
    cursor.select(QTextCursor::Document);
    cursor.removeSelectedText();

    for (int index = 0; index < paragraphs.size(); ++index) {
        const ContentPara &para = paragraphs.at(index);
        if (index > 0)
            cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
        for (const ContentRun &run : para.runs)
            cursor.insertText(run.text, run.format);
    }
    cursor.endEditBlock();
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
    /*!
     * 页眉页脚：**默认不写**（不写 = 不显示）。
     * 只写开关和文字，不写“第 N 页”这类渲染结果 ——
     * 页数会随内容变，写进文件就错了。
     */
    if (setup.headerEnabled)
        xml.writeAttribute(QStringLiteral("header"), setup.headerText);
    if (setup.footerEnabled)
        xml.writeAttribute(QStringLiteral("footer"), setup.footerText);
    if (setup.chromeFontSizePt > 0.1)
        xml.writeAttribute(QStringLiteral("chromePt"), num(setup.chromeFontSizePt));
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
    /*! 页眉页脚：老文件里根本没有这两个属性，读回来就是“不显示”（默认关） */
    if (attrs.hasAttribute(QStringLiteral("header"))) {
        setup->headerEnabled = true;
        setup->headerText = attrs.value(QStringLiteral("header")).toString();
    }
    if (attrs.hasAttribute(QStringLiteral("footer"))) {
        setup->footerEnabled = true;
        setup->footerText = attrs.value(QStringLiteral("footer")).toString();
    }
    setup->chromeFontSizePt =
        clampedAttr(attrs, QStringLiteral("chromePt"), setup->chromeFontSizePt, 4.0, 72.0);
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
    /*!
     * 老文件（kFormatVersion 1）里 `<content>` 装的是**一段 HTML 的 CDATA**，
     * 新文件里是 `<p>` / `<r>` 的结构。两者都读得进来：
     * `html` 只给老文件用，`paragraphs` 只给新文件用。
     */
    QString html;
    QVector<ContentPara> paragraphs;  //!< 新格式的正文
    bool hasParagraphs = false;       //!< 见到过 <p>：这份文件是新格式

    //! 正在攒的那一段（<p> 到 </p> 之间）
    ContentPara currentPara;
    bool inPara = false;
    //! 正在攒的那一段文本（<r> 到 </r> 之间）
    ContentRun currentRun;
    bool inRun = false;

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
    } else if (name == QLatin1String("p")) {
        /*!
         * 新格式的段落。`para` 属性只是**给人看的**（和 `<blocks>` 里的序号对得上），
         * 装文档时按出现顺序来 —— 文件被手改得序号乱了也不会把段落插错位置。
         *
         * 见到 `<p>` 就算"有正文"：空文档就是**一个空段落**，
         * 里面一个 `<r>` 都没有，不能因此把它当成"文件里没有正文"。
         */
        state->inPara = true;
        state->hasParagraphs = true;
        state->sawContent = true;
        state->currentPara = ContentPara();
        bool ok = false;
        const int para = attrs.value(QStringLiteral("para")).toInt(&ok);
        state->currentPara.number = ok ? para : state->paragraphs.size();
    } else if (name == QLatin1String("r")) {
        state->inRun = true;
        state->currentRun = ContentRun();
        readRunAttributes(attrs, &state->currentRun.format);
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

    const QString plain = document->toPlainText();
    if (plain.toUtf8().size() > kMaxContentBytes) {
        if (error)
            *error = QStringLiteral("文档太大（正文超过 %1 MB）")
                         .arg(kMaxContentBytes / (1024 * 1024));
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
        " tripa 排版文档：正文是纯文本，格式（字体/字号/粗斜/下划线/颜色）写在 "
        "content 里的 r 元素属性上，段落格式在 blocks 里；手写 / 扭曲这类效果"
        "不进排版，单独记成字符区间。详见 README 第 2.9 节。"));

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
     * 正文放最后：每段一个 `<p>`，段内按"格式变了就切一刀"分 `<r>`，
     * 文本原样放在 CDATA 里（转义成 &lt; 的话文件就没法看了）。
     */
    xml.writeStartElement(QStringLiteral("content"));
    xml.writeAttribute(QStringLiteral("hash"), contentHash(plain));
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
    writeContent(xml, document);
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
        } else if (xml.isEndElement()) {
            const QString name = xml.name().toString();
            if (name == QLatin1String("r") && state.inRun) {
                state.inRun = false;
                state.currentPara.text += state.currentRun.text;
                if (!state.currentRun.text.isEmpty())
                    state.currentPara.runs.append(state.currentRun);
                state.currentRun = ContentRun();
            } else if (name == QLatin1String("p") && state.inPara) {
                state.inPara = false;
                state.paragraphs.append(state.currentPara);
                state.currentPara = ContentPara();
            }
        } else if (xml.isCharacters()) {
            const QString text = xml.text().toString();
            /*!
             * 文本只从 `<r>` 里面收：`<p>` 之间那些缩进换行是
             * `setAutoFormatting(true)` 自己写的，收进来会凭空多出空行。
             *
             * 老文件的正文是一段 HTML 的 CDATA，那时候不在 `<r>` 里，
             * 走下面那个分支（按"非空白"判断，和原来一样）。
             */
            if (state.inRun) {
                state.currentRun.text += text;
                state.sawContent = true;
            } else if (!xml.isWhitespace()) {
                state.sawContent = true;
                state.html += text;
            }
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
    if (state.hasParagraphs)
        buildDocumentFromParagraphs(document, state.paragraphs);
    else
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
