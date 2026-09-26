#ifndef EFFECT_H
#define EFFECT_H

#include <QChar>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QTextDocument;
class QTextCursor;
class QTextCharFormat;

/*!
 * \brief 字符级效果（存进 QTextCharFormat 的自定义属性里）。
 *
 * 效果只影响"怎么画"，不改动字符本身：正文字符始终留在文档里，
 * 所以正则、选中、复制、保存都照常工作，而显示时用覆盖层换成
 * 手写笔迹 / 扭曲后的字形。
 */
enum class EffectKind {
    None = 0,
    RandomFont = 1,     //!< 随机字体（只随机改字体族，仍走正常排版）
    Handwriting = 2,    //!< 用手写 pattern 渲染
    Distortion = 3,     //!< 把噪声波叠加到字形轮廓上
};

//! 一个字符要用的随机字体 + 手写 + 扭曲的组合参数
struct EffectStyle
{
    EffectKind kind = EffectKind::None;
    quint32 seed = 0;   //!< 决定挑哪份手写样本、以及噪声波形

    bool isValid() const { return kind != EffectKind::None; }
    bool operator==(const EffectStyle &other) const
    {
        return kind == other.kind && seed == other.seed;
    }
    bool operator!=(const EffectStyle &other) const { return !(*this == other); }
};

//! 读取格式里的效果；没有则返回 kind = None
EffectStyle effectStyle(const QTextCharFormat &format);
//! 写入格式里的效果；kind = None 时清除
void setEffectStyle(QTextCharFormat *format, const EffectStyle &style);
bool hasEffect(const QTextCharFormat &format);

//! 清除整篇文档的效果
void clearEffects(QTextDocument *document);
//! 清除当前选区（无选区时清除光标处字符）的效果
void clearEffects(QTextCursor *cursor);

//! 从字符串里挑出汉字（含扩展区，不含标点）
QStringList chineseCharsIn(const QString &text, bool unique = true);
//! 从字符串里挑出英文/拉丁字母
QStringList englishCharsIn(const QString &text, bool unique = true);

/*!
 * \brief 统计一段文字里需要手写数据、但库里没有的字符。
 * \param needDetail 为 true 时返回"字符 (出现 N 次)"，否则只返回字符本身
 */
QStringList missingHandwritingChars(const QString &text,
                                    const QSet<QString> &available,
                                    bool needDetail = true);

//! 把字符列表整理成一行显示文本，过长时截断
QString joinCharList(const QStringList &chars, int maxItems = 24);

#endif // EFFECT_H
