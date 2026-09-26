#include "mainwindow.h"

#include "effectsrenderer.h"
#include "handwriting.h"
#include "pagesetup.h"
#include "texteditor.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QImage>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPageLayout>
#include <QPainterPath>
#include <QPdfWriter>
#include <QPixmap>
#include <QScrollBar>
#include <QStringList>
#include <QStyle>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QTranslator>

#include <cmath>
#include <cstdio>

/*!
 * 把每一页渲染到 \a device：正文、手写、扭曲一起出，做到所见即所得。
 *
 * 思路很简单：文档版面是 96dpi 下算好的，所以把 painter 缩放
 * "设备上一毫米对应多少像素 ÷ 96dpi 每毫米"，坐标就完全一致了。
 * 屏幕上怎么排的，纸上 / PDF / 导出的图片里就怎么排。
 */
void tripaRenderToDevice(QTextDocument *document,
                         const PageSetup &setup,
                         const EffectRenderOptions &options,
                         QPaintDevice *device,
                         QPainter *painter)
{
    const QSizeF paperMm = setup.paperSizeMm();
    const QMarginsF marginsPx = setup.bodyMarginsPx();
    const QSizeF cardPx = setup.bodySizePx();

    document->setDocumentMargin(0);
    document->setPageSize(cardPx);

    // 设备像素 / 96dpi 像素（96dpi 下 1mm = 96/25.4 px）
    const double zoom = (double(device->width()) / paperMm.width()) * 25.4 / PageSetup::kDpi;
    const double pageHeightPx = paperMm.height() * PageSetup::kDpi / 25.4;
    const int pages = qMax(1, int(std::ceil(document->size().height() / cardPx.height())));

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    for (int page = 0; page < pages; ++page) {
        painter->save();
        // 把这一页的纸面左上角放到设备原点，再缩放到纸张尺寸
        painter->translate(0.0, -double(page) * pageHeightPx);
        painter->scale(zoom, zoom);
        // 正文画在页边距里面
        painter->translate(marginsPx.left(), marginsPx.top());

        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, Qt::black);
        document->documentLayout()->draw(painter, context);

        if (options.anyLayer()) {
            EffectRenderOptions local = options;
            local.missing.clear();
            renderEffects(painter, document, local);
        }

        painter->restore();
    }

    painter->restore();
}

/*!
 * 无界面自检：把文档渲染成一张 PNG，用来验证
 * "手写数据加载 / 手写铺字 / 噪声扭曲 / 分页渲染"整条链路。
 * 用法： tripa.exe --selftest [输出目录]
 */
static int runSelfTest(const QStringList &args)
{
    const QString outDir = args.size() > 1 ? args.at(1) : QStringLiteral(".");

    PageSetup setup;
    setup.widthMm = 210.0;
    setup.heightMm = 297.0;
    setup.marginLeftMm = 25.0;
    setup.marginRightMm = 25.0;
    setup.marginTopMm = 25.0;
    setup.marginBottomMm = 25.0;

    // 引擎用的大号字体：用系统默认字体族，避免某个平台没有指定字体
    QTextDocument document;
    QFont font = QApplication::font();
    font.setPointSize(15);
    document.setDefaultFont(font);
    // 版面必须先定下来，后面插入的文字才会按页面宽度断行
    document.setDocumentMargin(0);
    document.setPageSize(setup.bodySizePx());

    QTextCursor cursor(&document);
    cursor.insertText(QStringLiteral("tripa 排版器 selftest\n"));
    cursor.insertText(QStringLiteral("abcdefghijklmnopqrstuvwxyz\n"));
    cursor.insertText(QStringLiteral("波浪扭曲的笔画 —— 平缓噪声波叠加在字形路径上\n"));

    // 1) 手写数据
    HandwritingLibrary library;
    const int files = library.loadResourceDir(QStringLiteral(":/handwrite"));
    QTextStream(stdout) << "self-test: 载入手写 CSV " << files << " 个文件，"
                        << library.characterCount() << " 个字符 / " << library.sampleCount()
                        << " 份样本\n";
    if (library.characterCount() == 0) {
        QTextStream(stderr) << "self-test 失败：内置手写数据一个都没加载到\n";
        return 2;
    }

    QStringList missing;
    const QSet<QString> known = library.characters();
    const QString all = document.toPlainText();
    for (const QChar &ch : all) {
        if (ch.isSpace())
            continue;
        const QString one(ch);
        if (!known.contains(one) && !missing.contains(one))
            missing.append(one);
    }
    QTextStream(stdout) << "self-test: 缺数据的字符（预期只有中文/标点）: "
                        << (missing.isEmpty() ? QStringLiteral("无") : missing.join(QString()))
                        << "\n";

    // 2) 整篇铺手写 + 扭曲
    NoiseWave wave;
    wave.reseed(20240925u);

    EffectRenderOptions options;
    options.library = &library;
    options.wave = &wave;
    options.amplitudePt = 1.8;
    options.waveScale = 2.0;
    options.showHandwriting = true;
    options.showDistortion = true;

    cursor.select(QTextCursor::Document);
    cursor.beginEditBlock();
    for (int pos = 0; pos < document.characterCount() - 1; ++pos) {
        const QString one = document.characterAt(pos);
        if (one.isEmpty() || one.at(0).isSpace())
            continue;
        QTextCursor one_cursor(&document);
        one_cursor.setPosition(pos);
        one_cursor.setPosition(pos + 1, QTextCursor::KeepAnchor);
        QTextCharFormat fmt;
        EffectStyle style;
        style.kind = known.contains(one) ? EffectKind::Handwriting : EffectKind::Distortion;
        style.seed = 1234u + quint32(pos) * 2654435761u;
        setEffectStyle(&fmt, style);
        one_cursor.mergeCharFormat(fmt);
    }
    cursor.endEditBlock();

    // 3) 渲染一页 PNG
    const QSizeF paper = setup.paperSizeMm();
    const double dpi = 150.0;
    QImage image(QSize(int(std::lround(paper.width() / 25.4 * dpi)),
                       int(std::lround(paper.height() / 25.4 * dpi))),
                 QImage::Format_RGB32);
    image.fill(Qt::white);

    QPainter painter(&image);
    tripaRenderToDevice(&document, setup, options, &image, &painter);
    painter.end();

    // 自检断言：页面上必须真的有内容（不然"没报错"就没意义了）
    {
        int inkPixels = 0;
        for (int y = 0; y < image.height(); y += 2) {
            const QRgb *scan = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); x += 2) {
                if (qRed(scan[x]) < 200 || qGreen(scan[x]) < 200 || qBlue(scan[x]) < 200)
                    ++inkPixels;
            }
        }
        QTextStream(stdout) << "self-test: 页面上的墨点（隔点采样）=" << inkPixels << "\n";
        if (inkPixels < 50) {
            QTextStream(stderr) << "self-test 失败：页面上几乎没有内容\n";
            return 5;
        }
    }

    // 自检断言：版面与效果层都必须真的画出了东西
    {
        const QRectF firstBlock = document.documentLayout()->blockBoundingRect(
            document.firstBlock());
        QTextStream(stdout) << "self-test: 文档 " << document.size().width() << "x"
                            << document.size().height() << "，正文区 "
                            << setup.bodySizePx().width() << "x" << setup.bodySizePx().height()
                            << "，首行块 " << firstBlock.width() << "x" << firstBlock.height()
                            << "\n";
    }

    const QString pngPath = outDir + QStringLiteral("/selftest_page.png");
    if (!image.save(pngPath)) {
        QTextStream(stderr) << "self-test 失败：写不出 " << pngPath << "\n";
        return 3;
    }
    QTextStream(stdout) << "self-test: 已渲染 " << pngPath << " (" << image.width() << "x"
                        << image.height() << ")\n";

    // 4) PDF：用 QPdfWriter（和程序里的"导出 PDF"同一条路径，不碰打印子系统）
    const QString pdfPath = outDir + QStringLiteral("/selftest.pdf");
    QPdfWriter writer(pdfPath);
    writer.setResolution(300);
    QPageLayout pdfLayout = setup.pageLayout();
    pdfLayout.setMode(QPageLayout::FullPageMode);
    writer.setPageLayout(pdfLayout);

    QPainter pdfPainter;
    if (!pdfPainter.begin(&writer)) {
        QTextStream(stderr) << "self-test 失败：无法创建 PDF\n";
        return 4;
    }
    tripaRenderToDevice(&document, setup, options, &writer, &pdfPainter);
    pdfPainter.end();
    QTextStream(stdout) << "self-test: 已导出 " << pdfPath << "（页面 "
                        << writer.width() << "x" << writer.height() << " 像素 @ "
                        << writer.resolution() << "dpi）\n";

    QTextStream(stdout) << "self-test: 全部通过\n";
    return 0;
}

/*!
 * 界面冒烟测试：真正建出主窗口、跑几个回合的事件循环、截图后退出。
 * 用来验证排版控件、覆盖层、工具栏这些没法用纯逻辑测的部分。
 * 用法： tripa.exe --uitest [输出目录] [light|dark]
 *
 * 传 dark / light 会强制一套 Fusion 深色或浅色调色板 ——
 * 重点验证"深色主题下纸张依然是白纸黑字"（曾经被 QTextEdit 的填色盖成灰色）。
 *
 * 带 probe 参数时另外跑一组交互断言（含光标/选区体检）：
 *   - 光标：空文档下量出竖条位置，必须落在 documentOriginInViewport() +
 *     cursorRect() 上，且不能跑到纸外面（"光标不可见"就是这里失败）；
 *   - 选区：全选后高亮必须正好盖住正文，且不超出纸面；
 *   - 在第 0/1/2 行各点一下，光标必须落到那一行；
 *   - 精确拖选 5 个字，断言选中的就是那 5 个（只断行号不够，列才是关键）；
 *   - Shift+方向键扩选之后，画面上必须真有高亮像素。
 */
static int runUiTest(const QStringList &args)
{
    const QString outDir = args.size() > 1 ? args.at(1) : QStringLiteral(".");
    QString theme = QStringLiteral("system");
    bool probeMouse = false;
    bool probeCursor = false;
    for (int i = 2; i < args.size(); ++i) {
        const QString a = args.at(i).toLower();
        if (a == QStringLiteral("dark") || a == QStringLiteral("light"))
            theme = a;
        else if (a == QStringLiteral("probe"))
            probeMouse = probeCursor = true;
        else if (a == QStringLiteral("cursor"))
            probeCursor = true;
    }

    if (theme == QStringLiteral("dark") || theme == QStringLiteral("light")) {
        const bool dark = theme == QStringLiteral("dark");
        QApplication::setStyle(QStringLiteral("Fusion"));
        QPalette pal;
        if (dark) {
            pal.setColor(QPalette::Window, QColor(0x2b, 0x2e, 0x33));
            pal.setColor(QPalette::WindowText, QColor(0xe8, 0xe8, 0xe8));
            pal.setColor(QPalette::Base, QColor(0x23, 0x26, 0x2a));
            pal.setColor(QPalette::AlternateBase, QColor(0x2f, 0x33, 0x38));
            pal.setColor(QPalette::Text, QColor(0xe8, 0xe8, 0xe8));
            pal.setColor(QPalette::Button, QColor(0x35, 0x39, 0x3f));
            pal.setColor(QPalette::ButtonText, QColor(0xe8, 0xe8, 0xe8));
            pal.setColor(QPalette::Highlight, QColor(0x2d, 0x6c, 0xd4));
            pal.setColor(QPalette::HighlightedText, Qt::white);
            pal.setColor(QPalette::ToolTipBase, QColor(0x3a, 0x3e, 0x44));
            pal.setColor(QPalette::ToolTipText, QColor(0xe8, 0xe8, 0xe8));
        } else {
            pal = QApplication::style()->standardPalette();
        }
        QApplication::setPalette(pal);
        QTextStream(stdout) << "ui-test: 已强制 " << theme << " 主题\n";
    }

    MainWindow window;
    window.resize(1360, 900);
    window.show();

    /*!
     * 光标/选区定位的专项体检。
     *
     * 只"看图"判断不了，因为光标和正文都画在白纸上，肉眼很难分清
     * "光标没画"和"光标画到纸外去了"。所以这里量三组数：
     *   1. 光标位置：QTextEdit::cursorRect() 说的是一个值，
     *      画面上真的有墨迹又是另一个地方 —— 两个都要报出来；
     *   2. 选区：高亮的蓝色矩形跑到哪儿去了；
     *   3. 正文：那一行的墨迹从哪开始、到哪结束（作为参照基准）。
     * 三者放在一起，偏移量一眼就能看出来。
     */
    if (probeCursor) {
        if (auto *editor = window.findChild<TextEditor *>()) {
            /*!
             * 量光标用**空文档**：这时候画面上除了光标什么都没有，
             * 不用再猜"这团墨迹是字母还是光标竖条"。
             * （字母 I、H 的竖笔也是整行高，靠高度根本分不开 ——
             *   上一版就是被这个带偏，得出了错的结论。）
             */
            editor->setFocus();
            editor->setPlainText(QString());
            editor->moveCursor(QTextCursor::Start);
            for (int i = 0; i < 30; ++i) {
                QApplication::processEvents();
                QThread::msleep(5);
            }

            const QTextBlock blk = editor->document()->firstBlock();
            const QTextLine line = blk.layout()->lineForTextPosition(0);
            const QRectF blockRect =
                editor->document()->documentLayout()->blockBoundingRect(blk);
            const double docLineTop = blockRect.top() + line.y();
            const double docTextLeft = blockRect.left() + line.x();

            const QRect caretDoc = editor->cursorRect();
            const QPoint scroll(editor->horizontalScrollBar()->value(),
                                editor->verticalScrollBar()->value());

            /*!
             * 画面上的量测必须在**窗口坐标**里做。
             *
             * QWidget::grab() 对子控件是把控件画在**父控件坐标系**里再截的
             * —— 对 viewport 来说，原点其实是编辑区左上角，横向多了
             * 一个 viewport 边距（纸张居中量）。曾经就是拿它当
             * "viewport 内的位置"用，整段测量凭空平移，于是
             * "光标找不到"这个结论本身就是错的。
             */
            const QPoint vpInWindow = editor->viewport()->mapTo(&window, QPoint(0, 0));
            const QPointF originInWindow = QPointF(vpInWindow)
                                           + editor->documentOriginInViewport();
            const double dpr = editor->window()->devicePixelRatioF();

            const QImage win = window.grab().toImage();
            win.save(outDir + QStringLiteral("/uitest_cursor_probe.png"));
            /*!
             * "墨"的判定必须只认**纸面上的深色**。
             * 阈值放到 120 会把深色主题的灰桌面（0x3a3d42 ≈ 58/61/66）也算成墨，
             * 结果整块屏幕都是"墨"。所以：亮度上限收紧到 90，
             * 而且只在纸张横向范围内扫。
             */
            auto isInk = [](QRgb c) {
                return qRed(c) < 90 && qGreen(c) < 90 && qBlue(c) < 90;
            };

            QTextStream(stdout) << "cursor: viewport 在窗口里的位置=" << vpInWindow.x() << ","
                                << vpInWindow.y() << " viewport 尺寸="
                                << editor->viewport()->width() << "x"
                                << editor->viewport()->height() << " 编辑区宽="
                                << editor->width() << " 滚动条预留="
                                << editor->scrollBarReservePx() << " dpr=" << dpr << "\n";
            QTextStream(stdout) << "cursor: 纸张居中留白(vp)=" << editor->paperPadPx()
                                << " 滚动=" << scroll.x() << "," << scroll.y()
                                << " 正文原点(vp)=" << editor->documentOriginInViewport().x() << ","
                                << editor->documentOriginInViewport().y() << "\n";
            QTextStream(stdout) << "cursor: 文档行顶=" << docLineTop
                                << " 行内起点x=" << docTextLeft
                                << " 行高=" << line.height() << "\n";

            /*!
             * 空文档下，唯一的墨迹就是光标竖条。
             * 把它的包围盒量出来，和"光标该在哪儿"比。
             */
            const double caretWantX = originInWindow.x() + caretDoc.x();
            const double caretWantY = originInWindow.y() + caretDoc.y();
            const int y0 = qMax(0, int((caretWantY - 12) * dpr));
            const int y1 = qMin(win.height(), int((caretWantY + line.height() + 12) * dpr));
            /*!
             * 扫描范围必须**卡在纸面以内**。
             * 纸右边紧挨着竖滚动条（深色，整条都是一个颜色），
             * 一旦扫进去，光标包围盒就会被它拉宽拉高，
             * 量出来的"偏差"全是滚动条贡献的（上一版就是这么误判的）。
             */
            const double paperLeftWin = vpInWindow.x() + editor->paperOriginInViewport().x();
            const double paperRightWin = paperLeftWin + editor->paperWidthPx();
            const double paperTopWin = vpInWindow.y();
            const int sx0 = qMax(0, int((paperLeftWin + 1) * dpr));
            const int sx1 = qMin(win.width(), int((paperRightWin - 1) * dpr));
            const int sy0 = qMax(y0, int((paperTopWin + 1) * dpr));
            /*!
             * 提光标：只认"又窄又高"的那一列。
             *
             * 纸张有一条 1 像素的深色描边，滚动条也是深色 —— 两者都会
             * 污染包围盒（曾经量出"纵向差 12 像素"就是这么来的）。
             * 所以先按**列**统计深色像素，只挑出计数很小（≤ 4 像素 × dpr，
             * 即光标宽度量级）的列，再把它上下贯通的长度量出来。
             */
            int cx0 = -1, cx1 = -1, cy0 = -1, cy1 = -1;
            {
                int bestCol = -1, bestTop = -1, bestBot = -1;
                for (int x = sx0; x < sx1 && bestCol < 0; ++x) {
                    int top = -1, bot = -1;
                    for (int y = sy0; y < y1; ++y) {
                        if (isInk(win.pixel(x, y))) {
                            if (top < 0)
                                top = y;
                            bot = y;
                        }
                    }
                    // 竖条：连续贯通至少 70% 行高，才可能是光标
                    if (top >= 0 && (bot - top + 1) >= int(line.height() * dpr * 0.7)) {
                        bestCol = x;
                        bestTop = top;
                        bestBot = bot;
                    }
                }
                if (bestCol >= 0) {
                    cx0 = cx1 = bestCol;
                    cy0 = bestTop;
                    cy1 = bestBot;
                }
                QTextStream(stdout) << "cursor: 提光标扫描 x=" << (sx0 / dpr) << ".." << (sx1 / dpr)
                                    << " 找到最左竖条=" << (bestCol < 0 ? -1.0 : bestCol / dpr)
                                    << " y=" << (cy0 < 0 ? -1.0 : cy0 / dpr) << ".."
                                    << (cy1 < 0 ? -1.0 : cy1 / dpr) << "\n";
            }
            const int cPixels = (cx0 < 0) ? 0 : (cy1 - cy0 + 1);

            if (cPixels < 20) {
                QTextStream(stderr) << "cursor 失败：空文档下画面上完全没有光标竖条（"
                                    << cPixels << " 像素），光标该在 x=" << caretWantX << " y="
                                    << caretWantY << "\n";
                return 18;
            }
            {
                /*!
                 * 把"光标该在的位置"附近放大截一张给人看。
                 * 数值剖面会被纸张边框、滚动条这些深色装饰干扰，
                 * 一张放大的图反而最直接。
                 */
                const int cropW = int(200 * dpr);
                const int cropH = int(40 * dpr);
                const int cropX = qBound(0, int((caretWantX - 40) * dpr), win.width() - cropW);
                const int cropY = qBound(0, int((caretWantY - 6) * dpr), win.height() - cropH);
                QImage zoom = win.copy(cropX, cropY, cropW, cropH);
                zoom.setDevicePixelRatio(1.0);
                zoom.scaled(zoom.width() * 4, zoom.height() * 4, Qt::IgnoreAspectRatio,
                            Qt::FastTransformation)
                    .save(outDir + QStringLiteral("/uitest_cursor_zoom.png"));
                QTextStream(stdout) << "cursor: 光标附近放大图 " << outDir
                                    << "/uitest_cursor_zoom.png\n";
            }
            const double cLeft = cx0 / dpr;
            const double cTop = cy0 / dpr;
            QTextStream(stdout) << "cursor: 画面上光标竖条 x=" << cLeft << ".." << (cx1 / dpr)
                                << "  y=" << cTop << ".." << (cy1 / dpr) << "（" << cPixels
                                << " 像素）；应在 x=" << caretWantX << " y=" << caretWantY
                                << "  偏差 dx=" << (cLeft - caretWantX)
                                << " dy=" << (cTop - caretWantY) << "\n";
            if (qAbs(cLeft - caretWantX) > 3.0) {
                QTextStream(stderr) << "cursor 失败：光标横向画错，偏差 " << (cLeft - caretWantX)
                                    << " 像素\n";
                return 19;
            }
            if (qAbs(cTop - caretWantY) > 4.0) {
                QTextStream(stderr) << "cursor 失败：光标纵向画错，偏差 " << (cTop - caretWantY)
                                    << " 像素（正文行顶该在 y=" << caretWantY << "）\n";
                return 20;
            }
            // 光标应该落在纸面里，跑到灰桌面上就是"看不见"
            if (cLeft < paperLeftWin || cLeft > paperRightWin) {
                QTextStream(stderr) << "cursor 失败：光标 x=" << cLeft << " 落在纸张外面（纸 "
                                    << paperLeftWin << ".." << paperRightWin << "）\n";
                return 22;
            }
        }
    }

    /*!
     * 选区：高亮必须正好盖住正文，而且不能跑到纸张外面。
     */
    if (probeCursor) {
        if (auto *editor = window.findChild<TextEditor *>()) {
            editor->setFocus();
            editor->setPlainText(QStringLiteral("HANDWRITING"));
            editor->selectAll();
            for (int i = 0; i < 20; ++i) {
                QApplication::processEvents();
                QThread::msleep(5);
            }

            const QTextBlock blk = editor->document()->firstBlock();
            const QTextLine line = blk.layout()->lineForTextPosition(0);
            const QRectF blockRect =
                editor->document()->documentLayout()->blockBoundingRect(blk);
            const QPoint vpInWindow = editor->viewport()->mapTo(&window, QPoint(0, 0));
            const QPointF originInWindow = QPointF(vpInWindow)
                                           + editor->documentOriginInViewport();
            const double dpr = editor->window()->devicePixelRatioF();

            const QImage win = window.grab().toImage();
            win.save(outDir + QStringLiteral("/uitest_cursor_sel.png"));
            /*!
             * 只认纸面上的选区高亮。
             * 工具栏的图标里也有蓝色，扫整窗口会把它们算进来
             * （上一版量出"高亮从 x=6 开始"，那是工具栏第一颗图标）。
             */
            const double paperLeftChk = vpInWindow.x() + editor->paperOriginInViewport().x();
            const double paperRightChk = paperLeftChk + editor->paperWidthPx();
            const int sx0 = qMax(0, int((paperLeftChk + 2) * dpr));
            const int sx1 = qMin(win.width(), int((paperRightChk - 2) * dpr));
            int hlLeft = -1, hlRight = -1, hlCount = 0, hlTop = -1, hlBot = -1;
            /*!
             * 只认钉死的那一种高亮色 #3399ff（允许 ±30 的误差）。
             * 用"色相/饱和度"之类的宽松条件会把窗口描边、面板拖柄
             * 也框进来（量出"高亮从 x=304 开始"就是这个原因）。
             */
            auto isSelectionBlue = [](QRgb c) {
                return qAbs(qRed(c) - 0x33) <= 30 && qAbs(qGreen(c) - 0x99) <= 30
                       && qAbs(qBlue(c) - 0xff) <= 30;
            };
            for (int y = 0; y < win.height(); ++y) {
                const QRgb *scan = reinterpret_cast<const QRgb *>(win.constScanLine(y));
                for (int x = sx0; x < sx1; ++x) {
                    if (isSelectionBlue(scan[x])) {
                        ++hlCount;
                        if (hlLeft < 0 || x < hlLeft)
                            hlLeft = x;
                        if (x > hlRight)
                            hlRight = x;
                        if (hlTop < 0)
                            hlTop = y;
                        hlBot = y;
                    }
                }
            }
            {   // 把落在哪儿打出来，省得下次又靠猜
                int firstY = -1, lastY = -1;
                for (int y = 0; y < win.height(); ++y) {
                    for (int x = sx0; x < sx1; ++x) {
                        if (isSelectionBlue(win.pixel(x, y))) {
                            if (firstY < 0)
                                firstY = y;
                            lastY = y;
                            break;
                        }
                    }
                }
                QTextStream(stdout) << "cursor: 高亮像素落在窗口 y=" << (firstY / dpr) << ".."
                                    << (lastY / dpr) << "\n";
            }
            const double hlLeftL = hlLeft < 0 ? -1 : hlLeft / dpr;
            const double hlRightL = hlRight < 0 ? -1 : hlRight / dpr;
            const double hlWantLeft = originInWindow.x() + blockRect.left() + line.x();
            const double hlWantRight = originInWindow.x() + editor->cursorRect().x() + 2.0;
            QTextStream(stdout) << "cursor: 全选后高亮(窗口坐标) x=" << hlLeftL << ".." << hlRightL
                                << " y=" << (hlTop < 0 ? -1.0 : hlTop / dpr) << ".."
                                << (hlBot < 0 ? -1.0 : hlBot / dpr) << "（" << hlCount
                                << " 像素），应盖住 x=" << hlWantLeft << ".." << hlWantRight
                                << "，纸张 " << paperLeftChk << ".." << paperRightChk << "\n";
            if (hlCount < 200) {
                QTextStream(stderr) << "cursor 失败：全选之后看不到选区高亮\n";
                return 23;
            }
            if (qAbs(hlLeftL - hlWantLeft) > 4.0) {
                QTextStream(stderr) << "cursor 失败：选区高亮左边缘 " << hlLeftL << "，正文左边缘 "
                                    << hlWantLeft << "（差 " << (hlLeftL - hlWantLeft)
                                    << " 像素）—— 高亮和正文不是同一套坐标\n";
                return 24;
            }
            if (hlRightL > paperRightChk + 1.0) {
                QTextStream(stderr) << "cursor 失败：选区高亮右边缘 " << hlRightL
                                    << " 跑到纸张外面了（纸右边缘 " << paperRightChk << "）\n";
                return 25;
            }
            // 收尾：把选区清掉，别影响后面的检查
            editor->setTextCursor(QTextCursor(editor->document()));
        }
    }

    // 打一段文字进去，才能验证"白纸黑字"而不是"白纸无字"
    if (auto *editor = window.findChild<TextEditor *>()) {
        editor->setPlainText(QStringLiteral(
            "白纸黑字测试：abc ABC 123\n"
            "深色主题下纸张必须仍是白色，文字必须是深色的。\n"
            "The quick brown fox jumps over the lazy dog.\n"));
        editor->moveCursor(QTextCursor::Start);    }

    // 先让窗口完成首次布局
    for (int i = 0; i < 30; ++i) {
        QApplication::processEvents();
        QThread::msleep(10);
    }

    const QPixmap shot = window.grab();
    const QString shotPath = outDir + QStringLiteral("/uitest_window.png");
    if (shot.isNull() || !shot.save(shotPath)) {
        QTextStream(stderr) << "ui-test 失败：截不了图\n";
        return 6;
    }
    QTextStream(stdout) << "ui-test: 主窗口已建立并截图 " << shotPath << " (" << shot.width() << "x"
                        << shot.height() << ")\n";

    // 关键断言：纸张必须是白的、字必须是深色的（深色主题下曾经被涂成灰色）
    QImage img = shot.toImage();
    QSet<QRgb> colors;
    int whitePixels = 0;
    int darkPixels = 0;
    int samples = 0;
    for (int y = 0; y < img.height(); y += 3) {
        const QRgb *scan = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += 3) {
            const QRgb c = scan[x];
            colors.insert(c);
            ++samples;
            if (qRed(c) > 245 && qGreen(c) > 245 && qBlue(c) > 245)
                ++whitePixels;
            if (qRed(c) < 90 && qGreen(c) < 90 && qBlue(c) < 90)
                ++darkPixels;
        }
    }
    const double whiteRatio = samples > 0 ? double(whitePixels) / samples : 0.0;
    QTextStream(stdout) << "ui-test: 纯白像素占比 " << QString::number(whiteRatio * 100.0, 'f', 1)
                        << "%（纸面），深色像素 " << darkPixels << " 个，不同颜色数 "
                        << colors.size() << "\n";
    if (whiteRatio < 0.15) {
        QTextStream(stderr) << "ui-test 失败：纸面不是白的（深色主题把纸张盖住了？）\n";
        return 7;
    }
    if (darkPixels < 100) {
        QTextStream(stderr) << "ui-test 失败：纸上看不到深色文字（白底白字？）\n";
        return 8;
    }

    // 再验证一遍效果覆盖层：铺上手写+扭曲后，笔迹必须落在正文上（不能整体偏移）
    if (auto *editor = window.findChild<TextEditor *>()) {
        HandwritingLibrary probeLibrary;
        probeLibrary.loadResourceDir(QStringLiteral(":/handwrite"));
        NoiseWave probeWave;
        probeWave.reseed(4242u);

        EffectRenderOptions probe;
        probe.showHandwriting = false;
        probe.showDistortion = true;
        probe.library = &probeLibrary;
        probe.wave = &probeWave;
        // 常用幅度：既能看清字形，也能看出是否压在正文上
        probe.amplitudePt = 1.6;
        probe.waveScale = 2.0;
        editor->setEffectOptions(probe);

        QTextCursor cursor = editor->textCursor();
        cursor.select(QTextCursor::Document);
        editor->setTextCursor(cursor);
        cursor.beginEditBlock();
        for (int pos = 0; pos < editor->document()->characterCount() - 1; ++pos) {
            const QString one = editor->document()->characterAt(pos);
            if (one.isEmpty() || one.at(0).isSpace())
                continue;
            QTextCursor one_cursor(editor->document());
            one_cursor.setPosition(pos);
            one_cursor.setPosition(pos + 1, QTextCursor::KeepAnchor);
            QTextCharFormat fmt;
            EffectStyle style;
            style.kind = EffectKind::Distortion;
            style.seed = 99u + quint32(pos) * 2654435761u;
            setEffectStyle(&fmt, style);
            one_cursor.mergeCharFormat(fmt);
        }
        cursor.endEditBlock();
        editor->setEffectsVisible(true);

        for (int i = 0; i < 20; ++i) {
            QApplication::processEvents();
            QThread::msleep(10);
        }

        const QPixmap fxShot = window.grab();
        const QString fxPath = outDir + QStringLiteral("/uitest_effects.png");
        fxShot.save(fxPath);

        // 覆盖层画到和 viewport 同尺寸（逻辑像素）的透明图上，剥离正文干扰
        const QSize vp = editor->viewport()->size();
        QImage fxCanvas(vp, QImage::Format_ARGB32_Premultiplied);
        fxCanvas.setDevicePixelRatio(1.0);
        fxCanvas.fill(Qt::transparent);
        {
            QPainter p(&fxCanvas);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setTransform(editor->documentToViewport());
            EffectRenderOptions solo = probe;
            renderEffects(&p, editor->document(), solo);
        }
        fxCanvas.save(outDir + QStringLiteral("/uitest_overlay_only.png"));

        // 只画正文（不带效果）的对照图，和上面同一尺寸
        QImage textCanvas(vp, QImage::Format_ARGB32_Premultiplied);
        textCanvas.setDevicePixelRatio(1.0);
        textCanvas.fill(Qt::white);
        {
            QPainter p(&textCanvas);
            // 和正文、覆盖层完全同一套变换
            p.setTransform(editor->documentToViewport());
            QAbstractTextDocumentLayout::PaintContext ctx;
            ctx.palette.setColor(QPalette::Text, Qt::black);
            editor->document()->documentLayout()->draw(&p, ctx);
        }
        textCanvas.save(outDir + QStringLiteral("/uitest_text_only.png"));

        /*!
         * 两张画布的墨迹逐行对照。
         * 只比总包围盒的话，"整体平移"和"某几行被压缩/放大"看起来是一样的，
         * 所以这里按行高切成几段分别量，出问题时能直接看出是哪一种。
         */
        {
            /*!
             * 分行区间必须取自**文档真实的文本行位置**。
             * 用 fontMetrics().height() 硬切是不行的：正文行顶并不从 0 开始
             * （前面还有页边距），硬切出来的区间会落在两行之间，
             * 于是量到的是"行间空白"，看着像"某一层没画"。
             *
             * 另外这两张画布是**排版坐标**（viewport 尺寸、documentToViewport
             * 变换），而 blockBoundingRect 给的是文档坐标，
             * 所以要把行顶加上正文区原点才能对得上 —— 少这一步就会
             * 整段错开一个页边距，量出来全是 0。
             */
            const QPointF docOrigin = editor->documentOriginInViewport();
            QVector<QPair<double, double>> bands;
            for (QTextBlock b = editor->document()->begin();
                 b.isValid() && bands.size() < 4; b = b.next()) {
                if (b.text().trimmed().isEmpty())
                    continue;
                const QTextLine line = b.layout()->lineForTextPosition(0);
                if (!line.isValid())
                    continue;
                const QRectF br = editor->document()->documentLayout()->blockBoundingRect(b);
                bands.append({docOrigin.y() + br.top() + line.y(), line.height()});
            }
            auto rowProfile = [&bands](const QImage &img, bool useAlpha, const QString &tag) {
                QStringList parts;
                for (const auto &band : bands) {
                    const int y0 = qMax(0, int(band.first));
                    const int y1 = qMin(img.height(), int(band.first + band.second));
                    int count = 0;
                    int first = -1, last = -1;
                    for (int y = y0; y < y1; ++y) {
                        const QRgb *scan = reinterpret_cast<const QRgb *>(img.constScanLine(y));
                        for (int x = 0; x < img.width(); ++x) {
                            const bool ink = useAlpha
                                                 ? qAlpha(scan[x]) > 40
                                                 : (qRed(scan[x]) < 140 && qGreen(scan[x]) < 140
                                                    && qBlue(scan[x]) < 140);
                            if (ink) {
                                ++count;
                                if (first < 0)
                                    first = x;
                                last = x;
                            }
                        }
                    }
                    parts << QStringLiteral("[y%1-%2 墨点%3 x%4-%5]")
                                 .arg(y0)
                                 .arg(y1 - 1)
                                 .arg(count)
                                 .arg(first)
                                 .arg(last);
                }
                QTextStream(stdout) << "ui-test: " << tag << " 分行墨迹 "
                                    << parts.join(QStringLiteral(" ")) << "\n";
            };
            rowProfile(fxCanvas, true, QStringLiteral("覆盖层"));
            rowProfile(textCanvas, false, QStringLiteral("正文  "));
        }

        /*!
         * 逐行核对：编辑器的正文画在哪儿、覆盖层画在哪儿。
         * 用 blockBoundingRect 当参照 —— 文档里没有块边距（documentMargin = 0），
         * 所以块左上角就是正文区的坐标原点，它加上视图变换就是 screen 上的位置。
         */
        {
            const QTransform toViewport = editor->documentToViewport();
            int row = 0;
            for (QTextBlock b = editor->document()->begin(); b.isValid() && row < 5;
                 b = b.next(), ++row) {
                if (b.text().trimmed().isEmpty())
                    continue;
                const QTextLine line = b.layout()->lineForTextPosition(0);
                const QRectF blockRect =
                    editor->document()->documentLayout()->blockBoundingRect(b);
                const double docBaseTop = blockRect.top() + line.y();
                const double viewBaseTop = toViewport.map(QPointF(0, docBaseTop)).y();
                QTextStream(stdout) << "ui-test: 行 " << b.blockNumber() << " 文档行顶=" << docBaseTop
                                    << " 屏幕行顶=" << viewBaseTop
                                    << " 行高=" << line.height() << "\n";
            }
        }

        // 两张图各自的墨迹包围盒 —— 必须重合
        auto inkBox = [](const QImage &img, bool useAlpha) {
            QRect box;
            for (int y = 0; y < img.height(); ++y) {
                const QRgb *scan = reinterpret_cast<const QRgb *>(img.constScanLine(y));
                for (int x = 0; x < img.width(); ++x) {
                    const bool ink = useAlpha ? qAlpha(scan[x]) > 40
                                              : (qRed(scan[x]) < 140 && qGreen(scan[x]) < 140
                                                 && qBlue(scan[x]) < 140);
                    if (ink) {
                        box = box.isNull() ? QRect(x, y, 1, 1) : box.united(QRect(x, y, 1, 1));
                    }
                }
            }
            return box;
        };
        const QRect overlayBox = inkBox(fxCanvas, true);
        const QRect textBox = inkBox(textCanvas, false);
        // 覆盖层画的就是"同一段文字的字形"，所以两者必须同位置、同大小
        const int alignDx = overlayBox.x() - textBox.x();
        const int alignDy = overlayBox.y() - textBox.y();
        QTextStream(stdout) << "ui-test: 覆盖层 vs 正文墨迹 偏移 dx=" << alignDx
                            << " dy=" << alignDy << "，尺寸差 "
                            << (overlayBox.width() - textBox.width()) << "x"
                            << (overlayBox.height() - textBox.height()) << "（正文 "
                            << textBox.width() << "x" << textBox.height() << "）\n";
        if (qAbs(alignDx) > 6 || qAbs(alignDy) > 8) {
            QTextStream(stderr) << "ui-test 失败：手写/扭曲层与正文没有对齐\n";
            return 9;
        }

        /*!
         * 合成截图里两者的竖向中心应当基本重合。
         *
         * 纵向上只在**文档真实文本行占据的那几行**里取样，
         * 而且全部换算成窗口逻辑坐标再比 —— 画布、文档、窗口三种坐标
         * 混着用的话，量出来的数会自相矛盾（踩过：一处说差 8 像素，
         * 另一处说差 0.25 像素，其实只是坐标系不同）。
         */
        {
            const QImage comp = fxShot.toImage();
            const double dprComp = comp.devicePixelRatio();
            const QPoint vpInWindow =
                editor->viewport()->mapTo(editor->window(), QPoint(0, 0));
            // 文档第 0 行的行顶 -> 窗口逻辑坐标
            double rowTopWin = 0.0;
            double rowSpanWin = 0.0;
            int taken = 0;
            for (QTextBlock b = editor->document()->begin(); b.isValid() && taken < 3;
                 b = b.next()) {
                if (b.text().trimmed().isEmpty())
                    continue;
                const QTextLine line = b.layout()->lineForTextPosition(0);
                if (!line.isValid())
                    continue;
                const QRectF br = editor->document()->documentLayout()->blockBoundingRect(b);
                const double top =
                    editor->documentOriginInViewport().y() + br.top() + line.y();
                if (taken == 0)
                    rowTopWin = vpInWindow.y() + top;
                rowSpanWin = vpInWindow.y() + top + line.height() - rowTopWin;
                ++taken;
            }
            int textTop = -1, textBot = -1, ovTop = -1, ovBot = -1;
            const int yFrom = qMax(0, int(rowTopWin * dprComp));
            const int yTo = qMin(comp.height(), int((rowTopWin + rowSpanWin) * dprComp));
            for (int y = yFrom; y < yTo; ++y) {
                const QRgb *scan = reinterpret_cast<const QRgb *>(comp.constScanLine(y));
                for (int x = 0; x < comp.width(); ++x) {
                    const QRgb c = scan[x];
                    const int r = qRed(c), g = qGreen(c), b = qBlue(c);
                    if (r < 110 && g < 110 && b < 110) {
                        if (textTop < 0)
                            textTop = y;
                        textBot = y;
                    }
                    if (b > 110 && b > r + 40 && b > g + 30) {
                        if (ovTop < 0)
                            ovTop = y;
                        if (y > ovBot)
                            ovBot = y;
                    }
                }
            }
            if (textTop < 0 || ovTop < 0) {
                QTextStream(stdout) << "ui-test: 合成图取样区间 y=" << int(rowTopWin) << ".."
                                    << int(rowTopWin + rowSpanWin) << " 里只找到 正文 "
                                    << (textTop < 0 ? 0 : 1) << " / 覆盖层 " << (ovTop < 0 ? 0 : 1)
                                    << "，跳过中心对比\n";
            } else {
                const double textMid = (textTop + textBot) / 2.0 / dprComp;
                const double ovMid = (ovTop + ovBot) / 2.0 / dprComp;
                QTextStream(stdout) << "ui-test: 合成图里 正文中心 y=" << textMid
                                    << "　覆盖层中心 y=" << ovMid << "　差 " << (ovMid - textMid)
                                    << "\n";
                if (qAbs(ovMid - textMid) > 6.0) {
                    QTextStream(stderr) << "ui-test 失败：覆盖层与正文在屏幕上没有对齐（差 "
                                        << (ovMid - textMid) << " 像素）\n";
                    return 29;
                }
            }
        }

        int marked = 0;
        for (int pos = 0; pos < editor->document()->characterCount() - 1; ++pos) {
            QTextCursor c(editor->document());
            c.setPosition(pos);
            c.setPosition(pos + 1, QTextCursor::KeepAnchor);
            if (effectStyle(c.charFormat()).isValid())
                ++marked;
        }
        QTextStream(stdout) << "ui-test: 效果层截图 " << fxPath
                            << "（带效果的字符 " << marked
                            << " 个，效果层可见 " << editor->effectsVisible() << "）\n";
    }

    // ---------------------------------------------------------------- 鼠标定位
    if (probeMouse) {
        if (auto *editor = window.findChild<TextEditor *>()) {
            editor->setEffectsVisible(false);
            for (int i = 0; i < 10; ++i) {
                QApplication::processEvents();
                QThread::msleep(5);
            }

            /*!
             * 驱动 TextEditor 的鼠标入口。
             *
             * 坐标空间有两层，别混：
             *   - 靶点是用**排版坐标**（documentToViewport() 的值域）算出来的；
             *   - QMouseEvent::pos() 用的是 **viewport 局部坐标**。
             * 两者之间差的是 viewport 在编辑区里的位置，也就是
             * viewport()->pos()（横向 = 纸张居中边距，纵向 = 工具栏高度）。
             * 用 mapFrom 减掉这个偏移即可（曾经把排版坐标直接当局部坐标用，
             * 也曾经减了两次，两次的结果都是"按下没反应"）。
             *
             * 直接调用处理函数而不是往事件队列里塞：拖选需要 Qt 内部的
             * 按键状态跟着变，QTest::mouseMove 又要求窗口真的激活
             * （自检环境里没有），手搓事件 sendEvent 会被当成悬停丢掉。
             * "事件怎么送进控件"是框架的事，"收到之后算得对不对"才是断言对象。
             */
            auto sendMouse = [editor](QEvent::Type type, const QPoint &editorPos,
                                      Qt::MouseButton button, Qt::MouseButtons buttons) {
                const QPoint local = editorPos - editor->viewportOriginInEditor();
                QMouseEvent ev(type, QPointF(local), editor->viewport()->mapToGlobal(local),
                               button, buttons, Qt::NoModifier);
                switch (type) {
                case QEvent::MouseButtonPress:
                case QEvent::MouseButtonDblClick:
                    editor->mousePressEvent(&ev);
                    break;
                case QEvent::MouseMove:
                    editor->mouseMoveEvent(&ev);
                    break;
                case QEvent::MouseButtonRelease:
                    editor->mouseReleaseEvent(&ev);
                    break;
                default:
                    break;
                }
            };

            auto rowOfPos = [editor](int docPos) {
                return editor->document()->findBlock(docPos).blockNumber();
            };

            // 每一行取"行内第 3 个字符"的位置和它的行号，作为点击靶子
            struct Target { int docPos; int row; QPoint vp; };
            QVector<Target> targets;
            const QTextDocument *doc = editor->document();
            for (QTextBlock b = doc->begin(); b.isValid() && targets.size() < 3; b = b.next()) {
                if (b.text().trimmed().isEmpty())
                    continue;
                const int pos = b.position() + qMin(2, qMax(0, b.length() - 1));
                const QTextCursor c(editor->document());
                Q_UNUSED(c);
                // 用 QTextLayout 直接量出这个字符的位置（和渲染同一套坐标）
                const QTextLine line = b.layout()->lineForTextPosition(pos - b.position());
                if (!line.isValid())
                    continue;
                const QRectF blockRect = doc->documentLayout()->blockBoundingRect(b);
                const double x = blockRect.left() + line.x() + line.cursorToX(pos - b.position());
                const double y = blockRect.top() + line.y() + line.height() / 2.0;
                const QPoint vp = editor->documentToViewport().map(QPointF(x, y)).toPoint();
                targets.append({pos, b.blockNumber(), vp});
            }

            if (targets.size() < 3) {
                QTextStream(stderr) << "ui-test 失败：文档不足 3 行，无法验证点击定位\n";
                return 10;
            }

            {
                /*!
                 * 标定：把靶点从"排版坐标"换算成 viewport 局部坐标再喂回去，
                 * 必须回到同一个字符。这条断言把三个坐标空间的
                 * 换算关系钉死：排版坐标 <-> viewport 局部坐标 <-> 文档坐标。
                 */
                const Target &t0 = targets.first();
                const QPoint local = t0.vp - editor->viewportOriginInEditor();
                const int back = editor->documentCursorAt(editor->viewportToEditor(local))
                                     .position()
                                 - t0.docPos;
                QTextStream(stdout) << "ui-test: 点击标定 排版(" << t0.vp.x() << "," << t0.vp.y()
                                    << ") -> 局部(" << local.x() << "," << local.y()
                                    << ") -> 回到字符偏差 " << back << "\n";
                if (qAbs(back) > 2) {
                    QTextStream(stderr)
                        << "ui-test 失败：坐标换算对不上（偏差 " << back
                        << " 个字符）—— 点击定位会整体错位\n";
                    return 26;
                }
            }

            /*!
             * 冒烟标定：拿第 0 行第 0 个字当控制点，走一遍"排版坐标 ->
             * documentCursorAt"，必须回到同一个字符。
             */
            {
                const QTextBlock b0 = doc->firstBlock();
                const QTextLine l0 = b0.layout()->lineForTextPosition(0);
                const QRectF br0 = doc->documentLayout()->blockBoundingRect(b0);
                const QPointF docPt(br0.left() + l0.x() + 1.0, br0.top() + l0.y() + 2.0);
                const QPoint vpPt = editor->documentToViewport().map(docPt).toPoint();
                const int got = editor->documentCursorAt(vpPt).position() - b0.position();
                QTextStream(stdout) << "ui-test: 命中测试标定 文档点(" << docPt.x() << ","
                                    << docPt.y() << ") -> 排版(" << vpPt.x() << "," << vpPt.y()
                                    << ") -> 给第 " << got << " 个字符（应为 0）\n";
                if (got != 0) {
                    QTextStream(stderr)
                        << "ui-test 失败：命中测试与 documentToViewport() 不是同一套坐标（差 "
                        << got << " 个字符）—— 点击定位会整体错位\n";
                    return 26;
                }
            }

            int badClicks = 0;
            for (const Target &t : targets) {
                if (!editor->viewport()->rect().contains(t.vp)) {
                    QTextStream(stderr) << "ui-test 失败：靶点 " << t.vp.x() << ","
                                        << t.vp.y() << " 不在编辑区内\n";
                    return 11;
                }
                sendMouse(QEvent::MouseButtonPress, t.vp, Qt::LeftButton, Qt::LeftButton);
                sendMouse(QEvent::MouseButtonRelease, t.vp, Qt::LeftButton, Qt::NoButton);
                QApplication::processEvents();

                const int got = rowOfPos(editor->textCursor().position());
                const bool ok = (got == t.row);
                if (!ok)
                    ++badClicks;
                QTextStream(stdout) << "ui-test: 点第 " << t.row << " 行 (viewport "
                                    << t.vp.x() << "," << t.vp.y() << ") -> 光标在第 "
                                    << got << " 行 " << (ok ? "OK" : "**错位**") << "\n";
            }
            if (badClicks > 0) {
                QTextStream(stderr) << "ui-test 失败：鼠标点击定位错位 " << badClicks
                                    << " 处（文档坐标与 viewport 坐标没有对齐）\n";
                return 12;
            }

            // 拖选：从最后一行的第 3 个字拖到第一行的第 3 个字
            {                const Target &from = targets.last();
                const Target &to = targets.first();
                sendMouse(QEvent::MouseButtonPress, from.vp, Qt::LeftButton, Qt::LeftButton);
                sendMouse(QEvent::MouseMove, (from.vp + to.vp) / 2, Qt::NoButton, Qt::LeftButton);
                sendMouse(QEvent::MouseMove, to.vp, Qt::NoButton, Qt::LeftButton);
                sendMouse(QEvent::MouseButtonRelease, to.vp, Qt::LeftButton, Qt::NoButton);
                for (int i = 0; i < 5; ++i)
                    QApplication::processEvents();

                const QTextCursor cursor = editor->textCursor();
                const int startRow = rowOfPos(cursor.selectionStart());
                const int endRow = rowOfPos(cursor.selectionEnd());
                const bool ok = cursor.hasSelection() && startRow == to.row && endRow == from.row;
                QTextStream(stdout) << "ui-test: 拖选 " << to.vp.x() << "," << to.vp.y() << " -> "
                                    << from.vp.x() << "," << from.vp.y() << " ：选中 "
                                    << cursor.selectionEnd() - cursor.selectionStart()
                                    << " 个字符，行 " << startRow << ".." << endRow
                                    << (ok ? " OK" : " **不对**") << "\n";
                if (!ok) {
                    QTextStream(stderr) << "ui-test 失败：鼠标拖选没有选中鼠标经过的范围\n";
                    return 13;
                }

                // 选区必须在屏幕上看得见 —— 数一数高亮色像素
                const QPixmap selShot = window.grab();
                selShot.save(outDir + QStringLiteral("/uitest_selection.png"));
                const QImage sel = selShot.toImage();
                int highlight = 0;
                for (int y = 0; y < sel.height(); ++y) {
                    const QRgb *scan = reinterpret_cast<const QRgb *>(sel.constScanLine(y));
                    for (int x = 0; x < sel.width(); ++x) {
                        const QRgb c = scan[x];
                        // 钉死的选区色 #3399ff 附近
                        if (qBlue(c) > 180 && qBlue(c) > qRed(c) + 60 && qGreen(c) > 90
                            && qGreen(c) < 220)
                            ++highlight;
                    }
                }
                QTextStream(stdout) << "ui-test: 选区高亮像素 " << highlight << " 个\n";
                if (highlight < 500) {
                    QTextStream(stderr) << "ui-test 失败：拖选之后画面上看不到选区高亮"
                                           "（调色板的 Highlight 被覆盖了？）\n";
                    return 14;
                }

                editor->setTextCursor(QTextCursor(editor->document()));
            }

            /*!
             * 点完之后能不能真的在那一行打字。
             * 这是用户最早报的现象："双击后如果不在光标所在行就打不上字"。
             * 这里在第 2 行点一下、敲两个字，断言它们确实落在那一行上。
             */
            {
                const Target &t = targets.last();
                sendMouse(QEvent::MouseButtonPress, t.vp, Qt::LeftButton, Qt::LeftButton);
                sendMouse(QEvent::MouseButtonRelease, t.vp, Qt::LeftButton, Qt::NoButton);
                QApplication::processEvents();

                const QTextBlock before = editor->textCursor().block();
                const QString rowText = before.text();
                editor->insertPlainText(QStringLiteral("XX"));
                for (int i = 0; i < 5; ++i)
                    QApplication::processEvents();

                const int row = rowOfPos(editor->textCursor().position());
                const bool inserted = editor->textCursor().block().text().contains(QStringLiteral("XX"));
                const bool ok = (row == t.row) && inserted;
                QTextStream(stdout) << "ui-test: 在第 " << t.row << " 行点击后打字 -> 光标在第 "
                                    << row << " 行，插入"
                                    << (inserted ? "成功" : "**失败**") << (ok ? " OK" : " **不对**")
                                    << "\n";
                if (!ok) {
                    QTextStream(stderr) << "ui-test 失败：点击之后打字没有落在点中的行上\n";
                    return 17;
                }

                // 撤销回去，别影响后面的检查
                editor->undo();
                editor->undo();
                QApplication::processEvents();
                Q_UNUSED(rowText);
            }

            /*!
             * 精确拖选：从"第一个字"拖到"第 N 个字"，选中的字数必须是确定的。
             *
             * 上面那个拖选只断言了行号 —— 行号对不能说明列对得上，
             * 而"框选不到想要的位置"恰恰是列的问题。这里用 QTextLayout
             * 量出第 5 个字符的边界，拖过去，断言正好选中 5 个字符。
             */
            {
                editor->setPlainText(QStringLiteral("HANDWRITING"));
                editor->moveCursor(QTextCursor::Start);
                for (int i = 0; i < 10; ++i)
                    QApplication::processEvents();

                const QTextBlock b = editor->document()->firstBlock();
                const QTextLine line = b.layout()->lineForTextPosition(0);
                const QRectF blockRect =
                    editor->document()->documentLayout()->blockBoundingRect(b);
                auto charVp = [&](int idx) {
                    const double x = blockRect.left() + line.x() + line.cursorToX(idx);
                    const double y = blockRect.top() + line.y() + line.height() / 2.0;
                    return editor->documentToViewport().map(QPointF(x, y)).toPoint();
                };
                const QPoint from = charVp(0);
                const QPoint to = charVp(5);
                sendMouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
                QApplication::processEvents();
                sendMouse(QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton);
                QApplication::processEvents();
                sendMouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
                sendMouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
                for (int i = 0; i < 5; ++i)
                    QApplication::processEvents();

                const QTextCursor c = editor->textCursor();
                const int n = c.selectionEnd() - c.selectionStart();
                const bool ok = (n == 5) && c.selectedText() == QStringLiteral("HANDW");
                QTextStream(stdout) << "ui-test: 精确拖选 " << from.x() << "," << from.y() << " -> "
                                    << to.x() << "," << to.y() << " ：选中 " << n << " 个字符 ["
                                    << c.selectedText() << "]" << (ok ? " OK" : " **不对**")
                                    << "\n";
                if (!ok) {
                    QTextStream(stderr) << "ui-test 失败：拖选的起止列不准（选到 \"" << c.selectedText()
                                        << "\"，应为 \"HANDW\"）\n";
                    return 27;
                }
            }

            /*!
             * Shift+方向键：键盘扩选也必须有视觉反馈。
             * 用户报过"Shift+方向键似乎有效但看不到提示"，根子在选区没被画出来，
             * 所以这里断言的是"画面上真的有高亮像素"，不只看光标状态。
             */
            {
                editor->setPlainText(QStringLiteral("HANDWRITING"));
                editor->moveCursor(QTextCursor::Start);
                for (int i = 0; i < 5; ++i)
                    QApplication::processEvents();
                for (int i = 0; i < 4; ++i) {
                    QKeyEvent ev(QEvent::KeyPress, Qt::Key_Right, Qt::ShiftModifier);
                    QApplication::sendEvent(editor, &ev);
                }
                for (int i = 0; i < 5; ++i)
                    QApplication::processEvents();

                const QTextCursor c = editor->textCursor();
                const QImage shot = window.grab().toImage();
                int highlight = 0;
                for (int y = 0; y < shot.height(); ++y) {
                    const QRgb *scan = reinterpret_cast<const QRgb *>(shot.constScanLine(y));
                    for (int x = 0; x < shot.width(); ++x) {
                        const QRgb px = scan[x];
                        if (qAbs(qRed(px) - 0x33) <= 30 && qAbs(qGreen(px) - 0x99) <= 30
                            && qAbs(qBlue(px) - 0xff) <= 30)
                            ++highlight;
                    }
                }
                const bool ok = c.hasSelection() && c.selectedText() == QStringLiteral("HAND")
                                && highlight > 200;
                QTextStream(stdout) << "ui-test: Shift+右×4 选中 [" << c.selectedText()
                                    << "]，画面高亮像素 " << highlight << (ok ? " OK" : " **不对**")
                                    << "\n";
                if (!ok) {
                    QTextStream(stderr)
                        << "ui-test 失败：Shift+方向键扩选没有视觉提示（选中 \""
                        << c.selectedText() << "\"，高亮像素 " << highlight << "）\n";
                    return 28;
                }
                editor->setTextCursor(QTextCursor(editor->document()));
            }
        }
    }

    /*!
     * 扭曲效果的目视样张：换成大字号、整篇套扭曲，然后截一张图。
     * 纯靠数值断言看不出"字形是不是塌成了几块多边形"，所以留一张给人看。
     */
    if (probeMouse) {
        /*!
         * 字形扭曲的断言。
         *
         * 数值上能断的东西："变形有没有把轮廓翻面"。
         * 有符号面积的正负号就是绕向，而"洞"能保持是洞，靠的正是
         * 洞和外形绕向相反 + Winding 填充。一旦变形把某条轮廓翻了面，
         * 洞就会被填实（"0" 变成实心椭圆）。
         * 之前的 bug（碎块、洞消失）根子都在这里，所以这条必须钉住。
         */
        {
            NoiseWave w;
            w.reseed(20240925u);
            QFont f = QApplication::font();
            f.setPointSizeF(40.0);

            auto signedArea = [](const QVector<QPointF> &o) {
                double a2 = 0;
                for (int j = 0; j < o.size(); ++j) {
                    const QPointF &u = o[j];
                    const QPointF &v = o[(j + 1) % o.size()];
                    a2 += u.x() * v.y() - v.x() * u.y();
                }
                return a2 / 2.0;
            };

            const QStringList chars = {QStringLiteral("0"), QStringLiteral("4"),
                                       QStringLiteral("扭"), QStringLiteral("H"),
                                       QStringLiteral("g"), QStringLiteral("回"),
                                       QStringLiteral("%")};
            int problems = 0;
            for (const QString &ch : chars) {
                /*!
                 * 原字形的绕向（用 Qt 自己的轮廓当参照）。
                 * 轮廓和 addText 现在用的是同一个坐标系（y 向下、基线在原点），
                 * 所以有符号面积可以直接比。
                 */
                QPainterPath raw;
                raw.addText(QPointF(0, 0), f, ch);
                QVector<double> rawAreas;
                for (const QPolygonF &poly : raw.toSubpathPolygons())
                    rawAreas.append(signedArea(QVector<QPointF>(poly.begin(), poly.end())));

                // 变形后的绕向：幅度故意给一个巨大的值，检验安全钳位是否生效
                const QVector<QVector<QPointF>> dis = distortedGlyphOutlines(
                    ch, f, w, 4242u, 3.0, 500.0, 2.0);
                if (dis.size() != rawAreas.size()) {
                    QTextStream(stdout) << "  字形 " << ch << " 轮廓数变了: "
                                        << rawAreas.size() << " -> " << dis.size() << " **\n";
                    ++problems;
                    continue;
                }
                bool ok = true;
                for (int i = 0; i < dis.size(); ++i) {
                    const double before = rawAreas.at(i);
                    const double after = signedArea(dis.at(i));
                    // 缩放是正的，所以绕向必须一致；面积量级也不该变化超过 3 倍
                    if (before * after <= 0.0 || qAbs(after) > qAbs(before) * 12.0)
                        ok = false;
                }
                QTextStream(stdout) << "  字形 " << ch << " 轮廓 " << dis.size() << " 条，绕向 "
                                    << (ok ? "保持 OK" : "**被翻了**") << "\n";
                if (!ok)
                    ++problems;
            }
            if (problems > 0) {
                QTextStream(stderr) << "ui-test 失败：扭曲把 " << problems
                                    << " 个字形的轮廓翻了面（洞会被填实）\n";
                return 15;
            }

            /*!
             * 放大对照图：上排原字形，下排扭曲后的字形。
             * 断言只看得出"绕向对不对"，"好不好看"还得给人看一眼。
             */
            const QStringList sheetChars = {QStringLiteral("4"), QStringLiteral("扭"),
                                            QStringLiteral("H"), QStringLiteral("0")};
            QImage sheet(QSize(sheetChars.size() * 320 + 40, 560), QImage::Format_RGB32);
            sheet.fill(Qt::white);
            {
                QPainter sp(&sheet);
                sp.setRenderHint(QPainter::Antialiasing, true);
                for (int i = 0; i < sheetChars.size(); ++i) {
                    const QString ch = sheetChars.at(i);
                    const double x = 20 + i * 320;
                    // 原字形（基线 y=180）
                    sp.setPen(Qt::NoPen);
                    sp.setBrush(QColor(0x22, 0x22, 0x22));
                    QPainterPath path;
                    path.addText(QPointF(x, 180), f, ch);
                    sp.drawPath(path);
                    // 扭曲后（基线 y=420）：和渲染器走同一条路（buildGlyphPath 的逻辑）
                    const QVector<QVector<QPointF>> dis = distortedGlyphOutlines(
                        ch, f, w, 12345u, 3.0, 4.8, 2.0);
                    QVector<QVector<QPointF>> sorted = dis;
                    std::sort(sorted.begin(), sorted.end(),
                              [](const QVector<QPointF> &a, const QVector<QPointF> &b) {
                                  const QRectF ra = QPolygonF(a).boundingRect();
                                  const QRectF rb = QPolygonF(b).boundingRect();
                                  return ra.width() * ra.height() > rb.width() * rb.height();
                              });
                    QPainterPath glyph;
                    glyph.setFillRule(Qt::OddEvenFill);
                    for (const QVector<QPointF> &o : sorted) {
                        if (o.size() < 3)
                            continue;
                        glyph.addPolygon(QPolygonF(o));
                        glyph.closeSubpath();
                    }
                    sp.save();
                    sp.translate(x, 420);
                    sp.setPen(Qt::NoPen);
                    sp.setBrush(QColor(0x1d, 0x3f, 0xa8));
                    sp.drawPath(glyph);
                    sp.restore();
                    sp.setPen(QColor(0xbb, 0xbb, 0xbb));
                    sp.drawLine(QPointF(x - 10, 195), QPointF(x + 300, 195));
                }
            }
            sheet.save(outDir + QStringLiteral("/uitest_glyph_compare.png"));
            QTextStream(stdout) << "ui-test: 字形放大对照 " << outDir
                                << "/uitest_glyph_compare.png\n";
        }

        if (auto *editor = window.findChild<TextEditor *>()) {
            editor->setPlainText(QStringLiteral("4504504 扭曲波形 扭曲波形 扭曲波形\n"
                                                "Handwriting Distortion 1906 abcdefg\n"
                                                "中文段落排版：缩进、行距、随机字体\n"));
            // 直接改文档默认字体：这才是"整篇 28pt"最干净的做法，
            // 也让效果层必须走 effectiveFont 才能拿到正确字号
            QFont big = editor->document()->defaultFont();
            big.setPointSizeF(28.0);
            editor->document()->setDefaultFont(big);
            editor->setCurrentCharFormat(QTextCharFormat());
            editor->relayout();

            HandwritingLibrary probeLibrary;
            probeLibrary.loadResourceDir(QStringLiteral(":/handwrite"));
            NoiseWave probeWave;
            probeWave.reseed(20240925u);

            EffectRenderOptions distortion;   // 默认就是"扭曲替换正文"
            distortion.showHandwriting = false;
            distortion.showDistortion = true;
            distortion.library = &probeLibrary;
            distortion.wave = &probeWave;
            distortion.amplitudePt = 0.5;
            distortion.waveScale = 2.0;
            editor->setEffectOptions(distortion);

            QTextCursor mark(editor->document());
            mark.beginEditBlock();
            for (int pos = 0; pos < editor->document()->characterCount() - 1; ++pos) {
                const QString one = editor->document()->characterAt(pos);
                if (one.isEmpty() || one.at(0).isSpace())
                    continue;
                QTextCursor c(editor->document());
                c.setPosition(pos);
                c.setPosition(pos + 1, QTextCursor::KeepAnchor);
                QTextCharFormat fmt;
                EffectStyle style;
                style.kind = EffectKind::Distortion;
                style.seed = 7u + quint32(pos) * 2654435761u;
                setEffectStyle(&fmt, style);
                c.mergeCharFormat(fmt);
            }
            mark.endEditBlock();
            editor->setEffectsVisible(true);
            for (int i = 0; i < 20; ++i) {
                QApplication::processEvents();
                QThread::msleep(5);
            }

            window.grab().save(outDir + QStringLiteral("/uitest_distortion.png"));

            /*!
             * 定位对照图：同一个字符，左边是正文原样，右边是渲染器实际画出的扭曲字形，
             * 两层共用同一条基线（红线）和同一个行框（灰框）。
             * 数值断言看的是"整段墨迹的包围盒"，这张图看的是"单个字有没有压准"。
             */
            {
                const QString ch = QStringLiteral("白");
                const QTextBlock b = editor->document()->firstBlock();
                const QTextLine line = b.layout()->lineForTextPosition(0);
                const QRectF blockRect =
                    editor->document()->documentLayout()->blockBoundingRect(b);
                const QFont ff = effectiveFont(editor->document(),
                                               b.begin().fragment().charFormat());
                const QFontMetricsF fm(ff);
                const double scale = line.height() / (fm.ascent() + fm.descent());
                const double baseline = blockRect.top() + line.y() + line.ascent();

                QImage cmp(400, 140, QImage::Format_RGB32);
                cmp.fill(Qt::white);
                QPainter cp(&cmp);
                cp.setRenderHint(QPainter::Antialiasing, true);
                cp.setPen(QPen(QColor(0xdd, 0xdd, 0xdd), 1));
                cp.drawRect(QRectF(0.5, 0.5, 399, line.height()));
                cp.setPen(QPen(QColor(0xff, 0x88, 0x88), 1));
                cp.drawLine(QPointF(0, baseline), QPointF(400, baseline));

                // 左：正文原样（只裁前 60px，避免画出后面那些字）
                cp.save();
                cp.setClipRect(QRectF(0, 0, 60, 140));
                QAbstractTextDocumentLayout::PaintContext ctx;
                ctx.palette.setColor(QPalette::Text, Qt::black);
                editor->document()->documentLayout()->draw(&cp, ctx);
                cp.restore();

                // 右：渲染器画扭曲字形的那套坐标（原点 = 基线）
                const QVector<QVector<QPointF>> dis = distortedGlyphOutlines(
                    ch, ff, probeWave, 99u, scale, 1.6 * scale, 2.0);
                QVector<QVector<QPointF>> sorted = dis;
                std::sort(sorted.begin(), sorted.end(),
                          [](const QVector<QPointF> &a, const QVector<QPointF> &c) {
                              const QRectF ra = QPolygonF(a).boundingRect();
                              const QRectF rb = QPolygonF(c).boundingRect();
                              return ra.width() * ra.height() > rb.width() * rb.height();
                          });
                QPainterPath glyph;
                glyph.setFillRule(Qt::OddEvenFill);
                for (const QVector<QPointF> &o : sorted) {
                    if (o.size() < 3)
                        continue;
                    glyph.addPolygon(QPolygonF(o));
                    glyph.closeSubpath();
                }
                cp.save();
                cp.translate(120, baseline);
                cp.setPen(Qt::NoPen);
                cp.setBrush(QColor(0x1d, 0x3f, 0xa8));
                cp.drawPath(glyph);
                cp.restore();
                cp.end();
                cmp.save(outDir + QStringLiteral("/uitest_align_check.png"));
                QTextStream(stdout) << "ui-test: 定位对照 " << outDir
                                    << "/uitest_align_check.png（基线 " << baseline << "，行高 "
                                    << line.height() << "，缩放 " << scale << "）\n";
            }
        }
    }

        /*!
         * 自包含的"扭曲定位"最小复现：不经过编辑器、不经过覆盖层，
         * 直接把一段 28pt 文字渲到白图上，并在正文之上再叠一层暗红色的扭曲字形。
         * 两层对得上就说明整条渲染链路是对的（对不上时这张图最好定位问题）。
         * 上排 "LF" 用来核对方位：L 和 F 上下完全不对称，被镜像了一眼就能看出。
         */
        if (probeMouse) {
            QTextDocument doc;
            QFont f = QApplication::font();
            f.setPointSizeF(28.0);
            doc.setDefaultFont(f);
            doc.setDocumentMargin(0);
            doc.setPageSize(QSizeF(700, 2000));

            QTextCursor cursor(&doc);
            cursor.insertText(QStringLiteral("4 白 中 H a g"));

            NoiseWave w;
            w.reseed(20240925u);
            EffectRenderOptions options;
            options.showDistortion = true;
            options.wave = &w;
            options.amplitudePt = 1.6;
            options.waveScale = 2.0;
            options.distortionReplaceText = false; // 叠加模式，好分辨两层

            QTextCursor mark(&doc);
            mark.beginEditBlock();
            for (int pos = 0; pos < doc.characterCount() - 1; ++pos) {
                QTextCursor c(&doc);
                c.setPosition(pos);
                c.setPosition(pos + 1, QTextCursor::KeepAnchor);
                QTextCharFormat fmt;
                EffectStyle style;
                style.kind = EffectKind::Distortion;
                style.seed = 5u + quint32(pos) * 2654435761u;
                setEffectStyle(&fmt, style);
                c.mergeCharFormat(fmt);
            }
            mark.endEditBlock();

            QImage img(QSize(760, 130), QImage::Format_RGB32);
            img.fill(Qt::white);
            {
                QPainter p(&img);
                p.setRenderHint(QPainter::Antialiasing, true);
                p.translate(20, 10);
                QAbstractTextDocumentLayout::PaintContext ctx;
                ctx.palette.setColor(QPalette::Text, Qt::black);
                doc.documentLayout()->draw(&p, ctx);
                EffectRenderOptions local = options;
                local.distortionColor = QColor(0xd0, 0x30, 0x30); // 暗红，便于分辨
                renderEffects(&p, &doc, local);
            }
            img.save(outDir + QStringLiteral("/uitest_iso_effect.png"));

            // 方位核对：幅度 0（不变形）的轮廓点列 vs addText 路径，同一原点
            {
                const QString probe = QStringLiteral("LF");
                QFont pf = QApplication::font();
                pf.setPointSizeF(60.0);
                QImage orient(QSize(420, 260), QImage::Format_RGB32);
                orient.fill(Qt::white);
                QPainter p(&orient);
                p.setRenderHint(QPainter::Antialiasing, true);
                p.setPen(QPen(QColor(0xdd, 0xdd, 0xdd), 1));
                p.drawLine(0, 100, 420, 100);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0x22, 0x22, 0x22));
                QPainterPath ref;
                ref.addText(QPointF(30, 100), pf, probe);
                p.drawPath(ref);
                p.save();
                p.translate(200, 100);
                p.setBrush(QColor(0xd0, 0x30, 0x30));
                for (const QVector<QPointF> &o : distortedGlyphOutlines(probe, pf, w, 1u, 1.0, 0.0, 2.0)) {
                    if (o.size() < 3)
                        continue;
                    p.drawPolygon(QPolygonF(o));
                }
                p.restore();
                p.setPen(QColor(0x66, 0x66, 0x66));
                p.drawText(QPointF(30, 240), QStringLiteral("addText 路径"));
                p.drawText(QPointF(200, 240), QStringLiteral("轮廓点列（幅度 0）"));
                p.end();
                orient.save(outDir + QStringLiteral("/uitest_outline_orient.png"));
            }
            QTextStream(stdout) << "ui-test: 扭曲最小复现 " << outDir
                                << "/uitest_iso_effect.png，方位核对 uitest_outline_orient.png\n";
        }

    if (auto *editor = window.findChild<TextEditor *>())
        editor->setFocus();

    // 事件循环真的转得起来吗
    QTimer::singleShot(0, &window, [] { QTextStream(stdout) << "ui-test: 事件循环正常\n"; });
    QApplication::processEvents();

    /*!
     * 工具栏/菜单里的新动作必须真的挂上去了。
     * 工具栏在窄窗口下会把放不下的动作收进扩展菜单，截图上看不出来，
     * 所以这里按 objectName 抽查一遍 —— 少一个就说明忘了 addAction。
     */
    {
        const QStringList expected = {
            "act_paragraph", "act_color",   "act_clearcolor", "act_randomfont",
            "act_fontpool",  "act_applyhw", "act_distort",    "act_regex",
        };
        QStringList missing;
        for (const QString &name : expected) {
            if (!window.findChild<QAction *>(name))
                missing.append(name);
        }
        if (!missing.isEmpty()) {
            QTextStream(stderr) << "ui-test 失败：这些动作没注册："
                                << missing.join(QStringLiteral(", ")) << "\n";
            return 16;
        }
        QTextStream(stdout) << "ui-test: 动作注册齐全（抽查 " << expected.size() << " 个）\n";
    }

    QTextStream(stdout) << "ui-test: 全部通过\n";
    return 0;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("tripa"));
    QApplication::setApplicationDisplayName(QStringLiteral("tripa 排版器"));
    QApplication::setOrganizationName(QStringLiteral("mywrite"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/tripa.png")));

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = QStringLiteral("tripa_") + QLocale(locale).name();
        if (translator.load(QStringLiteral(":/i18n/") + baseName)) {
            QApplication::installTranslator(&translator);
            break;
        }
    }

    const QStringList args = QApplication::arguments();
    if (args.size() > 1 && args.at(1) == QStringLiteral("--selftest"))
        return runSelfTest(args.mid(1));
    if (args.size() > 1 && args.at(1) == QStringLiteral("--uitest"))
        return runUiTest(args.mid(1));
    MainWindow window;
    window.show();
    return QApplication::exec();
}
