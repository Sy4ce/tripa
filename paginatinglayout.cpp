#include "paginatinglayout.h"

#include "paragraph.h" // tripaformat::kKeepLinesTogether / kWidowControl
#include "tripalog.h"

#include <QPainter>
#include <QPalette>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>

#include <cmath>

namespace {

/*!
 * 浮点比较的容差（文档坐标下 0.25 像素）。
 *
 * "这一行还放得下吗"是**整篇分页唯一的判据**，所以它必须是稳定的：
 * 拿 `>` 直接比浮点数时，正好贴边的一行会随浮点误差在"放得下/放不下"
 * 之间跳 —— 表现出来就是同样的内容偶尔多出一页空白。
 */
constexpr double kEps = 0.25;

inline bool fits(double bottom, double limit)
{
    return bottom <= limit + kEps;
}

} // namespace

// ---------------------------------------------------------------- 版面几何

RenderMetrics RenderMetrics::fromPageSetup(const PageSetup &setup)
{
    const QSizeF paperMm = setup.paperSizeMm();
    const QMarginsF margins = setup.bodyMarginsPx();
    RenderMetrics m;
    m.paperWidthPx = PageSetup::mmToPx(paperMm.width());
    m.paperHeightPx = PageSetup::mmToPx(paperMm.height());
    m.marginLeftPx = margins.left();
    m.marginTopPx = margins.top();
    m.marginRightPx = margins.right();
    m.marginBottomPx = margins.bottom();
    return m;
}

double RenderMetrics::bodyWidthPx() const
{
    return qMax(1.0, paperWidthPx - marginLeftPx - marginRightPx);
}

double RenderMetrics::bodyHeightPx() const
{
    return qMax(1.0, paperHeightPx - marginTopPx - marginBottomPx);
}

QRectF RenderMetrics::paperRect(int page) const
{
    return QRectF(0.0, double(page) * paperHeightPx, paperWidthPx, paperHeightPx);
}

QRectF RenderMetrics::bodyRect(int page) const
{
    return QRectF(marginLeftPx, double(page) * paperHeightPx + marginTopPx, bodyWidthPx(),
                  bodyHeightPx());
}

int RenderMetrics::pageAtY(double docY) const
{
    if (paperHeightPx <= 1.0)
        return 0;
    return int(qMax(0.0, std::floor(docY / paperHeightPx)));
}

bool RenderMetrics::operator==(const RenderMetrics &other) const
{
    const auto same = [](double a, double b) { return std::abs(a - b) < 0.01; };
    return same(paperWidthPx, other.paperWidthPx) && same(paperHeightPx, other.paperHeightPx)
           && same(marginLeftPx, other.marginLeftPx) && same(marginTopPx, other.marginTopPx)
           && same(marginRightPx, other.marginRightPx) && same(marginBottomPx, other.marginBottomPx);
}

PageChrome PageChrome::fromPageSetup(const PageSetup &setup)
{
    PageChrome chrome;
    chrome.headerEnabled = setup.headerEnabled;
    chrome.headerText = setup.headerText;
    chrome.footerEnabled = setup.footerEnabled;
    chrome.footerText = setup.footerText;
    chrome.fontSizePt = setup.chromeFontSizePt;
    return chrome;
}

// ---------------------------------------------------------------- 构造

PaginatingLayout::PaginatingLayout(QTextDocument *document)
    : QAbstractTextDocumentLayout(document)
{
    /*!
     * 文档坐标 = 纸面坐标，这一点在别处（TextEditor / tripaRenderToDevice）
     * 是硬编码的假设，这里只把默认版面设成"没纸"（无效），
     * 逼调用方调一次 setMetrics() —— 忘了调的后果是"什么都不显示"，
     * 比"显示成错的"好排查得多。
     */
    TRIPA_DEBUG("layout", QStringLiteral("分页排版已装到文档上（等待版面几何）"));
}

void PaginatingLayout::setMetrics(const RenderMetrics &metrics)
{
    if (m_metrics == metrics)
        return;
    const RenderMetrics old = m_metrics;
    m_metrics = metrics;
    TRIPA_INFO("layout",
               QStringLiteral("版面几何 %1x%2 页边距 %3/%4/%5/%6 -> %7x%8 页边距 %9/%10/%11/%12"
                              "（正文 %13x%14，整篇重新断行）")
                   .arg(old.paperWidthPx, 0, 'f', 1)
                   .arg(old.paperHeightPx, 0, 'f', 1)
                   .arg(old.marginLeftPx, 0, 'f', 1)
                   .arg(old.marginTopPx, 0, 'f', 1)
                   .arg(old.marginRightPx, 0, 'f', 1)
                   .arg(old.marginBottomPx, 0, 'f', 1)
                   .arg(m_metrics.paperWidthPx, 0, 'f', 1)
                   .arg(m_metrics.paperHeightPx, 0, 'f', 1)
                   .arg(m_metrics.marginLeftPx, 0, 'f', 1)
                   .arg(m_metrics.marginTopPx, 0, 'f', 1)
                   .arg(m_metrics.marginRightPx, 0, 'f', 1)
                   .arg(m_metrics.marginBottomPx, 0, 'f', 1)
                   .arg(m_metrics.bodyWidthPx(), 0, 'f', 1)
                   .arg(m_metrics.bodyHeightPx(), 0, 'f', 1));
    m_rebreakAll = true; // 栏宽/页高都变了，断行与分页都要重做
    relayout(QStringLiteral("版面几何变化"));
}

void PaginatingLayout::setChrome(const PageChrome &chrome)
{
    m_chrome = chrome;
    emit update();
}

void PaginatingLayout::setTitle(const QString &title)
{
    m_title = title;
    emit update();
}

void PaginatingLayout::invalidateAll()
{
    m_rebreakAll = true;
    relayout(QStringLiteral("显式失效"));
}

// ---------------------------------------------------------------- 分页

/*!
 * \brief 整篇重排：断行 + 逐行填页。
 *
 * 两件事分开做是有原因的：
 *   - **断行**（"这一行到哪儿断"）只跟块的文本、块格式、栏宽有关，
 *     跟它在第几页、在纸上的哪儿**毫无关系** —— 所以它按块缓存，
 *     改一个字不必把整篇重新断一遍（见 m_dirtyBlocks）；
 *   - **定位**（"这一行落在哪一页的哪一点"）很便宜，每行就是几次比较，
 *     所以每次重排都从头算一遍：分页是全局的，一行的行数一变，
 *     后面所有行的页号都可能跟着变，省这点计算不值得冒算错的险。
 */
void PaginatingLayout::relayout(const QString &reason)
{
    QTextDocument *doc = document();
    if (!doc)
        return;
    if (m_inRelayout) {
        /*!
         * 重入：`relayout()` 里会碰 `QTextLayout`，而 `QTextLayout` 在某些
         * 情况下会反过来碰文档（比如两端对齐重新断行）。真绕成环就是死循环，
         * 所以这里只记一笔、把这次请求丢掉，让外面那一次跑完。
         */
        m_relayoutPending = true;
        TRIPA_WARN("layout", QStringLiteral("重排重入（%1）—— 丢弃这一次，避免死循环").arg(reason));
        return;
    }
    if (!m_metrics.isValid()) {
        m_blocks.clear();
        m_lines.clear();
        m_pageLines.clear();
        m_lastPage = 0;
        return;
    }

    m_inRelayout = true;
    const int total = doc->blockCount();
    m_blocks.resize(total);
    m_lines.clear();
    m_pageLines.clear();
    m_lastPage = 0;

    /*!
     * 流的起点 = 第一页正文窗口的左上角。文档坐标里"第一页的正文窗口"
     * 从 (左边距, 上边距) 开始 —— 也就是说文档坐标的 (0,0) 是**纸的左上角**，
     * 不是正文的左上角。屏幕上的变换（documentToViewport）和打印时的
     * "第几页平移几个纸高"都建立在这个口径上。
     */
    double flowY = m_metrics.bodyRect(0).top();
    int page = 0;

    for (int i = 0; i < total; ++i) {
        const QTextBlock block = doc->findBlockByNumber(i);
        if (!block.isValid()) {
            m_blocks[i] = BlockRecord();
            continue;
        }
        BlockRecord record;
        positionBlock(block, i, &flowY, &page, &record);
        m_blocks[i] = record;
    }
    m_lastPage = page;
    m_inRelayout = false;

    const QSizeF size = documentSize();
    const bool pagesChanged = m_lastPage + 1 != m_pageLines.size();
    TRIPA_DEBUG("layout",
                QStringLiteral("重排完成（%1）：%2 段 / %3 行 / %4 页，正文窗口高=%5，"
                               "文档=%6x%7，视图下边界=%8")
                    .arg(reason)
                    .arg(total)
                    .arg(m_lines.size())
                    .arg(m_pageLines.size())
                    .arg(m_metrics.bodyHeightPx(), 0, 'f', 1)
                    .arg(size.width(), 0, 'f', 1)
                    .arg(size.height(), 0, 'f', 1)
                    .arg(contentBottomPx(), 0, 'f', 1));
    emit documentSizeChanged(size);
    if (pagesChanged)
        emit pageCountChanged(m_pageLines.size());
    emit update();
    emit layoutRebuilt();

    if (m_relayoutPending) {
        m_relayoutPending = false;
        // 重排期间又有新的变化：再来一遍（不是递归，是最多一次补做）
        invalidateAll();
    }
}

/*!
 * \brief 断行：把一段话按当前栏宽切成若干行。
 *
 * 只做"切"，**不定位** —— 行的 x/y 由 positionBlock() 按"第几页"决定。
 * 缩进和对齐的宽度关系在这里定死：
 *   - 可用宽度 = 正文宽 - 左缩进 - 右缩进；
 *   - 首行还要再让出 textIndent（首行缩进为正、悬挂缩进为负）。
 */
void PaginatingLayout::breakBlock(const QTextBlock &block) const
{
    QTextLayout *layout = block.layout();
    if (!layout)
        return;

    const QTextBlockFormat format = block.blockFormat();
    const double bodyWidth = m_metrics.bodyWidthPx();
    const double avail = qMax(1.0, bodyWidth - format.leftMargin() - format.rightMargin());
    const double indent = format.textIndent();

    /*!
     * 两端对齐必须交给 QTextEngine（它要按行宽把字撑开），所以这里把
     * 块的对齐方式写进 layout 的排版选项；居中 / 右对齐不写 ——
     * 它们只是"行摆在哪"，由 lineLeft() 算，不需要引擎参与。
     * 其余选项（换行规则、制表位、中文版式的那些开关）原样保留。
     */
    const Qt::Alignment alignment = format.alignment();
    if (alignment & Qt::AlignJustify) {
        QTextOption option = layout->textOption();
        if (option.alignment() != alignment) {
            option.setAlignment(alignment);
            layout->setTextOption(option);
        }
    }

    layout->beginLayout();
    double y = 0.0;
    int index = 0;
    while (true) {
        QTextLine line = layout->createLine();
        if (!line.isValid())
            break;
        const double width = (index == 0) ? qMax(1.0, avail - indent) : avail;
        line.setLineWidth(width);
        // 位置只是占位，真正的落点在 positionBlock() 里设
        line.setPosition(QPointF(0.0, y));
        y += lineAdvance(line, format);
        ++index;
    }
    layout->endLayout();
}

double PaginatingLayout::lineAdvance(const QTextLine &line, const QTextBlockFormat &format)
{
    const double height = line.height();
    switch (format.lineHeightType()) {
    case QTextBlockFormat::FixedHeight:
        return qMax(height, qMax(0.0, format.lineHeight()));
    case QTextBlockFormat::MinimumHeight:
        return qMax(height, qMax(0.0, format.lineHeight()));
    case QTextBlockFormat::ProportionalHeight:
        return height * qMax(0.0, format.lineHeight()) / 100.0;
    case QTextBlockFormat::LineDistanceHeight:
        return height + qMax(0.0, format.lineHeight());
    case QTextBlockFormat::SingleHeight:
    default:
        return height;
    }
}

//! 行的左边界（块内坐标）：缩进 + 对齐
double PaginatingLayout::lineLeft(const QTextBlockFormat &format, const QTextLine &line,
                                 bool firstLine, double avail) const
{
    const double indent = firstLine ? format.textIndent() : 0.0;
    const double width = qMax(1.0, avail - indent);
    double x = format.leftMargin() + indent;
    const Qt::Alignment alignment = format.alignment();
    if (alignment & Qt::AlignHCenter)
        x += qMax(0.0, (width - line.naturalTextWidth()) / 2.0);
    else if (alignment & Qt::AlignRight)
        x += qMax(0.0, width - line.naturalTextWidth());
    return x;
}

//! 一段话整段有多高（不含段前段后距）——"整段搬走"这类策略要问它
double PaginatingLayout::blockHeightPx(const QTextBlock &block) const
{
    QTextLayout *layout = block.layout();
    if (!layout)
        return 0.0;
    const QTextBlockFormat format = block.blockFormat();
    double total = 0.0;
    for (int i = 0; i < layout->lineCount(); ++i)
        total += lineAdvance(layout->lineAt(i), format);
    return total;
}

/*!
 * \brief 把一段话填进纸里（**按需分页**就发生在这一步）。
 *
 * 顺序：段前分页 -> 段前距 -> 整段策略（段中不分页 / 与下段同页）->
 * 逐行填（填不下就整行换页，孤行控制会把断点挪一行）。
 *
 * 每一步都只做"当前这一页放不放得下"的判断，**从不预先决定一共几页** ——
 * 所以没写到的页根本不存在，也就不会出现"后面凭空多出几页"。
 */
void PaginatingLayout::positionBlock(const QTextBlock &block, int blockNumber, double *flowYRef,
                                     int *pageRef, BlockRecord *record) const
{
    QTextLayout *layout = block.layout();
    const QTextBlockFormat format = block.blockFormat();

    // 1. 断行：缓存能复用就复用（行的断点跟页无关，见 relayout 的说明）
    const bool needBreak = m_rebreakAll || m_dirtyBlocks.contains(blockNumber);
    if (needBreak || !layout || layout->lineCount() == 0)
        breakBlock(block);

    const int lineTotal = layout ? layout->lineCount() : 0;
    QVector<double> advance;
    advance.reserve(lineTotal);
    double blockTotal = 0.0;
    for (int i = 0; i < lineTotal; ++i) {
        const double a = lineAdvance(layout->lineAt(i), format);
        advance.append(a);
        blockTotal += a;
    }

    const int policy = int(format.pageBreakPolicy());
    const bool breakBefore = (policy & QTextFormat::PageBreak_AlwaysBefore) != 0;
    const bool keepWithNext = (policy & QTextFormat::PageBreak_AlwaysAfter) != 0;
    const bool keepLinesTogether = format.boolProperty(tripaformat::kKeepLinesTogether);
    const bool widowControl = format.boolProperty(tripaformat::kWidowControl);

    double flowY = *flowYRef;
    int page = *pageRef;

    const auto pageTop = [this](int p) { return m_metrics.bodyRect(p).top(); };
    const auto pageBottom = [this](int p) { return m_metrics.bodyRect(p).bottom(); };
    const bool atPageTop = std::abs(flowY - pageTop(page)) < 0.5;

    // 2. 段前分页：显式要求（Word 的"段前分页"）
    if (breakBefore && !atPageTop) {
        ++page;
        flowY = pageTop(page);
        TRIPA_TRACE("layout",
                    QStringLiteral("第 %1 段要求段前分页 -> 第 %2 页").arg(blockNumber + 1).arg(page + 1));
    }

    // 3. 段前距：页首不留段前距（Word 的惯例）；放不下第一行时整段挪走
    const double spaceBefore = format.topMargin();
    if (!atPageTop && spaceBefore > 0.0) {
        const double firstLine = lineTotal > 0 ? advance.first() : 0.0;
        if (!fits(flowY + spaceBefore + firstLine, pageBottom(page))) {
            ++page;
            flowY = pageTop(page);
        } else {
            flowY += spaceBefore;
        }
    }

    // 4. 段中不分页：整段在一整页里放得下，但当前页剩下的地方放不下 → 整段搬到下一页
    if (keepLinesTogether && lineTotal > 0 && flowY > pageTop(page)
        && blockTotal <= m_metrics.bodyHeightPx() + kEps
        && !fits(flowY + blockTotal, pageBottom(page))) {
        ++page;
        flowY = pageTop(page);
    }

    /*!
     * 5. 与下段同页：本段 + 后面跟着的（同样要求与下段同页的）几段
     *    如果整体放不下当前页，就整段搬到下一页。
     *
     * 这是"段前分页"的镜像：它防止"小标题留在页尾、正文跑到下一页"。
     * 只在整体能放进一整页时才搬（放不下就说明本来就得跨页，硬搬没有意义）。
     */
    if (keepWithNext && lineTotal > 0 && flowY > pageTop(page)) {
        double need = blockTotal;
        QTextBlock next = block.next();
        int guard = 0;
        while (next.isValid() && guard++ < 64) {
            if (!next.layout() || next.layout()->lineCount() == 0)
                breakBlock(next);
            need += blockHeightPx(next) + next.blockFormat().topMargin();
            if (!next.blockFormat().pageBreakPolicy().testFlag(QTextFormat::PageBreak_AlwaysAfter))
                break;
            next = next.next();
        }
        if (need <= m_metrics.bodyHeightPx() + kEps && !fits(flowY + need, pageBottom(page))) {
            ++page;
            flowY = pageTop(page);
        }
    }

    /*!
     * 6. 逐行填页。这里就是"只有当字被挤到下边界时才换页"：
     *
     *    先看当前页剩余高度里能放下几行，剩下的第一行放不下时它**整行**
     *    搬到下一页的正文窗口顶部 —— 不会像以前那样被下边距裁掉半个字，
     *    也不会在原处留下空档。
     *
     *    孤行控制（可关，默认关）会在断点只有一行时把断点挪一行：
     *      - 页尾只剩一行 -> 那一行也别留着，跟后面的一起走；
     *      - 下一页只剩一行 -> 多带一行过去。
     *    这是 Word 的默认排版习惯，但它会让"断点"看起来不像"正好填满"，
     *    所以默认不开，由段落的「换行和分页」选项卡决定。
     */
    QVector<bool> newPageBefore(lineTotal, false);
    if (lineTotal > 0) {
        double avail = pageBottom(page) - flowY;
        int i = 0;
        int guard = 0;
        while (i < lineTotal && guard++ < lineTotal + 8) {
            double used = 0.0;
            int k = i;
            while (k < lineTotal && used + advance.at(k) <= avail + kEps) {
                used += advance.at(k);
                ++k;
            }
            if (k >= lineTotal)
                break; // 剩下的都放得下

            int breakAt = k;
            if (widowControl && lineTotal > 1) {
                if (k - i == 1)
                    breakAt = i; // 页尾只剩一行 -> 不留
                if (lineTotal - k == 1 && k > i)
                    breakAt = k - 1; // 下一页只剩一行 -> 多带一行
            }
            if (breakAt <= i) {
                /*!
                 * 一行都放不下：要么是孤行控制把断点退到了段首，要么是
                 * **这一行比一整页的正文窗口还高**。
                 * 后者如果还硬要换页就会死循环（换了页照样放不下），
                 * 所以这里强制放一行并报警 —— 宁可让它压住下边界，
                 * 也不能让界面卡死。
                 */
                if (k > i && avail >= m_metrics.bodyHeightPx() - kEps) {
                    TRIPA_WARN("layout",
                               QStringLiteral("第 %1 段有一行（%2 像素）比一整页正文窗口（%3）还高，"
                                              "只能让它压住下边界（页 %4）")
                                   .arg(blockNumber + 1)
                                   .arg(advance.at(i), 0, 'f', 1)
                                   .arg(m_metrics.bodyHeightPx(), 0, 'f', 1)
                                   .arg(page + 1));
                    breakAt = i + 1;
                } else {
                    breakAt = i;
                }
            }
            if (breakAt >= lineTotal)
                break;
            newPageBefore[breakAt] = true;
            avail = m_metrics.bodyHeightPx();
            i = breakAt;
        }
    }

    // 7. 落点：页号定完之后，位置就是纯粹的累加
    const double blockLeft = m_metrics.marginLeftPx;
    const double avail = qMax(1.0, m_metrics.bodyWidthPx() - format.leftMargin()
                                       - format.rightMargin());
    const int firstLineIndex = m_lines.size();
    const int firstPage = page;
    double layoutTop = flowY; // 第一行的上边界 = 块原点（QTextLayout::position 的 y）
    bool layoutTopSet = false;

    for (int i = 0; i < lineTotal; ++i) {
        if (newPageBefore.at(i)) {
            ++page;
            flowY = pageTop(page);
        }
        // 非 const：下面要给它 setPosition()（QTextLine 的位置是布局内的相对坐标）
        QTextLine line = layout->lineAt(i);
        const double height = line.height();
        const double advanceY = advance.at(i);
        /*!
         * 行在行框里的位置：行距比自然行高时，多出来的那点空隙**放在下面**
         * （和 Qt 原来的行为一致）；只有"固定值 / 最小值"这两种把字放在
         * 行框中间 —— 固定值通常比字高，贴顶会让文字整体偏上。
         */
        double glyphTop = flowY;
        if (format.lineHeightType() == QTextBlockFormat::FixedHeight
            || format.lineHeightType() == QTextBlockFormat::MinimumHeight)
            glyphTop = flowY + qMax(0.0, (advanceY - height) / 2.0);

        if (!layoutTopSet) {
            layoutTop = glyphTop;
            layoutTopSet = true;
        }
        line.setPosition(QPointF(lineLeft(format, line, i == 0, avail), glyphTop - layoutTop));

        LineRecord rec;
        rec.blockNumber = blockNumber;
        rec.lineIndex = i;
        rec.page = page;
        rec.rect = QRectF(blockLeft + line.x(), glyphTop,
                          qMax(line.width(), line.naturalTextWidth()), height);
        m_lines.append(rec);

        // 每页有哪些行：行是按流顺序收集的，所以页号只增不减，区间一定是连续的
        while (m_pageLines.size() <= page)
            m_pageLines.append(qMakePair(m_lines.size() - 1, m_lines.size() - 1));
        m_pageLines[page].second = m_lines.size() - 1;

        flowY += advanceY;
    }

    if (layout)
        layout->setPosition(QPointF(blockLeft, layoutTop));

    /*!
     * 逐段日志（Trace 级）：排查“屏幕上的字和算出来的位置不一致”时，
     * 一行就能看出这一段落在第几页、从哪个 y 开始、行距是多少。
     */
    if (lineTotal > 0) {
        const LineRecord &first = m_lines.at(firstLineIndex);
        const LineRecord &last = m_lines.at(m_lines.size() - 1);
        TRIPA_TRACE("layout",
                    QStringLiteral("第 %1 段：%2 行，页 %3->%4，首行 y=%5 末行 y=%6，行距≈%7")
                        .arg(blockNumber + 1)
                        .arg(lineTotal)
                        .arg(firstPage + 1)
                        .arg(page + 1)
                        .arg(first.rect.top(), 0, 'f', 2)
                        .arg(last.rect.top(), 0, 'f', 2)
                        .arg(lineTotal > 1 ? (last.rect.top() - first.rect.top()) / (lineTotal - 1) : 0.0,
                             0, 'f', 2));
    }
    // 8. 段后距（页尾不留：留着只会把下一页的第一行往下推）
    if (lineTotal > 0 && format.bottomMargin() > 0.0) {
        const double after = format.bottomMargin();
        if (fits(flowY + after, pageBottom(page)))
            flowY += after;
    }

    record->firstLine = firstLineIndex;
    record->lineCount = lineTotal;
    record->top = lineTotal > 0 ? m_lines.at(firstLineIndex).rect.top() : flowY;
    record->bottom = lineTotal > 0 ? m_lines.at(m_lines.size() - 1).rect.top() : flowY;
    record->firstPage = firstPage;
    record->lastPage = page;
    record->brokenLineCount = lineTotal;

    *flowYRef = flowY;
    *pageRef = page;
}

// ---------------------------------------------------------------- 查询

int PaginatingLayout::pageCount() const
{
    return qMax(1, m_pageLines.size());
}

double PaginatingLayout::contentBottomPx() const
{
    /*!
     * 视图的下边界 = **最后一页的下边界 + 一页高**。
     *
     * 这一条是用户直接提出来的：滚到底时应该能把最后一页的下边界
     * 顶到视窗最上面，好让"纸的下面是什么"一眼看完（底下是桌面，
     * 没有内容）—— 只要一页比视窗高，这个定义就正好够用。
     */
    return double(m_lastPage + 2) * m_metrics.paperHeightPx;
}

int PaginatingLayout::pageOfLine(int lineIndex) const
{
    if (lineIndex < 0 || lineIndex >= m_lines.size())
        return -1;
    return m_lines.at(lineIndex).page;
}

int PaginatingLayout::lineBlockNumber(int lineIndex) const
{
    if (lineIndex < 0 || lineIndex >= m_lines.size())
        return -1;
    return m_lines.at(lineIndex).blockNumber;
}

int PaginatingLayout::lineNumberInBlock(int lineIndex) const
{
    if (lineIndex < 0 || lineIndex >= m_lines.size())
        return -1;
    return m_lines.at(lineIndex).lineIndex;
}

QRectF PaginatingLayout::lineRect(int lineIndex) const
{
    if (lineIndex < 0 || lineIndex >= m_lines.size())
        return QRectF();
    return m_lines.at(lineIndex).rect;
}

int PaginatingLayout::linesOnPage(int page) const
{
    if (page < 0 || page >= m_pageLines.size())
        return 0;
    return m_pageLines.at(page).second - m_pageLines.at(page).first + 1;
}

/*!
 * 页 [firstPage, lastPage] 里出现过的段落号范围。
 *
 * 只扫这几页的行记录（m_pageLines 直接给出每页的行区间），
 * 所以跟稿子有多长无关 —— 这正是效果层按段落缓存的前提。
 */
QPair<int, int> PaginatingLayout::blockRangeOnPages(int firstPage, int lastPage) const
{
    (void)pageCount(); // 先确保分页算过
    int first = -1;
    int last = -1;
    const int pageFrom = qMax(0, qMin(firstPage, lastPage));
    const int pageTo = qMin(qMax(firstPage, lastPage), m_pageLines.size() - 1);
    for (int page = pageFrom; page <= pageTo; ++page) {
        const QPair<int, int> range = m_pageLines.at(page);
        if (range.second < range.first)
            continue;
        const int head = m_lines.at(range.first).blockNumber;
        const int tail = m_lines.at(range.second).blockNumber;
        if (head < 0 || tail < 0)
            continue;
        first = first < 0 ? head : qMin(first, head);
        last = qMax(last, tail);
    }
    if (first < 0)
        return qMakePair(0, -1);
    return qMakePair(first, last);
}

QString PaginatingLayout::describe() const
{
    QStringList parts;
    parts << QStringLiteral("正文窗口 %1x%2（纸 %3x%4，边距 %5/%6/%7/%8）")
                 .arg(m_metrics.bodyWidthPx(), 0, 'f', 1)
                 .arg(m_metrics.bodyHeightPx(), 0, 'f', 1)
                 .arg(m_metrics.paperWidthPx, 0, 'f', 1)
                 .arg(m_metrics.paperHeightPx, 0, 'f', 1)
                 .arg(m_metrics.marginLeftPx, 0, 'f', 1)
                 .arg(m_metrics.marginTopPx, 0, 'f', 1)
                 .arg(m_metrics.marginRightPx, 0, 'f', 1)
                 .arg(m_metrics.marginBottomPx, 0, 'f', 1);
    parts << QStringLiteral("%1 段 / %2 行 / %3 页").arg(m_blocks.size()).arg(m_lines.size()).arg(pageCount());
    for (int p = 0; p < m_pageLines.size(); ++p) {
        const int from = m_pageLines.at(p).first;
        const int to = m_pageLines.at(p).second;
        parts << QStringLiteral("第 %1 页：%2 行，文档 y=%3..%4（窗口 %5..%6）")
                     .arg(p + 1)
                     .arg(to - from + 1)
                     .arg(m_lines.at(from).rect.top(), 0, 'f', 1)
                     .arg(m_lines.at(to).rect.bottom(), 0, 'f', 1)
                     .arg(m_metrics.bodyRect(p).top(), 0, 'f', 1)
                     .arg(m_metrics.bodyRect(p).bottom(), 0, 'f', 1);
    }
    return parts.join(QStringLiteral("；"));
}

// ---------------------------------------------------------------- 绘制

/*!
 * \brief 画正文。
 *
 * 逐段调用 `QTextLayout::draw()` —— 也就是"让排版引擎自己画自己算出来的行"：
 * 行的位置是 positionBlock() 设的（含分页），所以**画出来的和量的完全是同一套**。
 *
 * 这里不碰纸张（纸、阴影、页眉页脚由调用方画）：一张纸要不要画、
 * 画在屏幕的哪儿，是视图的事；"字落在纸的哪儿"才是这里的责任。
 */
void PaginatingLayout::draw(QPainter *painter, const PaintContext &context)
{
    QTextDocument *doc = document();
    if (!doc || m_lines.isEmpty())
        return;

    const QRectF clip = context.clip;

    painter->save();
    /*!
     * 正文默认色必须由调用方给的调色板决定：QTextLayout 画没有显式前景色的
     * 字符时用的就是**画笔当前的颜色**。少了这一句，深色主题下正文会是白字
     * （屏幕上白纸白字，看着就像"文档空了"）。
     */
    painter->setPen(context.palette.color(QPalette::Text));

    for (int i = 0; i < m_blocks.size(); ++i) {
        const QTextBlock block = doc->findBlockByNumber(i);
        if (!block.isValid())
            continue;
        QTextLayout *layout = block.layout();
        if (!layout || layout->lineCount() == 0)
            continue;

        const BlockRecord &record = m_blocks.at(i);
        if (!clip.isEmpty() && !blockVisible(record, clip))
            continue;

        const QTextBlockFormat format = block.blockFormat();
        if (format.background().style() != Qt::NoBrush) {
            // 段落底色：一段一行行地铺（跨页时两页各自铺到自己那一段）
            painter->save();
            painter->setPen(Qt::NoPen);
            painter->setBrush(format.background());
            for (int k = record.firstLine; k < record.firstLine + record.lineCount; ++k)
                painter->drawRect(m_lines.at(k).rect);
            painter->restore();
        }

        QList<QTextLayout::FormatRange> selections = selectionRanges(context, block);
        /*!
         * `pos` 传 (0,0)，**不传 `layout->position()`**。
         *
         * `QTextLayout::draw(painter, pos)` 把 pos 当成“相对 layout 自己位置的
         * 偏移”：它内部会再加上 `layout->position()`。而 layout 的位置就是
         * 块在文档坐标里的左上角（positionBlock 里设的），于是再传一次
         * 就把整个块多移了一个页边距 —— 实测症状很典型：
         * 正文整体偏移到两倍的页边距处，而**效果层（手写/扭曲）还是对的**，
         * 因为它是用 `blockBoundingRect().top() + line.y()` 自己算的。
         * 屏幕上就是“字在一个地方、笔迹在另一个地方”，
         * 看着像“效果层错位”，其实错的是正文这一路。
         *
         * 自检里的 checkPaintMatchesGeometry 就是钉这个契约的：
         * 画出来的墨点必须落在 `lineRect()` 给出的行框里。
         */
        layout->draw(painter, QPointF(0.0, 0.0), selections);
    }

    painter->restore();
}

bool PaginatingLayout::blockVisible(const BlockRecord &record, const QRectF &clip) const
{
    for (int k = record.firstLine; k < record.firstLine + record.lineCount; ++k) {
        if (m_lines.at(k).rect.intersects(clip))
            return true;
    }
    return false;
}

/*!
 * \brief 把文档的选区换算成"这一段里要画的格式区间"。
 *
 * `QTextLayout::FormatRange::start` 是**块内**下标（相对 layout 的原点），
 * 不是文档下标也不是行内下标 —— 这是最容易搞错的一处，
 * 错了的表现是"选中高亮画在别的行上"。
 */
QList<QTextLayout::FormatRange> PaginatingLayout::selectionRanges(const PaintContext &context,
                                                                 const QTextBlock &block) const
{
    QList<QTextLayout::FormatRange> ranges;
    if (context.selections.isEmpty())
        return ranges;
    const int blockStart = block.position();
    const int blockEnd = blockStart + qMax(0, block.length() - 1);
    for (const QAbstractTextDocumentLayout::Selection &selection : context.selections) {
        const int from = qMax(selection.cursor.selectionStart(), blockStart);
        const int to = qMin(selection.cursor.selectionEnd(), blockEnd);
        if (from >= to)
            continue;
        QTextLayout::FormatRange range;
        range.start = from - blockStart;
        range.length = to - from;
        range.format = selection.format;
        ranges.append(range);
    }
    return ranges;
}

/*!
 * \brief 页眉页脚。默认两个都是关的（`PageChrome::headerEnabled/footerEnabled`）。
 *
 * \a painter 必须已经在**文档坐标**里（和正文同一个变换）：
 * 页眉画在纸的上边距里、页脚画在下边距里，所以它们跟着纸一起缩放、一起打印。
 */
void PaginatingLayout::drawChrome(QPainter *painter, int page, const QPalette &palette) const
{
    if (!m_chrome.anyEnabled() || !m_metrics.isValid())
        return;

    const QRectF paper = m_metrics.paperRect(page);
    QFont font = painter->font();
    font.setPointSizeF(qMax(5.0, m_chrome.fontSizePt));
    painter->save();
    painter->setFont(font);
    painter->setPen(palette.color(QPalette::Mid));

    const auto text = [this, page](const QString &raw) {
        QString s = raw;
        s.replace(QStringLiteral("{page}"), QString::number(page + 1));
        s.replace(QStringLiteral("{pages}"), QString::number(pageCount()));
        s.replace(QStringLiteral("{title}"), m_title);
        return s;
    };

    if (m_chrome.headerEnabled && m_metrics.marginTopPx > 2.0) {
        /*! 页眉贴着上边距的中线放：不碰正文窗口，也不至于贴到纸边上 */
        const QRectF area(paper.left() + m_metrics.marginLeftPx, paper.top(),
                          m_metrics.bodyWidthPx(), m_metrics.marginTopPx);
        painter->drawText(area, Qt::AlignHCenter | Qt::AlignVCenter, text(m_chrome.headerText));
    }
    if (m_chrome.footerEnabled && m_metrics.marginBottomPx > 2.0) {
        const QRectF area(paper.left() + m_metrics.marginLeftPx,
                          paper.bottom() - m_metrics.marginBottomPx, m_metrics.bodyWidthPx(),
                          m_metrics.marginBottomPx);
        painter->drawText(area, Qt::AlignHCenter | Qt::AlignVCenter, text(m_chrome.footerText));
    }
    painter->restore();
}

// ---------------------------------------------------------------- 几何

QSizeF PaginatingLayout::documentSize() const
{
    if (!m_metrics.isValid())
        return QSizeF(0.0, 0.0);
    const double height = double(m_lastPage + 1) * m_metrics.paperHeightPx;
    return QSizeF(m_metrics.paperWidthPx, qMax(m_metrics.paperHeightPx, height));
}

QRectF PaginatingLayout::blockBoundingRect(const QTextBlock &block) const
{
    if (!block.isValid())
        return QRectF();
    const int index = block.blockNumber();
    if (index < 0 || index >= m_blocks.size())
        return QRectF();
    const BlockRecord &record = m_blocks.at(index);
    if (record.lineCount <= 0)
        return QRectF(m_metrics.marginLeftPx, record.top, m_metrics.bodyWidthPx(), 0.0);
    const LineRecord &first = m_lines.at(record.firstLine);
    const LineRecord &last = m_lines.at(record.firstLine + record.lineCount - 1);
    /*!
     * 一段话跨页时，这个矩形**跨过纸缝**（上边界在第一页、下边界在第二页）。
     * 这是对的：它表示"这一段占了文档坐标的哪一段"，
     * 而文档坐标里的纸缝本来就没有内容。别拿它去判断"页内位置"，
     * 页内的判断一律用 lineRect()（每一行都带着自己的页号）。
     */
    return QRectF(first.rect.left(), first.rect.top(), m_metrics.bodyWidthPx(),
                  last.rect.bottom() - first.rect.top());
}

QRectF PaginatingLayout::frameBoundingRect(QTextFrame *frame) const
{
    if (!frame)
        return QRectF();
    if (frame == document()->rootFrame())
        return QRectF(QPointF(0.0, 0.0), documentSize());
    return QRectF();
}

/*!
 * \brief 命中测试：文档坐标 -> 字符位置。
 *
 * 只在**点所在的那一页**里找行，找不到就用离得最近的那一行
 * （纸上点到空白处时，光标应该落在最近的一行上，而不是跳到别的页）。
 */
int PaginatingLayout::hitTest(const QPointF &point, Qt::HitTestAccuracy accuracy) const
{
    QTextDocument *doc = document();
    if (!doc || m_lines.isEmpty())
        return 0;

    const int page = m_metrics.pageAtY(point.y());
    int best = -1;
    double bestDistance = 1e18;
    const auto consider = [&](int lineIndex, double y) {
        const double distance = std::abs(y - point.y());
        if (distance < bestDistance) {
            bestDistance = distance;
            best = lineIndex;
        }
    };
    const auto scan = [&](int p) {
        if (p < 0 || p >= m_pageLines.size())
            return;
        for (int i = m_pageLines.at(p).first; i <= m_pageLines.at(p).second; ++i)
            consider(i, m_lines.at(i).rect.center().y());
    };
    scan(page);
    if (best < 0) {
        // 这一页没有行（空页 / 点落在页外的桌面）：退到相邻的有内容的页
        scan(page - 1);
        scan(page + 1);
    }
    if (best < 0) {
        // 整篇都没有行：按"点在文档前半还是后半"给个位置
        return point.y() < m_metrics.bodyRect(0).center().y() ? 0 : doc->characterCount() - 1;
    }

    const LineRecord &record = m_lines.at(best);
    const QTextBlock block = doc->findBlockByNumber(record.blockNumber);
    if (!block.isValid())
        return 0;
    QTextLayout *layout = block.layout();
    if (!layout || record.lineIndex >= layout->lineCount())
        return block.position();
    const QTextLine line = layout->lineAt(record.lineIndex);
    const double lineLeftAbs = layout->position().x() + line.x();
    const double localX = point.x() - lineLeftAbs;
    if (accuracy == Qt::ExactHit && (localX < -kEps || localX > line.width() + kEps))
        return -1;
    const int posInBlock = line.xToCursor(qMax(0.0, localX), QTextLine::CursorBetweenCharacters);
    return qBound(0, block.position() + posInBlock, doc->characterCount() - 1);
}

/*!
 * \brief 光标矩形（文档坐标）。
 *
 * 不复用 `QTextEdit::cursorRect()`：它绕一圈 Qt 内部对滚动量的处理，
 * 而本控件里"光标在文档哪儿"只有一套口径（行位置 + 缩进 + 对齐），
 * 直接算出来最不容易错。闪烁相位是视图的事（见 TextEditor）。
 */
QRectF PaginatingLayout::caretRect(const QTextCursor &cursor) const
{
    const QTextBlock block = cursor.block();
    if (!block.isValid())
        return QRectF();
    QTextLayout *layout = block.layout();
    if (!layout || layout->lineCount() == 0)
        return QRectF();

    const int posInBlock = qMax(0, cursor.position() - block.position());
    QTextLine line = layout->lineForTextPosition(posInBlock);
    if (!line.isValid())
        line = layout->lineAt(layout->lineCount() - 1);
    if (!line.isValid())
        return QRectF();

    const QPointF origin = layout->position();
    const double x = origin.x() + line.x() + line.cursorToX(posInBlock);
    const double y = origin.y() + line.y();
    return QRectF(x, y, 1.0, line.height());
}

// ---------------------------------------------------------------- 变化通知

void PaginatingLayout::documentChanged(int from, int charsRemoved, int charsAdded)
{
    QTextDocument *doc = document();
    if (!doc) {
        relayout(QStringLiteral("documentChanged"));
        return;
    }

    /*!
     * 标脏：**只有被改到的那些块**需要重新断行，其余块的行断点原样有效
     * （断点只跟块的文本/格式和栏宽有关）。这是"每打一个字就把整篇重新排一遍"
     * 和"只重排这一段"的区别 —— 文档长了以后就是"能用"和"卡"的区别。
     *
     * 注意字符数变化时**后面的块也会被顶动**：那些块的行断点没变，
     * 但位置要跟着流重算 —— 位置本来就是每轮重排都全量算的（见 relayout）。
     */
    const int first = doc->findBlock(qMax(0, from)).blockNumber();
    const int lastPosition = qMax(from, from + qMax(charsRemoved, charsAdded));
    const int last = doc->findBlock(qMin(lastPosition, qMax(0, doc->characterCount() - 1)))
                         .blockNumber();
    for (int i = qMax(0, first); i <= last; ++i)
        m_dirtyBlocks.insert(i);

    relayout(QStringLiteral("正文变化"));

    // 脏标记在重排里用掉了，这里统一清掉（下次变化会重新标）
    m_dirtyBlocks.clear();
}

// ---------------------------------------------------------------- 装配

PaginatingLayout *paginatingLayoutOf(const QTextDocument *document)
{
    if (!document)
        return nullptr;
    return qobject_cast<PaginatingLayout *>(document->documentLayout());
}

PaginatingLayout *installPaginatingLayout(QTextDocument *document)
{
    if (!document)
        return nullptr;
    if (PaginatingLayout *existing = paginatingLayoutOf(document))
        return existing;
    /*!
     * `setDocumentLayout()` 会接管所有权（旧布局被删掉），所以这里 new 出来就交出去。
     * 资源句柄（图片等）不受影响：Qt6 里这个接口只有这一个版本。
     */
    PaginatingLayout *layout = new PaginatingLayout(document);
    document->setDocumentLayout(layout);
    TRIPA_INFO("layout", QStringLiteral("装上分页排版（原布局不是分页排版）"));
    return layout;
}
