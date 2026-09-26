#include "texteditor.h"
#include "effectsoverlay.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QFile>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

TextEditor::TextEditor(QWidget *parent)
    : QTextEdit(parent)
{
    m_overlay = new EffectsOverlay(this);
    m_overlay->hide();

    // 纸张、桌面底色、阴影全部自己画。
    // 关键：QTextEdit 默认会用调色板把整个 viewport 填一遍，那样在深色主题下
    // 白色的纸面会被盖成灰色，所以这里既关掉 viewport 的自动填充，
    // 又把文档的基色/文字色固定成"白纸黑字"。
    setFrameShape(QFrame::NoFrame);
    viewport()->setAutoFillBackground(false);
    /*!
     * 横向滚动条永远关掉。
     *
     * "纸张居中"是用横向滚动值实现的，但滚动值一变，QTextEdit 的
     * mapToContents()（= viewport 坐标 + 滚动值）就跟着变，而它并不知道
     * 那段偏移其实只是留白 —— 点击定位会整段错位，纸也会被推出窗口。
     * 所以居中的偏移量必须对 Qt 隐形：滚动条关掉之后
     * horizontalScrollBar()->value() 被钳成 0，两边就都对齐了。
     *
     * 代价：纸张比窗口宽时看不全，需要放大窗口（排版软件普遍这么处理）。
     */
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QPalette pal = palette();
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive}) {
        pal.setColor(group, QPalette::Base, Qt::white);
        pal.setColor(group, QPalette::Text, QColor(0x1a, 0x1a, 0x1a));
        /*!
         * 选区高亮和光标色也是"纸面上的颜色"，必须一起钉死。
         * 只改 Base/Text 的话，深色主题给出的 Highlight / HighlightedText
         * 会跟着进来：选中文字的浅色前景落到白纸上就等于隐形 ——
         * 看起来就像"选区没有视觉提示"。
         */
        pal.setColor(group, QPalette::Highlight, QColor(0x33, 0x99, 0xff));
        pal.setColor(group, QPalette::HighlightedText, Qt::white);
    }
    setPalette(pal);
    viewport()->setPalette(pal);
    m_overlay->setPalette(pal);

    // 光标必须能显示：焦点策略要能吃键盘，宽度给 2px 才看得清
    setFocusPolicy(Qt::StrongFocus);
    setCursorWidth(2);

    connect(verticalScrollBar(), &QScrollBar::valueChanged,
            m_overlay, qOverload<>(&QWidget::update));
    connect(horizontalScrollBar(), &QScrollBar::valueChanged,
            m_overlay, qOverload<>(&QWidget::update));
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, &TextEditor::updateScrollRange);
    // 内容变了要重算滚动范围（页数变了），效果层也要重画
    connect(document(), &QTextDocument::contentsChanged, this, [this] {
        updateScrollRange();
        m_overlay->update();
    });

    //! 闪烁：借用 QTextEdit 自己的闪烁定时器相位（它在暗相位会把
    //! cursorRect() 置空），所以这里只要保证光标被画出来就行。
    connect(this, &QTextEdit::cursorPositionChanged, this, [this] {
        viewport()->update();
    });

    setPageSetup(m_pageSetup);
    setEffectsVisible(false);
}

// ---------------------------------------------------------------- 版面几何

double TextEditor::paperWidthPx() const
{
    return PageSetup::mmToPx(m_pageSetup.paperSizeMm().width());
}

double TextEditor::paperHeightPx() const
{
    return PageSetup::mmToPx(m_pageSetup.paperSizeMm().height());
}

/*!
 * 纸张居中的留白量 = **viewport 内容边距**（setViewportMargins 的左边距）。
 *
 * 基准必须用**编辑区宽度**，不能用 viewport()->width()：
 * viewport 的宽度本身就是这个函数的结果决定的，拿它当输入就是自指，
 * 会算出一个越滚越偏的值（曾经就是这样，纸张被推到右边缘、右边只剩 14 像素，
 * 而光标则落在纸外面）。用编辑区宽度 + 给竖滚动条留的固定宽度，
 * 结果就只跟窗口尺寸有关，跟滚动条当前是否可见无关 —— 不会震荡。
 *
 * 纸张放不下时返回 0，让水平方向的滚动范围去兜底。
 */
double TextEditor::paperPadPx() const
{
    const double available = double(width()) - scrollBarReservePx();
    return qMax(0.0, (available - paperWidthPx()) / 2.0);
}

int TextEditor::scrollBarReservePx() const
{
    return style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);
}

QPointF TextEditor::paperOriginInViewport() const
{
    return QPointF(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
}

/*!
 * viewport 左上角在**编辑区坐标**里的位置。
 *
 * 等于 contentsRect 原点 + viewport 边距 + 边框。
 * 自检要把"排版坐标"换成"event->pos() 用的 viewport 局部坐标"，
 * 靠的就是减掉这个量。
 *
 * 不要用 viewport()->pos() 代替：那是相对**父控件**的位置，
 * 而且取值时机不同结果也会变（实测拿它算会减两次，
 * 事件落到纸外面，"按下没反应"）。
 */
QPoint TextEditor::viewportOriginInEditor() const
{
    return QPoint(contentsRect().left() + viewportMargins().left() + frameWidth(),
                  contentsRect().top() + viewportMargins().top() + frameWidth());
}

QPointF TextEditor::bodyOriginPx() const
{
    const QMarginsF m = m_pageSetup.bodyMarginsPx();
    return QPointF(m.left(), m.top());
}

QRectF TextEditor::pageRectInDocument() const
{
    return QRectF(QPointF(0.0, 0.0), QSizeF(paperWidthPx(), paperHeightPx()));
}

/*!
 * 文档坐标 -> viewport 坐标。**正文、光标、选区、效果覆盖层
 * 四者必须共用这一个变换。**
 *
 * 内容边距（纸张居中量）已经由 setViewportMargins 消化掉了 ——
 * viewport 原点的 x 本身就是纸张左边缘，所以这里只剩两段：
 *
 *   1. 页边距 bodyOriginPx()：文档坐标以"第一页正文区左上角"为原点，
 *      而 QTextEdit 眼里的"内容原点"是 viewport 原点。
 *   2. 滚动量。
 *
 * 反过来（viewport -> 文档坐标）也是同一套：
 *   QTextEditPrivate::mapToContents() 算的是
 *   "viewport 坐标 - viewport 边距 + 滚动值"，
 *   在"内容原点 = 文档原点 - 页边距"的前提下正好和这个变换互逆。
 * 两套不能对不上，否则就是"字画在一处、光标画在另一处、点哪儿都不对"。
 */
QTransform TextEditor::documentToViewport() const
{
    const QPointF origin = documentOriginInViewport();
    return QTransform::fromTranslate(origin.x(), origin.y());
}

//! 第一页正文区左上角在 viewport 里的位置（当前滚动量下）
QPointF TextEditor::documentOriginInViewport() const
{
    const QPointF body = bodyOriginPx();
    const QPointF paper = paperOriginInViewport();
    return QPointF(paper.x() + body.x(), paper.y() + body.y());
}

//! 第 \a page 页正文区原点相对第一页的偏移（文档坐标）
QPointF TextEditor::pageTopLeft(int page) const
{
    return QPointF(0.0, page * paperHeightPx());
}

/*!
 * 排版坐标点 -> 文档坐标下的光标。
 *
 * **入参是排版坐标**（也就是 documentToViewport() 的值域、
 * 自检里靶点用的那套）。鼠标事件拿到的是 viewport 局部坐标，
 * 必须先过 viewportToEditor() 换算过来。
 *
 * 这里直接问 QAbstractTextDocumentLayout —— 也就是**画字的那个对象本人**，
 * 所以"命中测试"和"绘制"一定共用同一套坐标，不存在两套对不上的可能。
 *
 * 不走 QTextEdit::cursorForPosition()：它把 viewport 边距按自己那套算法
 * 折进去了，和正文实际画的位置差一个页边距，实测同一个点会给出
 * 完全不同的行。
 */
QTextCursor TextEditor::documentCursorAt(const QPoint &editorPos) const
{
    const QPointF doc = documentToViewport().inverted().map(QPointF(editorPos));
    const int pos = document()->documentLayout()->hitTest(doc, Qt::FuzzyHit);
    QTextCursor c(document());
    if (pos >= 0)
        c.setPosition(pos);
    return c;
}

/*!
 * viewport 局部坐标 -> 排版坐标。
 *
 * **event->pos() 是 viewport 局部坐标**：原点在 viewport 左上角，
 * 不含纸张居中边距（那部分已经做成 viewport 自己的边距了）。
 * 加上 viewport 在编辑区里的位置，就得到排版坐标。
 */
QPoint TextEditor::viewportToEditor(const QPoint &viewportPos) const
{
    return viewportPos + viewportOriginInEditor();
}

//! viewport 局部坐标 -> 文档坐标
QPointF TextEditor::viewportToDocument(const QPoint &viewportPos) const
{
    return documentToViewport().inverted().map(QPointF(viewportToEditor(viewportPos)));
}

/*!
 * 鼠标事件全部自己处理。
 *
 * QTextEdit 自己的实现走的是它内部那套坐标（viewport 边距不参与），
 * 和正文实际画的位置对不上，所以这里统一换成 documentCursorAt()。
 */
void TextEditor::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QTextEdit::mousePressEvent(event);
        return;
    }

    setFocus(Qt::MouseFocusReason);
    m_dragSelecting = true;
    /*!
     * 连击计数：双击选词、三击选整段。
     * 必须用**时间**判断是不是同一次连击 —— 只看计数器的话，
     * 上一次双击留下的计数会让很久之后的单击被当成三击。
     */
    if (m_clickClock.isValid() && m_clickClock.restart() < QApplication::doubleClickInterval())
        ++m_clickChain;
    else {
        m_clickClock.restart();
        m_clickChain = 1;
    }

    QTextCursor c = documentCursorAt(viewportToEditor(event->pos()));
    if (event->type() == QEvent::MouseButtonDblClick)
        c.select(QTextCursor::WordUnderCursor);
    else if (m_clickChain >= 3)
        c.select(QTextCursor::BlockUnderCursor);

    setTextCursor(c);
    viewport()->update();
    event->accept();
}

void TextEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragSelecting || !(event->buttons() & Qt::LeftButton)) {
        QTextEdit::mouseMoveEvent(event);
        return;
    }

    QTextCursor c = documentCursorAt(viewportToEditor(event->pos()));
    QTextCursor anchor = textCursor();
    anchor.setPosition(c.position(), QTextCursor::KeepAnchor);
    setTextCursor(anchor);
    viewport()->update();
    event->accept();
}

void TextEditor::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QTextEdit::mouseReleaseEvent(event);
        return;
    }
    /*!
     * 单击（没有拖动过）要把选区收掉 —— 这是编辑器的通用行为，
     * 也是"点一下取消选择"能work的前提。
     */
    if (m_dragSelecting) {
        m_dragSelecting = false;
        if (!textCursor().hasSelection())
            m_clickChain = 0;
    }
    viewport()->update();
    event->accept();
}

/*!
 * \brief 把滚动范围钉成"整张纸"。
 *
 * 这是鼠标能不能点准的关键。
 * QTextEdit 自己算的滚动范围只覆盖文档内容（正文区），可我们画的是
 * 纸 + 页边距 + 正文。于是 viewport 原点、文档原点和纸面原点三者错位，
 * 点击位置被系统性偏移了一个"纸张左边距 + 正文上边距"，
 * 表现就是"点哪一行都不对、打字落不到那一行"。
 *
 * 做法：
 *   - 上下：范围 = 正文上边距 + 内容 + 下边距，滚动值 0 时
 *     正文区上边缘正好留出 bodyTop 的空白；
 *   - 左右：纸张居中量做成 **viewport 左边距**（setViewportMargins），
 *     于是 viewport 原点的 x 就是纸张左边缘。这边距**只由编辑区宽度算出**
 *     （见 paperPadPx），viewport 宽度不参与 —— 否则"边距改宽度、
 *     宽度改边距"会互相推挤到死循环。纸张比剩下空间还宽时才启用水平滚动兜底。
 */
void TextEditor::updateScrollRange()
{
    const QMarginsF body = m_pageSetup.bodyMarginsPx();
    const QSizeF content = document()->size();

    const double totalHeight = body.top() + content.height() + body.bottom();
    verticalScrollBar()->setRange(0, qMax(0, int(std::ceil(totalHeight))
                                                 - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());

    const int pad = int(std::lround(paperPadPx()));
    const int paper = int(std::ceil(paperWidthPx()));
    const int hRange = qMax(0, paper - qMax(0, viewport()->width() - 2 * pad));
    horizontalScrollBar()->setRange(0, hRange);
    horizontalScrollBar()->setPageStep(viewport()->width());

    if (viewportMargins().left() != pad)
        setViewportMargins(pad, 0, 0, 0);

    updateOverlayGeometry();
}

void TextEditor::updatePageMargins()
{
    // 纸张尺寸变了：重新钉住正文宽高、重算滚动范围与居中位置
    applyLayoutMetrics();
}

/*!
 * 版面尺寸的铁律：
 *   QTextEdit 默认的 LineWrapMode::WidgetWidth 会**忽略** document()->pageSize()，
 *   直接按 viewport 宽度断行。纸张居中之后 viewport 比正文区宽得多，
 *   所以必须显式把换行宽度钉在"正文宽度"上，断行位置才等于打印时的断行位置。
 *   两处都要设：document()->setPageSize() 决定分页高度，换行宽度决定每行长度。
 */
void TextEditor::applyLayoutMetrics()
{
    const QSizeF body = m_pageSetup.bodySizePx();

    document()->setDocumentMargin(0);
    document()->setPageSize(body);

    setLineWrapMode(QTextEdit::FixedPixelWidth);
    setLineWrapColumnOrWidth(int(std::lround(body.width())));

    updateScrollRange();
    viewport()->update();
    m_overlay->update();
}

// ---------------------------------------------------------------- 对外接口

void TextEditor::setPageSetup(const PageSetup &setup)
{
    m_pageSetup = setup;
    applyLayoutMetrics();
}

void TextEditor::setEffectOptions(const EffectRenderOptions &options)
{
    m_options = options;
    m_overlay->setOptions(m_options);
    m_overlay->update();
}

void TextEditor::setEffectsVisible(bool visible)
{
    if (m_effectsVisible == visible)
        return;
    m_effectsVisible = visible;
    m_overlay->setVisible(visible);
    m_overlay->update();
    viewport()->update();
    emit effectsVisibilityChanged(visible);
}

void TextEditor::relayout()
{
    document()->markContentsDirty(0, document()->characterCount());
    updateScrollRange();
    viewport()->update();
    m_overlay->update();
}

/*!
 * 效果覆盖层永远盖在 viewport 上，位置就是 viewport 的整个矩形。
 * 它是 WA_TransparentForMouseEvents 的，所以不会吃掉鼠标事件
 * —— 选区、光标、点击定位仍然由 QTextEdit 处理。
 */
void TextEditor::updateOverlayGeometry()
{
    if (!m_overlay)
        return;
    m_overlay->setGeometry(viewport()->rect());
    if (m_effectsVisible)
        m_overlay->raise();
}

// ---------------------------------------------------------------- 绘制

/*!
 * 绘制分三层，**两层用的原点不一样，这是最容易搞错的地方**：
 *
 *   - 纸面：画在 paperOriginInViewport()。纸张居中量已经由 viewport 边距
 *     消化掉了（viewport 原点的 x 就是纸张左边缘），所以这里不能再加
 *     paperPadPx()，否则纸会被推右一个居中量。
 *   - 正文：画在 documentOriginInViewport() = 纸面原点 + 页边距。
 *     QTextEdit 画光标、画选区用的是 documentLayout()->draw() 的原点，
 *     也就是**内容坐标原点**，所以正文必须画在 documentOriginInViewport()，
 *     不能画在纸面原点上，也不能靠 documentToViewport() 之外的任何近似。
 *
 * 光标 / 选区 / 鼠标命中测试全部由 QTextEdit 在"内容坐标"里算，
 * 三个原点一旦不一致，症状就是"字画在一处、光标画在另一处、点哪儿都不对"。
 */
void TextEditor::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, false);

    const double paperW = paperWidthPx();
    const double paperH = paperHeightPx();
    const QSizeF bodySize = m_pageSetup.bodySizePx();
    const QPointF paperOrigin = paperOriginInViewport();
    const QPointF docOrigin = documentOriginInViewport();
    const int scrollY = verticalScrollBar()->value();

    // 1. 桌面底色
    painter.fillRect(event->rect(), QColor(0x3a, 0x3d, 0x42));

    // 2. 逐页画纸：阴影 -> 白纸 -> 正文
    const int firstPage = qMax(0, int((event->rect().top() + scrollY) / paperH));
    const int lastPage = int((event->rect().bottom() + scrollY) / paperH);

    QAbstractTextDocumentLayout::PaintContext context;
    context.palette = palette();
    context.palette.setColor(QPalette::Text, QColor(0x1a, 0x1a, 0x1a));
    context.palette.setColor(QPalette::Base, Qt::white);
    /*!
     * cursorPosition 保持 -1：让 Qt 自己别画光标，由下面 drawCaret() 统一负责。
     *
     * 为什么要自己画？因为"光标该不该显示"（闪烁相位）在 Qt 里是
     * QWidgetTextControl 的私有状态，只由它自己的闪烁定时器维护。
     * 本控件整个 paintEvent 都是自己写的（要画纸张、页边距），
     * 走到 documentLayout()->draw() 的时候那个状态早就对不上了 ——
     * 光标要么不画，要么画在另一套坐标里。
     * 自己画只有一处坐标来源（documentToViewport()），不会再分叉。
     */
    context.cursorPosition = -1;
    /*!
     * 选区高亮也必须自己填。
     *
     * QTextEdit 平时是在 QWidgetTextControl::drawContents() 里把当前选区
     * 塞进 PaintContext::selections 再交给文档布局画的；
     * 本控件为了画纸张整个 paintEvent 都是自己写的，根本没走那条路，
     * 于是 selections 一直是空的 —— 表现就是"能选中（键盘、鼠标都正常，
     * 复制也正常）但屏幕上完全看不出选没选"。
     *
     * 颜色取调色板里的 Highlight / HighlightedText，也就是
     * 构造函数里钉死的那套（深色主题下默认给的浅色前景落到白纸上等于隐形）。
     */
    {
        const QTextCursor sel = textCursor();
        if (sel.hasSelection()) {
            QAbstractTextDocumentLayout::Selection s;
            s.cursor = sel;
            s.format.setBackground(palette().color(QPalette::Highlight));
            s.format.setForeground(palette().color(QPalette::HighlightedText));
            context.selections.append(s);
        }
    }

    for (int page = firstPage; page <= lastPage; ++page) {
        // 这一页的纸面（viewport 坐标）
        const QRectF pageRect(paperOrigin + QPointF(0.0, page * paperH),
                              QSizeF(paperW, paperH));

        // 阴影：只在最外侧留一圈，多页之间不重复描边
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 70));
        painter.drawRect(pageRect.translated(2.0, 3.0));

        painter.setBrush(Qt::white);
        painter.setPen(QPen(QColor(0x8a, 0x8d, 0x92), 1));
        painter.drawRect(pageRect);

        painter.save();
        painter.setClipRect(pageRect);

        if (page > 0) {
            // 后续页面的正文区域提示（虚线框），方便看排版范围
            const QRectF bodyRect(docOrigin + QPointF(0.0, page * paperH),
                                  bodySize);
            painter.setPen(QPen(QColor(70, 130, 200), 1, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(bodyRect);
        }

        /*!
         * 正文：原点 = documentOriginInViewport()，再挪到第 page 页的正文区。
         * 这个平移量必须和 documentToViewport() 里的完全一致，
         * 否则第 2 页起的光标会一行行错开。
         */
        painter.translate(docOrigin + QPointF(0.0, page * paperH));
        painter.setClipRect(QRectF(0.0, 0.0, bodySize.width(), bodySize.height()));
        document()->documentLayout()->draw(&painter, context);
        painter.restore();
    }

    // 3. 光标：自己画，坐标只认 documentToViewport()
    drawCaret(&painter, docOrigin);

    // 4. 手写 / 扭曲效果层由 EffectsOverlay 负责
}

/*!
 * 画光标竖条。
 *
 * QTextEdit::cursorRect() 给的是**文档坐标**（已在实测中核对：
 * 光标在 "HANDWRITING" 末尾时它是 x=141，正好等于那串字的宽度）。
 * 所以这里只需要加一次 documentOriginInViewport()，也就是
 * documentToViewport() —— 和正文、覆盖层完全同一个原点。
 *
 * 闪烁：用 QApplication::cursorFlashTime() 驱动 QTextEdit 自带的 blink 定时器，
 * 相位由 cursorRect() 是否为空来判断（Qt 在"不该显示"时返回空矩形）。
 * 这样跟系统的闪烁设置保持一致，不用自己维护相位。
 */
void TextEditor::drawCaret(QPainter *painter, const QPointF &docOrigin)
{
    if (!hasFocus() && !viewport()->hasFocus())
        return;                       // 没焦点不画光标，跟系统惯例一致

    const QRect caret = cursorRect();   // 文档坐标
    if (caret.isEmpty())
        return;                         // 闪烁的暗相位

    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setBrush(palette().color(QPalette::Text));
    const QRectF r(QPointF(docOrigin.x() + caret.x(), docOrigin.y() + caret.y()),
                   QSizeF(qMax(1, caret.width()), caret.height()));
    painter->drawRect(r);
    painter->restore();
}

void TextEditor::resizeEvent(QResizeEvent *event)
{
    QTextEdit::resizeEvent(event);
    updateScrollRange();
}

void TextEditor::scrollContentsBy(int dx, int dy)
{
    QTextEdit::scrollContentsBy(dx, dy);
    updateOverlayGeometry();
    m_overlay->update();
}
