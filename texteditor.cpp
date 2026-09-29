#include "texteditor.h"

#include "effectsrenderer.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QFile>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QWheelEvent>

namespace {

/*!
 * 纸张外面那一圈"桌面"的颜色。
 *
 * 特意不用调色板颜色：这个控件是自己画纸的（见 paintEvent），
 * 纸面必须永远是白的、桌面必须是**确定的**一种灰 ——
 * 跟着主题走的话，浅色主题下桌面变成浅灰、深色主题下变成近黑，
 * "纸是不是白的"这种判断在两种主题下就成了两回事。
 */
const QColor kDeskColor(0x3a, 0x3d, 0x42);

} // namespace

TextEditor::TextEditor(QWidget *parent)
    : QTextEdit(parent)
{
    // 纸张、桌面底色、阴影全部自己画。
    // 关键：QTextEdit 默认会用调色板把整个 viewport 填一遍，那样在深色主题下
    // 白色的纸面会被盖成灰色，所以这里既关掉 viewport 的自动填充，
    // 又把文档的基色/文字色固定成"白纸黑字"。
    setFrameShape(QFrame::NoFrame);
    viewport()->setAutoFillBackground(false);
    /*!
     * 横向滚动条永远关掉 —— 但**滚动值照用**（见 wheelEvent 里的 Shift+滚轮平移）。
     *
     * 当年关掉它，是因为"纸张居中"曾经是用横向滚动值实现的：滚动值一变，
     * QTextEdit 的 mapToContents()（= viewport 坐标 + 滚动值）就跟着变，
     * 而它并不知道那段偏移其实只是留白 —— 于是一变滚动值，点击定位就整段错位、
     * 纸也被推出窗口。所以居中的偏移量必须对 Qt 隐形。
     *
     * 现在居中量已经是 viewport 左边距（见 updateScrollRange），
     * 滚动值的含义回到了标准模型：**它就是纸面相对 viewport 的平移量**
     * （paperOriginInViewport() 里那个负号）。正文、光标、效果层、命中测试
     * 全都走同一个变换（documentToViewport），所以拿它做横向平移是安全的
     * （自检里平移前后各做一次命中，结果必须一样）。
     *
     * 那为什么条子还是关着？因为一旦让它可见，Qt 会自己接管横向范围
     * （_q_adjustScrollbars 按**文档宽度**设，我们按**纸张宽度**设，两者不一样），
     * 而且缩放之后纸本来就常常比窗口宽，条子会反复出现/消失。
     * 所以：**值我们用，条子不显示**。
     *
     * 代价：纸张比窗口宽时看不全，得用 Shift+滚轮平移（或把窗口拉大）。
     * 注意滚动值**不是**被钳成 0 的（实测 setValue(300) 站得住），
     * 所以别的地方不要随手去写它。
     */
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QPalette pal = palette();
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive}) {
        pal.setColor(group, QPalette::Base, Qt::white);
        pal.setColor(group, QPalette::Text, QColor(0x1a, 0x1a, 0x1a));
        /*!
         * 纸张外面那一圈桌面必须**一个颜色**，而这圈桌面里有一半不属于
         * viewport：纸张居中量做成了 viewport 左边距（见 updateScrollRange），
         * 所以纸左边那 151 像素其实是**控件自己的背景**。
         *
         * 那一块在 paintEvent 里刷不到 —— QAbstractScrollArea 只把
         * **viewport** 的绘制事件转给 paintEvent（实测事件矩形正好是
         * viewport 的尺寸，而控件比它宽一个居中量），控件自身那条边
         * 不在任何一次绘制事件里，它一直是父窗口的背景色，
         * 于是纸上左右两侧一深一浅（右边更浅，一眼就能看出来）。
         * 唯一覆盖得到它的地方是控件自己的背景画刷（见下面的
         * setBackgroundRole / setAutoFillBackground）。
         */
        pal.setColor(group, QPalette::Window, kDeskColor);
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
    /*!
     * 让 Qt 用调色板把控件自己的背景刷出来（含 viewport 之外那一圈）。
     * viewport 的调色板是单独设的，不受影响；纸面照旧由 paintEvent 自己画。
     */
    setBackgroundRole(QPalette::Window);
    setAutoFillBackground(true);
    viewport()->setPalette(pal);

    // 光标必须能显示：焦点策略要能吃键盘，宽度给 2px 才看得清
    setFocusPolicy(Qt::StrongFocus);
    setCursorWidth(2);

    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, &TextEditor::updateScrollRange);
    // 内容变了要重算滚动范围（页数变了），画面也要重画
    connect(document(), &QTextDocument::contentsChanged, this, [this] {
        updateScrollRange();
        viewport()->update();
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
 * 纸张尺寸取**含缩放**的那个：放大之后纸变宽，留白自然跟着变小、纸仍然居中。
 */
double TextEditor::paperPadPx() const
{
    const double available = double(width()) - scrollBarReservePx();
    return qMax(0.0, (available - paperViewWidthPx()) / 2.0);
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
 * 只有自检用得到（要把 viewport 局部坐标换成窗口坐标去截图比对）；
 * 正文、光标、选区、鼠标全部走 viewport 局部坐标，不经过这里。
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
 * 文档坐标 -> viewport 坐标。**正文、光标、选区、效果层
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
    /*!
     * 缩放和原点合成**一个**变换交给所有调用方：正文、光标、选区、鼠标命中、
     * 效果层全用它（6 个参数直接写出来，比 translate()/scale() 连写更不容易
     * 搞错顺序：m11 = m22 = 缩放，dx/dy = 原点）。
     * 逆变换就是"视口坐标 -> 文档坐标"，命中测试也走它。
     */
    return QTransform(m_zoom, 0.0, 0.0, m_zoom, origin.x(), origin.y());
}

//! 第一页正文区左上角在 viewport 里的位置（当前滚动量下）
QPointF TextEditor::documentOriginInViewport() const
{
    const QPointF body = bodyOriginPx();
    const QPointF paper = paperOriginInViewport();
    return QPointF(paper.x() + body.x() * m_zoom, paper.y() + body.y() * m_zoom);
}

//! 第 \a page 页正文区原点相对第一页的偏移（文档坐标）
QPointF TextEditor::pageTopLeft(int page) const
{
    return QPointF(0.0, page * paperHeightPx());
}

/*!
 * viewport 局部坐标点 -> 文档坐标下的光标（鼠标命中测试的唯一入口）。
 *
 * 入参就是 `QMouseEvent::pos()` 那一套坐标，**别的什么都不是**。
 * 这里直接问 QAbstractTextDocumentLayout —— 也就是**画字的那个对象本人**，
 * 所以"命中测试"和"绘制"一定共用同一套坐标，不存在两套对不上的可能。
 *
 * 这里踩过一个大坑：接口原来声明成"排版坐标"（也就是文档坐标 + 页边距那套），
 * 可 documentToViewport() 的值域其实是 **viewport 局部坐标** ——
 * 也就是 documentToViewport() 自己的输出。命名和实现差了一层，
 * 于是调用方（自检）按"排版坐标"多减了一次 viewport 原点，
 * 命中测试就整段偏掉一个"纸张居中量 + 工具栏高度"，
 * 每次点击都落在纸外面，hitTest 返回行尾。
 * 现在接口签名直接写 QPoint（viewport 局部），不再有歧义。
 *
 * 不走 QTextEdit::cursorForPosition()：它把 viewport 边距按自己那套算法
 * 折进去了，和正文实际画的位置差一个页边距，实测同一个点会给出完全不同的行。
 */
QTextCursor TextEditor::documentCursorAt(const QPoint &viewportPos) const
{
    const QPointF doc = documentToViewport().inverted().map(QPointF(viewportPos));
    const int pos = document()->documentLayout()->hitTest(doc, Qt::FuzzyHit);
    QTextCursor c(document());
    if (pos >= 0)
        c.setPosition(pos);
    return c;
}

//! viewport 局部坐标 -> 排版坐标（= 文档坐标 + 正文区原点）
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

    QTextCursor c = documentCursorAt(event->pos());
    if (event->type() == QEvent::MouseButtonDblClick)
        c.select(QTextCursor::WordUnderCursor);
    else if (m_clickChain >= 3)
        c.select(QTextCursor::BlockUnderCursor);

    setTextCursor(c);
    /*!
     * 锚点 = **按下那一点**对应的字符位置。
     *
     * 即使双击已经选了一整个词，也仍然用按下点当锚点：
     * 这样继续往右拖是"从整个词往右扩"，往左拖不会突然把锚点甩到词的另一头。
     */
    m_dragAnchor = documentCursorAt(event->pos()).position();
    viewport()->update();
    event->accept();
}

void TextEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragSelecting || !(event->buttons() & Qt::LeftButton)) {
        QTextEdit::mouseMoveEvent(event);
        return;
    }

    /*!
     * 用**按下时记下的锚点**扩选，不能读 textCursor().anchor()。
     * 那个值在每次扩选之后就已经被推到上一次的终点了 ——
     * 于是往左拖会把选区锁死在"行首..起点"，
     * 往右拖会塌成一个点（"选不到中间就停"就是这么来的）。
     */
    QTextCursor c(document());
    c.setPosition(m_dragAnchor);
    c.setPosition(documentCursorAt(event->pos()).position(), QTextCursor::KeepAnchor);
    setTextCursor(c);
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

    // 滚动值是**视图像素**（见 paperOriginInViewport），所以整张纸的高度要乘缩放
    const double totalHeight = (body.top() + content.height() + body.bottom()) * m_zoom;
    verticalScrollBar()->setRange(0, qMax(0, int(std::ceil(totalHeight))
                                                 - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());

    const int pad = int(std::lround(paperPadPx()));
    const int paper = int(std::ceil(paperViewWidthPx()));
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
}

// ---------------------------------------------------------------- 对外接口

void TextEditor::setPageSetup(const PageSetup &setup)
{
    m_pageSetup = setup;
    applyLayoutMetrics();
}

// ---------------------------------------------------------------- 缩放

void TextEditor::setZoom(double zoom)
{
    applyZoom(zoom, QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));
}

void TextEditor::setZoomAt(double zoom, const QPointF &viewportAnchor)
{
    applyZoom(zoom, viewportAnchor);
}

void TextEditor::zoomBy(double delta, const QPointF &viewportAnchor)
{
    applyZoom(m_zoom + delta, viewportAnchor);
}

/*!
 * 缩放的真正实现。
 *
 * 铁律：**只有这一个地方改 m_zoom**。缩放牵动的东西比看上去多 ——
 * 绘制变换（documentToViewport）、纸张居中量、滚动范围、光标、命中测试 ——
 * 少改一处就是"字画在一处、点哪儿都不对"。全部收在这里，
 * 而且一律走已有的那几条路（applyLayoutMetrics / updateScrollRange）。
 */
void TextEditor::applyZoom(double zoom, const QPointF &viewportAnchor)
{
    zoom = qBound(kZoomMin, zoom, kZoomMax);
    if (qFuzzyCompare(zoom, m_zoom))
        return;

    /*!
     * 锚点：缩放前后，\a viewportAnchor 那一点底下的**文档位置**必须还在原处，
     * 否则 Ctrl+滚轮时鼠标底下那个字会一路往边上跑（放大时特别明显）。
     * 先按旧比例算出那个文档位置，缩完再把竖滚动条摆到"它正好回到锚点"的位置。
     *
     * 横向不用管：纸张居中量做成了 viewport 左边距（见 updateScrollRange），
     * 横向滚动值恒为 0，纸自己会重新居中。
     */
    const QPointF docAnchor = documentToViewport().inverted().map(viewportAnchor);

    m_zoom = zoom;
    applyLayoutMetrics(); // 重算滚动范围 + 纸张居中量，顺带重绘

    QScrollBar *bar = verticalScrollBar();
    const double wanted =
        bodyOriginPx().y() * m_zoom + docAnchor.y() * m_zoom - viewportAnchor.y();
    bar->setValue(qBound(bar->minimum(), int(std::lround(wanted)), bar->maximum()));

    emit zoomChanged(m_zoom);
    viewport()->update();
}

/*!
 * 光标矩形（**文档坐标**）。
 *
 * `QTextEdit::cursorRect()` 返回的是"文档坐标 - 滚动量"（实测：
 * 文档 y=130 的光标，竖滚动 200 之后返回 -70，正好差一个滚动量；
 * 横滚动 300 时 x 从 0 变成 -300，同一个道理）。
 * 本控件自己的坐标变换是"文档坐标 × 缩放 + 纸面原点"，纸面原点里**已经**含了
 * `-滚动量`，所以照着 cursorRect() 画就会再减一次 —— 滚动之后光标直接跑到纸外面，
 * 屏幕上一个光标都看不到。这里把它加回去，恢复成纯文档坐标，
 * 之后所有地方（画光标、按视图像素滚动）都只有这一套口径。
 */
QRect TextEditor::caretRectInDocument() const
{
    const QRect r = cursorRect();
    if (r.isEmpty())
        return r;
    return r.translated(horizontalScrollBar()->value(), verticalScrollBar()->value());
}

/*!
 * 把光标滚进可视区（**按视图像素**算）。
 *
 * Qt 自己那套自动滚动（QWidgetTextControlPrivate::ensureCursorVisible）把
 * "文档像素"和"视口像素"当成同一个单位：它拿光标在文档里的 y 跟滚动条值、
 * 视口高度直接比。缩放之后这两者差一个倍数 —— 实测放大到 200% 时，
 * 光标在文档里的 y 只要小于视口高度，Qt 就认定"看得见"，一个字都不滚，
 * 而屏幕上光标早就跑到纸下面去了（打字打到看不见）。
 */
void TextEditor::ensureCaretVisible()
{
    const QRect caret = caretRectInDocument(); // 文档坐标
    if (caret.isEmpty())
        return;

    const double bodyTop = bodyOriginPx().y();
    const double top = (bodyTop + caret.top()) * m_zoom;
    const double bottom = (bodyTop + caret.bottom()) * m_zoom;

    QScrollBar *bar = verticalScrollBar();
    const double value = bar->value();
    const double height = viewport()->height();

    if (top < value)
        bar->setValue(qBound(bar->minimum(), int(std::floor(top)), bar->maximum()));
    else if (bottom > value + height)
        bar->setValue(qBound(bar->minimum(), int(std::ceil(bottom - height)), bar->maximum()));
}

void TextEditor::keyPressEvent(QKeyEvent *event)
{
    QTextEdit::keyPressEvent(event);
    /*!
     * 基类走完之后再补一次：光标移动 / 输入 / 翻页时 Qt 已经按它那套坐标
     * 滚过一次了（缩放后是错的），谁最后设滚动值谁说了算。
     */
    ensureCaretVisible();
}

/*!
 * 滚轮三件事：Ctrl=缩放、Shift=横向平移、其余=上下滚。
 *
 * 自己算"这一档是多少**视图像素**"：滚动值在本控件里就是视图像素
 * （见 paperOriginInViewport），而 Qt 那一套按"行距个像素"来，
 * 放大到 200% 之后一档只滚半行。乘上缩放，一档永远是同样几行。
 */
void TextEditor::wheelEvent(QWheelEvent *event)
{
    const QPoint pixel = event->pixelDelta();
    const QPoint angle = event->angleDelta();

    if (event->modifiers() & Qt::ControlModifier) {
        // 一档 ±10%，锚点是鼠标位置 —— 和状态栏右下角的滑块是同一个入口
        const int dy = !pixel.isNull() ? pixel.y() : angle.y();
        if (dy == 0) {
            event->ignore();
            return;
        }
        zoomBy(dy > 0 ? kZoomStep : -kZoomStep, event->position());
        event->accept();
        return;
    }

    double delta = 0.0;
    if (!pixel.isNull())
        delta = pixel.y() * m_zoom; // 触控板：给的就是像素增量
    else if (angle.y() != 0)
        delta = angle.y() / 120.0 * QApplication::wheelScrollLines() * fontMetrics().lineSpacing()
                * m_zoom;

    if (delta != 0.0) {
        /*!
         * Shift+滚轮 = 横向平移。
         *
         * 纸张比编辑区宽时才动得了（放大之后很常见），
         * 平移量就是横向滚动值 —— 绕开横向滚动条（它是关着的，见构造函数），
         * 但走的是同一条路：纸面原点里本来就含 `-横向滚动值`，
         * 所以正文、光标、效果层、鼠标命中会一起跟着走，不需要第二套坐标。
         */
        QScrollBar *bar = (event->modifiers() & Qt::ShiftModifier) ? horizontalScrollBar()
                                                                  : verticalScrollBar();
        if (bar->maximum() > bar->minimum()) {
            bar->setValue(qBound(bar->minimum(), int(std::lround(bar->value() - delta)),
                                 bar->maximum()));
            event->accept();
            return;
        }
    }

    // 没得滚（或没有增量 / 横向滚轮）：交给 Qt 那套
    QTextEdit::wheelEvent(event);
}

void TextEditor::setEffectOptions(const EffectRenderOptions &options)
{
    m_options = options;
    viewport()->update();
}

void TextEditor::setEffectsVisible(bool visible)
{
    if (m_effectsVisible == visible)
        return;
    m_effectsVisible = visible;
    viewport()->update();
    emit effectsVisibilityChanged(visible);
}

void TextEditor::relayout()
{
    document()->markContentsDirty(0, document()->characterCount());
    updateScrollRange();
    viewport()->update();
}

/*!
 * 效果层和正文是同一次绘制（见 paintEvent），所以这里只要让 viewport 重画。
 *
 * 名字保留是为了不动调用方（滚动、改页面设置那几处），
 * 但"几何"这件事已经不存在了 —— 以前这里要给那个盖在 viewport 上的
 * 透明子控件摆位置并 raise()。
 */
void TextEditor::updateOverlayGeometry()
{
    viewport()->update();
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
    /*!
     * 注意：这个函数拿到的是 **viewport 的绘制事件**。
     *
     * QAbstractScrollArea 把 viewport 的事件转给控件自己（viewportEvent），
     * 所以 event->rect() 是 viewport 坐标、也只覆盖 viewport ——
     * 实测事件矩形正好等于 viewport 的尺寸（959x792），而控件是 1110 宽，
     * 纸张左边那条居中留白不在任何一次绘制事件里。
     * 它靠控件自己的背景画刷刷成桌面色（见构造函数里的调色板 + autoFillBackground），
     * 想在这里补刷是刷不到的（试过：控件自己的 painter 画的像素不会出现）。
     */
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, false);

    const double paperW = paperViewWidthPx();
    const double paperH = paperViewHeightPx();
    /*!
     * 正文区尺寸分两套口径，**不能混**：
     *   - bodyDocSize 是文档坐标（= 100% 的像素数），排版、裁剪、效果层都用它；
     *   - bodyViewSize 是屏幕上的大小，画虚框提示、算页位置用它。
     * 两者只差一个缩放倍数，混用就是"纸对得上、正文对不上"。
     */
    const QSizeF bodyDocSize = m_pageSetup.bodySizePx();
    const QSizeF bodyViewSize = bodyDocSize * m_zoom;
    const QPointF paperOrigin = paperOriginInViewport();
    const QPointF docOrigin = documentOriginInViewport();
    const int scrollY = verticalScrollBar()->value();

    // 1. 桌面底色
    painter.fillRect(event->rect(), kDeskColor);

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

    /*!
     * 会被"变形后的字形 / 手写笔迹"整格替换掉的位置：把原字**裁掉**。
     *
     * 这是"变形后的文字有个白底、把选框挡住"的正解。之前是在效果层里往
     * 纸面上刷一块底色去盖原字 —— 可效果层画在正文（含选区高亮）之上，
     * 那块底色把选区也一起盖掉了，只在边上留 1 像素蓝边。
     * 换成"正文这一格不画"之后，隐藏原字和显示选区互不干扰，
     * 也彻底不需要任何底色；打印和导出（tripaRenderToDevice）用同一份计划，
     * 屏幕上什么样纸上就什么样。
     *
     * 为什么用裁剪路径而不是 QTextLayout::setFormats（给这一格设透明前景）：
     * setFormats 会 invalidate 布局 —— 而布局一旦失效，任何**之前**拿到的
     * QTextLine / blockBoundingRect 都成了悬垂引用，别处再调 line.y() 就直接
     * 段错误（实测就是这么崩的，崩在 QTextLine::y() 里）。
     * 裁剪是纯绘制期的，不碰布局、不发信号、不进撤销栈，谁也影响不到。
     */
    QVector<QPair<int, int>> hiddenRanges;
    QPainterPath hiddenHoles;   // 文档坐标：要被裁掉的那些格子
    QAbstractTextDocumentLayout::PaintContext selectionOnly;
    bool hasSelectionPass = false;
    if (m_effectsVisible && m_options.anyLayer()) {
        const QVector<EffectDrawItem> items = planEffects(document(), m_options, &hiddenRanges);
        if (!hiddenRanges.isEmpty()) {
            hiddenHoles.setFillRule(Qt::OddEvenFill);
            /*!
             * 只挖 item.hidden 的那些格子。
             *
             * 这里以前是"只要有一个字被替换，就把本轮**所有**效果字符的格子
             * 全挖掉"——手写层开了替换、扭曲层还在叠加模式时，
             * 扭曲那些字的原字也被顺手裁掉了，屏幕上看着就是"叠加上去的字
             * 底下空空如也，没法对照"。该不该隐藏由 planEffects 一处说了算。
             */
            for (const EffectDrawItem &item : items) {
                if (item.hidden)
                    hiddenHoles.addRect(item.charRect);
            }
        }
    }

    /*!
     * 选区背景必须**单独画一遍、且不带上面那套裁剪**。
     *
     * 裁剪是按"格子矩形"挖的，而选区高亮画的正是同一批格子 ——
     * 一起交给 documentLayout()->draw() 的话，高亮会连同原字一起被挖掉：
     * 选中变形后的文字时，变形层是深蓝墨、底下的高亮也没了，
     * 看着就是"选区被盖住了"（实测选区色像素从 8400 掉到 1084）。
     *
     * 所以分两步走：
     *   1. 不裁剪，先把选区背景铺满（前景设成透明，免得把正文字形也画一遍）；
     *   2. 再带上"挖洞"裁剪，画正文 —— 此时原字被挖掉，铺好的高亮留在下面。
     */
    if (!hiddenHoles.isEmpty()) {
        QAbstractTextDocumentLayout::PaintContext selOnly = context;
        for (QAbstractTextDocumentLayout::Selection &s : selOnly.selections) {
            // 前景透明：这一遍只要背景色，正文字形由下一遍（带裁剪）负责
            s.format.setForeground(QColor(0, 0, 0, 0));
        }
        selectionOnly = selOnly;
        hasSelectionPass = !selectionOnly.selections.isEmpty();
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
                                  bodyViewSize);
            painter.setPen(QPen(QColor(70, 130, 200), 1, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(bodyRect);
        }

        /*!
         * 正文：原点 = documentOriginInViewport()，再挪到第 page 页的正文区，
         * 最后乘上缩放 —— **正文、效果层、光标三者必须共用这一个变换**
         * （documentToViewport 里就是它），否则第 2 页起的光标会一行行错开，
         * 放大之后字和光标也会各走各的。
         *
         * 缩放之后坐标系就回到**文档坐标**了，所以下面所有矩形
         * （裁剪、挖洞、页范围的奇偶路径）一律用 bodyDocSize，不是屏幕尺寸。
         */
        painter.translate(docOrigin + QPointF(0.0, page * paperH));
        painter.scale(m_zoom, m_zoom);
        painter.setClipRect(QRectF(QPointF(0.0, 0.0), bodyDocSize));

        // 第一遍：选区背景（不带"挖洞"裁剪，否则高亮会被一起挖掉）
        if (hasSelectionPass)
            document()->documentLayout()->draw(&painter, selectionOnly);

        if (!hiddenHoles.isEmpty()) {
            /*!
             * 第二遍：裁掉"会被变形字形替换掉的格子"。
             *
             * 这是个"矩形 + 一堆洞"的奇偶路径：偶数次穿越为实、奇数次为空，
             * 所以正文矩形和洞之间那块就被挖掉了。
             * 注意每页都要用**这一页的正文矩形**重算：文档坐标是全局的、
             * 跨页累加的，只按第一页的正文矩形挖的话，第 2 页起会被整页裁掉。
             */
            QPainterPath pageClip;
            pageClip.setFillRule(Qt::OddEvenFill);
            pageClip.addRect(QRectF(QPointF(0.0, 0.0), bodyDocSize));
            pageClip.addPath(hiddenHoles);
            painter.setClipPath(pageClip, Qt::IntersectClip);
        }
        document()->documentLayout()->draw(&painter, context);

        /*!
         * 手写 / 扭曲层：**和正文画在同一次绘制里**，就在正文之后、光标之前。
         *
         * 它以前是画在一个盖在 viewport 上的透明子控件（EffectsOverlay）里的。
         * 那样有两个代价，第二个是致命的：
         *   1. 两套重绘时机（滚动、改选项都要手动通知）容易不同步；
         *   2. "透明子控件"在 QWidget 里的合成语义很脆 ——
         *      WA_TranslucentBackground 会让 Qt 把子控件的区域从父控件的
         *      不透明区域里挖掉，于是父控件那块干脆不画，子控件的内容又没真正
         *      合成上去。屏幕上看是好的，可 QWidget::grab() / 打印 / 导出
         *      全都只剩一个洞（实测：覆盖层单独抓图有 2834 个像素，
         *      合成抓图里一个新墨点都没有）。
         * 画在一起就没这回事：就是一次普通绘制，屏幕、抓图、PDF、打印机
         * 走的是同一段代码、同一份坐标。
         */
        if (m_effectsVisible && m_options.anyLayer()) {
            /*!
             * 关键：**先把上面那套"挖洞"的裁剪去掉**再画效果层。
             *
             * 洞的位置正是要被变形字形替换掉的那些格子，也就是效果层要画的
             * 地方 —— 带着裁剪画效果层，等于把新字形也一起裁掉了：
             * 原字确实没了，新字也没了，屏幕上就只剩一片空白
             * （实测"消失的像素 1154 个、新墨 0 个"就是这个）。
             */
            painter.setClipping(false);
            painter.setClipRect(QRectF(QPointF(0.0, 0.0), bodyDocSize));
            EffectRenderOptions local = m_options;
            local.missing.clear();
            renderEffects(&painter, document(), local);
        }
        painter.restore();
    }

    // 3. 光标：自己画，坐标只认 documentToViewport()
    drawCaret(&painter, docOrigin);
}

/*!
 * 画光标竖条。
 *
 * 位置一律走 `caretRectInDocument()`（**文档坐标**，滚动量已经加回来了），
 * 再加一次 `documentOriginInViewport()` —— 和正文、效果层完全同一个原点；
 * 缩放也走同一个倍数（光标跟着字一起变大，不然放大之后光标细得看不见）。
 *
 * 闪烁：用 `QApplication::cursorFlashTime()` 驱动 QTextEdit 自带的 blink 定时器，
 * 相位由 `cursorRect()` 是否为空来判断（Qt 在"不该显示"时返回空矩形）。
 * 这样跟系统的闪烁设置保持一致，不用自己维护相位。
 */
void TextEditor::drawCaret(QPainter *painter, const QPointF &docOrigin)
{
    if (!hasFocus() && !viewport()->hasFocus())
        return;                       // 没焦点不画光标，跟系统惯例一致

    const QRect caret = caretRectInDocument();   // 文档坐标
    if (caret.isEmpty())
        return;                         // 闪烁的暗相位

    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setBrush(palette().color(QPalette::Text));
    const QRectF r(QPointF(docOrigin.x() + caret.x() * m_zoom,
                           docOrigin.y() + caret.y() * m_zoom),
                   QSizeF(qMax(1.0, caret.width() * m_zoom), caret.height() * m_zoom));
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
    // 滚动之后纸面、正文、光标的原点全部跟着变，整块重画
    viewport()->update();
}
