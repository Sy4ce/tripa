#include "texteditor.h"

#include "effectsrenderer.h"
#include "richdocument.h"
#include "tripalog.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QFocusEvent>
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
#include <QTimer>
#include <QWheelEvent>

#include <cmath>
#include <numeric>

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

/*!
 * 绘制里的日志节流：`paintEvent` 一秒能跑几百次，**每次绘制只打一行**，
 * 而且只在"页范围或几何真的变了"时打。
 *
 * 这一条对排查"后几页又出现同样的字"是关键的：只要把每页的
 * 纸面矩形 / 正文窗口 / 文档尺寸一起记下来，一眼就能看出
 * "第 2 页那个窗口里到底装的是文档的哪一段"。
 */
struct PaintLogState
{
    quint64 serial = 0;
    int firstPage = -1;
    int lastPage = -1;
    qint64 lastDocHeight = -1;
    int lastScroll = -1;
    int lastZoomPercent = -1;
};
PaintLogState g_paintLog;

} // namespace

TextEditor::TextEditor(QWidget *parent)
    : QTextEdit(parent)
    , m_rich(new RichDocument(this))
{
    /*!
     * 三层就位：富文本层先建好，渲染层的**分页排版**装到它的文档上，
     * 最后控件挂到这个文档上。顺序不能反 —— QTextEdit 一旦先拿到别的文档，
     * 就会按"连续排版"那套算一遍几何，白算一次，而且很容易忘了再装回来。
     */
    setDocument(m_rich->text());
    /*!
     * `QTextEdit::setDocument()` 会不会顺手换掉文档布局，是 Qt 的实现细节。
     * `installPaginatingLayout()` 是幂等的（文档上已经是分页排版就原样返回），
     * 所以这里再确认一次：不确认的话，某天 Qt 换了行为，
     * 症状就是"分页又回到一页复制一页"，而且极难查。
     */
    m_layout = installPaginatingLayout(m_rich->text());
    m_layout->setTitle(tr("未命名"));

    /*!
     * 计算层就位（effectplanner.h）。它只依赖上面两层，不看任何控件状态 ——
     * 所以自检里可以抛开窗口单独驱动它。
     *
     * 两个信号的分工：
     *   - `planChanged`：又算好了一批格子 -> 重画（一次一片，不会被信号淹）；
     *   - `workPending`：有活等着算 -> 转给主窗口的 JobRunner 分片做，
     *     进度显示在状态栏上。控件自己**不**在这里算任何几何。
     */
    m_planner = new EffectPlanner(m_rich->text(), m_layout, this);
    connect(m_planner, &EffectPlanner::planChanged, this, [this] {
        viewport()->update();
    });
    connect(m_planner, &EffectPlanner::workPending, this, [this] {
        emit effectWorkNeeded();
    });

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
    /*!
     * 竖滚动条**常驻**（`ScrollBarAlwaysOn`），不许 Qt 按需显示/隐藏。
     *
     * 这是踩出来的活锁，症状是"打字/缩放时界面卡死、CPU 满"：
     * 竖滚动条的显隐会改变 viewport 宽度（14 像素），而 viewport 一变尺寸
     * 就会重排；重排之后 Qt（`_q_adjustScrollbars`）按**文档尺寸**重算我们
     * 精心设好的滚动范围，于是"该不该显示滚动条"又被判成另一个答案 ——
     * 实测宽度在 **1110 <-> 1096** 之间来回跳，一秒 130 多次 resize
     * （日志里 `TextEditor 一秒里 resize 了 133 次` 就是它），
     * 每个来回还各自跑一遍排版 + 绘制。
     *
     * 常驻之后这条反馈通路就断了。顺带还解决两件事：
     *   - `paperPadPx()` 里减掉的那个"滚动条预留宽度"终于**真的**恒等于
     *     实际占位，纸张居中不会再因为滚动条冒出来而跳一下；
     *   - 光标定位、滚动范围不再受"当前有没有滚动条"影响。
     * 代价只是在内容很短时右边也有一条滚动条 —— 排版软件里这很正常。
     */
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

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

    /*!
     * 版面相关的回调**一律排在事件循环的下一轮**（见 `scheduleLayoutUpdate()`），
     * 不能在这里直接连到 `updateScrollRange()`。
     *
     * 原因（这是实测出来的活锁，不是假想）：`updateScrollRange()` 里会
     * `setViewportMargins()` 改 viewport 尺寸，而 viewport 变尺寸又会触发
     * 重排、重排又发 `documentSizeChanged` —— 直接相连就成了自激环：
     * 实测一秒里被调了 **3704 次**、界面完全卡死（日志里那条 WARN 就是它）。
     * 排到下一轮 + 合并重复请求之后，每轮最多算一次，环自然断开。
     */
    connect(m_layout, &PaginatingLayout::layoutRebuilt, this, &TextEditor::scheduleLayoutUpdate);
    connect(m_layout, &QAbstractTextDocumentLayout::documentSizeChanged, this,
            &TextEditor::scheduleLayoutUpdate);
    connect(m_layout, &PaginatingLayout::pageCountChanged, this, &TextEditor::pageCountChanged);
    //! 文档级格式变了（默认字体、换行规则）：断行结果会变，整篇重新断行
    connect(m_rich, &RichDocument::globalFormatChanged, this, [this] {
        m_layout->invalidateAll();
        if (m_planner)
            m_planner->invalidateAll(); // 字宽/行高变了：格子和基线全变
        scheduleLayoutUpdate();
        viewport()->update();
    });
    // 内容变了要重算滚动范围（页数变了），画面也要重画
    connect(m_rich, &RichDocument::contentChanged, this, [this] {
        scheduleLayoutUpdate();
        viewport()->update();
    });

    /*!
     * 光标闪烁自己维护相位。
     *
     * 以前是借 QTextEdit 自己的闪烁定时器（它在暗相位让 `cursorRect()` 返回空矩形），
     * 但光标矩形现在由渲染层算（`PaginatingLayout::caretRect()`）——
     * 它只回答"光标在文档坐标的哪儿"，不回答"这一相该不该显示"。
     * 相位是纯视图的事，自己拿着反而更简单，也更好测
     * （`QApplication::setCursorFlashTime(0)` = 不闪，自检就是这么量的）。
     */
    m_caretTimer = new QTimer(this);
    connect(m_caretTimer, &QTimer::timeout, this, [this] {
        m_caretVisible = !m_caretVisible;
        viewport()->update();
    });
    connect(this, &QTextEdit::cursorPositionChanged, this, [this] {
        restartCaretBlink();
        viewport()->update();
    });
    restartCaretBlink();

    setPageSetup(m_pageSetup);
    setEffectsVisible(false);
}

//! 光标闪烁：亮一相、暗一相，节奏跟系统设置一致
void TextEditor::restartCaretBlink()
{
    m_caretVisible = true;
    const int flash = QApplication::cursorFlashTime();
    if (flash <= 0) {
        m_caretTimer->stop(); // 系统里关了闪烁（自检也会这么设）：一直亮着
        return;
    }
    m_caretTimer->start(qMax(100, flash / 2));
}

void TextEditor::focusInEvent(QFocusEvent *event)
{
    QTextEdit::focusInEvent(event);
    restartCaretBlink();
    viewport()->update();
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
 * 文档坐标 -> viewport 坐标。**正文、光标、选区、效果层、
 * 鼠标命中五者必须共用这一个变换。**
 *
 * 文档坐标就是**纸面坐标**（第 p 页的纸在文档坐标里是
 * `[p*纸高, (p+1)*纸高) × [0, 纸宽]`），纸张居中量已经由
 * setViewportMargins 消化掉了 —— viewport 原点的 x 就是纸张左边缘。
 * 所以这里只剩两件事：
 *
 *   1. 缩放（比例尺是**视图属性**，改它不动文档里任何一个数字）；
 *   2. 滚动量（paperOriginInViewport() 里那个负号）。
 *
 * 于是整篇只有一个仿射变换：**屏幕 = 文档 × 缩放 - 滚动量**。
 * 分页完全是文档坐标里的事（页与页之间隔着上下边距，见 paginatinglayout.h），
 * 所以"第几页"根本不需要出现在这里 —— 这正是把分页从视图里赶出去的好处：
 * 命中测试就是这个变换的逆变换，不可能出现"字画在一处、点哪儿都不对"。
 */
QTransform TextEditor::documentToViewport() const
{
    const QPointF paper = paperOriginInViewport();
    /*!
     * 缩放和原点合成**一个**变换交给所有调用方（6 个参数直接写出来，
     * 比 translate()/scale() 连写更不容易搞错顺序：
     * m11 = m22 = 缩放，dx/dy = 纸面原点）。
     * 逆变换就是"视口坐标 -> 文档坐标"，命中测试也走它。
     */
    return QTransform(m_zoom, 0.0, 0.0, m_zoom, paper.x(), paper.y());
}

/*!
 * \brief 文档坐标的原点（**第一页纸的左上角**）在 viewport 里的位置。
 *
 * 口径提醒：文档坐标的原点是**纸的左上角**，不是正文的左上角 ——
 * 正文区左上角 = 这个点 + 页边距 × 缩放（用的是 bodyOriginPx()）。
 *
 * 这个点同时就是本控件唯一那个坐标变换的原点
 * （documentToViewport = 先平移到它、再乘缩放），所以
 * `documentOriginInViewport() + 文档坐标 × 缩放` 永远是屏幕位置 ——
 * 自检里很多处都是这么算的，把原点定在纸角，那些公式就不用改。
 */
QPointF TextEditor::documentOriginInViewport() const
{
    return paperOriginInViewport();
}

//! 第 \a page 页正文区左上角在**文档坐标**里的位置
QPointF TextEditor::pageTopLeft(int page) const
{
    const QPointF body = bodyOriginPx();
    return QPointF(body.x(), page * paperHeightPx() + body.y());
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
void TextEditor::updateScrollRange(const QString &source)
{
    /*!
     * 重入诊断：这个函数是从 `documentSizeChanged` / `contentsChanged` /
     * `resizeEvent` 里回调进来的，而它自己又会去 `setViewportMargins`
     * （= 改 viewport 尺寸 = 触发 resizeEvent / 重排）。
     * 一旦环闭不上，就是"一秒几百次"的活锁 —— 从外面看是**界面卡死**，
     * 用户报告里那句"打字/缩放时容易卡死或崩"就有它一份。
     *
     * 这里只报告不拦截（拦截会把真正的调用漏掉），日志里连续冒出同一行就说明中了。
     */
    static int callDepth = 0;
    static int callCount = 0;
    static qint64 lastReportMs = -1;
    static QVector<int> history; //!< 前几秒的调用次数（用来判断"突然暴涨"）
    ++callDepth;
    ++callCount;
    if (m_rangeCallSources.size() < 12)
        m_rangeCallSources.append(source);
    struct Guard
    {
        int *depth;
        explicit Guard(int *d)
            : depth(d)
        {
        }
        ~Guard() { --*depth; }
    } guard(&callDepth);

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (lastReportMs < 0 || nowMs - lastReportMs > 1000) {
        /*!
         * 判"活锁"要看两件事，只看绝对值会误报：
         *
         *   - **绝对上限 500 次/秒**：正常操作一秒也就几十次
         *     （打字、缩放、拉窗口都会触发重排 + 滚动范围重算，
         *      自检 soak 组里正常峰值约 50 次/秒），而真正卡死的活锁是
         *     每秒几百到几千次；
         *   - **相对暴涨**：比前 3 秒的均值高 10 倍以上也要报 ——
         *     那种"没有卡死但一直在自喂"的软环，绝对值可能只有一两百，
         *     但它会让界面明显发顿，正是用户嘴里的"用一会儿就不对劲"。
         */
        /*!
         * 判据只有一条：**绝对上限 500 次/秒**。
         *
         * 试过再加一条"比前 3 秒均值高 10 倍就报"，结果全是误报 ——
         * 快速连按 Ctrl+滚轮、拖状态栏滑块本来就是"一秒钟几十次"的爆发型操作
         * （实测自检 soak 组里 44 次/秒就被报了）。而真正的活锁是
         * **每秒几百到几千次**（踩到的那次是 3704 次/秒），
         * 跟正常操作的量级差着两个数量级，绝对值就够了。
         */
        if (callCount > 500) {
            TRIPA_WARN("layout",
                       QStringLiteral("updateScrollRange 一秒里被调了 %1 次（递归深度 %2）—— "
                                      "多半是「改边距 -> viewport 变尺寸 -> 重排 -> 再改边距」"
                                      "绕成了环。视口=%3x%4 左边距=%5 滚动范围=%6..%7 "
                                      "文档高=%8 缩放=%9 来源=[%10]")
                           .arg(callCount)
                           .arg(callDepth)
                           .arg(viewport()->width())
                           .arg(viewport()->height())
                           .arg(viewportMargins().left())
                           .arg(verticalScrollBar()->minimum())
                           .arg(verticalScrollBar()->maximum())
                           .arg(m_layout->documentSize().height(), 0, 'f', 1)
                           .arg(m_zoom, 0, 'f', 3)
                           .arg(m_rangeCallSources.join(QLatin1Char(','))));
        }
        history.append(callCount);
        while (history.size() > 3)
            history.removeFirst();
        callCount = 0;
        m_rangeCallSources.clear();
        lastReportMs = nowMs;
    }

    /*!
     * 滚动范围 = **视图下边界** - 视口高度。
     *
     * 视图下边界的定义在渲染层（`PaginatingLayout::contentBottomPx()`）：
     * **最后一页的下边界 + 一页高**。用户要的就是这个 ——
     * 滚到底时能把最后一页的下边界顶到视窗最上面，
     * 底下那一截是桌面（没有内容），一眼能看出"写到哪儿了"。
     *
     * 文档坐标 × 缩放 = 视图像素，而滚动值本身就是视图像素
     * （paperOriginInViewport() 里那个负号），所以换算只有一次乘法。
     */
    const double contentBottom = m_layout->contentBottomPx() * m_zoom;

    /*!
     * 滚动范围**只在真的变了**时才改，而且改之前记一条日志。
     * 这个函数是从 `documentSizeChanged` / `contentsChanged` 里回调进来的，
     * 一旦它反过来又改了文档尺寸，就会自己喂自己 —— 那种死循环
     * 从外面看就是"打字/缩放时随机卡死或崩"，日志里会留下一串
     * 时间戳挨得极近的调用，一眼能认出来。
     */
    const int newMaximum = qMax(0, int(std::ceil(contentBottom)) - viewport()->height());
    verticalScrollBar()->setPageStep(viewport()->height());
    if (newMaximum != verticalScrollBar()->maximum()) {
        TRIPA_DEBUG("layout",
                    QStringLiteral("滚动范围 %1 -> %2（页数=%3 视图下边界=%4 缩放=%5 视口高=%6）")
                        .arg(verticalScrollBar()->maximum())
                        .arg(newMaximum)
                        .arg(m_layout->pageCount())
                        .arg(contentBottom, 0, 'f', 1)
                        .arg(m_zoom, 0, 'f', 3)
                        .arg(viewport()->height()));
    }
    verticalScrollBar()->setRange(0, newMaximum);

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
 * 把"版面要重算"排到下一轮。见头文件里的说明：直接相连会绕成
 * "改边距 -> viewport 变尺寸 -> 重排 -> 又发 documentSizeChanged" 的
 * 自激环（实测一秒 3704 次、界面卡死）。
 */
void TextEditor::scheduleLayoutUpdate()
{
    if (m_layoutUpdatePending)
        return;
    m_layoutUpdatePending = true;
    QTimer::singleShot(0, this, [this] {
        m_layoutUpdatePending = false;
        updateScrollRange(QStringLiteral("延迟的版面更新"));
    });
}

/*!
 * 版面几何的交接点：**页面设置 -> 渲染层**。
 *
 * 这里只做一件事：把纸张、页边距、页眉页脚换算成渲染层的输入
 * （RenderMetrics / PageChrome），然后由渲染层重排。
 *
 * 视图**不再往文档里塞纸高**。以前这里有一句
 * `document()->setPageSize(QSizeF(正文宽, 纸高))`，那是两个 bug 的病根：
 * 文档一旦知道自己的纸有多大，分页就变成了"内容连续排下来 + 每页裁一个
 * 正文窗口"——
 *   - 每张纸的窗口里装的都是**文档开头那一段**（第二页重复第一页的字）；
 *   - 越过正文区下边界的字不会跑到下一页，它们落在下边距里被裁掉
 *     （写着写着出下边界）。
 * 现在分页全在渲染层（paginatinglayout.h），纸的尺寸不再进文档。
 *
 * 断行宽度仍然要落在文档上（`setLineWrapColumnOrWidth`）：段落对话框、
 * .tripa 的兼容字段都会读 `document()->pageSize().width()`，而这个值
 * 必须**等于渲染层断行用的正文宽度** —— 两处不一致就是
 * "屏幕上断行位置和纸上不一样"。
 */
void TextEditor::applyLayoutMetrics()
{
    const RenderMetrics metrics = RenderMetrics::fromPageSetup(m_pageSetup);
    TRIPA_SCOPE(tripalog::Level::Trace, "layout", QStringLiteral("applyLayoutMetrics"));

    document()->setDocumentMargin(0);
    setLineWrapMode(QTextEdit::FixedPixelWidth);
    setLineWrapColumnOrWidth(int(std::lround(metrics.bodyWidthPx())));

    m_layout->setMetrics(metrics); // 纸张 / 边距变了 -> 整篇重新断行 + 分页
    m_layout->setChrome(PageChrome::fromPageSetup(m_pageSetup));

    TRIPA_DEBUG("layout",
                QStringLiteral("应用版面：正文 %1x%2，纸 %3x%4，缩放=%5，页数=%6")
                    .arg(metrics.bodyWidthPx(), 0, 'f', 1)
                    .arg(metrics.bodyHeightPx(), 0, 'f', 1)
                    .arg(metrics.paperWidthPx, 0, 'f', 1)
                    .arg(metrics.paperHeightPx, 0, 'f', 1)
                    .arg(m_zoom, 0, 'f', 3)
                    .arg(m_layout->pageCount()));

    updateScrollRange(QStringLiteral("applyLayoutMetrics"));
    viewport()->update();
}

// ---------------------------------------------------------------- 对外接口

void TextEditor::setPageSetup(const PageSetup &setup)
{
    m_pageSetup = setup;
    applyLayoutMetrics();
}

/*!
 * \brief 页眉页脚（默认都不显示）。
 *
 * 只改渲染层的装饰，**不动任何行的位置** —— 页眉画在上边距里、
 * 页脚画在下边距里，那是纸上的空白，正文窗口一个像素也不会被占。
 * 于是"开不开页眉"永远不会让文字重排（这一点很重要：
 * 排版一变，用户看到的断行位置就变了，那才是真的烦）。
 */
void TextEditor::setPageChrome(const PageChrome &chrome)
{
    m_layout->setChrome(chrome);
    viewport()->update();
}

//! 当前一共几页（**按需分页**的结果：没写到的页根本不存在）
int TextEditor::pageCount() const
{
    return m_layout ? m_layout->pageCount() : 1;
}

//! 视图下边界（文档坐标）= 最后一页下边界 + 一页高
//! ——滚到底时能把最后一页的下边界顶到视窗最上面。
double TextEditor::contentBottomPx() const
{
    return m_layout ? m_layout->contentBottomPx() : 0.0;
}

//! 字边距（字距，百分比）：走富文本层，改完整篇重新断行（每个字的宽度都变了）
void TextEditor::setParagraphCharSpacing(double percent)
{
    QTextCursor cursor = textCursor();
    m_rich->setCharSpacingPercent(&cursor, percent);
    m_layout->invalidateAll();
    scheduleLayoutUpdate();
    viewport()->update();
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
    const int oldZoomPercent = int(std::lround(m_zoom * 100.0));

    m_zoom = zoom;
    applyLayoutMetrics(); // 重算滚动范围 + 纸张居中量，顺带重绘

    QScrollBar *bar = verticalScrollBar();
    /*!
     * 缩放后把锚点那一格摆回原处：视图像素 = 文档坐标 × 缩放 - 滚动值，
     * 所以"它还在锚点"就是 `docAnchor.y() * 新缩放 - 滚动值 = 锚点 y`。
     *
     * 旧写法里还有一个 `bodyOriginPx().y() * m_zoom` —— 那是"文档原点是
     * 正文左上角"时代的产物。现在文档原点是**纸的左上角**，
     * 这一项必须去掉，否则每缩一次纸就整体往下跑一个上边距。
     */
    const double wanted = docAnchor.y() * m_zoom - viewportAnchor.y();
    bar->setValue(qBound(bar->minimum(), int(std::lround(wanted)), bar->maximum()));

    emit zoomChanged(m_zoom);
    viewport()->update();

    TRIPA_INFO("zoom",
               QStringLiteral("缩放 %1% -> %2%（纸 %3x%4 视图像素，锚点 %5,%6，滚动 %7/%8）")
                   .arg(oldZoomPercent)
                   .arg(int(std::lround(m_zoom * 100.0)))
                   .arg(paperViewWidthPx(), 0, 'f', 1)
                   .arg(paperViewHeightPx(), 0, 'f', 1)
                   .arg(viewportAnchor.x(), 0, 'f', 1)
                   .arg(viewportAnchor.y(), 0, 'f', 1)
                   .arg(verticalScrollBar()->value())
                   .arg(verticalScrollBar()->maximum()));
}

/*!
 * 光标矩形（**文档坐标**）。
 *
 * 几何一律来自渲染层（`PaginatingLayout::caretRect()`）：它算的是
 * "这一行落在哪一页的哪一点"，所以分页之后光标不可能跑到纸外面 ——
 * 光标和正文用的是同一份行位置。
 *
 * 以前这里是把 `QTextEdit::cursorRect()` 的滚动量加回去（那个接口返回的是
 * "文档坐标 - 滚动量"，而本控件的纸面原点里已经含了 `-滚动量`，
 * 照着画就会再减一次，滚动之后屏幕上一个光标都看不到）。
 * 那套绕圈子的换算现在没有了。
 *
 * 闪烁相位**不在**这里体现（那是视图的事，见 m_caretVisible）。
 */
QRectF TextEditor::caretRectInDocument() const
{
    return m_layout->caretRect(textCursor());
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
    const QRectF caret = caretRectInDocument(); // 文档坐标
    if (caret.isEmpty())
        return;

    // 视图像素 = 文档坐标 × 缩放（纸面原点已经在滚动值里了）
    const double top = caret.top() * m_zoom;
    const double bottom = caret.bottom() * m_zoom;

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
    /*!
     * 选项要告诉计算层：显示哪一层、是否替换、幅度、波数、噪声波 —— 这些一变，
     * 算好的几何就不是同一个东西了，该扔的扔掉（颜色 / 笔宽不在此列，见
     * EffectPlanner::setOptions）。
     */
    if (m_planner)
        m_planner->setOptions(options);
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
    /*!
     * "按当前字体重新排版" = 整篇重新断行：字号、字体族、字距一变，
     * 每个字的宽度都变了，缓存下来的断点全部作废。
     */
    m_layout->invalidateAll();
    if (m_planner)
        m_planner->invalidateAll(); // 每个字的位置都可能变了，显示表整篇作废
    updateScrollRange(QStringLiteral("relayout"));
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

/*!
 * 任务正在改文档时把重画压住（见头文件里的说明：不压住就是 0.4 秒变 30 秒）。
 */
void TextEditor::setRepaintsDeferred(bool deferred)
{
    if (m_repaintsDeferred == deferred)
        return;
    m_repaintsDeferred = deferred;
    // 放开之后补一次重画：压住的这段时间画面保持原样，现在一次性画成新的
    if (!m_repaintsDeferred)
        viewport()->update();
}

// ---------------------------------------------------------------- 绘制

/*!
/*!
 * 绘制分四步，**所有坐标都是文档坐标**（纸面坐标），
 * 只在最外面设一次变换（documentToViewport）：
 *
 *   1. 桌面底色（纸外面那一圈）；
 *   2. 逐页画纸：阴影 -> 白纸 -> 边框 -> 页眉页脚（默认不显示）；
 *   3. 正文（含选区、挖洞）和手写 / 扭曲效果，裁剪到**可见页的正文窗口**里；
 *   4. 光标。
 *
 * 为什么现在可以“一次画完”：**分页已经在文档坐标里做掉了** ——
 * 第 p 页的字就落在 `[p*纸高 + 上边距, ...]` 那一段（见 paginatinglayout.h），
 * 所以“第几页”不再需要第二套原点。
 *
 * 以前是每页 `translate` 一个纸高再裁一个正文窗口，而文档内容是**连续**
 * 排下来的 —— 每页窗口里装的都是文档开头那一段，这就是
 * “第二页重复第一页的字”；越过正文区下边界的字也只会被裁掉
 * （“写着写着出下边界”）。两处病根都是“视图自己在做分页”。
 */
void TextEditor::paintEvent(QPaintEvent *event)
{
    /*!
     * 任务正在改文档的时候不重画（见 setRepaintsDeferred 的说明）。
     *
     * 这不是"省一次绘制"，而是断掉一个正反馈：重画 -> 重新规划可见段落
     * -> 要求重算几百个字的几何 -> …… 实测让 0.4 秒的活跑了 30 秒。
     */
    if (m_repaintsDeferred)
        return;

    /*!
     * 注意：这个函数拿到的是 **viewport 的绘制事件**。
     *
     * QAbstractScrollArea 把 viewport 的事件转给控件自己（viewportEvent），
     * 所以 event->rect() 是 viewport 坐标、也只覆盖 viewport ——
     * 纸张左边那条居中留白不在任何一次绘制事件里。
     * 它靠控件自己的背景画刷刷成桌面色（见构造函数里的调色板 + autoFillBackground），
     * 想在这里补刷是刷不到的（试过：控件自己的 painter 画的像素不会出现）。
     */
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, false);

    const RenderMetrics metrics = m_layout->metrics();
    const double paperViewH = paperViewHeightPx();
    const int scrollY = verticalScrollBar()->value();
    const int pages = m_layout->pageCount();

    // 1. 桌面底色
    painter.fillRect(event->rect(), kDeskColor);

    // 2. 这一轮要画哪几页：严格按屏幕上的纸面位置算（纸面高 = 纸高 × 缩放）
    const double viewTop = event->rect().top() + scrollY;
    const double viewBottom = event->rect().bottom() + scrollY;
    const int firstPage = qBound(0, int(std::floor(viewTop / qMax(1.0, paperViewH))), pages - 1);
    const int lastPage = qBound(0, int(std::floor(viewBottom / qMax(1.0, paperViewH))), pages - 1);

    /*!
     * 绘制日志（见文件顶部 g_paintLog 的说明）：几何一变就打一行，
     * 附带“这一轮要画哪几页、它们的正文窗口落在文档的哪一段”。
     * 排查“后几页又冒出同样的字”时，这几行就是全部证据。
     */
    ++g_paintLog.serial;
    const qint64 docHeight = qint64(m_layout->documentSize().height());
    const int zoomPercent = int(std::lround(m_zoom * 100.0));
    const bool geometryChanged = firstPage != g_paintLog.firstPage
                                 || lastPage != g_paintLog.lastPage
                                 || docHeight != g_paintLog.lastDocHeight
                                 || scrollY != g_paintLog.lastScroll
                                 || zoomPercent != g_paintLog.lastZoomPercent;
    if (geometryChanged || g_paintLog.serial % 120 == 0) {
        g_paintLog.firstPage = firstPage;
        g_paintLog.lastPage = lastPage;
        g_paintLog.lastDocHeight = docHeight;
        g_paintLog.lastScroll = scrollY;
        g_paintLog.lastZoomPercent = zoomPercent;
        TRIPA_DEBUG("paint",
                    QStringLiteral("重绘 #%1 区域=%2,%3 %4x%5 滚动=%6/%7 缩放=%8% 页=%9/%10 "
                                   "页范围=%11..%12 文档=%13x%14 纸=%15x%16")
                        .arg(g_paintLog.serial)
                        .arg(event->rect().x())
                        .arg(event->rect().y())
                        .arg(event->rect().width())
                        .arg(event->rect().height())
                        .arg(scrollY)
                        .arg(verticalScrollBar()->maximum())
                        .arg(zoomPercent)
                        .arg(pages)
                        .arg(m_layout->lastContentPage() + 1)
                        .arg(firstPage)
                        .arg(lastPage)
                        .arg(m_layout->documentSize().width(), 0, 'f', 1)
                        .arg(m_layout->documentSize().height(), 0, 'f', 1)
                        .arg(paperViewWidthPx(), 0, 'f', 1)
                        .arg(paperViewH, 0, 'f', 1));
    }

    /*!
     * **从这里开始全部是文档坐标。**
     * 一个变换管到底：纸、页眉页脚、正文、效果、光标。
     * 多设一个原点就多一个“字画在一处、光标在另一处”的机会。
     */
    const QTransform docToView = documentToViewport();
    painter.setTransform(docToView);

    for (int page = firstPage; page <= lastPage; ++page) {
        const QRectF paper = metrics.paperRect(page);
        /*!
         * 面包屑：崩在绘制里时，dump 上会写着“正在画第几页”，
         * 再配上下面的几何，就能定位到“哪一页、哪一段文档”。
         */
        if (page == firstPage || geometryChanged) {
            const QRectF body = metrics.bodyRect(page);
            tripalog::breadcrumb(QStringLiteral("paintEvent 画第 %1 页（正文窗口 y=%2..%3）")
                                     .arg(page + 1)
                                     .arg(body.top(), 0, 'f', 1)
                                     .arg(body.bottom(), 0, 'f', 1));
        }
        TRIPA_TRACE("paint",
                    QStringLiteral("第 %1 页 纸面=%2,%3 %4x%5 正文窗口=%6..%7（行 %8 条）")
                        .arg(page + 1)
                        .arg(paper.x(), 0, 'f', 1)
                        .arg(paper.y(), 0, 'f', 1)
                        .arg(paper.width(), 0, 'f', 1)
                        .arg(paper.height(), 0, 'f', 1)
                        .arg(metrics.bodyRect(page).top(), 0, 'f', 1)
                        .arg(metrics.bodyRect(page).bottom(), 0, 'f', 1)
                        .arg(m_layout->linesOnPage(page)));

        // 阴影：只在最外侧留一圈，多页之间不重复描边
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 70));
        painter.drawRect(paper.translated(2.0, 3.0));

        painter.setBrush(Qt::white);
        // 笔宽 0 = 1 设备像素：纸张边框不跟着缩放变粗
        painter.setPen(QPen(QColor(0x8a, 0x8d, 0x92), 0));
        painter.drawRect(paper);

        /*!
         * 页眉页脚：**默认都不显示**，在页面设置里可以开（`PageChrome`）。
         * 它们画在纸的上下边距里，占不到正文窗口，所以开不开都不会
         * 让文字重排。老版本在这里无条件写“第 N 页”——
         * 那不是页脚，那是把纸边距当成垃圾桶。
         */
        m_layout->drawChrome(&painter, page, palette());
    }

    QAbstractTextDocumentLayout::PaintContext context;
    context.palette = palette();
    context.palette.setColor(QPalette::Text, QColor(0x1a, 0x1a, 0x1a));
    context.palette.setColor(QPalette::Base, Qt::white);
    /*!
     * cursorPosition 保持 -1：让 Qt 自己别画光标，由下面 drawCaret() 统一负责。
     *
     * 为什么要自己画？“光标该不该显示”（闪烁相位）在 Qt 里是
     * QWidgetTextControl 的私有状态，而本控件整个 paintEvent 都是自己写的，
     * 走到 documentLayout()->draw() 的时候那个状态早就对不上了。
     * 自己画只有一处坐标来源（render 层的行落点），不会再分叉。
     */
    context.cursorPosition = -1;
    // 可见范围（文档坐标）：渲染层用它跳过看不见的段落
    context.clip = docToView.inverted().mapRect(QRectF(event->rect()));
    /*!
     * 选区高亮也必须自己填。
     *
     * QTextEdit 平时是在 QWidgetTextControl::drawContents() 里把当前选区
     * 塞进 PaintContext::selections 再交给文档布局画的；
     * 本控件为了画纸张整个 paintEvent 都是自己写的，根本没走那条路，
     * 于是 selections 一直是空的 —— 表现就是“能选中（键盘、鼠标都正常，
     * 复制也正常）但屏幕上完全看不出选没选”。
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
     * 会被“变形后的字形 / 手写笔迹”整格替换掉的位置：把原字**裁掉**。
     *
     * 之前是在效果层里往纸面上刷一块底色去盖原字——可效果层画在正文
     * （含选区高亮）之上，那块底色把选区也一起盖掉了，只在边上留 1 像素蓝边。
     * 换成“正文这一格不画”之后，隐藏原字和显示选区互不干扰，
     * 也彻底不需要任何底色；打印和导出（tripaRenderToDevice）用同一份计划。
     *
     * 为什么用裁剪路径而不是 QTextLayout::setFormats（给这一格设透明前景）：
     * setFormats 会 invalidate 布局 —— 而布局一旦失效，任何**之前**拿到的
     * QTextLine / blockBoundingRect 都成了悬垂引用（实测会直接段错误）。
     */
    /*!
     * 效果显示表：**只要看得见的那几段**（计算层的按需单位）。
     *
     * 以前这里是 `planEffects(document(), ...)` —— 每帧把全篇过一遍，
     * 而且一次绘制要过**两遍**（这里挖洞一遍、画效果时又一遍）。
     * 稿子越长每帧越慢，20000 字的时候光规划就 150ms。
     *
     * 现在：按段落缓存（effectplanner.h），几何在后台分片算，
     * 这里只读结果 —— 视图一帧的代价只跟屏幕上有几段有关。
     */
    /*!
     * 一帧的**分相耗时**（只在一帧很慢时才打日志，见函数末尾）。
     *
     * 排查"点一下加噪声卡好几秒"这类问题时，光知道"卡"没用：
     * 可能是规划、可能是绘制、可能是几何。这里把每一相的毫秒量下来，
     * 下次再卡就是日志里一行字的事。
     */
    QElapsedTimer phaseClock;
    phaseClock.start();
    qint64 planMs = 0;
    qint64 textMs = 0;
    qint64 effectMs = 0;
    int itemCount = 0;

    const QPair<int, int> visibleBlocks = m_layout->blockRangeOnPages(firstPage, lastPage);
    const bool wantEffects = m_effectsVisible && m_options.anyLayer();
    m_planner->beginFrame();
    if (wantEffects)
        m_planner->requestRange(visibleBlocks.first, visibleBlocks.second);
    planMs = phaseClock.restart();

    QRegion hiddenHoles; // 文档坐标：要被裁掉的那些格子
    QAbstractTextDocumentLayout::PaintContext selectionOnly;
    bool hasSelectionPass = false;
    if (wantEffects) {
        /*!
         * 只挖 `item.hidden` 的那些格子 —— 而且只有**几何已经算好**的格子才是 hidden
         * （见 prepareEffectItem）。几何还没算好的格子正文照原样画着，
         * 不会出现"原字没了、新字还没来"的空窗。
         */
        for (int n = visibleBlocks.first; n <= visibleBlocks.second; ++n) {
            const QVector<EffectDrawItem> &items = m_planner->items(n);
            itemCount += items.size();
            for (const EffectDrawItem &item : items) {
                if (item.hidden)
                    hiddenHoles += item.charRect.toAlignedRect();
            }
        }
    }

    /*!
     * 选区背景必须**单独画一遍、且不带“挖洞”裁剪**。
     *
     * 裁剪是按“格子矩形”挖的，而选区高亮画的正是同一批格子 ——
     * 一起交给 draw() 的话，高亮会连同原字一起被挖掉，
     * 看着就是“选区被盖住了”。所以分两步走：
     *   1. 先把选区背景铺满（前景设成透明，免得把正文字形也画一遍）；
     *   2. 再带上“挖洞”裁剪，画正文 —— 此时原字被挖掉，铺好的高亮留在下面。
     */
    if (!hiddenHoles.isEmpty()) {
        QAbstractTextDocumentLayout::PaintContext selOnly = context;
        for (QAbstractTextDocumentLayout::Selection &s : selOnly.selections)
            s.format.setForeground(QColor(0, 0, 0, 0));
        selectionOnly = selOnly;
        hasSelectionPass = !selectionOnly.selections.isEmpty();
    }

    /*!
     * 3. 正文窗口的裁剪 + 挖洞（**用 QRegion，不用 QPainterPath**）。
     *
     * “可见页的正文窗口”是这一页允许出现文字的唯一区域：
     * 页边距、纸缝（页与页之间那道空白）都在窗口外面。
     * 就算排版算错了，屏幕上也不可能看到字压住下边界 ——
     * 这一层裁剪是“不越界”的最后一道保险。
     *
     * 挖洞不用 QPainterPath 不是口味问题，是实测出来的性能悬崖：
     * 每页几百个格子当洞加进一个奇偶规则的 QPainterPath 里，
     * 光是把它变成裁剪区就要好几秒，而且格子越多越慢
     * （实测一帧 469ms -> 3261ms -> 8176ms）。
     * QRegion 是**按行合并**的：一整行里相邻的格子会并成一个矩形，
     * 几百个洞最后只剩下几十个矩形，裁剪代价回到正常量级。
     */
    QRegion bodyRegion;
    for (int page = firstPage; page <= lastPage; ++page)
        bodyRegion += metrics.bodyRect(page).toAlignedRect();

    painter.save();
    painter.setClipRegion(bodyRegion);

    if (hasSelectionPass)
        m_layout->draw(&painter, selectionOnly);

    if (!hiddenHoles.isEmpty())
        painter.setClipRegion(bodyRegion.subtracted(hiddenHoles));

    m_layout->draw(&painter, context);
    textMs = phaseClock.restart();

    /*!
     * 手写 / 扭曲层：**和正文画在同一次绘制里**，就在正文之后、光标之前。
     *
     * 它以前是画在一个盖在 viewport 上的透明子控件（EffectsOverlay）里的。
     * 那样有两个代价，第二个是致命的：
     *   1. 两套重绘时机（滚动、改选项都要手动通知）容易不同步；
     *   2. “透明子控件”在 QWidget 里的合成语义很脆 ——
     *      WA_TranslucentBackground 会让 Qt 把子控件的区域从父控件的
     *      不透明区域里挖掉，于是 QWidget::grab() / 打印 / 导出
     *      全都只剩一个洞（实测：单独抓图有 2834 个像素，合成抓图里
     *      一个新墨点都没有）。
     * 画在一起就没这回事：就是一次普通绘制，屏幕、抓图、PDF、打印机
     * 走的是同一段代码、同一份坐标。
     */
    if (wantEffects) {
        /*!
         * 关键：**先把“挖洞”的裁剪去掉**再画效果层。
         *
         * 洞的位置正是要被变形字形替换掉的那些格子，也就是效果层要画的
         * 地方 —— 带着裁剪画效果层，等于把新字形也一起裁掉了：
         * 原字确实没了，新字也没了，屏幕上就只剩一片空白。
         * 换成普通的“正文窗口”裁剪：效果层不该画到页边距外面去。
         */
        painter.setClipping(false);
        painter.setClipRegion(bodyRegion);
        EffectRenderOptions local = m_options;
        local.missing.clear();
        /*!
         * **只落笔，不算几何**：每一项的折线 / 填充路径都是计算层
         * （EffectPlanner 的后台任务）事先算好的，这里只是查表 + 画。
         * 还没算好的项 `art.ready` 是假，`drawEffectItem` 直接跳过 ——
         * 那一格的正文没被挖掉，所以屏幕上看到的是原字，不是空白。
         */
        for (int n = visibleBlocks.first; n <= visibleBlocks.second; ++n)
            renderPreparedEffects(&painter, m_planner->items(n), local);
        effectMs = phaseClock.restart();
    }

    painter.restore(); // 正文窗口的裁剪

    // 4. 光标：自己画，坐标和正文完全同一套
    drawCaret(&painter);

    /*!
     * 一帧超过 100ms 就要说清楚"时间花在哪一相"。
     *
     * 阈值 100ms 是有意义的：60fps 的一帧是 16ms，100ms 已经能看出顿挫；
     * 而正常一帧（含效果层）也就几十毫秒，所以这里不会被日常绘制刷屏。
     */
    if (const qint64 total = phaseClock.elapsed() + planMs + textMs + effectMs; total > 100) {
        TRIPA_WARN("paint",
                   QStringLiteral("慢帧 %1ms：规划 %2ms / 正文 %3ms / 效果 %4ms（%5 页，%6 段，%7 个效果格子）")
                       .arg(total)
                       .arg(planMs)
                       .arg(textMs)
                       .arg(effectMs)
                       .arg(lastPage - firstPage + 1)
                       .arg(visibleBlocks.second - visibleBlocks.first + 1)
                       .arg(itemCount));
    }
}

/*!
 * 画光标竖条。
 *
 * 位置一律走 `caretRectInDocument()`（**文档坐标**，几何来自渲染层的行落点），
 * 和正文、效果层完全同一套坐标 —— 分页之后光标不可能跑到纸外面。
 *
 * 闪烁看 `m_caretVisible`（自己的 QTimer 维护相位，见 restartCaretBlink），
 * 但**系统关了闪烁（cursorFlashTime() == 0）就一直亮着**：
 * 可见性最终由系统的闪烁设置说了算，自己维护相位时很容易把
 * “相位”和“设置”搞成两回事（设置改了相位不知道）。所以每次都现问一次。
 *
 * 线宽用“设备像素 ÷ 缩放”换算，所以放大之后光标不会变成一根粗棒。
 */
void TextEditor::drawCaret(QPainter *painter)
{
    if (!hasFocus() && !viewport()->hasFocus())
        return; // 没焦点不画光标，跟系统惯例一致
    // 闪烁的暗相位（系统把闪烁关了就一直亮着）
    if (QApplication::cursorFlashTime() > 0 && !m_caretVisible)
        return;

    const QRectF caret = caretRectInDocument(); // 文档坐标
    if (caret.isEmpty())
        return;

    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setBrush(palette().color(QPalette::Text));
    const double width = qMax(1.0, double(cursorWidth())) / qMax(0.01, m_zoom);
    painter->drawRect(QRectF(caret.x(), caret.y(), width, caret.height()));
    painter->restore();
}

void TextEditor::resizeEvent(QResizeEvent *event)
{
    /*!
     * 记录每一次尺寸变化（含旧尺寸）：排查"一秒几百次 resize"这类活锁时，
     * 必须知道**尺寸在两个值之间来回跳**还是"尺寸没变却一直发事件"。
     */
    static int resizeCount = 0;
    static qint64 lastResizeMs = -1;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (lastResizeMs < 0 || nowMs - lastResizeMs > 1000) {
        /*! 阈值 150：正常拉窗口一秒也就几十次（见下面 updateScrollRange 里的说明） */
        if (resizeCount > 150)
            TRIPA_WARN("layout",
                       QStringLiteral("TextEditor 一秒里 resize 了 %1 次（现在 %2x%3）—— "
                                      "尺寸在两个值之间来回跳，或者尺寸没变却一直发事件")
                           .arg(resizeCount)
                           .arg(width())
                           .arg(height()));
        resizeCount = 0;
        lastResizeMs = nowMs;
    }
    ++resizeCount;
    TRIPA_DEBUG("layout",
                QStringLiteral("resize：%1x%2 -> %3x%4（viewport %5x%6 边距=%7 滚动=%8..%9）")
                    .arg(event->oldSize().width())
                    .arg(event->oldSize().height())
                    .arg(event->size().width())
                    .arg(event->size().height())
                    .arg(viewport()->width())
                    .arg(viewport()->height())
                    .arg(viewportMargins().left())
                    .arg(verticalScrollBar()->minimum())
                    .arg(verticalScrollBar()->maximum()));

    QTextEdit::resizeEvent(event);
    updateScrollRange(QStringLiteral("resizeEvent"));
}

void TextEditor::scrollContentsBy(int dx, int dy)
{
    QTextEdit::scrollContentsBy(dx, dy);
    // 滚动之后纸面、正文、光标的原点全部跟着变，整块重画
    viewport()->update();
}
