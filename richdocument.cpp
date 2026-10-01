#include "richdocument.h"

#include "tripalog.h"

#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>

RichDocument::RichDocument(QObject *parent)
    : QObject(parent)
    , m_document(new QTextDocument(this))
{
    /*!
     * 文档边距必须是 0：这个文档的坐标将来会被渲染层接管成**纸面坐标**，
     * 任何"文档自带的一圈留白"都会变成"字整体偏一个边距"。
     * 页边距是纸的事，由渲染层负责（RenderMetrics）。
     */
    m_document->setDocumentMargin(0);

    connect(m_document, &QTextDocument::contentsChanged, this, &RichDocument::contentChanged);
}

// ---------------------------------------------------------------- 正文

void RichDocument::setPlainText(const QString &text)
{
    m_document->setPlainText(text);
}

QString RichDocument::plainText() const
{
    return m_document->toPlainText();
}

int RichDocument::characterCount() const
{
    return m_document->characterCount();
}

void RichDocument::clear()
{
    QTextCursor cursor(m_document);
    cursor.select(QTextCursor::Document);
    cursor.removeSelectedText();
    // 段落分隔符删不掉，所以这里一定还剩一个空段落 —— 正好是新文档该有的样子
    m_document->clearUndoRedoStacks();
    m_document->setModified(false);
}

// ---------------------------------------------------------------- 字符格式

QFont RichDocument::defaultFont() const
{
    return m_document->defaultFont();
}

void RichDocument::setDefaultFont(const QFont &font)
{
    m_document->setDefaultFont(font);
    // 字体变了 -> 每个字的宽度都变了 -> 断行必须整篇重来
    notifyGlobalFormatChanged();
}

/*!
 * \brief 某一个字符自己的格式。
 *
 * 不能用“把光标放到该位置再问 `charFormat()`”：光标停在一个位置上时，
 * 那个接口给的是**光标前面那个字符**的格式（块首还特殊），于是整体错一格 ——
 * 实测“给 3 个字符加效果”会数出 4 个。这里改成把“这一个字符”选中再问，
 * 语义就没歧义了。
 */
QTextCharFormat RichDocument::formatAt(int position) const
{
    const int lastChar = qMax(0, m_document->characterCount() - 2); // 末位是段落分隔符
    const int pos = qBound(0, position, lastChar);
    QTextCursor cursor(m_document);
    cursor.setPosition(pos);
    cursor.setPosition(qMin(pos + 1, lastChar + 1), QTextCursor::KeepAnchor);
    return cursor.charFormat();
}

void RichDocument::mergeCharFormat(QTextCursor *cursor, const QTextCharFormat &format)
{
    if (!cursor)
        return;
    cursor->mergeCharFormat(format);
}

// ---------------------------------------------------------------- 段落

ParagraphFormat RichDocument::paragraphFormat(const QTextBlock &block, double *charWidthOut) const
{
    if (charWidthOut)
        *charWidthOut = ParagraphFormat::charWidthPx(m_document);
    ParagraphFormat result;
    if (block.isValid() && block.blockFormat().isValid())
        result = ParagraphFormat::fromBlockFormat(block.blockFormat());
    return result;
}

/*!
 * \brief 应用段落格式。
 *
 * 两处一起改，缺一个都会"看着生效、其实半截"：
 *   - **每一段**的块格式（行距、缩进、分页意愿）—— `mergeBlockFormat`
 *     会自动按段应用，选区跨几段就改几段；
 *   - **整篇文档**的排版选项（中西文间距、换行规则那些是 QTextOption 上的，
 *     Qt 里没有"每段一份"）—— 见 ParagraphFormat::applyToDocument。
 */
void RichDocument::applyParagraphFormat(QTextCursor *cursor, const ParagraphFormat &format)
{
    if (!cursor)
        return;
    QTextCursor work = *cursor;
    if (!work.hasSelection())
        work.select(QTextCursor::BlockUnderCursor);
    const double charWidth = ParagraphFormat::charWidthPx(m_document);
    work.beginEditBlock();
    work.mergeBlockFormat(format.toBlockFormat(charWidth));
    work.endEditBlock();
    format.applyToDocument(m_document);

    TRIPA_DEBUG("doc",
                QStringLiteral("段落格式已应用：对齐=%1 行距规则=%2/%3 段前=%4pt 段后=%5pt "
                               "左缩进=%6 字符")
                    .arg(int(format.alignment))
                    .arg(format.lineSpacingRule)
                    .arg(format.lineSpacingValue, 0, 'f', 2)
                    .arg(format.spaceBeforePt, 0, 'f', 1)
                    .arg(format.spaceAfterPt, 0, 'f', 1)
                    .arg(format.leftIndentChars, 0, 'f', 2));

    emit metricsChanged();
    notifyGlobalFormatChanged();
}

double RichDocument::charSpacingPercent(const QTextBlock &block) const
{
    if (!block.isValid())
        return 100.0;
    const QTextCharFormat format = formatAt(block.position());
    if (!format.hasProperty(QTextFormat::FontLetterSpacing))
        return 100.0;
    return format.fontLetterSpacing();
}

void RichDocument::setCharSpacingPercent(QTextCursor *cursor, double percent)
{
    if (!cursor)
        return;
    QTextCursor work = *cursor;
    if (!work.hasSelection())
        work.select(QTextCursor::BlockUnderCursor);
    QTextCharFormat format;
    format.setFontLetterSpacing(qBound(0.0, percent, 1000.0));
    work.mergeCharFormat(format);
    /*!
     * 字距是**字**的属性，但它改了每个字的宽度 -> 断行跟着变。
     * mergeCharFormat 会通知"这一段变了"，所以不需要整篇失效。
     */
    emit metricsChanged();
}

// ---------------------------------------------------------------- 效果数据

EffectStyle RichDocument::effectAt(int position) const
{
    const QTextCharFormat format = formatAt(position);
    return effectStyle(format);
}

void RichDocument::setEffect(int position, int length, const EffectStyle &style)
{
    if (length <= 0)
        return;
    QTextCursor cursor(m_document);
    cursor.setPosition(qMax(0, position));
    cursor.setPosition(qMin(qMax(0, position) + length, m_document->characterCount() - 1),
                       QTextCursor::KeepAnchor);
    QTextCharFormat format;
    setEffectStyle(&format, style);
    cursor.mergeCharFormat(format);
}

int RichDocument::effectCharCount() const
{
    int count = 0;
    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        for (int i = 0; i < block.length() - 1; ++i) {
            if (hasEffect(formatAt(block.position() + i)))
                ++count;
        }
    }
    return count;
}

void RichDocument::clearAllEffects()
{
    clearEffects(m_document);
    TRIPA_INFO("doc", QStringLiteral("已清除整篇的效果数据（正文未改动）"));
}

// ---------------------------------------------------------------- 通知

void RichDocument::notifyGlobalFormatChanged()
{
    emit globalFormatChanged();
}
