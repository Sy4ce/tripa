/*
 * 渲染层（分页排版）的自检：**按需分页、一行都不越下边界、每一页只有自己那一段内容**。
 *
 * 这一组断言直接对应用户报过的两个 bug，不是"顺手加的覆盖率"：
 *   - 「写着写着就出下边界」 -> checkBoundariesAndUniqueness / checkTypingSession
 *   - 「缩小了看还是有多页复制」 -> checkBoundariesAndUniqueness（各页拼起来 == 全文）
 *
 * 跑法： cmake --build build --target test_layout && ./build/test_layout.exe
 */

#include "paginatinglayout.h"
#include "pagesetup.h"
#include "paragraph.h"
#include "richdocument.h"

#include <QApplication>
#include <QDebug>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextLine>

#include <cmath>

static int g_failures = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        if (cond) {                                                              \
            qInfo().noquote() << "  ok   " << (what);                            \
        } else {                                                                 \
            ++g_failures;                                                        \
            qWarning().noquote() << "  FAIL " << (what);                         \
        }                                                                        \
    } while (false)

namespace {

//! 文档坐标下的比较容差（行高常常是分数，留一点余量）
constexpr double kEps = 0.6;

//! 程序默认的 A4 + 20mm 页边距
PageSetup a4Setup()
{
    PageSetup setup;
    setup.presetName = QStringLiteral("A4");
    setup.widthMm = 210.0;
    setup.heightMm = 297.0;
    setup.landscape = false;
    setup.marginLeftMm = 20.0;
    setup.marginTopMm = 20.0;
    setup.marginRightMm = 20.0;
    setup.marginBottomMm = 20.0;
    return setup;
}

/*!
 * 一份"装好了渲染层"的文档。富文本层只负责内容，纸和分页全在渲染层 ——
 * 这里就是这两层之间那条界线：文档上没有一行代码提到"纸"。
 */
struct Fixture
{
    QTextDocument doc;
    PaginatingLayout *layout = nullptr;
    PageSetup setup = a4Setup();

    Fixture()
    {
        QFont font(QStringLiteral("Microsoft YaHei"));
        font.setPointSizeF(12.0);
        doc.setDefaultFont(font);
        doc.setDocumentMargin(0);
        layout = installPaginatingLayout(&doc);
        layout->setMetrics(RenderMetrics::fromPageSetup(setup));
    }

    RenderMetrics metrics() const { return layout->metrics(); }
};

//! 填 \a count 段文字（每段一行多一点，和"写代码/写提纲"那种节奏接近）
void fillParagraphs(QTextDocument *doc, int count, const QString &tag = QStringLiteral("排版"))
{
    QStringList lines;
    lines.reserve(count);
    for (int i = 0; i < count; ++i)
        lines << QStringLiteral("%1 第 %2 段：正文内容正文内容正文内容").arg(tag).arg(i + 1);
    doc->setPlainText(lines.join(QLatin1Char('\n')));
}

//! 某一行上的文字（自检要按行核对"内容有没有重复"）
QString lineTextOf(const QTextDocument &doc, const PaginatingLayout &layout, int lineIndex)
{
    const int blockNumber = layout.lineBlockNumber(lineIndex);
    const int lineNumber = layout.lineNumberInBlock(lineIndex);
    if (blockNumber < 0 || lineNumber < 0)
        return QString();
    const QTextBlock block = doc.findBlockByNumber(blockNumber);
    if (!block.isValid() || !block.layout())
        return QString();
    const QTextLine line = block.layout()->lineAt(lineNumber);
    if (!line.isValid())
        return QString();
    return block.text().mid(line.textStart(), line.textLength());
}

//! 去掉所有空白（比较"全文 = 各页拼起来"时用；断行位置会吃掉空格）
QString squeezed(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar &ch : text) {
        if (!ch.isSpace())
            out.append(ch);
    }
    return out;
}

//! 每页上的文字（下标 = 页号）
QStringList textPerPage(const QTextDocument &doc, const PaginatingLayout &layout)
{
    QStringList perPage;
    perPage.resize(qMax(1, layout.pageCount()));
    for (int i = 0; i < layout.lineCount(); ++i) {
        const int page = layout.pageOfLine(i);
        if (page >= 0 && page < perPage.size())
            perPage[page] += lineTextOf(doc, layout, i);
    }
    return perPage;
}

//! 有没有行跑到它那一页的正文窗口外面去（"写着写着出下边界"就是这个）
int linesOutsideBody(const PaginatingLayout &layout)
{
    const RenderMetrics metrics = layout.metrics();
    int bad = 0;
    for (int i = 0; i < layout.lineCount(); ++i) {
        const int page = layout.pageOfLine(i);
        const QRectF body = metrics.bodyRect(page);
        const QRectF rect = layout.lineRect(i);
        if (rect.bottom() > body.bottom() + kEps || rect.top() < body.top() - kEps)
            ++bad;
    }
    return bad;
}

// ---------------------------------------------------------------- 用例

/*!
 * 按需分页：**只有真的写满了才新起一页**，没写到的地方没有页。
 *
 * "不需新启一页时不显示后面的页"就是这一条：页数必须在
 * "最后一行放不下"的那一次才 +1，而不是一上来就按固定页数铺开。
 */
void checkOnDemandPaging()
{
    Fixture f;
    CHECK(f.layout->pageCount() == 1, "空文档 = 1 页（页不是预先铺出来的）");

    // 一段一段地加，直到第一次出现第 2 页
    int paragraphsUntilPage2 = 0;
    for (int n = 1; n <= 200; ++n) {
        fillParagraphs(&f.doc, n);
        if (f.layout->pageCount() >= 2) {
            paragraphsUntilPage2 = n;
            break;
        }
    }
    CHECK(paragraphsUntilPage2 > 0, "写着写着确实会需要第 2 页（用例本身成立）");
    CHECK(f.layout->pageCount() == 2,
          QStringLiteral("第 2 页是在第 %1 段之后才出现的，而且只多了一页").arg(paragraphsUntilPage2));

    /*!
     * 第 1 页必须**填得差不多满**：如果刚写几行就换页，说明分页判据算错了
     * （比如把页高当成了正文高度、或者行距被算了两遍）。
     */
    const QRectF body0 = f.metrics().bodyRect(0);
    const QRectF lastOnPage0 = f.layout->lineRect(f.layout->linesOnPage(0) - 1);
    const double leftover = body0.bottom() - lastOnPage0.bottom();
    CHECK(leftover < lastOnPage0.height() + kEps,
          QStringLiteral("第 1 页是填到底才换的页（底部剩 %1 像素，不到一行）").arg(leftover, 0, 'f', 1));

    // 减掉最后一段：内容回去了，页数也要跟着回去（"不需新启一页时不显示后面的页"）
    fillParagraphs(&f.doc, paragraphsUntilPage2 - 1);
    CHECK(f.layout->pageCount() == 1, "把最后一段删掉，第 2 页立刻消失（不是留着一张空纸）");
}

/*!
 * 两条硬规矩，也是用户报的两个 bug 的直接判据：
 *   1. 任何一行都不许越过正文窗口的下边界；
 *   2. 每一页只有自己那一段内容 —— 各页文字按顺序拼起来正好是全文。
 */
void checkBoundariesAndUniqueness()
{
    Fixture f;
    fillParagraphs(&f.doc, 120);
    const int pages = f.layout->pageCount();
    CHECK(pages >= 3, QStringLiteral("120 段排出了 %1 页（够多页才测得出重复）").arg(pages));
    CHECK(f.layout->lineCount() > 0, "确实排出了行");

    CHECK(linesOutsideBody(*f.layout) == 0,
          QStringLiteral("%1 行全部落在各自那一页的正文窗口里（一行都没出下边界）")
              .arg(f.layout->lineCount()));

    const QStringList perPage = textPerPage(f.doc, *f.layout);
    QSet<QString> seen;
    int duplicates = 0;
    for (int p = 0; p < perPage.size(); ++p) {
        const QString text = squeezed(perPage.at(p));
        if (text.isEmpty())
            continue;
        if (seen.contains(text))
            ++duplicates;
        seen.insert(text);
    }
    CHECK(duplicates == 0, "没有任何一页的内容和别的页一模一样（第二页重复第一页）");

    const QString joined = squeezed(perPage.join(QString()));
    CHECK(joined == squeezed(f.doc.toPlainText()),
          QStringLiteral("各页文字按顺序拼起来 == 全文（%1 字符，不重复也不丢字）").arg(joined.size()));
}

/*!
 * 视图下边界的定义：**最后一页的下边界 + 一页高**。
 * 有了它，滚到底就能把最后一页的下边界顶到视窗最上面。
 */
void checkViewBottom()
{
    Fixture f;
    fillParagraphs(&f.doc, 60);
    const double paperH = f.metrics().paperHeightPx;
    const int last = f.layout->lastContentPage();

    CHECK(qAbs(f.layout->documentSize().height() - double(last + 1) * paperH) < 0.01,
          "文档高 = 最后一页的下边界");
    CHECK(qAbs(f.layout->contentBottomPx() - double(last + 2) * paperH) < 0.01,
          "视图下边界 = 最后一页下边界 + 一页高（滚到底能把纸底顶到视窗最上面）");
    CHECK(f.layout->documentSize().width() > 0.0, "文档宽度 = 纸张宽度（横向不出纸）");
}

/*!
 * 命中测试与光标：**两者必须和正文用同一份行落点**。
 *
 * 分页之后"文档坐标 y"和"第几页"是绑在一起的，命中测试要是还按
 * "连续排版"那套算，就会出现"点第 3 页却定位到第 1 页"。
 */
void checkHitTestAndCaret()
{
    Fixture f;
    fillParagraphs(&f.doc, 40);

    int checked = 0;
    int badHit = 0;
    int badCaret = 0;
    for (int i = 0; i < f.layout->lineCount(); ++i) {
        const QRectF rect = f.layout->lineRect(i);
        const QTextBlock block = f.doc.findBlockByNumber(f.layout->lineBlockNumber(i));
        if (!block.isValid() || !block.layout())
            continue;
        const QTextLine line = block.layout()->lineAt(f.layout->lineNumberInBlock(i));
        if (!line.isValid())
            continue;
        const int lineStart = block.position() + line.textStart();
        const int lineEnd = lineStart + line.textLength();

        // 行中间那一点：命中的字符必须落在这一行里
        const QPointF probe(rect.left() + rect.width() / 2.0, rect.center().y());
        const int pos = f.layout->hitTest(probe, Qt::FuzzyHit);
        if (pos < lineStart || pos > lineEnd)
            ++badHit;

        // 命中位置的光标矩形必须还落在这两行之间（同一行）
        QTextCursor cursor(&f.doc);
        cursor.setPosition(qBound(0, pos, f.doc.characterCount() - 1));
        const QRectF caret = f.layout->caretRect(cursor);
        if (caret.isEmpty() || caret.center().y() < rect.top() - kEps
            || caret.center().y() > rect.bottom() + kEps)
            ++badCaret;
        ++checked;
    }
    CHECK(checked > 20, QStringLiteral("逐行核对 %1 行（用例本身要有量）").arg(checked));
    CHECK(badHit == 0, "行内任一点命中到的字符都在这一行里");
    CHECK(badCaret == 0, "命中位置的光标矩形落在同一行上（点哪儿光标就在哪儿）");
}

/*!
 * 页眉页脚：**默认都不显示**；打开了也不许动排版。
 *
 * 页眉画在上边距里、页脚画在下边距里 —— 那是纸上的空白，
 * 占不到正文窗口，所以"开不开页眉"不该让任何一个字挪位置。
 */
/*!
 * **画出来的位置必须等于算出来的位置**。
 *
 * 这一条是“所见即所得”的底线：命中测试、光标、效果层全走 lineRect()，
 * 而屏幕上看到的是 draw() 画出来的。两者一旦差一个偏移（比如多算一个
 * 页边距），症状就是“点哪儿都不对、笔迹跑到字外面”—— 而且从截图上看
 * 只是“整个错位了”，很难看出是哪一层的问题。
 *
 * 做法：把正文画到一张白纸上（文档坐标 1:1），再数每一行的行框里有几个墨点。
 */
void checkPaintMatchesGeometry()
{
    Fixture f;
    fillParagraphs(&f.doc, 12);
    const RenderMetrics m = f.metrics();
    QImage image(int(m.paperWidthPx), int(m.paperHeightPx), QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, Qt::black);
        f.layout->draw(&painter, context);
    }

    int empty = 0;
    int checked = 0;
    for (int i = 0; i < qMin(8, f.layout->lineCount()); ++i) {
        const QRectF rect = f.layout->lineRect(i);
        int ink = 0;
        for (int y = qMax(0, int(rect.top())); y <= qMin(image.height() - 1, int(rect.bottom())); ++y) {
            for (int x = qMax(0, int(rect.left()));
                 x <= qMin(image.width() - 1, int(rect.right())); ++x) {
                if (qRed(image.pixel(x, y)) < 140)
                    ++ink;
            }
        }
        if (ink < 5)
            ++empty;
        ++checked;
    }
    CHECK(checked >= 5, QStringLiteral("抽了 %1 行核对（用例本身要有量）").arg(checked));
    CHECK(empty == 0, "每一行的行框里都有字（draw 画的位置 == lineRect 的位置）");
}

void checkChrome()
{
    Fixture f;
    fillParagraphs(&f.doc, 60);

    CHECK(!f.layout->chrome().anyEnabled(), "默认不显示页眉页脚");

    const int pagesBefore = f.layout->pageCount();
    QVector<QRectF> before;
    for (int i = 0; i < f.layout->lineCount(); ++i)
        before.append(f.layout->lineRect(i));

    PageChrome chrome;
    chrome.headerEnabled = true;
    chrome.headerText = QStringLiteral("{title}");
    chrome.footerEnabled = true;
    chrome.footerText = QStringLiteral("第 {page} 页 / 共 {pages} 页");
    f.layout->setTitle(QStringLiteral("自检文档"));
    f.layout->setChrome(chrome);

    CHECK(f.layout->pageCount() == pagesBefore, "开了页眉页脚，页数不变");
    bool moved = false;
    for (int i = 0; i < f.layout->lineCount() && i < before.size(); ++i) {
        if (f.layout->lineRect(i) != before.at(i))
            moved = true;
    }
    CHECK(!moved, "开了页眉页脚，一个字都没挪（排版和装饰是两回事）");

    const RenderMetrics m = f.metrics();
    const QRectF header(0.0, 0.0, m.paperWidthPx, m.marginTopPx);
    const QRectF footer(0.0, m.paperHeightPx - m.marginBottomPx, m.paperWidthPx, m.marginBottomPx);
    CHECK(!m.bodyRect(0).intersects(header), "页眉在正文窗口外面（上边距里）");
    CHECK(!m.bodyRect(0).intersects(footer), "页脚在正文窗口外面（下边距里）");
}

/*!
 * 字号变大 -> 整篇重新断行 -> 页数变多，而且**仍然一行都不越界**。
 *
 * 这一条防的是"断行缓存该失效的时候没失效"：缓存一旦算旧了，
 * 屏幕上就会出现"字按旧字号排、却按新字号画"的错位。
 */
void checkReflowAfterFontChange()
{
    Fixture f;
    fillParagraphs(&f.doc, 60);
    const int before = f.layout->pageCount();

    QFont bigger = f.doc.defaultFont();
    bigger.setPointSizeF(24.0);
    f.doc.setDefaultFont(bigger);
    f.layout->invalidateAll(); // 文档级格式变了：断行缓存必须整篇作废

    const int after = f.layout->pageCount();
    CHECK(after > before, QStringLiteral("字号 12 -> 24 磅：页数 %1 -> %2").arg(before).arg(after));
    CHECK(linesOutsideBody(*f.layout) == 0, "重新断行之后仍然没有一行越过正文窗口");
}

/*!
 * 段落的排版意愿：段前分页 / 段中不分页。
 *
 * 它们是**富文本层的段落属性**（存在块格式里），由渲染层在执行分页时兑现 ——
 * 这正是两层分工的样子：段落说"我要从新的一页开始"，渲染层负责把它放上去。
 */
void checkParagraphPolicies()
{
    Fixture f;
    fillParagraphs(&f.doc, 60);

    // 找一个"本来不在页首"的段落（它才测得出段前分页）
    int target = -1;
    int firstLineIndex = -1;
    for (int i = 0; i < f.layout->lineCount(); ++i) {
        const int blockNumber = f.layout->lineBlockNumber(i);
        if (blockNumber <= 0 || f.layout->lineNumberInBlock(i) != 0)
            continue;
        const QRectF body = f.metrics().bodyRect(f.layout->pageOfLine(i));
        if (f.layout->lineRect(i).top() > body.top() + kEps) {
            target = blockNumber;
            firstLineIndex = i;
            break;
        }
    }
    CHECK(target > 0, "找到一个本来不在页首的段落（用例本身要有量）");
    if (target < 0)
        return;

    const int pageBefore = f.layout->pageOfLine(firstLineIndex);
    QTextBlock block = f.doc.findBlockByNumber(target);
    QTextBlockFormat format = block.blockFormat();
    format.setPageBreakPolicy(QTextBlockFormat::PageBreak_AlwaysBefore);
    QTextCursor cursor(block);
    cursor.setBlockFormat(format);

    int afterIndex = -1;
    for (int i = 0; i < f.layout->lineCount(); ++i) {
        if (f.layout->lineBlockNumber(i) == target && f.layout->lineNumberInBlock(i) == 0) {
            afterIndex = i;
            break;
        }
    }
    CHECK(afterIndex >= 0, "设了段前分页之后，这一段还在文档里");
    if (afterIndex < 0)
        return;

    const int pageAfter = f.layout->pageOfLine(afterIndex);
    const QRectF body = f.metrics().bodyRect(pageAfter);
    CHECK(pageAfter == pageBefore + 1, "段前分页：这一段搬到了新的一页");
    CHECK(qAbs(f.layout->lineRect(afterIndex).top() - body.top()) < kEps,
          "它正好从新一页正文窗口的顶部开始");

    // 段中不分页：整段在一页里放得下时，绝不允许被拆到两页上
    QTextCursor whole(&f.doc);
    whole.select(QTextCursor::Document);
    QTextBlockFormat keep;
    keep.setProperty(tripaformat::kKeepLinesTogether, true);
    whole.mergeBlockFormat(keep);
    int split = 0;
    QHash<int, int> pagesOfBlock;
    for (int i = 0; i < f.layout->lineCount(); ++i) {
        const int blockNumber = f.layout->lineBlockNumber(i);
        const int page = f.layout->pageOfLine(i);
        if (pagesOfBlock.contains(blockNumber) && pagesOfBlock.value(blockNumber) != page) {
            // 整段放不进一整页时允许跨页，否则就是违规
            if (f.layout->blockBoundingRect(f.doc.findBlockByNumber(blockNumber)).height()
                <= f.metrics().bodyHeightPx() + kEps)
                ++split;
        }
        pagesOfBlock.insert(blockNumber, page);
    }
    CHECK(split == 0, "段中不分页：放得下的段落一个都没被拆到两页上");
}

/*!
 * 行边距（行距）真的按倍数把行推开 —— 它是段落属性，兑现的是渲染层。
 */
void checkLineSpacing()
{
    Fixture f;
    f.doc.setPlainText(QStringLiteral("第一行\n第二行"));
    const double single = f.layout->lineRect(1).top() - f.layout->lineRect(0).top();
    CHECK(single > 1.0, "单倍行距量得出一个行距");

    QTextCursor cursor(&f.doc);
    cursor.select(QTextCursor::Document);
    QTextBlockFormat format;
    format.setLineHeight(200, QTextBlockFormat::ProportionalHeight);
    cursor.mergeBlockFormat(format);

    const double doubled = f.layout->lineRect(1).top() - f.layout->lineRect(0).top();
    CHECK(qAbs(doubled - 2.0 * single) < 1.0,
          QStringLiteral("2 倍行距：行距 %1 -> %2（正好翻倍）").arg(single, 0, 'f', 1).arg(doubled, 0, 'f', 1));
}

/*!
 * 富文本层自己的活：段落格式（含"段中不分页 / 孤行控制"这两项自定义属性）、
 * 字边距、逐字效果数据，都能原样存回来。
 */
void checkRichDocument()
{
    RichDocument rich;
    QTextCursor cursor(rich.text());
    cursor.insertText(QStringLiteral("第一段文字"));

    ParagraphFormat format;
    format.alignment = Qt::AlignCenter;
    format.lineSpacingRule = ParagraphFormat::DoubleLine;
    format.keepLinesTogether = true;
    format.widowControl = true;
    rich.applyParagraphFormat(&cursor, format);

    double charWidth = 0.0;
    const ParagraphFormat back = rich.paragraphFormat(rich.text()->firstBlock(), &charWidth);
    CHECK(back.alignment == Qt::AlignCenter, "段落对齐往返");
    /*!
     * 2 倍行距存的是“比例行高 200%”，读回来是「多倍行距 2.0」——
     * 这两者是同一个东西的两种叫法（段落对话框里也是这么对映的）。
     */
    CHECK(back.lineSpacingRule == ParagraphFormat::MultipleLines
              && qAbs(back.lineSpacingValue - 2.0) < 0.01,
          QStringLiteral("段落行距往返（2 倍 -> 多倍行距 %1）").arg(back.lineSpacingValue, 0, 'f', 2));
    CHECK(back.keepLinesTogether, "段中不分页往返（QTextBlockFormat 没有这个字段，用自定义属性存）");
    CHECK(back.widowControl, "孤行控制往返（同上）");

    // 字边距（字距）
    rich.setCharSpacingPercent(&cursor, 150.0);
    CHECK(qAbs(rich.charSpacingPercent(rich.text()->firstBlock()) - 150.0) < 0.01,
          "字边距（字距 150%）往返");

    // 逐字效果数据（手写 / 抖动）也归富文本层管
    EffectStyle style;
    style.kind = EffectKind::Distortion;
    style.seed = 4242u;
    rich.setEffect(0, 3, style);
    CHECK(rich.effectAt(0) == style, "逐字效果数据（抖动种子）往返");
    const int effectChars = rich.effectCharCount();
    CHECK(effectChars == 3,
          QStringLiteral("带效果的字符数统计正确（实测 %1，应为 3）").arg(effectChars));
    rich.clearAllEffects();
    CHECK(!rich.effectAt(0).isValid(), "清效果只清数据，正文一个字不动");
    CHECK(rich.plainText() == QStringLiteral("第一段文字"), "清效果之后正文没变");
}

/*!
 * 边打字边量：**用户就是这么写出 bug 的**。
 *
 * 每插几个字就把两条硬规矩核一遍（不越界、不和别的页重复），
 * 这样"写着写着才出问题"这类 bug 不会再溜过去。
 */
void checkTypingSession()
{
    Fixture f;
    QTextCursor cursor(&f.doc);

    int violations = 0;
    int duplicates = 0;
    int maxPages = 1;

    for (int i = 0; i < 1200; ++i) {
        cursor.insertText(QStringLiteral("字"));
        if (i % 14 == 13)
            cursor.insertBlock(); // 偶尔回车：正是"写着写着"的节奏

        if (i % 6 != 0)
            continue; // 抽样检查，跑得快一点

        const int pages = f.layout->pageCount();
        maxPages = qMax(maxPages, pages);
        violations += linesOutsideBody(*f.layout);

        const QStringList perPage = textPerPage(f.doc, *f.layout);
        QSet<QString> seen;
        for (const QString &text : perPage) {
            const QString squeezedText = squeezed(text);
            if (squeezedText.isEmpty())
                continue;
            if (seen.contains(squeezedText))
                ++duplicates;
            seen.insert(squeezedText);
        }
    }

    CHECK(maxPages >= 2, QStringLiteral("打了 1200 个字，排出了 %1 页（用例本身要有量）").arg(maxPages));
    CHECK(violations == 0, QStringLiteral("边打字边量：一次都没出现「字跑到正文窗口外面」"));
    CHECK(duplicates == 0, QStringLiteral("边打字边量：一次都没出现「两页内容一样」"));
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    qInfo().noquote() << "== 分页排版（渲染层）自检";

    checkOnDemandPaging();
    checkBoundariesAndUniqueness();
    checkViewBottom();
    checkHitTestAndCaret();
    checkChrome();
    checkPaintMatchesGeometry();
    checkReflowAfterFontChange();
    checkParagraphPolicies();
    checkLineSpacing();
    checkRichDocument();
    checkTypingSession();

    if (g_failures == 0) {
        qInfo().noquote() << "test_layout: 全部通过";
        return 0;
    }
    qWarning().noquote() << "test_layout: 失败" << g_failures << "项";
    return 1;
}
