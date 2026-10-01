#ifndef TEXTEDITOR_H
#define TEXTEDITOR_H

#include "effectsrenderer.h"
#include "effectplanner.h"
#include "pagesetup.h"
#include "paginatinglayout.h"

#include <QElapsedTimer>
#include <QTextEdit>
#include <QTransform>

class QPainter;
class QTimer;

class RichDocument;

/*!
 * \brief 所见即所得的排版编辑区（**视图层**）。
 *
 * 三层各管一件事，界线要清楚：
 *
 *   - **富文本层**（richdocument.h）：正文 + 格式 + 手写/抖动数据 + 段落的字行边距。
 *     它不知道纸有多大，也不知道"页"是什么；
 *   - **渲染层**（paginatinglayout.h）：按需分页。文字只往正文窗口里填，
 *     填不下就整行换页；没写到的地方就没有页；
 *   - **视图层**（本文件）：缩放、滚动、绘制（纸 + 正文 + 效果 + 光标）、
 *     鼠标/键盘。**它不改版面数字** —— 比例尺只是"屏幕上多大"。
 *
 * 坐标口径（三层之间唯一的接口，记错一个就是"字画在一处、点哪儿都不对"）：
 *
 *   - **文档坐标 = 纸面坐标**：第 p 页的纸是
 *     `[p*纸高, (p+1)*纸高) × [0, 纸宽]`，正文窗口由渲染层算；
 *   - **viewport 坐标** = 文档坐标 × 缩放 + 纸面原点（纸面原点里含滚动量）；
 *   - 两者之间只有一个变换：`documentToViewport()`。
 *     正文、光标、选区、效果层、鼠标命中共用它的正/逆变换。
 *
 * 手写 / 扭曲效果**和正文在同一次 paintEvent 里画**（不是独立的透明子控件）：
 * 一次绘制里只有一套坐标，屏幕、抓图、打印、导出走的是同一段代码。
 */
class TextEditor : public QTextEdit
{
    Q_OBJECT
public:
    explicit TextEditor(QWidget *parent = nullptr);

    /*!
     * \brief 视图缩放：1.0 = 100%（初始值就是 100%）。
     *
     * 只改"屏幕上显示多大"，别的什么都不改：排版、分页、打印、导出 PDF
     * 和 .tripa 里存的任何数字都不跟着变 —— 在排版软件里比例尺是**视图属性**，
     * 不是文档属性（换个比例尺不该让文件内容变一个字节）。
     * 实现上是给绘制和命中测试共用那一个变换乘一个比例，
     * 分页仍然是 100% 那一套，所以"屏幕上看的样子"和"纸上印的样子"
     * 只差一个放大倍数。
     */
    static constexpr double kZoomMin = 0.25;
    static constexpr double kZoomMax = 4.0;
    static constexpr double kZoomStep = 0.10; //!< 一次 ±10%

    //! 设置缩放比例，并以**视口中心**为锚点（中心那一点对应的文档位置不动）
    void setZoom(double zoom);
    //! 设置缩放比例，并把 \a viewportAnchor 那一点钉住（Ctrl+滚轮用鼠标位置）
    void setZoomAt(double zoom, const QPointF &viewportAnchor);
    //! 在 \a viewportAnchor 处缩放 \a delta（+0.1 = 放大 10%）
    void zoomBy(double delta, const QPointF &viewportAnchor);
    double zoom() const { return m_zoom; }

    // ---------------------------------------------------------------- 两层

    //! 富文本层：正文 + 格式 + 效果数据 + 段落字行边距（**没有**任何纸张概念）
    RichDocument *richDocument() const { return m_rich; }
    //! 渲染层：分页排版（纸张几何、每行落在哪一页、命中测试）
    PaginatingLayout *layout() const { return m_layout; }
    /*!
     * 效果层的**计算层**：显示表缓存 + 待算队列（见 effectplanner.h）。
     *
     * 视图只读它、只排队，**不算几何** —— "渲染"与"计算"的分界线就在这里。
     * 主窗口拿它去开后台任务（`EffectWorkJob`）。
     */
    EffectPlanner *effectPlanner() const { return m_planner; }

    //! 当前一共几页（**按需分页**的结果：没写到的页根本不存在）
    int pageCount() const;
    //! 视图下边界（文档坐标）= 最后一页下边界 + 一页高：滚到底能把最后一页顶到视窗最上面
    double contentBottomPx() const;

    //! 段落里的"字边距"（字距，百分比）—— 走富文本层，这里只是顺手转一下
    void setParagraphCharSpacing(double percent);

    // ---------------------------------------------------------------- 版面

    void setPageSetup(const PageSetup &setup);
    const PageSetup &pageSetup() const { return m_pageSetup; }
    //! 页眉页脚：默认都不显示；页面设置里开了之后由 setPageSetup() 一起带进来
    void setPageChrome(const PageChrome &chrome);

    //! 正文区在第一页里的左上角偏移（像素，96dpi）
    QPointF bodyOriginPx() const;
    //! 一张纸在文档坐标里的矩形（文档坐标原点就是第一页纸的左上角）
    QRectF pageRectInDocument() const;

    //! 100% 下的纸张尺寸（文档 / 打印口径）；屏幕上的尺寸要乘缩放，见下面两个
    double paperWidthPx() const;
    double paperHeightPx() const;
    //! 屏幕上那张纸（含缩放）的尺寸
    double paperViewWidthPx() const { return paperWidthPx() * m_zoom; }
    double paperViewHeightPx() const { return paperHeightPx() * m_zoom; }
    /*!
     * 纸张居中留白量（含缩放）。
     * 它通过 setViewportMargins 做成 viewport 的内容边距，
     * 所以 viewport 原点的 x **就是**纸张左边缘 ——
     * paperOriginInViewport() 里不再含它，画纸时不要重复加。
     */
    double paperPadPx() const;
    //! 纸张左上角在 viewport 坐标里的位置（含滚动量，不含居中留白）
    QPointF paperOriginInViewport() const;
    //! 给竖滚动条预留的宽度（居中量里减掉的就是它，诊断用）
    int scrollBarReservePx() const;

    //! 文档坐标 -> viewport 坐标（**唯一**的坐标变换：平移 + 缩放）
    QTransform documentToViewport() const;
    //! 第一页正文区左上角在 viewport 里的位置（诊断 / 自检用）
    QPointF documentOriginInViewport() const;
    //! 第 page 页正文区左上角在**文档坐标**里的位置
    QPointF pageTopLeft(int page) const;

    //! viewport 局部坐标（= QMouseEvent::pos()）-> 文档坐标下的光标
    QTextCursor documentCursorAt(const QPoint &viewportPos) const;
    //! viewport 局部坐标 -> 文档坐标
    QPointF viewportToDocument(const QPoint &viewportPos) const;
    //! viewport 局部坐标 -> 排版坐标（= 文档坐标 + 正文区原点）
    QPoint viewportToEditor(const QPoint &viewportPos) const;
    //! viewport 左上角在编辑区坐标里的位置（自检用来换算事件坐标）
    QPoint viewportOriginInEditor() const;

    void setEffectOptions(const EffectRenderOptions &options);
    const EffectRenderOptions &effectOptions() const { return m_options; }
    void setEffectsVisible(bool visible);
    bool effectsVisible() const { return m_effectsVisible; }

    //! 按当前字体重新排版（字号等变化后调用）
    void relayout();
    //! 重新计算滚动范围与纸张居中（纸张 / 窗口尺寸变化后调用）
    void updatePageMargins();

public slots:
    void updateOverlayGeometry();
    /*!
     * 重算滚动范围与纸张居中量。
     *
     * \a source 只用来**诊断**（日志里会带上"是谁调进来的"）：
     * 这个函数一旦被高频调用就是活锁，而"谁在调"是唯一能定位它的线索。
     */
    void updateScrollRange(const QString &source = QStringLiteral("直接调用"));

    /*!
     * \brief 把"版面要重算"这件事排到事件循环的下一轮（同轮内多次请求只算一次）。
     *
     * 为什么必须延迟：`updateScrollRange()` 会改 viewport 边距，而 viewport
     * 尺寸一变就会重排、重排又会发 `documentSizeChanged` ——
     * 直接把那个信号连到 `updateScrollRange()` 就是一个自激环，
     * **实测一秒里调用 3704 次、界面彻底卡死**（自检 soak 组用日志抓出来的）。
     * 延迟一轮 + 合并重复请求之后，每轮最多算一次，环就断了。
     *
     * 需要"立刻生效"的地方（鼠标命中、缩放锚点、自检）仍然直接调
     * `updateScrollRange()` —— 那条路是同步的、不带环。
     */
    void scheduleLayoutUpdate();
    /*!
     * 把光标滚进可视区，用**视图像素**算。
     *
     * Qt 自己的自动滚动（QWidgetTextControl 里那套）把"文档像素"和
     * "视口像素"当成一回事，缩放之后这两者差一个倍数 —— 于是
     * 放大到 200% 时光标跑到屏幕外面，Qt 还认为它是可见的，一个字都不滚。
     * 键盘事件走完之后补一次这里，光标就一定看得见。
     *
     * 公开是为了让自检能复现"打字的人眼睛盯着光标"这件事
     * （见 --uitest 的 pagefix：边打边量，视口跟着光标走）。
     */
    void ensureCaretVisible();
    /*!
     * \brief 任务正在改文档时把重画压住。
     *
     * 为什么必须有这个开关（实测出来的，见 --uitest progress）：
     * 给一千多个字套效果时，每一片都会让文档变一次 —— 而视图只要重画一次，
     * 就会重新规划看得见的那几段、并要求重算它们的字形几何（几百个字）。
     * 结果是一个正反馈：**改一点 -> 重画 -> 重算几百个字的几何 -> 再改一点**。
     * 实测这项操作本来只要 0.4 秒，开着重画跑了 **30 秒**。
     *
     * 压住之后：改的这段时间屏幕上保持原样（状态栏的进度条一直在走），
     * 改完一次性重画 —— 用户看到的就是"一眨眼，效果好了"。
     */
    void setRepaintsDeferred(bool deferred);
    bool repaintsDeferred() const { return m_repaintsDeferred; }

    /*!
     * 鼠标处理放在 public 是有意的：自检（--uitest）需要直接驱动这三个入口
     * 才能验证"点得准、拖得对"。
     *
     * 为什么不靠合成事件：拖选要求 Qt 内部的按键状态跟着变，
     * QTest::mouseMove 又依赖窗口真的被激活（无头环境下没有），
     * 手搓 QMouseEvent + sendEvent 送进去会被当成悬停丢掉。
     * 事件怎么送进控件是框架的事，"收到事件之后算得对不对"才是这里要测的。
     */
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    //! 键盘走完 Qt 那一套之后，再按**视图像素**把光标滚进可视区（缩放后 Qt 算的是文档像素）
    void keyPressEvent(QKeyEvent *event) override;
    //! Ctrl+滚轮 = 缩放；普通滚轮按"视图像素"滚（缩放后一档仍是同样的行数）
    void wheelEvent(QWheelEvent *event) override;

signals:
    void effectsVisibilityChanged(bool visible);
    //! 缩放变了（1.0 = 100%）；状态栏的缩放控件据此同步
    void zoomChanged(double zoom);
    //! 页数变了（状态栏"第 x/y 页"用它）
    void pageCountChanged(int pages);
    /*!
     * 有字形几何等着算（滚动到新的一页、刚套完效果）。
     *
     * 视图**不自己算**：它把这件事交给主窗口的 `JobRunner`，
     * 由后者分片做完并把进度显示在状态栏上。
     */
    void effectWorkNeeded();
protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void focusInEvent(QFocusEvent *event) override;

private:
    //! 把页面设置换算成渲染层的版面几何（纸张 / 页边距 / 页眉页脚）
    void applyLayoutMetrics();
    //! 自己画光标竖条（Qt 的光标可见性状态在私有类里，拿不到）
    void drawCaret(QPainter *painter);
    //! 光标在**文档坐标**里的矩形（几何来自渲染层的行落点）
    QRectF caretRectInDocument() const;
    //! 缩放的真正实现：改比例、重算滚动范围，再把锚点那一格摆回原处
    void applyZoom(double zoom, const QPointF &viewportAnchor);
    //! 光标闪烁：相位由自己维护（见构造函数里的说明）
    void restartCaretBlink();

    RichDocument *m_rich = nullptr;
    PaginatingLayout *m_layout = nullptr;
    EffectPlanner *m_planner = nullptr;

    PageSetup m_pageSetup;
    EffectRenderOptions m_options;
    bool m_effectsVisible = true;
    //! 视图缩放，1.0 = 100%
    double m_zoom = 1.0;
    //! 光标当前这一相是不是"亮"（闪烁由 QTimer 驱动，见 restartCaretBlink）
    bool m_caretVisible = true;
    QTimer *m_caretTimer = nullptr;
    //! 是否正在按住左键拖选
    bool m_dragSelecting = false;
    /*!
     * 拖选的锚点 = **按下时**那个字符的位置。
     *
     * 必须在按下时记下来，不能在 mouseMoveEvent 里读 textCursor().anchor()：
     * 每扩一次选区，锚点就被推到上一次的终点上。
     *   - 往右拖时锚点被顶到右边 → 选区塌成一个点，看起来"选不动"；
     *   - 往左拖时锚点被顶到左边 → 选区变成"行首到起点"，
     *     这就是"起点和终点永远在一行的开始或末尾、不能选到中间就停"。
     */
    int m_dragAnchor = 0;
    //! 拖选时用的连击计数
    int m_clickChain = 0;
    QElapsedTimer m_clickClock;
    //! `scheduleLayoutUpdate()` 已经排过一次了吗（合并同一轮里的重复请求）
    bool m_layoutUpdatePending = false;
    //! 任务正在改文档：暂停重画（见 setRepaintsDeferred）
    bool m_repaintsDeferred = false;
    /*!
     * 最近一秒里 `updateScrollRange()` 都是谁调进来的（诊断活锁用）。
     * 触发源包括：布局信号、内容变化、设页面、缩放、窗口尺寸、光标定位、外部直接调用。
     */
    QStringList m_rangeCallSources;
};

#endif // TEXTEDITOR_H
