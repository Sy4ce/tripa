#ifndef RICHDOCUMENT_H
#define RICHDOCUMENT_H

#include "effect.h"
#include "paragraph.h"

#include <QObject>
#include <QString>

class QFont;
class QTextBlock;
class QTextCharFormat;
class QTextCursor;
class QTextDocument;

/*!
 * \file richdocument.h
 * \brief 富文本层：**只存"写了什么、长什么样"**。
 *
 * 这一层里有的东西：
 *   - 正文（纯文本 + 逐字格式：字体、字号、粗斜下划线、颜色）；
 *   - 每个字符上的效果数据（手写样本的种子、扭曲的种子 —— 见 effect.h），
 *     它们存在 `QTextCharFormat` 的自定义属性里，正文里根本没有它们，
 *     所以复制、查找、存盘都不受影响；
 *   - 每个段落的**字边距和行边距**（字距、行距、段前段后、缩进、对齐，
 *     以及"段前分页 / 段中不分页 / 与下段同页 / 孤行控制"这些排版意愿）。
 *
 * 这一层里**没有**的东西：纸张、页边距、页眉页脚、页号、分页 ——
 * 一个字落在第几页、纸有多大，全是渲染层（paginatinglayout.h）的事。
 *
 * 为什么这条界线必须划清（这是重构的全部理由，不是洁癖）：
 *   旧模型把"纸高"塞进了文档自己（`document()->setPageSize(正文宽, 纸高)`），
 *   于是"内容"和"版面"互相污染 —— 同一份内容在每张纸上都从文档开头开始画
 *   （第二页重复第一页的字），超出正文区下边界的字就落在下边距里被裁掉
 *   （写着写着出下边界）。文档一旦知道自己的纸有多大，这两个 bug 就会回来。
 *
 * 所以：**这个文件里不许出现 paper / page / margin 这类词**，
 * 需要纸的地方一律去渲染层问。
 */
class RichDocument : public QObject
{
    Q_OBJECT
public:
    explicit RichDocument(QObject *parent = nullptr);

    //! 底层的 QTextDocument（编辑控件要挂在它上面；渲染层从它拿文本和格式）
    QTextDocument *text() const { return m_document; }

    // ---------------------------------------------------------------- 正文

    void setPlainText(const QString &text);
    QString plainText() const;
    int characterCount() const;
    //! 清空（留下一个空段落，光标有地方可去）
    void clear();

    // ---------------------------------------------------------------- 字符格式

    QFont defaultFont() const;
    void setDefaultFont(const QFont &font);
    QTextCharFormat formatAt(int position) const;
    void mergeCharFormat(QTextCursor *cursor, const QTextCharFormat &format);

    // ---------------------------------------------------------------- 段落：字、行边距

    /*!
     * \brief 读出某一段的段落格式（含字距 / 行距 / 缩进 / 分页意愿）。
     *
     * \a charWidthOut 非空时输出"一个字符有多宽"——界面上缩进是按**字符**
     * 填的，换算成像素要用它（见 ParagraphFormat::charWidthPx）。
     */
    ParagraphFormat paragraphFormat(const QTextBlock &block, double *charWidthOut = nullptr) const;

    //! 把段落格式应用到选区覆盖的每一段（没选区就是光标所在那一段）
    void applyParagraphFormat(QTextCursor *cursor, const ParagraphFormat &format);

    //! 字边距：字与字之间空多少（百分比，100 = 正常）
    double charSpacingPercent(const QTextBlock &block) const;
    void setCharSpacingPercent(QTextCursor *cursor, double percent);

    // ---------------------------------------------------------------- 效果数据

    //! 某个字符上挂着的手写 / 抖动数据
    EffectStyle effectAt(int position) const;
    //! 给 [position, position+length) 这一段字符写效果数据
    void setEffect(int position, int length, const EffectStyle &style);
    //! 全篇有多少字符带效果
    int effectCharCount() const;
    //! 清掉整篇的效果数据（正文一个字都不动）
    void clearAllEffects();

    // ---------------------------------------------------------------- 通知

    /*!
     * \brief 文档级格式变了（默认字体、换行规则、中西文间距……）。
     *
     * 这类变化不经过"某一段被改了"那条通知，但**断行结果会变**
     * （字宽、可断点都变了），所以渲染层必须整篇重新断行 ——
     * 这就是这个信号的唯一用途（TextEditor 连到 `PaginatingLayout::invalidateAll()`）。
     */
    void notifyGlobalFormatChanged();

signals:
    void contentChanged();
    //! 段落的字/行边距变了
    void metricsChanged();
    //! 断行缓存必须整篇失效（见 notifyGlobalFormatChanged）
    void globalFormatChanged();

private:
    QTextDocument *m_document = nullptr;
};

#endif // RICHDOCUMENT_H
