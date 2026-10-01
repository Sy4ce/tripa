#ifndef PAGINATINGLAYOUT_H
#define PAGINATINGLAYOUT_H

#include "pagesetup.h"

#include <QAbstractTextDocumentLayout>
#include <QPair>
#include <QRectF>
#include <QSet>
#include <QSizeF>
#include <QString>
#include <QTextLayout>
#include <QVector>

class QTextBlock;
class QTextBlockFormat;
class QTextCursor;
class QTextFrame;
class QTextLine;

/*!
 * \file paginatinglayout.h
 * \brief 渲染层的核心：**按需分页**的排版。
 *
 * 这一层负责"哪个字落在哪一页的哪一行"，富文本层（richdocument.h）
 * 只负责"写了什么、每个字什么格式"。两层分开是被两个真事逼出来的：
 *
 *   - **第二页重复第一页的字**：旧模型把"分页高度"塞进文档自己
 *     （`document()->setPageSize(正文宽, 纸高)`），绘制时每页都把整段文档
 *     平移一个纸高、再裁一个正文窗口 —— 而文档内容是**连续**排下来的，
 *     于是每张纸的窗口里装的都是文档开头那一段；
 *   - **写着写着出下边界**：文档内容连续排下来时，越过正文区下边界的字
 *     既不会跑到下一页，也不会停下 —— 它们落在下边距里，被裁掉了。
 *
 * 现在的模型（**文档坐标 = 纸面坐标**，这是全部几何的地基）：
 *
 *   - 第 p 页的纸面在文档坐标里是 `[p*纸高, (p+1)*纸高) × [0, 纸宽]`；
 *   - 第 p 页的正文区（"正文窗口"）是纸面内缩进页边距的那个矩形；
 *   - **正文只按行往正文窗口里填**：这一行填不下当前页的剩余高度时，
 *     它整行搬到下一页的正文区顶部（"只有当字被挤到下边界时才换页"）；
 *   - 于是文档坐标是"纸张平铺"出来的：页与页之间**隔着下边距 + 上边距**，
 *     画字的地方永远是正文窗口，纸的其余部分永远干净；
 *   - 屏幕、打印、导出共用同一份文档坐标，视图那边只差一个
 *     平移 + 缩放（见 TextEditor::documentToViewport）。
 *
 * "按需分页"是这条模型的直接结果：页只在这一行真的填不下时才新起一页，
 * 所以**没有内容就没有页** —— 后面的页不会自己冒出来。
 */

/*!
 * \brief 版面几何：纸张 + 页边距。单位是 96dpi 的文档像素，**不含缩放**。
 *
 * 缩放是视图属性（`TextEditor::setZoom`），它只改"屏幕上多大"，
 * 不影响这里的任何一个数字 —— 换个比例尺不该让断行位置变一个像素。
 */
struct RenderMetrics
{
    double paperWidthPx = 0.0;
    double paperHeightPx = 0.0;
    double marginLeftPx = 0.0;
    double marginTopPx = 0.0;
    double marginRightPx = 0.0;
    double marginBottomPx = 0.0;

    //! 从页面设置算出来（纸张方向、装订线都在里面）
    static RenderMetrics fromPageSetup(const PageSetup &setup);

    bool isValid() const { return paperWidthPx > 1.0 && paperHeightPx > 1.0; }

    //! 正文区（= 断行宽度 / 每页可用高度）
    double bodyWidthPx() const;
    double bodyHeightPx() const;

    //! 第 \a page 页的纸面矩形（文档坐标）
    QRectF paperRect(int page) const;
    //! 第 \a page 页的**正文窗口**：这一页允许出现文字的唯一区域（文档坐标）
    QRectF bodyRect(int page) const;
    //! 文档坐标的 y 落在第几页（越界时钳到合法的页号）
    int pageAtY(double docY) const;

    bool operator==(const RenderMetrics &other) const;
    bool operator!=(const RenderMetrics &other) const { return !(*this == other); }
};

/*!
 * \brief 页眉页脚。**默认两个都不显示**。
 *
 * 文字里可以用 `{page}`（当前页，从 1 起）、`{pages}`（总页数）、
 * `{title}`（文档标题）。页眉画在上边距里、页脚画在下边距里，
 * 都**不会**碰到正文窗口 —— 它们和正文是两套位置，互不干扰。
 */
struct PageChrome
{
    bool headerEnabled = false;
    QString headerText;
    bool footerEnabled = false;
    QString footerText;
    double fontSizePt = 9.0;

    static PageChrome fromPageSetup(const PageSetup &setup);
    bool anyEnabled() const { return headerEnabled || footerEnabled; }
};

/*!
 * \brief 把富文本"填"进纸张的排版引擎。
 *
 * 它挂在 `QTextDocument` 上（`QTextDocument::setDocumentLayout`），
 * 所以文档的**所有几何**都从这里出：绘制、命中测试、光标矩形、
 * 上下移动光标（Qt 自己走 `QTextBlock::layout()` 里那些行的位置）——
 * 全都只有这一份口径，不存在"字画在一处、光标在另一处"的可能。
 */
class PaginatingLayout : public QAbstractTextDocumentLayout
{
    Q_OBJECT
public:
    explicit PaginatingLayout(QTextDocument *document);

    // ---------------------------------------------------------------- 输入

    //! 纸张 / 页边距。变了就整篇重排。
    void setMetrics(const RenderMetrics &metrics);
    const RenderMetrics &metrics() const { return m_metrics; }

    //! 页眉页脚（默认都关着）
    void setChrome(const PageChrome &chrome);
    const PageChrome &chrome() const { return m_chrome; }

    //! 文档标题（页眉页脚里的 `{title}`）
    void setTitle(const QString &title);

    /*!
     * \brief 丢掉全部断行缓存，整篇重排。
     *
     * 断行只跟"块的文本 + 块格式 + 栏宽"有关，这三样里前两样由
     * `documentChanged()` 通知、第三样由 `setMetrics()` 负责。
     * 剩下的情况（换默认字体、换排版选项）不会经过这两条路，
     * 所以调用方改完文档级格式后要自己喊一声。
     */
    void invalidateAll();

    // ---------------------------------------------------------------- 查询

    //! 按需分页的结果：**有内容的最后一页 + 1**（空文档 = 1 页）
    int pageCount() const override;
    //! 有内容的最后一页（0 起；空文档 = 0）
    int lastContentPage() const { return m_lastPage; }
    /*!
     * \brief 视图的下边界（文档坐标）。
     *
     * 定义：**最后一页的下边界 + 一页高**。
     * 于是"滚到底"时可以把最后一页的下边界顶到视窗最上面
     * （只要一页比视窗高），底下那一截是桌面，不会再有内容。
     */
    double contentBottomPx() const;
    //! 正文窗口（文档坐标）：文档坐标的口径全在这里，绘制和命中都以它为准
    QRectF bodyWindow(int page) const { return m_metrics.bodyRect(page); }

    // 断行 / 分页的结果（自检、日志、诊断用）
    int blockCount() const { return m_blocks.size(); }
    int lineCount() const { return m_lines.size(); }
    int pageOfLine(int lineIndex) const;
    //! 某一行属于哪一段（块号）；越界返回 -1
    int lineBlockNumber(int lineIndex) const;
    //! 某一行是段内第几行（0 起）
    int lineNumberInBlock(int lineIndex) const;
    QRectF lineRect(int lineIndex) const;
    //! 第 \a page 页上有几行（"写满才新起一页"这句话的判据）
    int linesOnPage(int page) const;
    /*!
     * \brief 页 [firstPage, lastPage] 里出现过的段落号范围（闭区间）。
     *
     * 效果层要的是"屏幕上这几页涉及哪些段落"：它按段落缓存显示表，
     * 不知道范围就只能遍历全篇（稿子越长每帧越慢）。
     * 越界 / 空文档返回 first > last。
     */
    QPair<int, int> blockRangeOnPages(int firstPage, int lastPage) const;
    //! 一段话（自检失败时贴进日志里，比一串数字好读得多）
    QString describe() const;

    // ---------------------------------------------------------------- 绘制

    void draw(QPainter *painter, const PaintContext &context) override;
    /*!
     * 画页眉页脚（以及给自检用的纸张边框）。
     *
     * 不放在 `draw()` 里是有意的：`draw()` 画的是**正文**，
     * 页眉页脚属于"纸的装饰"，调用方（屏幕 / 打印 / 导出）在自己的
     * 纸张绘制那一步调它 —— 三个地方画的页眉页脚必须是同一份代码。
     */
    void drawChrome(QPainter *painter, int page, const QPalette &palette) const;

    // ---------------------------------------------------------------- 几何

    int hitTest(const QPointF &point, Qt::HitTestAccuracy accuracy) const override;
    //! 光标矩形（**文档坐标**）；空文档返回空矩形
    QRectF caretRect(const QTextCursor &cursor) const;
    //! 某个块在文档坐标里的范围（可能跨页：上下边界之间那道纸的空白不算内容）
    QRectF blockBoundingRect(const QTextBlock &block) const override;
    QRectF frameBoundingRect(QTextFrame *frame) const override;
    QSizeF documentSize() const override;

    void documentChanged(int from, int charsRemoved, int charsAdded) override;

signals:
    //! 重排完了（页数或行位置可能变了）；视图据此刷新滚动范围
    void layoutRebuilt();

private:
    //! 一行在文档坐标里的落点
    struct LineRecord
    {
        int blockNumber = -1;
        int lineIndex = 0;
        int page = 0;
        QRectF rect; //!< 文档坐标
    };
    //! 一个块的落点与断行缓存状态
    struct BlockRecord
    {
        int firstLine = 0;   //!< 在 m_lines 里的下标
        int lineCount = 0;
        double top = 0.0;    //!< 第一行的上边界（文档坐标）
        double bottom = 0.0; //!< 最后一行的下边界（含行距带来的推进量）
        int firstPage = 0;
        int lastPage = 0;
        int brokenLineCount = -1; //!< 断行缓存：上次断出几行（-1 = 没断过）
    };

    //! 整篇重排（断行缓存能复用就复用，见 invalidateAll 的说明）
    void relayout(const QString &reason);
    //! 断行：按当前栏宽把一段话切成若干行（只切，不定位置）
    void breakBlock(const QTextBlock &block) const;
    //! 断行 + 逐行定位（按需换页就发生在这里）；结果写进 m_lines / m_pageLines
    void positionBlock(const QTextBlock &block, int blockNumber, double *flowY, int *page,
                       BlockRecord *record) const;

    //! 一行的高度推进量（含行距规则）
    static double lineAdvance(const QTextLine &line, const QTextBlockFormat &format);
    //! 一段话在**一整页**里放得下吗（放不下就只能逐行填，不能整段搬走）
    double blockHeightPx(const QTextBlock &block) const;
    //! 对齐 / 缩进算出来的行左边界（块内坐标）
    double lineLeft(const QTextBlockFormat &format, const QTextLine &line, bool firstLine,
                    double avail) const;
    //! 这一段有没有行落在 \a clip 里（绘制时的可见性判断）
    bool blockVisible(const BlockRecord &record, const QRectF &clip) const;
    //! 选区 -> 这一段里要画的格式区间（块内下标）
    QList<QTextLayout::FormatRange> selectionRanges(const PaintContext &context,
                                                    const QTextBlock &block) const;

    RenderMetrics m_metrics;
    PageChrome m_chrome;
    QString m_title;

    /*!
     * 断行缓存。行**怎么断**只跟块的文本/格式和栏宽有关，
     * 跟它在第几页毫无关系 —— 所以改一个字不必把整篇重新断一遍，
     * 只要把脏块的断行重做、位置全部重算（位置很便宜）。
     */
    mutable QVector<BlockRecord> m_blocks;
    mutable QVector<LineRecord> m_lines;
    mutable QVector<QPair<int, int>> m_pageLines; //!< 每页的行区间（按需增长）
    mutable int m_lastPage = 0;
    //! 文本变过的块（断行要重做）
    QSet<int> m_dirtyBlocks;
    //! 整篇都要重新断行（换栏宽 / 显式 invalidateAll）
    bool m_rebreakAll = true;
    //! 防重入：重排里如果又触发了 documentChanged，排到下一轮再做
    bool m_inRelayout = false;
    bool m_relayoutPending = false;
};

/*!
 * \brief 把文档上的分页排版取出来；没有就装一个。
 *
 * 装的时候**不碰文档内容**，只接管几何。`QTextDocument` 自己会拥有这个布局
 * （`setDocumentLayout` 会接管所有权），所以不要在别处 delete 它。
 */
PaginatingLayout *paginatingLayoutOf(const QTextDocument *document);
//! 同上，但文档上已经有别的布局时也换掉（用于"文档是外面造好的"那条路）
PaginatingLayout *installPaginatingLayout(QTextDocument *document);

#endif // PAGINATINGLAYOUT_H
