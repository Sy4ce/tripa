#ifndef TEXTEDITOR_H
#define TEXTEDITOR_H

#include "effectsrenderer.h"
#include "pagesetup.h"

#include <QElapsedTimer>
#include <QTextEdit>
#include <QTransform>

class QPainter;

/*!
 * \brief 所见即所得的排版编辑区。
 *
 * 版面约定（很重要，打印和导出的对齐都依赖它）：
 *   - 文档（QTextDocument）只有"正文"那么大：pageSize = 纸张 - 页边距，
 *     documentMargin = 0，所以文档坐标 (0,0) 就是第一页正文区的左上角；
 *   - 第 n 页的正文区左上角在文档坐标 (0, n * 纸张高度)；
 *   - 纸面、阴影、居中位置全部在这里自己画，viewport 不填色，
 *     这样深色主题下纸张依然是白纸黑字。
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
     * 文档布局仍然是 100% 那一套，所以"屏幕上看的样子"和"纸上印的样子"
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

    void setPageSetup(const PageSetup &setup);
    const PageSetup &pageSetup() const { return m_pageSetup; }

    //! 正文区在第一页里的左上角偏移（像素，96dpi）
    QPointF bodyOriginPx() const;
    //! 一张纸在文档坐标里的矩形（原点在第一页左上角；文档口径，不含缩放）
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

    //! 文档坐标 -> viewport 坐标（滚动 + 纸张居中 + 正文页边距）
    QTransform documentToViewport() const;
    //! 第一页正文区左上角在 viewport 里的位置
    QPointF documentOriginInViewport() const;
    //! 第 page 页正文区原点相对第一页的偏移（文档坐标）
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
    void updateScrollRange();
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

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    //! 把正文宽高钉到页面设置上（pageSize + 换行宽度），纸张/窗口变化都走这里
    void applyLayoutMetrics();
    //! 自己画光标竖条（Qt 的光标可见性状态在私有类里，拿不到）
    void drawCaret(QPainter *painter, const QPointF &docOrigin);
    /*!
     * 光标矩形，**文档坐标**。
     *
     * 不要直接用 `QTextEdit::cursorRect()`：它给的是"文档坐标 **减掉滚动量**"
     * （实测：文档 y=130 的光标在滚动 200 之后返回 -70）。
     * 滚动量为 0 时两者一样，所以"没滚动过"的场合看不出问题 ——
     * 一旦滚下去，照它画的光标就会整体再上移一个滚动量，
     * 屏幕上直接看不见（这正是它一直在干的事）。
     */
    QRect caretRectInDocument() const;
    //! 缩放的真正实现：改比例、重算滚动范围，再把锚点那一格摆回原处
    void applyZoom(double zoom, const QPointF &viewportAnchor);

    PageSetup m_pageSetup;
    EffectRenderOptions m_options;
    bool m_effectsVisible = true;
    //! 视图缩放，1.0 = 100%
    double m_zoom = 1.0;
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
    //! 连击计数：2 = 双击选词，3 = 三击选段
    int m_clickChain = 0;
    QElapsedTimer m_clickClock;
};

#endif // TEXTEDITOR_H
