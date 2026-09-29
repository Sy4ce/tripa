#include "effect.h"

#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextFragment>

#include <QHash>
#include <algorithm>

namespace {
constexpr int kKindProperty = QTextFormat::UserProperty + 0x5710;
constexpr int kSeedProperty = QTextFormat::UserProperty + 0x5711;
} // namespace

//! 清掉格式里的效果属性，其余（字体、颜色……）原样保留
static void stripEffect(QTextCharFormat *format)
{
    format->clearProperty(kKindProperty);
    format->clearProperty(kSeedProperty);
}

EffectStyle effectStyle(const QTextCharFormat &format)
{
    EffectStyle style;
    const QVariant kind = format.property(kKindProperty);
    if (!kind.isValid())
        return style;

    style.kind = static_cast<EffectKind>(kind.toInt());
    const QVariant seed = format.property(kSeedProperty);
    style.seed = seed.isValid() ? seed.toUInt() : 0u;
    return style;
}

void setEffectStyle(QTextCharFormat *format, const EffectStyle &style)
{
    if (!format)
        return;
    if (style.kind == EffectKind::None) {
        // 注意：必须是 clearProperty（属性"不存在"），
        // 这样 mergeCharFormat 才会把原格式里的效果属性真正去掉。
        format->clearProperty(kKindProperty);
        format->clearProperty(kSeedProperty);
        return;
    }
    format->setProperty(kKindProperty, int(style.kind));
    format->setProperty(kSeedProperty, uint(style.seed));
}

bool hasEffect(const QTextCharFormat &format)
{
    return effectStyle(format).isValid();
}

EffectKind distortionEffectKind(const QTextCharFormat &format)
{
    /*!
     * 只认"这个字现在正用手写"这一件事，不看手写库里有没有数据 ——
     * 按库判断的话，给一段普通文字套扭曲会突然把字变成手写体，
     * 那是另一件事，用户没要求。
     */
    return effectStyle(format).kind == EffectKind::Handwriting ? EffectKind::Handwriting
                                                               : EffectKind::Distortion;
}

void clearEffects(QTextDocument *document)
{
    if (!document)
        return;

    QTextCursor cursor(document);
    cursor.beginEditBlock();
    for (int pos = 0; pos < document->characterCount() - 1; ++pos) {
        QTextCursor one(document);
        one.setPosition(pos);
        one.setPosition(pos + 1, QTextCursor::KeepAnchor);
        if (!hasEffect(one.charFormat()))
            continue;
        QTextCharFormat fmt = one.charFormat();
        stripEffect(&fmt);
        one.setCharFormat(fmt);
    }
    cursor.endEditBlock();
}

void clearEffects(QTextCursor *cursor)
{
    if (!cursor)
        return;

    QTextCursor work(*cursor);
    if (!work.hasSelection())
        work.select(QTextCursor::WordUnderCursor);

    work.beginEditBlock();
    // 逐字符处理：QTextCharFormat 不带某个属性时 mergeCharFormat 不会删掉原有属性，
    // 所以这里把"原格式去掉效果属性后的完整格式"整体 setCharFormat 回去
    // （效果之外的字体、颜色等全都原样保留）。
    for (int pos = work.selectionStart(); pos < work.selectionEnd(); ++pos) {
        QTextCursor one(work.document());
        one.setPosition(pos);
        one.setPosition(pos + 1, QTextCursor::KeepAnchor);
        if (!hasEffect(one.charFormat()))
            continue;
        QTextCharFormat fmt = one.charFormat();
        stripEffect(&fmt);
        one.setCharFormat(fmt);
    }
    work.endEditBlock();

    *cursor = work;
}

// ------------------------------------------------------------ 字符分类

QStringList chineseCharsIn(const QString &text, bool unique)
{
    QStringList out;
    QSet<QString> seen;
    for (const QChar &c : text) {
        // Script_Han 覆盖 CJK 统一表意文字、扩展区、兼容区
        if (c.script() != QChar::Script_Han)
            continue;
        const QString s(c);
        if (unique && seen.contains(s))
            continue;
        seen.insert(s);
        out.append(s);
    }
    return out;
}

QStringList englishCharsIn(const QString &text, bool unique)
{
    QStringList out;
    QSet<QString> seen;
    for (const QChar &c : text) {
        if (!c.isLetter())
            continue;
        if (c.script() == QChar::Script_Han)
            continue;
        // 只保留拉丁/希腊/西里尔这类"字母文字"，汉字已单独处理
        const QString s(c);
        if (unique && seen.contains(s))
            continue;
        seen.insert(s);
        out.append(s);
    }
    return out;
}

QStringList missingHandwritingChars(const QString &text,
                                    const QSet<QString> &available,
                                    bool needDetail)
{
    QVector<QString> order;
    QHash<QString, int> counts;

    for (const QChar &c : text) {
        if (c.isSpace() || c.category() == QChar::Other_Control)
            continue;
        const QString s(c);
        if (available.contains(s))
            continue;
        if (!counts.contains(s))
            order.append(s);
        counts[s] += 1;
    }

    QStringList out;
    for (const QString &s : order) {
        if (needDetail)
            out.append(QStringLiteral("%1 (出现 %2 次)").arg(s).arg(counts.value(s)));
        else
            out.append(s);
    }
    return out;
}

QString joinCharList(const QStringList &chars, int maxItems)
{
    if (chars.size() <= maxItems)
        return chars.join(QString());
    return chars.mid(0, maxItems).join(QString())
           + QStringLiteral("… 等 %1 个").arg(chars.size());
}
