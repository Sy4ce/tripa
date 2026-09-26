#ifndef TEXTEDITOR_H
#define TEXTEDITOR_H

#include "effectsrenderer.h"
#include "pagesetup.h"

#include <QElapsedTimer>
#include <QTextEdit>
#include <QTransform>

class EffectsOverlay;
class QPainter;

/*!
 * \brief 所见即所得的排版编辑区。
 *
 * 版面约定（很重要，覆盖层和打印都依赖它）：
 *   - 文档（QTextDocument）只有"正文"那么大：pageSize = 纸张 - 页边距，
 *     documentMargin = 0，所以文档坐标 (0,0) 就是第一页正文区的左上角；
 *   - 第 n 页的正文区左上角在文档坐标 (0, n * 纸张高度)；
 *   - 纸面、阴影、居中位置全部在这里自己画，viewport 不填色，
 *     这样深色主题下纸张依然是白纸黑字。
 *
 * 手写 / 扭曲效果由 EffectsOverlay（viewport 的子控件）单独绘制，
 * 它用同一套 documentToViewport() 变换对齐，所以不会影响文档本身。
 */
class TextEditor : public QTextEdit
{
    Q_OBJECT
public:
    explicit TextEditor(QWidget *parent = nullptr);

    void setPageSetup(const PageSetup &setup);
    const PageSetup &pageSetup() const { return m_pageSetup; }

    //! 正文区在第一页里的左上角偏移（像素，96dpi）
    QPointF bodyOriginPx() const;
    //! 一张纸在文档坐标里的矩形（原点在第一页左上角）
    QRectF pageRectInDocument() const;

    double paperWidthPx() const;
    double paperHeightPx() const;
    /*!
     * 纸张居中留白量。
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

    //! 排版坐标点 -> 文档坐标下的光标（鼠标命中测试的唯一入口）
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

signals:
    void effectsVisibilityChanged(bool visible);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    //! 把正文宽高钉到页面设置上（pageSize + 换行宽度），纸张/窗口变化都走这里
    void applyLayoutMetrics();
    //! 自己画光标竖条（Qt 的光标可见性状态在私有类里，拿不到）
    void drawCaret(QPainter *painter, const QPointF &docOrigin);

    PageSetup m_pageSetup;
    EffectRenderOptions m_options;
    bool m_effectsVisible = true;
    EffectsOverlay *m_overlay = nullptr;
    //! 是否正在按住左键拖选
    bool m_dragSelecting = false;
    //! 连击计数：2 = 双击选词，3 = 三击选段
    int m_clickChain = 0;
    QElapsedTimer m_clickClock;
};

#endif // TEXTEDITOR_H
