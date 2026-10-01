#include "mainwindow.h"

#include "baselineadjust.h"
#include "effectsrenderer.h"
#include "effectplanner.h"
#include "handwriting.h"
#include "jobrunner.h"
#include "pagesetup.h"
#include "paginatinglayout.h"
#include "proofsheet.h"
#include "texteditor.h"
#include "tripalog.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPageLayout>
#include <QPagedPaintDevice>
#include <QPainterPath>
#include <QPdfWriter>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QSlider>
#include <QStringList>
#include <QStyle>
#include <QSysInfo>
#include <QTableWidget>
#include <QToolButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QTranslator>
#include <QWheelEvent>

#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstdint>

#if defined(Q_OS_WIN)
#  include <windows.h> // 崩溃自检要 RaiseException（见 runCrashChild）
#  include <process.h> // _spawnl：拉子进程，且不依赖命名管道（见 runCrashDumpProbe）
#endif

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

    /*!
     * 打印 / 导出走的就是**编辑时那一套分页** —— 同一个 PaginatingLayout，
     * 只把版面几何换成打印机 / PDF 的纸张。
     *
     * 这是“所见即所得”唯一可信的做法：屏幕上在哪儿断行、第几页放什么，
     * 纸上就是同一份答案 —— 两边都是这一个渲染层算出来的。
     * 以前这条路自己又写了一遍“正文区摞起来”的排法，还顺手把
     * `document()->setPageSize()` 改成了正文区尺寸：两套排法各排各的，
     * 迟早对不上（“屏幕上和导出不一样”就是这么来的）。
     */
    PaginatingLayout *layout = installPaginatingLayout(document);
    const RenderMetrics savedMetrics = layout->metrics();
    const PageChrome savedChrome = layout->chrome();

    const RenderMetrics metrics = RenderMetrics::fromPageSetup(setup);
    layout->setMetrics(metrics);
    layout->setChrome(PageChrome::fromPageSetup(setup));

    // 设备像素 / 96dpi 像素（96dpi 下 1mm = 25.4 px）
    const double zoom = (double(device->width()) / paperMm.width()) * 25.4 / PageSetup::kDpi;
    const int pages = layout->pageCount();

    /*!
     * 会被效果“整格替换掉”的字符：正文这一格必须**不画**。
     *
     * 以前是让效果层往上刷一块纸色去盖原字，那块底色在屏幕上还会盖掉
     * 选区高亮。现在改成绘制期把这一格裁掉（奇偶路径控洞），
     * 屏幕和纸面走同一份计划，谁也不欠谁一块底色。
     */
    /*!
     * 效果层交给**计算层**（effectplanner.h）：它按段落算显示表、把几何算好，
     * 这里只读结果。
     *
     * 以前这里是 `planEffects(document, options)` 一次算全篇 + 每页再
     * `renderEffects()` 一次（后者内部又 `planEffects` 一遍）——
     * 十页的稿子等于把全篇规划十一遍，而且几何是在**逐页绘制时才现算**的。
     * 现在按页准备（`requestRange` + `work()` 到底），既快又与屏幕同一份口径。
     */
    EffectPlanner planner(document, layout);
    planner.setOptions(options);

    /*!
     * 翻页：PDF / 打印机必须显式 `newPage()`，否则 N 页全画在第一页上
     * （后几页用 `translate(-page*纸高)` 挪到了纸外面，看着就像“只导出第一页”）。
     * 图像设备（QImage）没有页的概念，整篇按一张图画，超出部分自然被裁掉。
     */
    QPagedPaintDevice *paged = dynamic_cast<QPagedPaintDevice *>(device);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    for (int page = 0; page < pages; ++page) {
        if (page > 0 && paged)
            paged->newPage();

        painter->save();
        // 文档坐标 -> 设备像素：先缩放，再把这一页的纸面左上角放到设备原点
        painter->scale(zoom, zoom);
        painter->translate(0.0, -double(page) * metrics.paperHeightPx);

        /*!
         * 这一页要画的段落：显示表 + **几何**都先就位（打印 / 导出是模态操作，
         * 这里同步算完；交互式绘制才需要分片，见 EffectWorkJob）。
         *
         * 按页准备而不是"一次算全篇"：一是显示表缓存有上限（见
         * EffectPlanner 里的 kMaxCachedBlocks），二是长稿子一次全算会白占内存。
         */
        planner.beginFrame();
        const QPair<int, int> pageBlocks = layout->blockRangeOnPages(page, page);
        planner.requestRange(pageBlocks.first, pageBlocks.second);
        /*!
         * `requestRange` 只登记"这一段要算"，真正排队是 `requestGeometry()`
         * （界面上它等 150ms 的"安静"，免得打字时白烧 CPU）。
         * 打印 / 导出是模态操作，不需要等 —— 而是现在就算完。
         */
        planner.requestGeometry();
        while (planner.work(256)) {
            // 一直算到这一页的几何都好了
        }

        /*!
         * 本页的“正文窗口 + 挖洞”裁剪路径（**页内坐标**：纸面左上角已平移到原点）。
         *
         * 洞必须挑**落在本页**的那些：别的页的洞平移过来会变成一块
         * “实心岛”，平白多给出一片可绘制区域。
         */
        const QRectF bodyLocal(metrics.marginLeftPx, metrics.marginTopPx, metrics.bodyWidthPx(),
                               metrics.bodyHeightPx());
        QPainterPath windowPath;
        windowPath.setFillRule(Qt::OddEvenFill);
        windowPath.addRect(bodyLocal);
        for (int n = pageBlocks.first; n <= pageBlocks.second; ++n) {
            const QVector<EffectDrawItem> &items = planner.items(n);
            for (const EffectDrawItem &item : items) {
                if (!item.hidden)
                    continue;
                const QRectF shifted =
                    item.charRect.translated(0.0, -double(page) * metrics.paperHeightPx);
                if (shifted.intersects(bodyLocal))
                    windowPath.addRect(shifted);
            }
        }

        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, Qt::black);
        // 可见范围（文档坐标）：渲染层据此跳过看不见的段落
        context.clip = metrics.bodyRect(page);

        painter->setClipPath(windowPath, Qt::IntersectClip);
        layout->draw(painter, context);

        if (options.anyLayer()) {
            /*!
             * 画效果层之前**必须先去掉“挖洞”的裁剪**。
             *
             * 洞的位置就是会被效果字形替换掉的那些格子 —— 也就是效果层要画的地方。
             * 带着这套裁剪去画效果层，等于把新字形也一起裁掉：原字没了、新字也没了。
             * 换成普通的“正文窗口”裁剪：效果层不该画到页边距外面去。
             */
            painter->setClipping(false);
            QPainterPath bodyOnly;
            bodyOnly.addRect(bodyLocal);
            painter->setClipPath(bodyOnly);
            EffectRenderOptions local = options;
            local.missing.clear();
            for (int n = pageBlocks.first; n <= pageBlocks.second; ++n)
                renderPreparedEffects(painter, planner.items(n), local);
        }

        // 页眉页脚：和屏幕上同一段代码（默认都不显示）
        layout->drawChrome(painter, page, QPalette());

        painter->restore();
    }

    painter->restore();

    /*!
     * 打印是“借用”文档的版面：算完还给编辑时那一套。
     * 不还的话，打印完屏幕上的分页就变成了打印机纸张的分页
     * （换台打印机，屏幕上的断行就变了 —— 这种事发生一次就够吓人的）。
     *
     * 本来就没版面（独立文档：自检渲染、导出图片）时就把这一套留着 ——
     * 还成“无效”的话文档就再也没法算几何了，blockBoundingRect 全变成空矩形。
     */
    if (savedMetrics.isValid())
        layout->setMetrics(savedMetrics);
    layout->setChrome(savedChrome);
}

/*!
 * 量出一张图里所有墨迹的包围盒（页面上除了目标字形什么都没有时用它）。
 * 全白 = 返回空矩形。
 */
static QRect inkBoundsOf(const QImage &image)
{
    int minX = image.width();
    int minY = image.height();
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *scan = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb c = scan[x];
            if (qRed(c) < 200 || qGreen(c) < 200 || qBlue(c) < 200) {
                minX = qMin(minX, x);
                maxX = qMax(maxX, x);
                minY = qMin(minY, y);
                maxY = qMax(maxY, y);
            }
        }
    }
    if (maxX < 0)
        return QRect();
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

/*!
 * \brief 等效果层的几何算完（自检用）。
 *
 * 效果几何是**后台分片算**的（见 effectplanner.h）：套上效果的头几帧，
 * 原字照旧画着（那时还没挖洞），算好了才一片一片换成扭曲字形/手写笔迹。
 * 自检要量"最终样子"，所以得先等它安静下来 —— 而且无头环境不会主动派发
 * 绘制事件，而"哪些格子要算几何"正是绘制时决定的，所以这里还要主动推重绘。
 */
static void windowSettleEffects(MainWindow &window, int timeoutMs = 5000)
{
    auto *editor = window.findChild<TextEditor *>();
    if (!editor || !editor->effectPlanner())
        return;

    EffectPlanner *planner = editor->effectPlanner();
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        editor->viewport()->repaint();  // 让"看得见的这几页"登记进来
        planner->requestGeometry();     // 不等 150ms 的"安静"了
        while (planner->work(64)) { }
        bool ready = true;
        const QPair<int, int> range = editor->layout()->blockRangeOnPages(0, 9999);
        for (int n = range.first; n <= range.second && ready; ++n) {
            for (const EffectDrawItem &item : planner->items(n)) {
                if (!item.geometryDone) {
                    ready = false;
                    break;
                }
            }
        }
        if (ready && !planner->hasPendingWork())
            break;
        QApplication::processEvents();
    }
    QApplication::processEvents();
}

/*!
 * 无界面自检：把文档渲染成一张 PNG，用来验证
 * 数出 \a rect 范围内有多少墨点（用来验证"该有字的地方真的有字"）。
 */
static int inkPixelsIn(const QImage &image, const QRect &rect)
{
    const QRect area = rect.intersected(QRect(QPoint(0, 0), image.size()));
    int count = 0;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        const QRgb *scan = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = area.left(); x <= area.right(); ++x) {
            const QRgb c = scan[x];
            if (qRed(c) < 200 || qGreen(c) < 200 || qBlue(c) < 200)
                ++count;
        }
    }
    return count;
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
    /*!
     * 和界面的默认值保持一致：手写"遮住正文"。
     * 这样自检渲染出来的就是用户实际会得到的样子（原字不画、只剩笔迹），
     * 也顺带验证了导出路径上"挖洞裁掉原字"这一步。
     */
    options.handwritingReplaceText = true;

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

    /*!
     * 3.5) 基线 / 大小校正必须真的作用到纸上。
     *
     * "xml 存下来了"不算数 —— 存了但渲染层没用，用户看到的就是"调了个寂寞"。
     * 所以这里拿一个内置样本单独渲染三次，量墨迹包围盒：
     *   默认      -> 基准
     *   基线 +15% -> 整体下移，高度基本不变
     *   大小 150% -> 高度长大约一半（顺带验证"宽度受限"的样本也放得大，
     *               宽度不一起缩放的话一横这类样本是放不动的）
     */
    {
        QTextDocument probe;
        QFont probeFont = QApplication::font();
        probeFont.setPointSize(40);
        probe.setDefaultFont(probeFont);
        probe.setDocumentMargin(0);
        probe.setPageSize(setup.bodySizePx());

        QTextCursor pc(&probe);
        pc.insertText(QStringLiteral("a"));
        QTextCursor one(&probe);
        one.setPosition(0);
        one.setPosition(1, QTextCursor::KeepAnchor);
        QTextCharFormat fmt;
        EffectStyle style;
        style.kind = EffectKind::Handwriting;
        style.seed = 4242u;
        setEffectStyle(&fmt, style);
        one.mergeCharFormat(fmt);

        EffectRenderOptions probeOptions;
        probeOptions.library = &library;
        probeOptions.showHandwriting = true;
        probeOptions.handwritingReplaceText = true;

        auto renderProbe = [&]() {
            QImage img(image.size(), QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            tripaRenderToDevice(&probe, setup, probeOptions, &img, &p);
            p.end();
            return img;
        };

        // 内置数据的来源路径：a.csv 里只有一个字符，下标就是 0
        const QString probeSource = QStringLiteral(":/handwrite/a.csv");

        library.setAdjustment(probeSource, 0, HandwritingAdjustment());
        const QRect base = inkBoundsOf(renderProbe());

        /*!
         * 挖洞裁剪不许把新字形一起裁掉。
         *
         * 这一条是被真事逼出来的：导出路径带着"挖洞"的裁剪去画效果层，
         * 而洞的位置正是效果层要画的地方 —— 于是导出的 PDF / 图片里
         * 手写笔迹几乎全被裁没了，屏幕上却是好的（屏幕那条路会先
         * setClipping(false)，见 TextEditor::paintEvent）。
         * 所以这里专门量"被替换掉的那个格子里有多少墨"，
         * 光量整页墨点总数是发现不了的（扭曲层的墨会把数字撑住）。
         * 阈值：正常约 500 点，被裁掉时只剩越过格子边界的那几十点。
         */
        {
            const QVector<EffectDrawItem> probeItems = planEffects(&probe, probeOptions);
            if (!probeItems.isEmpty()) {
                const QRectF cell = probeItems.first().charRect;
                const double toDevice = dpi / PageSetup::kDpi;
                /*!
                 * 格子坐标**已经是纸面坐标**（含页边距），所以直接乘设备比例就行。
                 *
                 * 这里曾经又加了一遍 `marginsPx` —— 那是旧模型（文档坐标 = 正文坐标）
                 * 留下的写法。旧模型下多算一次刚好落回格子上，新模型下等于把量尺
                 * 往右下方挪了一个页边距，量到的是一片空白：
                 * “格子里 0 个墨点”看起来像导出丢笔迹，实际上是**自检量错了地方**。
                 */
                const QRect dev(int(cell.left() * toDevice),
                                int(cell.top() * toDevice),
                                int(cell.width() * toDevice),
                                int(cell.height() * toDevice));
                const int inside = inkPixelsIn(renderProbe(), dev);
                QTextStream(stdout) << "self-test: 被替换的格子里墨点=" << inside
                                    << "（格子 " << dev.width() << "x" << dev.height()
                                    << " 设备像素）\n";
                if (inside < 150) {
                    QTextStream(stderr)
                        << "self-test 失败：被替换的格子里几乎没有新墨 —— "
                           "效果层大概又被'挖洞'的裁剪裁掉了（导出会丢手写笔迹）\n";
                    return 11;
                }
            }
        }

        HandwritingAdjustment lower;
        lower.baseline = 0.15;
        library.setAdjustment(probeSource, 0, lower);
        const QRect shifted = inkBoundsOf(renderProbe());

        HandwritingAdjustment bigger;
        bigger.size = 1.5;
        library.setAdjustment(probeSource, 0, bigger);
        const QRect scaled = inkBoundsOf(renderProbe());

        // 量完就还原，别影响后面 PDF 那一步
        library.setAdjustment(probeSource, 0, HandwritingAdjustment());

        const double lineH = QFontMetricsF(probeFont).ascent() + QFontMetricsF(probeFont).descent();
        const double expectShift = 0.15 * lineH * dpi / 96.0;
        QTextStream(stdout) << "self-test: 校正前墨迹 " << base.width() << "x" << base.height()
                            << " @y=" << base.top() << "，基线+15% 后 @y=" << shifted.top()
                            << "（预期下移 " << int(expectShift) << "px），大小 150% 后 "
                            << scaled.width() << "x" << scaled.height() << "\n";

        if (base.isEmpty() || shifted.isEmpty() || scaled.isEmpty()) {
            QTextStream(stderr) << "self-test 失败：单字页面没渲染出墨迹，量不了校正效果\n";
            return 6;
        }
        if (shifted.top() < base.top() + int(expectShift * 0.5)) {
            QTextStream(stderr) << "self-test 失败：基线 +15% 之后墨迹没有下移"
                                   "（渲染层可能没读校正值）\n";
            return 7;
        }
        if (qAbs(shifted.height() - base.height()) > qMax(3, base.height() / 10)) {
            QTextStream(stderr) << "self-test 失败：只调基线不该改变字的大小（"
                                << base.height() << " -> " << shifted.height() << "）\n";
            return 8;
        }
        if (scaled.height() < int(base.height() * 1.25)) {
            QTextStream(stderr) << "self-test 失败：大小 150% 之后墨迹没有变大（"
                                << base.height() << " -> " << scaled.height() << "）\n";
            return 9;
        }

        /*!
         * 三张并排存一张对比图，肉眼也能核对。
         *
         * 三格必须用**同一个裁剪框和同一个缩放**：每格各自"贴合自己的墨迹"
         * 的话，平移和缩放都会被抵消掉，三张看起来一模一样（白做）。
         * 裁剪框按"默认那一张"的墨迹往外留足余量（下面要多留，字会往下沉、会变大）。
         */
        const int pad = 10;
        const QImage defaultImage = renderProbe();
        const QRect crop = base.adjusted(-pad, -pad * 2, pad, pad * 5)
                               .intersected(QRect(QPoint(0, 0), defaultImage.size()));
        const int panelW = 220;
        const int panelH = 320;
        const double kScale = qMin(double(panelW - 2 * pad) / crop.width(),
                                   double(panelH - 2 * pad) / crop.height());
        QImage compare(panelW * 3, panelH, QImage::Format_RGB32);
        compare.fill(Qt::white);
        auto paste = [&](const QImage &src, int slot) {
            const QSizeF target(crop.width() * kScale, crop.height() * kScale);
            const QRectF dest(slot * panelW + (panelW - target.width()) / 2.0,
                              (panelH - target.height()) / 2.0,
                              target.width(), target.height());
            QPainter cp(&compare);
            cp.setRenderHint(QPainter::SmoothPixmapTransform, true);
            cp.drawImage(dest, src, crop);
        };
        paste(defaultImage, 0);
        library.setAdjustment(probeSource, 0, lower);
        paste(renderProbe(), 1);
        library.setAdjustment(probeSource, 0, bigger);
        paste(renderProbe(), 2);
        library.setAdjustment(probeSource, 0, HandwritingAdjustment());
        const QString cmpPath = outDir + QStringLiteral("/selftest_baseline_adjust.png");
        if (!compare.save(cmpPath)) {
            QTextStream(stderr) << "self-test 失败：写不出 " << cmpPath << "\n";
            return 10;
        }
        QTextStream(stdout) << "self-test: 校正对比图 " << cmpPath
                            << "（左：默认；中：基线 +15%；右：大小 150%）\n";
    }

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
 * 基线 / 大小调整对话框的实测。
 *
 * 验三件事，缺一条这个功能就是"看着有、其实不work"：
 *   1. 打开时按 (文件, 字符下标) 读出已有的校正，并在预览里体现；
 *   2. 动 spinbox 立刻写进库里（正文实时跟着变，靠的是同一个库指针）；
 *   3. 关窗口写回 `<csv 同名>.xml`，重新载入 CSV 能读回来。
 *
 * 必须用临时目录里的 CSV：内置资源（qrc）是只读的，写盘那一步根本走不到。
 * \param sourceDir 非空时用它里面的 CSV 直接开（用来对着真实数据截图），
 *        否则现场造一份小的。
 */
static int runBaselineDialogProbe(const QString &outDir, const QString &sourceDir)
{
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        QTextStream(stderr) << "ui-test 失败：建立临时目录失败\n";
        return 40;
    }

    QString csvPath;
    if (!sourceDir.isEmpty()) {
        const QStringList files = QDir(sourceDir).entryList({QStringLiteral("*.csv")}, QDir::Files);
        if (files.isEmpty()) {
            QTextStream(stderr) << "ui-test 失败：" << sourceDir << " 里没有 CSV\n";
            return 41;
        }
        /*!
         * 先拷贝再调：对话框关窗口时会往 CSV 同目录写 xml，
         * 直接对着用户的数据跑就等于偷偷改了人家的目录。
         */
        csvPath = tmp.filePath(files.first());
        if (!QFile::copy(QDir(sourceDir).filePath(files.first()), csvPath)) {
            QTextStream(stderr) << "ui-test 失败：拷贝样例 CSV 失败\n";
            return 42;
        }
    } else {
        csvPath = tmp.filePath(QStringLiteral("probe.csv"));
        QFile f(csvPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream(stderr) << "ui-test 失败：写不出临时 CSV\n";
            return 43;
        }
        // 三个字，笔画数与点数各不相同，好认
        f.write(QStringLiteral("好云一\n"
                               "0,0,0.5\n10,10,0.5\n20,40,0.5\n\n"
                               "100,0,0.4\n120,30,0.6\n\n\n"
                               "200,50,0.3\n260,50,0.3\n")
                    .toUtf8());
        f.close();
    }

    HandwritingLibrary library;
    QString error;
    if (!library.loadFile(csvPath, &error)) {
        QTextStream(stderr) << "ui-test 失败：载入临时 CSV 失败：" << error << "\n";
        return 44;
    }

    HandwritingBaselineDialog dialog(&library, QApplication::font().family(), 0.20);
    dialog.resize(1100, 720);
    dialog.show();
    for (int i = 0; i < 30; ++i) {
        QApplication::processEvents();
        QThread::msleep(5);
    }

    QTableWidget *table = dialog.findChild<QTableWidget *>();
    if (!table || table->rowCount() < 2) {
        QTextStream(stderr) << "ui-test 失败：调整对话框没列出字符（表行数 "
                            << (table ? table->rowCount() : -1) << "）\n";
        return 45;
    }
    QTextStream(stdout) << "ui-test: 基线调整对话框列出 " << table->rowCount() << " 行（"
                        << library.fileEntries(csvPath).size() << " 个槽位）\n";

    dialog.grab().save(outDir + QStringLiteral("/uitest_baseline_dialog.png"));
    if (QTableWidgetItem *cell = table->item(0, 2))
        cell->icon().pixmap(QSize(84, 84), 2.0)
            .save(outDir + QStringLiteral("/uitest_preview0.png"));

    /*!
     * 自动对齐：按参考字形的墨迹框算基线 + 大小。
     * 拿"完全不成形"的极端值做对照 —— 之前是手调，现在是点一下。
     */
    QPushButton *alignAll = nullptr;
    for (QPushButton *b : dialog.findChildren<QPushButton *>()) {
        if (b->text().contains(QStringLiteral("自动对齐")) && b->text().contains(QStringLiteral("全部")))
            alignAll = b;
    }
    if (!alignAll) {
        QTextStream(stderr) << "ui-test 失败：找不到「自动对齐（本文件全部）」按钮\n";
        return 46;
    }
    alignAll->click();
    for (int i = 0; i < 10; ++i)
        QApplication::processEvents();

    const int adjusted = library.adjustedEntryCount();
    QTextStream(stdout) << "ui-test: 自动对齐后 " << adjusted << " 个字符有校正\n";
    if (adjusted <= 0) {
        QTextStream(stderr) << "ui-test 失败：自动对齐一个值都没算出来\n";
        return 47;
    }
    dialog.grab().save(outDir + QStringLiteral("/uitest_baseline_dialog_aligned.png"));
    if (QTableWidgetItem *cell = table->item(0, 2))
        cell->icon().pixmap(QSize(84, 84), 2.0)
            .save(outDir + QStringLiteral("/uitest_preview0_aligned.png"));

    /*!
     * 再截一段中段的：分页大文件（一个 CSV 一整页字）里，
     * 标签是识别结果、笔画分组还要靠几何修，个别槽位可能对不上，
     * 这一段截图就是用来看"字"和预览里的笔迹对不对得上的。
     */
    if (table->rowCount() > 55) {
        table->scrollToItem(table->item(45, 0));
        for (int i = 0; i < 10; ++i)
            QApplication::processEvents();
        dialog.grab().save(outDir + QStringLiteral("/uitest_baseline_dialog_mid.png"));
    }

    // 再手改一格，验证 spinbox -> 库这条线。
    // 控件一律按"单元格里的那个"来取：findChildren 会连上一批（延迟删除的）
    // 控件一起捞出来，拿错就测不到真正在用的那一个。
    QDoubleSpinBox *baselineSpin = qobject_cast<QDoubleSpinBox *>(table->cellWidget(0, 3));
    if (!baselineSpin) {
        QTextStream(stderr) << "ui-test 失败：表格单元格里没有基线 spinbox\n";
        return 48;
    }
    baselineSpin->setValue(7.5);
    QApplication::processEvents();

    const HandwritingSample *first = library.sampleAt(csvPath, 0);
    if (!first || qAbs(library.adjustment(*first).baseline - 0.075) > 1e-6) {
        QTextStream(stderr) << "ui-test 失败：改 spinbox 没有写进库里（当前 "
                            << (first ? library.adjustment(*first).baseline : -99.0) << "）\n";
        return 49;
    }

    // 关窗口 = 落盘（点 X / Esc / 按钮都走 done()）
    dialog.close();
    QApplication::processEvents();

    const QString xmlPath = HandwritingLibrary::adjustmentPathFor(csvPath);
    if (!QFile::exists(xmlPath)) {
        QTextStream(stderr) << "ui-test 失败：关闭对话框没有写出 " << xmlPath << "\n";
        return 50;
    }

    HandwritingLibrary reloaded;
    if (!reloaded.loadFile(csvPath)) {
        QTextStream(stderr) << "ui-test 失败：重开 CSV 失败\n";
        return 51;
    }
    const HandwritingSample *again = reloaded.sampleAt(csvPath, 0);
    if (!again || qAbs(reloaded.adjustment(*again).baseline - 0.075) > 1e-6) {
        QTextStream(stderr) << "ui-test 失败：重开之后校正没读回来\n";
        return 52;
    }

    QTextStream(stdout) << "ui-test: 基线/大小校正 可调、可存、可复用（" << xmlPath << "）\n";

    /*!
     * 校对表也顺带看一眼：它的预览和对话框用的是同一套画法
     * （都是"dpr 2 的 pixmap + 逻辑坐标"），改了绘制口径就得一起验证，
     * 而且它平时没有别的自动检查覆盖。
     */
    {
        HandwritingProofSheet sheet(&library, {}, 0.20);
        sheet.resize(760, 520);
        sheet.show();
        for (int i = 0; i < 20; ++i)
            QApplication::processEvents();
        sheet.grab().save(outDir + QStringLiteral("/uitest_proofsheet.png"));
        sheet.close();
        QApplication::processEvents();
        QTextStream(stdout) << "ui-test: 校对表截图 " << outDir << "/uitest_proofsheet.png\n";
    }
    return 0;
}

/*!
 * \brief 在窗口截图里量"纸面"的横向范围（纯白那一段），单位是设备像素。
 *
 * 取整行里**最左和最右**的白像素，不是"第一段连续的白"：正文的墨会把白色打断，
 * 只取第一段的话量到的是"纸左边缘到第一个字"，纸宽就废了。
 * 这一行上除了纸面没有别的白色（桌面、侧栏都是深色），所以首尾就是纸的左右边界。
 */
static bool measurePaperSpan(const QImage &img, int yDevice, int *left, int *right)
{
    if (yDevice < 0 || yDevice >= img.height())
        return false;

    int l = -1;
    int r = -1;
    for (int x = 0; x < img.width(); ++x) {
        const QRgb c = img.pixel(x, yDevice);
        if (qRed(c) > 245 && qGreen(c) > 245 && qBlue(c) > 245) {
            if (l < 0)
                l = x;
            r = x;
        }
    }
    if (l < 0 || r <= l)
        return false;
    *left = l;
    *right = r;
    return true;
}

/*!
 * \brief 缩放自检。
 *
 * 「比例尺是视图属性」这句话不是口号，得能验：
 *   1. 屏幕上那张纸真的按比例变宽了（量纯白像素的横向范围，不看算出来的数）；
 *   2. 放大之后鼠标还点得准（文档坐标 <-> 视口坐标的逆变换，鼠标命中走的就是它）；
 *   3. Ctrl+滚轮能改缩放，而且**鼠标底下那个字不动**（锚点）；
 *   4. 状态栏右下角的滑块 / 百分比和编辑区永远一致；
 *   5. 缩放**不改文档**（document()->isModified() 必须还是 false）——
 *      不然"调个比例尺"会把用户的文件标成已修改，甚至影响保存。
 *
 * 用 125% 而不是 200% 来量纸宽：窗口里放得下才能量出左右两条边界；
 * 200% 单独用来验"纸比编辑区宽"时居中量归零、纸从最左边开始画。
 *
 * 正文特意写成**几十行**：锚点要成立，竖滚动条必须有可滚的余量 ——
 * 只有两行字时滚动范围是 0，缩放时想滚也滚不动（那种情况下"鼠标底下的字不动"
 * 本来就做不到，不是 bug）。
 */
static int runZoomProbe(MainWindow &window, const QString &outDir)
{
    TextEditor *editor = window.findChild<TextEditor *>();
    QSlider *slider = window.findChild<QSlider *>(QStringLiteral("zoomSlider"));
    QLabel *label = window.findChild<QLabel *>(QStringLiteral("zoomLabel"));
    if (!editor || !slider || !label) {
        QTextStream(stderr) << "zoom 失败：找不到编辑区或状态栏上的缩放控件（滑块/百分比）\n";
        return 30;
    }

    {
        QStringList lines;
        for (int i = 0; i < 60; ++i)
            lines << QStringLiteral("第 %1 行：ZOOM 缩放对照 abc 123").arg(i + 1);
        editor->setPlainText(lines.join(QLatin1Char('\n')));
    }
    editor->moveCursor(QTextCursor::Start);
    editor->document()->setModified(false);
    editor->setZoom(1.0);
    for (int i = 0; i < 20; ++i) {
        QApplication::processEvents();
        QThread::msleep(5);
    }

    const double dpr = window.devicePixelRatioF();
    const double paper0 = editor->paperViewWidthPx();
    const double pad0 = editor->paperPadPx();
    const QPointF origin0 = editor->documentOriginInViewport();
    // 纸面原点里本来就含滚动量：比“几何还原”时要把它减掉（滚动是另一回事）
    const int scroll0 = editor->verticalScrollBar()->value();

    int span0Left = 0;
    int span0Right = 0;
    {
        const QImage win = window.grab().toImage();
        if (!measurePaperSpan(win, int(700 * dpr), &span0Left, &span0Right)) {
            QTextStream(stderr) << "zoom 失败：100% 时在截图里量不到纸面\n";
            return 31;
        }
    }
    const double span0 = span0Right - span0Left + 1;
    QTextStream(stdout) << "zoom: 100% 纸宽(算)=" << paper0 << " 居中留白=" << pad0
                        << " 纸宽(量)=" << span0 / dpr << " 设备像素比=" << dpr << "\n";
    if (qAbs(span0 / dpr - paper0) > 3.0) {
        QTextStream(stderr) << "zoom 失败：100% 时量到的纸宽 " << span0 / dpr
                            << " 和页面设置的纸宽 " << paper0 << " 对不上\n";
        return 32;
    }

    // ---- 125%：屏幕上的纸必须真的宽了 25%，而且鼠标还点得准 ----
    editor->setZoom(1.25);
    for (int i = 0; i < 20; ++i) {
        QApplication::processEvents();
        QThread::msleep(5);
    }

    const double paper125 = editor->paperViewWidthPx();
    if (qAbs(paper125 - paper0 * 1.25) > 0.5) {
        QTextStream(stderr) << "zoom 失败：125% 时纸宽 " << paper125 << " 不等于 "
                            << paper0 * 1.25 << "\n";
        return 33;
    }
    /*!
     * 居中留白**不是**按比例缩放的：它是"编辑区宽度减掉纸宽再平分"，
     * 编辑区宽度没变、纸宽乘了 1.25，所以留白按
     * pad0 - 纸宽 × (1.25 - 1) / 2 变小。写死"pad0×1.25"是错的（第一版就是这么写的）。
     */
    const double expectPad125 = pad0 - paper0 * 0.25 / 2.0;
    if (qAbs(editor->paperPadPx() - expectPad125) > 1.5) {
        QTextStream(stderr) << "zoom 失败：125% 时纸张居中留白 " << editor->paperPadPx()
                            << " 不等于 " << expectPad125 << "（纸没重新居中）\n";
        return 34;
    }

    {
        const QImage win = window.grab().toImage();
        win.save(outDir + QStringLiteral("/uitest_zoom.png"));
        int l = 0;
        int r = 0;
        if (!measurePaperSpan(win, int(700 * dpr), &l, &r)) {
            QTextStream(stderr) << "zoom 失败：125% 时在截图里量不到纸面\n";
            return 35;
        }
        const double span = r - l + 1;
        QTextStream(stdout) << "zoom: 125% 纸宽(量)=" << span / dpr << "（100% 时 "
                            << span0 / dpr << "）纸左边缘=" << l / dpr << "\n";
        if (qAbs(span / span0 - 1.25) > 0.02) {
            QTextStream(stderr) << "zoom 失败：屏幕上的纸没有按 1.25 倍放大（量到 "
                                << span / span0 << "）\n";
            return 36;
        }
    }

    /*!
     * 命中测试：拿一个字符的**文档坐标中点**正变换到视口坐标，
     * 再让命中测试（鼠标点下去走的那条路）反算回来，位置必须一样。
     * 缩放一旦只改了绘制没改逆变换，这里立刻就不等 —— 症状就是"点哪儿都不对"。
     */
    {
        const int want = 8; // 第一行里的一个字（"对"）
        const QTextBlock block = editor->document()->firstBlock();
        const QTextLine line = block.layout()->lineForTextPosition(want);
        const QRectF blockRect = editor->document()->documentLayout()->blockBoundingRect(block);
        if (!line.isValid()) {
            QTextStream(stderr) << "zoom 失败：取不到第一行\n";
            return 37;
        }
        const double x0 = line.cursorToX(want);
        const double x1 = line.cursorToX(want + 1);
        const QPointF docPoint(blockRect.left() + (x0 + x1) / 2.0,
                               blockRect.top() + line.y() + line.height() / 2.0);
        const QPointF vpPoint = editor->documentToViewport().map(docPoint);
        const QTextCursor hit = editor->documentCursorAt(vpPoint.toPoint());
        QTextStream(stdout) << "zoom: 125% 命中测试 文档点=" << docPoint.x() << ","
                            << docPoint.y() << " -> 视口=" << vpPoint.x() << "," << vpPoint.y()
                            << " -> 命中位置=" << hit.position() << "（应为 " << want << "）\n";
        if (hit.position() != want) {
            QTextStream(stderr) << "zoom 失败：放大后鼠标命中错位，位置 " << hit.position()
                                << " 而不是 " << want << "\n";
            return 38;
        }
    }

    // 状态栏那套控件必须跟着编辑区走
    if (slider->value() != 125 || label->text() != QStringLiteral("125%")) {
        QTextStream(stderr) << "zoom 失败：状态栏没跟着改（滑块 " << slider->value()
                            << " 标签「" << label->text() << "」）\n";
        return 39;
    }
    // 反过来：拖滑块要能改编辑区
    slider->setValue(75);
    QApplication::processEvents();
    if (qAbs(editor->zoom() - 0.75) > 1e-9) {
        QTextStream(stderr) << "zoom 失败：拖状态栏滑块没有改到编辑区的缩放（"
                            << editor->zoom() << "）\n";
        return 40;
    }

    /*!
     * Ctrl+滚轮：改缩放，而且**鼠标底下那一点**（纵向）不能跑。
     * 横向不锚定是有意的 —— 纸张靠 viewport 左边距重新居中，
     * 放大时纸本来就该往中间收。
     */
    {
        editor->setZoom(1.0);
        QApplication::processEvents();
        const QPointF mouse(160.0, 200.0);
        const QPointF docUnderMouse = editor->documentToViewport().inverted().map(mouse);

        QWheelEvent wheel(mouse, QPointF(editor->viewport()->mapToGlobal(mouse.toPoint())), QPoint(),
                          QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase,
                          false);
        QApplication::sendEvent(editor->viewport(), &wheel);
        QApplication::processEvents();

        const double zoomAfter = editor->zoom();
        const QPointF back = editor->documentToViewport().map(docUnderMouse);
        QTextStream(stdout) << "zoom: Ctrl+滚轮 一档 -> " << zoomAfter * 100.0
                            << "%；锚点 y 从 " << mouse.y() << " 变成 " << back.y() << "\n";
        if (qAbs(zoomAfter - (1.0 + TextEditor::kZoomStep)) > 1e-9) {
            QTextStream(stderr) << "zoom 失败：Ctrl+滚轮没有把缩放改一档（变成 " << zoomAfter
                                << "）\n";
            return 41;
        }
        if (qAbs(back.y() - mouse.y()) > 1.5) {
            QTextStream(stderr) << "zoom 失败：Ctrl+滚轮之后鼠标底下那一点跑了 "
                                << (back.y() - mouse.y()) << " 像素\n";
            return 42;
        }
        if (slider->value() != qRound(zoomAfter * 100.0)) {
            QTextStream(stderr) << "zoom 失败：Ctrl+滚轮之后状态栏滑块没同步（"
                                << slider->value() << "）\n";
            return 43;
        }
    }

    // 200%：纸比编辑区宽，居中量归零、纸从编辑区最左边开始画
    {
        editor->setZoom(2.0);
        for (int i = 0; i < 20; ++i) {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        const QImage win = window.grab().toImage();
        win.save(outDir + QStringLiteral("/uitest_zoom_200.png"));
        int l = 0;
        int r = 0;
        const bool measured = measurePaperSpan(win, int(700 * dpr), &l, &r);
        QTextStream(stdout) << "zoom: 200% 居中留白=" << editor->paperPadPx()
                            << " 纸宽(算)=" << editor->paperViewWidthPx()
                            << (measured ? QStringLiteral(" 纸左边缘=%1").arg(l / dpr)
                                         : QStringLiteral(" 量不到"))
                            << "\n";
        if (editor->paperPadPx() != 0.0) {
            QTextStream(stderr) << "zoom 失败：纸比编辑区宽时纸张居中留白应当是 0（现在是 "
                                << editor->paperPadPx() << "）\n";
            return 44;
        }
        if (measured && l > 2) {
            QTextStream(stderr) << "zoom 失败：纸比编辑区宽时纸应当从最左边开始画（量到 "
                                << l / dpr << "）\n";
            return 45;
        }
        /*!
         * 判"居中量归零"要看 **viewport 左边距**，不能拿 viewport 宽度和编辑区宽度比：
         * 内容一长竖滚动条就出来了，viewport 本来就该窄一条滚动条的宽度。
         */
        if (editor->viewport()->x() != 0) {
            QTextStream(stderr) << "zoom 失败：居中留白没归零，viewport 左边距是 "
                                << editor->viewport()->x() << "\n";
            return 46;
        }
    }

    // 回到 100%：几何必须原样回来（滚动位置也拨回 0 —— 缩放本来就该能滚）
    editor->setZoom(1.0);
    editor->verticalScrollBar()->setValue(0);
    for (int i = 0; i < 20; ++i) {
        QApplication::processEvents();
        QThread::msleep(5);
    }

    /*!
     * 滚动之后光标还得画在对的位置。
     *
     * 这里踩过一个一直没被发现的坑：`QTextEdit::cursorRect()` 返回的是
     * **"文档坐标 - 滚动量"**（实测：文档 y=130 的光标在竖滚动 200 之后返回 -70，
     * 横滚动 300 时 x 从 0 变成 -300），而本控件画光标时加的那个原点里**已经**含了
     * -滚动量 —— 于是滚动之后被再减一次，光标直接画到纸外面去（屏幕上看不见它）。
     * 以前的自检只在"没滚动过"的空文档上量光标，正好绕开了这一条。
     *
     * 手法：整篇全是空行（画面上除了光标一根墨都没有），把光标放到第 20 行、
     * 竖滚动 200，然后在截图里找那根竖条，和"算出来的位置"比。
     */
    {
        QStringList blanks;
        for (int i = 0; i < 60; ++i)
            blanks << QString();
        editor->setPlainText(blanks.join(QLatin1Char('\n')));
        const QTextBlock block = editor->document()->findBlockByNumber(20);
        QTextCursor caretCursor(editor->document());
        caretCursor.setPosition(block.position());
        editor->setTextCursor(caretCursor);
        /*!
         * 光标只在**有焦点**时画（见 TextEditor::drawCaret）。无头自检里
         * 窗口不一定被激活，所以这里既 `setFocus()` 又直接给 viewport 焦点，
         * 保证量到的不是"因为没焦点所以没画"。
         */
        editor->setFocus(Qt::OtherFocusReason);
        editor->viewport()->setFocus(Qt::OtherFocusReason);
        /*!
         * 再把闪烁关掉：`cursorFlashTime() == 0` 时 Qt 的光标一直可见。
         * 不关的话"抓 60 次总有一次在亮相位"是靠运气的 ——
         * 实测同一份二进制跑三次会失败一次（每次 grab 都在同一个相位上，
         * 撞上暗相位就永远找不到那根竖条）。
         */
        QApplication::setCursorFlashTime(0);
        editor->verticalScrollBar()->setValue(200);
        for (int i = 0; i < 20; ++i) {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        QTextStream(stdout) << "zoom: [光标量测] 焦点 editor=" << editor->hasFocus()
                            << " viewport=" << editor->viewport()->hasFocus() << "\n";

        /*!
         * 量测一律在 **viewport 自己的那张图**里做：
         * `window.grab()` 会拿到子控件的旧后备存储（实测滚动之后纸是新的、
         * 纸上的字和光标还是滚动之前那一帧），而 viewport 的图没有这个问题，
         * 光标、正文、纸面本来就都画在里面。
         */
        const double dpr = window.devicePixelRatioF();
        const QPointF docOrigin = editor->documentOriginInViewport();
        const double docY = editor->document()->documentLayout()->blockBoundingRect(block).top();
        /*!
         * 期望位置一律用**那一个变换**（documentToViewport）算。
         *
         * 手算“纸面原点 + 页边距 + 滚动量”看着简单，但文档坐标改成纸面
         * 口径（原点 = 纸的左上角）之后，`blockBoundingRect().top()`
         * 里就已经含了正文上边距 —— 再用“正文原点 + 文档 y”去算，
         * 上边距就被算了两遍（这里真踩过：光标明明画对了，量的时候找不到）。
         */
        /*!
         * 期望位置一律用**那一个变换**（documentToViewport）算 ——
         * 文档坐标 = 纸面坐标，所以“文档原点 + 页边距 + 滚动量”那种手算
         * 迟早会多算/少算一项（这里真踩过：正文上边距被算了两遍）。
         * 横向也要用块自己的左边界：光标在正文左边距上，不是纸的左边。
         */
        const QRectF blockRect = editor->document()->documentLayout()->blockBoundingRect(block);
        const QPointF wantPoint =
            editor->documentToViewport().map(QPointF(blockRect.left(), docY));
        const double wantTop = wantPoint.y(); // viewport 局部坐标
        const double wantLeft = wantPoint.x();
        QTextStream(stdout) << "zoom: [光标量测] 正文原点 y=" << docOrigin.y()
                            << " 第 20 段文档 y=" << docY << " 预期(view 局部) y=" << wantTop
                            << " 滚动=" << editor->verticalScrollBar()->value() << "/"
                            << editor->verticalScrollBar()->maximum() << " 视口高="
                            << editor->viewport()->height() << "\n";

        int foundTop = -1;
        int foundLeft = -1;
        int foundBottom = -1;
        for (int attempt = 0; attempt < 60 && foundTop < 0; ++attempt) {
            QApplication::processEvents();
            QThread::msleep(25); // 光标会闪：暗相位抓不到，多抓几次
            /*!
             * 每次抓之前**强制一次同步重绘**：`viewport()->grab()` 拿的是
             * 后备存储里的内容，光 `processEvents()` 不保证绘制已经发生 ——
             * 不强制的话可能反复抓到同一帧（正好是暗相位就永远找不到光标）。
             */
            editor->viewport()->repaint();
            QApplication::processEvents();
            const QImage shot = editor->viewport()->grab().toImage();
            const int y0 = qMax(0, int((wantTop - 8) * dpr));
            const int y1 = qMin(shot.height(), int((wantTop + 40) * dpr));
            const int x0 = qMax(0, int(wantLeft * dpr));
            const int x1 = qMin(shot.width(), int((wantLeft + 60) * dpr));
            for (int x = x0; x < x1 && foundTop < 0; ++x) {
                int top = -1;
                int bottom = -1;
                for (int y = y0; y < y1; ++y) {
                    const QRgb px = shot.pixel(x, y);
                    if (qRed(px) < 90 && qGreen(px) < 90 && qBlue(px) < 90) {
                        if (top < 0)
                            top = y;
                        bottom = y;
                    }
                }
                // 竖条：连续贯通大半个行高才算光标
                if (top >= 0 && (bottom - top + 1) >= int(14 * dpr)) {
                    foundTop = top;
                    foundBottom = bottom;
                    foundLeft = x;
                }
            }
        }
        if (foundTop < 0) {
            QTextStream(stderr) << "zoom 失败：滚动 200 之后画面上找不到光标竖条"
                                   "（它该画在窗口 y≈"
                                << wantTop << " 的那一行上）\n";
            return 49;
        }
        const double dy = foundTop / dpr - wantTop;
        const double dx = foundLeft / dpr - wantLeft;
        QTextStream(stdout) << "zoom: 滚动 200 后光标竖条 量到 y=" << foundTop / dpr << "（应为 "
                            << wantTop << "，偏差 " << dy << "）、x=" << foundLeft / dpr
                            << "（应为 " << wantLeft << "，偏差 " << dx << "）、长约 "
                            << (foundBottom - foundTop + 1) / dpr << "\n";
        if (qAbs(dy) > 4.0 || qAbs(dx) > 4.0) {
            QTextStream(stderr) << "zoom 失败：滚动之后光标画错位置（偏移 " << dx << "," << dy
                                << "）—— cursorRect() 给的是「文档坐标 - 滚动量」，得加回来\n";
            return 49;
        }
    }

    /*!
     * 放大之后纸比编辑区宽：Shift+滚轮要能横向平移，平移之后鼠标还得点得准
     * （横向平移量就在那个唯一的坐标变换里，命中测试自然也一起走）。
     */
    {
        editor->setPlainText(QStringLiteral("横向平移 abcdefghijklmnopqrstuvwxyz\n"));
        editor->setZoom(2.0);
        for (int i = 0; i < 20; ++i) {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        QScrollBar *hbar = editor->horizontalScrollBar();
        if (hbar->maximum() <= 0) {
            QTextStream(stdout) << "zoom: 200% 时横向没有可平移的余量，跳过横向平移检查\n";
        } else {
            hbar->setValue(0);
            QApplication::processEvents();
            /*!
             * 命中测试要在**平移前后各做一次**：两次都得对，才能说明
             * "平移量进了那个唯一的变换"（只测平移后的话，
             * 一个与平移量无关的固定偏差也会被算成平移的错）。
             */
            auto hitAt = [&](int want) {
                const QTextBlock block = editor->document()->firstBlock();
                const QTextLine line = block.layout()->lineForTextPosition(want);
                const QRectF blockRect =
                    editor->document()->documentLayout()->blockBoundingRect(block);
                const double x0 = line.cursorToX(want);
                const double x1 = line.cursorToX(want + 1);
                const QPointF docPoint(blockRect.left() + (x0 + x1) / 2.0,
                                       blockRect.top() + line.height() / 2.0);
                const QPointF vpPoint = editor->documentToViewport().map(docPoint);
                const QTextCursor hit = editor->documentCursorAt(vpPoint.toPoint());
                return QVector<double>{docPoint.x(), vpPoint.x(), double(hit.position())};
            };
            const int want = 8;
            const QVector<double> before0 = hitAt(want);
            hbar->setValue(150);
            QApplication::processEvents();
            const QVector<double> after0 = hitAt(want);
            QTextStream(stdout) << "zoom: 命中对照 平移前 文档x=" << before0.at(0) << " 视口x="
                                << before0.at(1) << " 命中=" << before0.at(2)
                                << "；平移 150 后 文档x=" << after0.at(0)
                                << " 视口x=" << after0.at(1) << " 命中=" << after0.at(2) << "\n";
            if (after0.at(1) >= before0.at(1)) {
                QTextStream(stderr) << "zoom 失败：横向平移之后那个文档点并没有往左移"
                                       "（视口 x "
                                    << before0.at(1) << " -> " << after0.at(1) << "）\n";
                return 49;
            }
            /*!
             * 判据是"同一个文档点，平移前后命中同一个字符"：
             * 平移量必须进那个唯一的坐标变换，命中测试才会跟着走。
             * 不强求命中 == want —— `cursorToX()` 给的是**步进边界**，
             * 而 `hitTest()` 按字形本身的边界判，两者在窄字符上会差一格
             * （实测：这里 want=8 时命中 9，平移前后都是 9，所以拿"前后一致"当判据）。
             */
            if (after0.at(2) != before0.at(2) || qAbs(after0.at(2) - want) > 1.0) {
                QTextStream(stderr) << "zoom 失败：横向平移之后鼠标命中变了（" << before0.at(2)
                                    << " -> " << after0.at(2) << "）\n";
                return 49;
            }
            hbar->setValue(0);
            QApplication::processEvents();
            QWheelEvent shiftWheel(
                QPointF(300, 200), QPointF(editor->viewport()->mapToGlobal(QPoint(300, 200))),
                QPoint(), QPoint(0, -120), Qt::NoButton, Qt::ShiftModifier, Qt::NoScrollPhase,
                false);
            QApplication::sendEvent(editor->viewport(), &shiftWheel);
            QApplication::processEvents();
            QTextStream(stdout) << "zoom: 200% Shift+滚轮 横向滚动值 0 -> " << hbar->value()
                                << "（上限 " << hbar->maximum() << "）纸面原点 x="
                                << editor->paperOriginInViewport().x() << "\n";
            if (hbar->value() <= 0) {
                QTextStream(stderr) << "zoom 失败：Shift+滚轮没有横向平移（值还是 " << hbar->value()
                                    << "）\n";
                return 49;
            }
        }
        editor->setZoom(1.0);
        editor->horizontalScrollBar()->setValue(0);
        QApplication::processEvents();
    }

    if (qAbs(editor->paperViewWidthPx() - paper0) > 0.5
        || qAbs(editor->paperPadPx() - pad0) > 1.5
        // 减掉滚动量再比：缩放只该改几何，不该把“滚到哪儿”也算进去
        || qAbs((editor->documentOriginInViewport().y() + editor->verticalScrollBar()->value())
                - (origin0.y() + scroll0)) > 1.0) {
        QTextStream(stderr) << "zoom 失败：回到 100% 之后几何没还原（纸宽 "
                            << editor->paperViewWidthPx() << " 留白 " << editor->paperPadPx()
                            << "）\n";
        return 47;
    }
    if (editor->document()->isModified()) {
        QTextStream(stderr) << "zoom 失败：改缩放把文档标成了已修改（缩放必须只是视图属性）\n";
        return 48;
    }

    QTextStream(stdout) << "zoom: 缩放自检通过（屏幕按比例放大、命中不错位、"
                           "Ctrl+滚轮有锚点、状态栏同步、文档不被改动）\n";
    return 0;
}

/*!
 * \brief "给手写加噪声"自检。
 *
 * 用户要的是：**已经有手写的字，套扭曲时噪声加在手写笔迹上**，
 * 而不是把它换回机打字体的字形再加噪声。这条路上有三件事必须一起成立：
 *   1. Ctrl+D 之后字符格式里还是 Handwriting（不是被改写成 Distortion）——
 *      这是一条写死的判据，见 distortionEffectKind()；
 *   2. 画面上第一行里出现的仍然是**手写层的深蓝墨**，近黑的机打字墨几乎为零
 *      （手写"遮住正文"是默认状态）；
 *   3. 反过来，没铺过手写的字套扭曲，仍然要变成"扭曲后的字形"（老行为不能丢）。
 *
 * 手写数据没载入时直接跳过：那种情况下 act_applyhw 会弹一个模态框，
 * 在自检里等于卡死，所以先看侧栏那句话再决定跑不跑。
 */
static int runHandwritingNoiseProbe(MainWindow &window, const QString &outDir)
{
    TextEditor *editor = window.findChild<TextEditor *>();
    QAction *applyHw = window.findChild<QAction *>(QStringLiteral("act_applyhw"));
    QAction *distort = window.findChild<QAction *>(QStringLiteral("act_distort"));
    if (!editor || !applyHw || !distort) {
        QTextStream(stderr) << "hwnoise 失败：找不到编辑区或手写/扭曲动作\n";
        return 50;
    }

    {
        // 侧栏那句"尚未载入手写数据…"就是"按下 Ctrl+H 会弹框"的信号
        bool loaded = false;
        bool empty = false;
        for (QLabel *label : window.findChildren<QLabel *>()) {
            if (label->text().contains(QStringLiteral("尚未载入手写数据")))
                empty = true;
            if (label->text().contains(QStringLiteral("已载入"))
                && label->text().contains(QStringLiteral("个字符")))
                loaded = true;
        }
        if (empty || !loaded) {
            QTextStream(stdout) << "hwnoise: 跳过（窗口里没有手写数据，铺手写会弹模态框）\n";
            return 0;
        }
    }

    const QString sample = QStringLiteral("abcdefgh");

    /*!
     * 纸面第一行的横条（窗口坐标，设备像素）——只在这里数墨，
     * 免得把工具栏、侧栏的东西算进来。
     */
    auto lineBand = [&](QRect *rect) {
        const QTextBlock block = editor->document()->firstBlock();
        const QRectF blockRect = editor->document()->documentLayout()->blockBoundingRect(block);
        const QPoint vpInWindow = editor->viewport()->mapTo(&window, QPoint(0, 0));
        const double dpr = window.devicePixelRatioF();
        /*!
         * 位置一律走 documentToViewport()（文档坐标 -> 屏幕）。
         *
         * 以前是“文档原点 + blockRect.top()”手算，那时文档原点是**正文左上角**，
         * 正好对得上；现在文档原点是**纸的左上角**（见 paginatinglayout.h），
         * 同一个手算就漏掉了一个页边距 —— 量到的是纸边上的空白，
         * 看着就像“一个字都没画”。
         */
        const QPointF origin =
            QPointF(vpInWindow)
            + editor->documentToViewport().map(QPointF(blockRect.left(), blockRect.top()));
        const QRectF band(origin.x() - 4.0, origin.y() - 2.0,
                          qMax(60.0, blockRect.width()) + 14.0, blockRect.height() + 8.0);
        *rect = QRect(int(band.left() * dpr), int(band.top() * dpr), int(band.width() * dpr),
                      int(band.height() * dpr));
        return dpr;
    };

    auto countInk = [](const QImage &img, const QRect &band, int *navy, int *black) {
        *navy = 0;
        *black = 0;
        const QRect area = band.intersected(img.rect());
        for (int y = area.top(); y <= area.bottom(); ++y) {
            for (int x = area.left(); x <= area.right(); ++x) {
                const QRgb c = img.pixel(x, y);
                /*!
                 * 手写层的墨是深蓝 0x111c4b（"遮住正文"时的颜色），
                 * 机打字 / 扭曲层的墨是近黑 0x1a1a1a —— 两者一眼能分开：
                 * 前者蓝分量明显高于红分量，后者三通道挨在一起。
                 */
                if (qBlue(c) > qRed(c) + 25 && qBlue(c) < 180 && qRed(c) < 120)
                    ++*navy;
                else if (qRed(c) < 70 && qGreen(c) < 70 && qBlue(c) < 70
                         && qAbs(qRed(c) - qBlue(c)) < 14)
                    ++*black;
            }
        }
    };

    editor->setPlainText(sample);
    editor->moveCursor(QTextCursor::Start);
    editor->document()->setModified(false);

    // ---- 1) 铺手写（真实入口：Ctrl+H）----
    editor->selectAll();
    applyHw->trigger();
    editor->moveCursor(QTextCursor::Start); // 收掉选区：量墨时不要和高亮色混在一起
    for (int i = 0; i < 20; ++i) {
        QApplication::processEvents();
        QThread::msleep(5);
    }

    auto kindsInDocument = [&] {
        QSet<int> kinds;
        for (int pos = 0; pos < editor->document()->characterCount() - 1; ++pos) {
            QTextCursor one(editor->document());
            one.setPosition(pos);
            one.setPosition(pos + 1, QTextCursor::KeepAnchor);
            kinds.insert(int(effectStyle(one.charFormat()).kind));
        }
        return kinds;
    };

    {
        const QSet<int> kinds = kindsInDocument();
        if (kinds != QSet<int>{int(EffectKind::Handwriting)}) {
            QTextStream(stderr) << "hwnoise 失败：铺完手写之后字符格式里不是清一色的手写效果\n";
            return 51;
        }
    }

    int navyBefore = 0;
    int blackBefore = 0;
    {
        const QImage win = window.grab().toImage();
        win.save(outDir + QStringLiteral("/uitest_hwnoise_handwriting.png"));
        QRect band;
        const double dpr = lineBand(&band);
        Q_UNUSED(dpr);
        countInk(win, band, &navyBefore, &blackBefore);
        QTextStream(stdout) << "hwnoise: 铺手写后 手写墨=" << navyBefore << " 机打墨="
                            << blackBefore << "\n";
        if (navyBefore < 150 || blackBefore > navyBefore / 4) {
            QTextStream(stderr) << "hwnoise 失败：铺上手写后画面上应当主要是手写墨（手写墨 "
                                << navyBefore << "、机打墨 " << blackBefore << "）\n";
            return 52;
        }
    }

    // ---- 2) 给这些手写字套扭曲：必须保留手写，噪声加到笔迹上 ----
    editor->selectAll();
    distort->trigger();
    editor->moveCursor(QTextCursor::Start);
    for (int i = 0; i < 20; ++i) {
        QApplication::processEvents();
        QThread::msleep(5);
    }

    {
        const QSet<int> kinds = kindsInDocument();
        if (kinds != QSet<int>{int(EffectKind::Handwriting)}) {
            QTextStream(stderr) << "hwnoise 失败：套扭曲之后手写效果被换掉了（这是这次要修的 bug："
                                   "噪声应当加在手写笔迹上，而不是用回原来的字体）\n";
            return 53;
        }
    }

    int navyAfter = 0;
    int blackAfter = 0;
    {
        const QImage win = window.grab().toImage();
        win.save(outDir + QStringLiteral("/uitest_hwnoise_distorted.png"));
        QRect band;
        lineBand(&band);
        countInk(win, band, &navyAfter, &blackAfter);
        QTextStream(stdout) << "hwnoise: 套扭曲后 手写墨=" << navyAfter << " 机打墨=" << blackAfter
                            << "\n";
        if (navyAfter < 150 || blackAfter > navyAfter / 4) {
            QTextStream(stderr) << "hwnoise 失败：给手写字加噪声之后画面上冒出了机打字墨（手写墨 "
                                << navyAfter << "、机打墨 " << blackAfter
                                << "）—— 字被换回原字体了\n";
            return 54;
        }
    }
    // 噪声真的动了：笔迹的像素不会一模一样
    {
        const QImage a(outDir + QStringLiteral("/uitest_hwnoise_handwriting.png"));
        const QImage b(outDir + QStringLiteral("/uitest_hwnoise_distorted.png"));
        if (a.size() == b.size() && !a.isNull()) {
            int diff = 0;
            for (int y = 0; y < a.height(); y += 2)
                for (int x = 0; x < a.width(); x += 2)
                    if (a.pixel(x, y) != b.pixel(x, y))
                        ++diff;
            QTextStream(stdout) << "hwnoise: 加噪声前后画面差异像素=" << diff << "\n";
            if (diff == 0) {
                QTextStream(stderr) << "hwnoise 失败：套扭曲前后画面一模一样，噪声没有加到笔迹上\n";
                return 55;
            }
        }
    }

    // ---- 3) 没铺过手写的字：套扭曲仍然要换成扭曲字形（老行为）----
    /*!
     * 注意先把"当前字符格式"清空：QTextEdit::setPlainText 是**带着当前格式**
     * 插入的（Qt 有意这么设计，方便接着上次的字体往下写），
     * 不清的话刚铺的手写效果会跟着新文字一起进来，这里就不是"普通文字"了。
     */
    editor->setCurrentCharFormat(QTextCharFormat());
    editor->setPlainText(sample);
    {
        QTextCursor all = editor->textCursor();
        all.select(QTextCursor::Document);
        editor->setTextCursor(all);
    }
    distort->trigger();
    editor->moveCursor(QTextCursor::Start);
    for (int i = 0; i < 20; ++i) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    {
        const QSet<int> kinds = kindsInDocument();
        if (kinds != QSet<int>{int(EffectKind::Distortion)}) {
            QTextStream(stderr) << "hwnoise 失败：普通文字套扭曲之后应当是扭曲字形效果"
                                   "（这条路上不该出现手写），实际收到 "
                                << kinds.size() << " 种效果\n";
            return 56;
        }
    }

    QTextStream(stdout) << "hwnoise: 手写加噪声自检通过（手写保留、机打字不回来、"
                           "普通文字照旧扭曲字形）\n";
    return 0;
}

/*!
 * \brief 长活儿自检：阻塞式操作必须变成"分片 + 进度条 + 可取消"。
 *
 * 用户报的问题是"写着写着就卡"：给整篇加噪声、铺手写这类操作原来是一口气跑完的
 * （两千字 78ms，两万字 800ms），期间窗口完全没响应。
 *
 * 这里量的是**行为**，不是实现：
 *   1. 动作一触发就返回（不阻塞在菜单回调里）；
 *   2. 任务进行期间事件循环还在转（挂一个 0 间隔心跳数次数）；
 *   3. 状态栏上真的出现了进度条，而且百分比在走；
 *   4. 干完之后编辑器恢复可编辑（临时只读必须被恢复，否则"打完字不能编辑"）；
 *   5. Esc 能取消，而且**取消 = 什么都没发生**（效果字符数回到 0）。
 */
static int runProgressProbe(MainWindow &window, const QString &outDir)
{
    TextEditor *editor = window.findChild<TextEditor *>();
    if (!editor) {
        QTextStream(stderr) << "progress 失败：找不到编辑区\n";
        return 70;
    }

    QProgressBar *bar = window.findChild<QProgressBar *>(QStringLiteral("jobBar"));
    QLabel *label = window.findChild<QLabel *>(QStringLiteral("jobLabel"));
    if (!bar || !label) {
        QTextStream(stderr) << "progress 失败：状态栏上没有进度条 / 任务名标签\n";
        return 71;
    }

    // 数一份文档里有多少字符带着效果
    auto effectChars = [editor]() {
        int n = 0;
        QTextDocument *doc = editor->document();
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
                const QTextFragment f = it.fragment();
                if (!f.isValid())
                    continue;
                if (effectStyle(f.charFormat()).isValid())
                    n += f.text().size();
            }
        }
        return n;
    };

    // 一段够长的稿子：两千字以上，任务一定会跨很多片
    {
        QStringList lines;
        for (int i = 0; i < 200; ++i)
            lines << QStringLiteral("进度自检第%1行，正文内容正文内容正文内容")
                         .arg(i + 1, 2, 10, QLatin1Char('0'));
        editor->setCurrentCharFormat(QTextCharFormat());
        editor->setPlainText(lines.join(QLatin1Char('\n')));
        QTextCursor all = editor->textCursor();
        all.select(QTextCursor::Document);
        editor->setTextCursor(all);
        /*!
         * 视口滚回开头，但**选区必须留着**。
         *
         * 不能用 `moveCursor(QTextCursor::Start)`：那会把选区清掉，
         * 于是动作里那句"请先选中一段文字"就弹出来了 —— 而这是个模态对话框，
         * 它的嵌套事件循环会把整轮自检卡死（实测卡了 350 秒、中途撺了 600 次重绘）。
         * 保留锚点、只把插入点移到开头，既滚到了顶又没动选区。
         */
        /*!
         * 注意 `QTextCursor::setPosition(0, KeepAnchor)` 在光标**已经在 0** 时
         * 会把锚点也拉到 0（选区塔掉）—— 实测就是这么丢的选区。
         * 所以先让锚点落在文档末尾，再带着锚点移到开头。
         */
        QTextCursor top(editor->document());
        top.setPosition(editor->document()->characterCount() - 1);
        top.setPosition(0, QTextCursor::KeepAnchor);
        editor->setTextCursor(top);
        editor->verticalScrollBar()->setValue(0);
        QTextStream(stdout) << "ui-test: progress 选前状态 选区="
                            << (editor->textCursor().hasSelection() ? 1 : 0) << " ["
                            << editor->textCursor().selectionStart() << ".."
                            << editor->textCursor().selectionEnd() << "] 只读="
                            << (editor->isReadOnly() ? 1 : 0) << "\n";
    }
    for (int i = 0; i < 10; ++i)
        QApplication::processEvents();

    QAction *distort = window.findChild<QAction *>(QStringLiteral("act_distort"));
    if (!distort) {
        QTextStream(stderr) << "progress 失败：找不到「笔画扭曲」动作\n";
        return 72;
    }
    /*!
     * 没选区就会弹一个模态对话框，而它的嵌套事件循环会把自检整个卡住。
     * 所以这里先自己确认一下：真没选区是**自检的错**，早报早好。
     */
    if (!editor->textCursor().hasSelection()) {
        QTextStream(stderr) << "progress 失败：触发动作之前没有选区（会弹模态对话框卡住）\n";
        return 72;
    }

    /*!
     * 心跳：任务期间事件循环每转一圈就 +1。
     * 这是"界面还活着"唯一能直接量到的证据 —— 同步跑完的活儿心跳只会是 0 或 1。
     */
    int ticks = 0;
    QTimer heartbeat;
    heartbeat.setInterval(0);
    QObject::connect(&heartbeat, &QTimer::timeout, [&ticks] { ++ticks; });
    heartbeat.start();

    QElapsedTimer actionClock;
    actionClock.start();
    distort->trigger();
    const double triggerMs = actionClock.nsecsElapsed() / 1e6;
    if (triggerMs > 50.0) {
        QTextStream(stderr) << "progress 失败：动作回调里就卡了 " << triggerMs
                            << "ms（应当立刻返回，活交给后台分片）\n";
        return 73;
    }

    // 等任务跑完：心跳在转，同时把状态栏上看到的进度都记下来
    int maxPercent = 0;
    int barVisibleTicks = 0;
    qint64 worstIterationMs = 0;
    int labelChanges = 0;
    int loopCount = 0;
    QString lastLabel;
    QStringList seenLabels;
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 20000) {
        QElapsedTimer iteration;
        iteration.start();
        QApplication::processEvents();
        if (bar->isVisible()) {
            ++barVisibleTicks;
            maxPercent = qMax(maxPercent, bar->value() * 100 / qMax(1, bar->maximum()));
        }
        if (label->text() != lastLabel) {
            lastLabel = label->text();
            ++labelChanges;
            if (seenLabels.size() < 6)
                seenLabels.append(lastLabel);
        }
        /*!
         * 什么时候算完了：套完效果 **+ 看得见的格子几何都算好了**。
         *
         * 两个都要等：套格式只是第一段（很快），真正长的是后面
         * 把可见页那几百个字的字形算出来（要进度条的就是那一段）。
         * 而"队列空了"不等于"算完了"—— 刚套完效果的那 150ms 里
         * 几何还排在"等安静"的定时器上，队列本来就是空的。
         */
        if (effectChars() > 0 && !editor->isReadOnly() && wait.elapsed() > 300) {
            bool ready = true;
            if (loopCount++ % 10 == 0) {
                const QPair<int, int> range = editor->layout()->blockRangeOnPages(0, 0);
                for (int n = range.first; n <= range.second && ready; ++n) {
                    for (const EffectDrawItem &item : editor->effectPlanner()->items(n)) {
                        /*!
                         * 看 `geometryDone` 而不是 `art.ready`：
                         * "算过了但算不出来"（格子无效、缺手写数据）是合法结局，
                         * 没结局的只有一种 —— 还没算。
                         */
                        if (!item.geometryDone) {
                            ready = false;
                            break;
                        }
                    }
                }
            } else {
                ready = false;
            }
            if (ready)
                break;
        }

        /*!
         * 无头环境里窗口没有被"暴露"，Qt 不会主动派发绘制事件 ——
         * 而"看得见的这一页要算几何"这个决定是**绘制时**做的（见 paintEvent）。
         * 所以这里主动推一下重绘，模拟"用户正看着这一页"。
         */
        if (loopCount % 5 == 0)
            editor->viewport()->repaint();
        QThread::msleep(2);
        worstIterationMs = qMax(worstIterationMs, iteration.elapsed());
    }
    heartbeat.stop();
    const qint64 totalMs = wait.elapsed();
    if (EffectPlanner *planner = editor->effectPlanner()) {
        QTextStream(stdout) << "ui-test: progress 几何状态：已算 " << planner->preparedItemCount()
                            << " 项 / 待算 " << planner->pendingItemCount() << " 项（规划了 "
                            << planner->plannedBlockCount() << " 段，编辑区可见="
                            << (editor->viewport()->isVisible() ? 1 : 0) << "）\n";
    }

    const int applied = effectChars();
    QTextStream(stdout) << "ui-test: progress 套扭曲：动作回调 " << triggerMs
                        << "ms，任务共 " << totalMs << "ms（单次 processEvents 最长 "
                        << worstIterationMs << "ms），心跳 " << ticks << " 次，进度条最高 "
                        << maxPercent << "%（可见 " << barVisibleTicks << " 次，文案变了 "
                        << labelChanges << " 次），效果字符 " << applied << "\n";
    for (const QString &text : std::as_const(seenLabels))
        QTextStream(stdout) << "ui-test: progress 状态栏文案：" << text << "\n";

    if (ticks < 5) {
        QTextStream(stderr) << "progress 失败：任务期间事件循环几乎没转（心跳 " << ticks
                            << "）—— 还是阻塞式\n";
        return 74;
    }
    /*!
     * 进度条是"晚点出现"的（250ms）：任务很快的时候弹一下再消失比不弹还刺眼。
     * 所以只有任务真的跑久了，才要求它出现过。
     */
    if (totalMs > 1000 && (barVisibleTicks == 0 || labelChanges < 2)) {
        QTextStream(stderr) << "progress 失败：跑 " << totalMs
                            << "ms 的任务居然没弹出进度（可见 " << barVisibleTicks
                            << " 次，文案变了 " << labelChanges << " 次）\n";
        return 75;
    }
    if (labelChanges == 0) {
        QTextStream(stderr) << "progress 失败：状态栏上一条进度都没报\n";
        return 75;
    }
    if (worstIterationMs > 200) {
        QTextStream(stderr) << "progress 失败：单次事件循环花了 " << worstIterationMs
                            << "ms —— 还有一段是同步跑的\n";
        return 75;
    }
    /*!
     * 选区必须还在。
     *
     * 用户看到的症状：选一段字、点【笔画扭曲】、完事之后再点一次，
     * 弹出来"请先选中一段文字"—— 而眼里那段字明明还高亮着。
     * 原因是任务开工前的 `setReadOnly(true)` 把选区丢了（见 beginJobEdit）。
     */
    if (!editor->textCursor().hasSelection()) {
        QTextStream(stderr)
            << "progress 失败：干完活之后选区丢了（再点一次就会弹「请先选中一段文字」）\n";
        return 76;
    }

    if (applied == 0 || editor->isReadOnly()) {
        QTextStream(stderr) << "progress 失败：任务没干完或者编辑器还锁着（只读="
                            << (editor->isReadOnly() ? 1 : 0) << "）\n";
        return 76;
    }

    // 几何也要真的算好：效果层画得出来才算数
    {
        EffectPlanner *planner = editor->effectPlanner();
        int ready = 0;
        int items = 0;
        const QPair<int, int> range = editor->layout()->blockRangeOnPages(0, 0);
        planner->beginFrame();
        planner->requestRange(range.first, range.second);
        QElapsedTimer geo;
        geo.start();
        while (planner->hasPendingWork() && geo.elapsed() < 10000) {
            planner->work(64);
            QApplication::processEvents();
        }
        int attempted = 0;
        for (int n = range.first; n <= range.second; ++n) {
            for (const EffectDrawItem &item : planner->items(n)) {
                ++items;
                if (item.art.ready)
                    ++ready;
                if (item.geometryDone)
                    ++attempted;
            }
        }
        QTextStream(stdout) << "ui-test: progress 第 1 页几何：算了 " << attempted << "/" << items
                            << " 项，其中能画 " << ready << " 项（" << geo.elapsed() << "ms，段 "
                            << range.first << ".." << range.second << "）\n";
        if (items == 0 || attempted != items || ready == 0) {
            QTextStream(stderr) << "progress 失败：第一页还有格子的几何没算好（算了 " << attempted
                                << "/" << items << "）\n";
            return 77;
        }
    }

    /*!
     * 取消：取消必须**停在分片边界并且什么都没留下**。
     *
     * 做法是先把整篇的效果清掉，再套一次然后立刻取消；
     * 结果是"效果字符数为 0"——这正是 EffectStyleJob::finish() 里写回原格式那一段。
     */
    {
        QTextCursor all = editor->textCursor();
        all.select(QTextCursor::Document);
        editor->setTextCursor(all);
        clearEffects(&all);
        editor->setTextCursor(all);
        for (int i = 0; i < 10; ++i)
            QApplication::processEvents();
        const int before = effectChars();
        /*!
         * 取消走**状态栏上那个 ✕ 按钮**（用户看得见、点得到的那条路）。
         *
         * 为什么不发 Esc：快捷键是 QShortcut 的活，它要经过
         * QApplication 的快捷键分派（无头环境下窗口不是 active window，分派不成立），
         * 直接 sendEvent 一个 QKeyEvent 是**绕过**那套机制的，测不到真东西。
         */
        QToolButton *cancelButton = window.findChild<QToolButton *>(QStringLiteral("jobCancel"));
        if (!cancelButton) {
            QTextStream(stderr) << "progress 失败：状态栏上没有取消按钮\n";
            return 79;
        }

        int cancelTicks = 0;
        QTimer cancelHeartbeat;
        cancelHeartbeat.setInterval(0);
        QObject::connect(&cancelHeartbeat, &QTimer::timeout, [&cancelTicks] { ++cancelTicks; });
        cancelHeartbeat.start();

        distort->trigger();
        // 让它跑几片（进度条动起来）再取消
        QElapsedTimer run;
        run.start();
        while (run.elapsed() < 40) {
            QApplication::processEvents();
            QThread::msleep(1);
        }
        cancelButton->click();

        bool stoppedClean = false;
        while (run.elapsed() < 5000) {
            QApplication::processEvents();
            if (!editor->isReadOnly() && effectChars() == 0) {
                stoppedClean = true;
                break;
            }
            QThread::msleep(2);
        }
        cancelHeartbeat.stop();

        const int after = effectChars();
        QTextStream(stdout) << "ui-test: progress 取消：取消前 " << before << " 个效果字符，"
                            << "取消后 " << after << " 个，心跳 " << cancelTicks << " 次，只读="
                            << (editor->isReadOnly() ? 1 : 0) << "\n";
        if (!stoppedClean || after != 0 || editor->isReadOnly()) {
            QTextStream(stderr) << "progress 失败：取消应当把改动全部收回（还剩 " << after
                                << " 个效果字符，只读=" << (editor->isReadOnly() ? 1 : 0) << "）\n";
            return 78;
        }
    }

    /*!
     * 用户点名要的那一条：**没选中文字时，直接作用于全文，不再弹窗**。
     *
     * 从前这里会弹一个模态对话框"请先选中一段文字" —— 用户的原话是
     * "还是有这个弹窗"。现在：没有选区 = 全文，状态栏明说一句。
     *
     * 量法：先把选区收起来（光标只是一个插入点），再点【笔画扭曲】，
     * 看是不是**全文**都被扭曲了、而且回调准时返回（没弹窗就不会被嵌套
     * 事件循环卡住）。
     */
    {
        QTextCursor caret = editor->textCursor();
        caret.clearSelection();
        caret.setPosition(editor->document()->characterCount() / 2);
        editor->setTextCursor(caret);
        QApplication::processEvents();

        QTextCursor all = editor->textCursor();
        all.select(QTextCursor::Document);
        editor->setTextCursor(all);
        clearEffects(&all);
        editor->setTextCursor(caret);
        for (int i = 0; i < 10; ++i)
            QApplication::processEvents();

        QElapsedTimer clock;
        clock.start();
        distort->trigger();
        const double ms = clock.nsecsElapsed() / 1e6;

        QElapsedTimer run;
        run.start();
        while (run.elapsed() < 15000) {
            QApplication::processEvents();
            if (effectChars() > 0 && !editor->isReadOnly())
                break;
            QThread::msleep(2);
        }

        const int covered = effectChars();
        const int total = qMax(0, editor->document()->characterCount() - 1);
        QTextStream(stdout) << "ui-test: progress 无选区：回调 " << ms << "ms，任务 " << run.elapsed()
                            << "ms，扭曲了 " << covered << " 个字符（可铺字的格子约 " << total
                            << " 个）\n";
        if (ms > 300.0) {
            QTextStream(stderr) << "progress 失败：没选区时动作里卡了 " << ms
                                << "ms（可能又弹模态窗了）\n";
            return 80;
        }
        // 全文兜底：覆盖的字符数必须远超“当前视口”那一页的量
        if (covered < total / 2) {
            QTextStream(stderr) << "progress 失败：没选区时应当作用于全文，实际只动了 " << covered
                                << " 个字符（全文约 " << total << "）\n";
            return 81;
        }
    }

    QTextStream(stdout) << "ui-test: progress 长活儿自检通过（分片、进度、可取消、收尾恢复、无选区=全文）\n";
    Q_UNUSED(outDir);
    return 0;
}

/*!
 * \brief 分页自检：写满字的文档必须**一页一份内容**，页边距四边都管用。
 *
 * 这一组是照着两个真实缺陷写的：
 *   1. 正文排到第一页正文区以下之后，屏幕上会把同一份内容在第 2、3…页
 *      再来一遍（外加一个不知名的蓝色虚线框），而且"写满了字就一定要崩"；
 *   2. 四边页边距里只有左右和第一页的上边距"看起来有效"，
 *      下边距从来没被任何东西挡住过。
 *
 * 量法一律**看屏幕像素**，不看算出来的数：
 *   - 每张纸上最上面和最下面那点墨的位置，必须落在正文区里（四边页边距都留白）；
 *   - 缩放到 100% / 25% / 400% 来回折腾不许崩（Qt 的断言失败是直接 abort）。
 */
static int runPageLayoutProbe(MainWindow &window, const QString &outDir)
{
    TextEditor *editor = window.findChild<TextEditor *>();
    if (!editor) {
        QTextStream(stderr) << "pagefix 失败：找不到编辑区\n";
        return 60;
    }

    /*!
     * 正文写成**带序号的短段落**：一行一段，行数给足一页装不下
     * （A4 正文区大约 38 行），这样第 2 页一定该有内容。
     *
     * 光标必须留在**文档开头**：光标竖条本身也是"墨"，而 `setPlainText()`
     * 会把光标留在文档末尾 —— 那根竖条会跑到页边距外面去，
     * 量出来的"最后一点墨"就是它，白报一个"越界"。
     */
    const int totalLines = 120;
    {
        QStringList lines;
        lines.reserve(totalLines);
        for (int i = 0; i < totalLines; ++i)
            lines << QStringLiteral("管%1").arg(i + 1);
        editor->setCurrentCharFormat(QTextCharFormat());
        editor->setPlainText(lines.join(QLatin1Char('\n')));
    }
    editor->moveCursor(QTextCursor::Start);
    for (int i = 0; i < 20; ++i) {
        QApplication::processEvents();
        QThread::msleep(3);
    }
    QTextStream(stdout) << "pagefix: 光标位置=" << editor->textCursor().position() << "\n";

    /**
     * 改动编辑区状态之后，必须**强制一次同步重绘**再截图。
     *
     * `QApplication::processEvents()` 只保证事件被派发，绘制有可能是异步的；
     * `QWidget::grab()` 拿的是控件当前后备存储里的内容 —— 于是抓到的还是
     * "上一次重绘"那一帧。自检里踩过这个坑：滚动之后截的图其实还是滚动之前那一帧，
     * 白查了半天"正文没跟着纸走"。
     */
    auto settle = [&] {
        for (int i = 0; i < 10; ++i) {
            QApplication::processEvents();
            QThread::msleep(3);
        }
        editor->viewport()->repaint();
        QApplication::processEvents();
    };
    auto grabViewport = [&] { return editor->viewport()->grab().toImage(); };

    /*!
     * 量"每页各画了什么"最省事的办法：**把比例尺调到 40%**。
     *
     * 0.4 倍时整张 A4 纸只有 449 视图像素高，而编辑区有 792 高 ——
     * 第 1、2 页（甚至第 3 页的一截）**一次性都在画面上**，
     * 一张截图就能把好几页摆在一起比，不用滚动、不用换帧、不怕抓到旧帧。
     * 0.4 也正是用户报告问题时用的比例尺。
     */
    const double measureZoom = 0.4;
    editor->setZoom(measureZoom);
    editor->verticalScrollBar()->setValue(0);
    settle();

    const double dpr = window.devicePixelRatioF();
    const double paperH = editor->paperViewHeightPx(); // 视图像素口径
    const double paperW = editor->paperViewWidthPx();
    const double bodyTop = editor->bodyOriginPx().y() * editor->zoom();
    const double bodyBottom = bodyTop + editor->pageSetup().bodySizePx().height() * editor->zoom();

    /*!
     * 一张纸的量测结果。坐标一律是"**相对这张纸的左上角**"的视图像素，
     * 纸面、正文都在 viewport 里，所以量的是 viewport 自己的那张图。
     */
    struct PageMeasure
    {
        double paperTop = 0.0;  //!< 这张纸的顶在 viewport 里的 y
        double firstInk = -1.0; //!< 最上面那点墨相对纸顶的距离
        double lastInk = -1.0;
        int bands = 0;          //!< 墨带条数 ≈ 这一页装了几行
        bool measurable = false;
        bool fullyVisible = false;
    };

    /*!
     * 扫描窗口取 **纸面 ∩ viewport**，不能只按纸面算 ——
     * 纸有一部分在视口外面时，那一段在截图里其实是**桌面**（深灰），
     * 会被当成墨（踩过：于是"那张纸通篇是墨"）。
     * 横向再往里缩 8 像素避开纸张那圈 1px 的灰边。
     */
    auto measurePage = [&](const QImage &shot, int page, double paperHNow) {
        PageMeasure m;
        const QPointF origin = editor->paperOriginInViewport();
        m.paperTop = origin.y() + page * paperHNow;
        const double paperX = origin.x();
        const double viewW = shot.width() / dpr;
        const double viewH = shot.height() / dpr;

        const double loX = qMax(paperX + 8.0, 0.0);
        const double hiX = qMin(paperX + paperW - 8.0, viewW);
        const double loY = qMax(m.paperTop + 4.0, 0.0);
        const double hiY = qMin(m.paperTop + paperHNow - 4.0, viewH);
        /*!
         * 露出来不足 2 像素的纸不量：那点宽度里连纸张边框都算进去了，
         * 量出来的"墨"其实是边框；而且它也不该被当成"看得见的那一页"。
         */
        if (hiX - loX < 8.0 || hiY - loY < 2.0)
            return m;
        m.measurable = true;
        m.fullyVisible = m.paperTop >= 0.0 && m.paperTop + paperHNow <= viewH;

        const int x0 = int(std::lround(loX * dpr));
        const int x1 = qMin(shot.width() - 1, int(std::lround(hiX * dpr)));
        const int y0 = int(std::lround(loY * dpr));
        const int y1 = qMin(shot.height() - 1, int(std::lround(hiY * dpr)));
        bool inBand = false;
        for (int y = y0; y <= y1; ++y) {
            bool ink = false;
            for (int x = x0; x <= x1 && !ink; ++x) {
                const QRgb c = shot.pixel(x, y);
                if (qRed(c) < 200 || qGreen(c) < 200 || qBlue(c) < 200)
                    ink = true;
            }
            if (ink) {
                const double rel = y / dpr - m.paperTop;
                if (m.firstInk < 0.0)
                    m.firstInk = rel;
                m.lastInk = rel;
                if (!inBand) {
                    ++m.bands;
                    inBand = true;
                }
            } else {
                inBand = false;
            }
        }
        return m;
    };

    /*!
     * 四边页边距一律按**正文区**判：每张纸上那点墨都必须落在
     * [bodyTop, bodyBottom] 里。容差 2 像素（墨不一定顶到格子边）。
     *
     * 正文区范围**当场按当前比例尺算**：40% 时正文区只有 389 高、100% 时有 971 高，
     * 拿一个缩放比例算出来的数去判另一个比例尺的量测，会报出假的"越界"。
     *
     * 以前只有第 1 页的上边距和左右边距成立：正文是从纸顶开始连续画的，
     * 每页的切片还整体少挪了一个下边距 —— 于是第 2 页起"顶到纸边、越进下边距"。
     */
    auto checkMargins = [&](int page, const PageMeasure &m, const char *where) {
        if (!m.measurable || m.firstInk < 0.0)
            return 0;
        const double top = editor->bodyOriginPx().y() * editor->zoom();
        const double bottom = top + editor->pageSetup().bodySizePx().height() * editor->zoom();
        /*!
         * 容差放宽到 6 像素：只露出来一小截的纸，扫描窗口的上边界
         * 本身就是"视口顶"，墨和它相减会有几个像素的出入。
         */
        if (m.firstInk < top - 6.0) {
            QTextStream(stderr) << "pagefix 失败（" << where << "）：第 " << page + 1
                                << " 页的墨跑进了上边距（" << m.firstInk << " < " << top << "）\n";
            return 61;
        }
        if (m.lastInk > bottom + 6.0) {
            QTextStream(stderr) << "pagefix 失败（" << where << "）：第 " << page + 1
                                << " 页的墨越过了下边距（" << m.lastInk << " > " << bottom << "）\n";
            return 62;
        }
        return 0;
    };

    /*!
     * 一、40% 下第 1、2 页必须**各有各的内容**。
     *
     * 判据用"墨带的条数 + 首末墨的位置"：一页装满的行数是个定值，
     * 第 2 页也装满 → 两页的行数应当差不多、而**墨的位置必须错开**；
     * 老代码里第 2 页画的是"文档开头"，两页的墨会**完全重合**。
     */
    {
        const QImage shot = grabViewport();
        shot.save(outDir + QStringLiteral("/uitest_pagefix_zoom40.png"));

        int measured = 0;
        int bands[3] = {0, 0, 0};
        double firstInk[3] = {-1.0, -1.0, -1.0};
        double lastInk[3] = {-1.0, -1.0, -1.0};
        for (int page = 0; page < 3; ++page) {
            const PageMeasure m = measurePage(shot, page, paperH);
            QTextStream(stdout) << "pagefix: 40% 第 " << page + 1 << " 页 纸顶=" << m.paperTop
                                << (m.fullyVisible ? "（整页可见）" : "（只看到一部分）")
                                << " 正文区=" << bodyTop << ".." << bodyBottom << " 墨=" << m.firstInk
                                << ".." << m.lastInk << " 行数=" << m.bands << "\n";
            if (!m.measurable || m.firstInk < 0.0)
                continue;
            ++measured;
            bands[page] = m.bands;
            firstInk[page] = m.firstInk;
            lastInk[page] = m.lastInk;
            const int rc = checkMargins(page, m, "40%");
            if (rc != 0)
                return rc;
        }

        if (bands[0] < 20) {
            QTextStream(stderr) << "pagefix 失败：40% 下第 1 页只量到 " << bands[0]
                                << " 行（120 行的文档应当装满一页）\n";
            return 63;
        }
        if (bands[1] <= 0) {
            QTextStream(stderr) << "pagefix 失败：40% 下第 2 页上一个字都没有"
                                   "（120 行的文档应当有第 2 页内容）\n";
            return 64;
        }
        if (qAbs(firstInk[0] - firstInk[1]) < 1.0 && qAbs(lastInk[0] - lastInk[1]) < 1.0
            && bands[0] == bands[1]) {
            QTextStream(stderr) << "pagefix 失败：第 2 页和第 1 页的墨**完全重合**（都是 "
                                << firstInk[0] << ".." << lastInk[0] << "，" << bands[0]
                                << " 行）—— 第 2 页又在画文档开头\n";
            return 65;
        }
        QTextStream(stdout) << "pagefix: 40% 下量到 " << measured << " 页，第 1 页 " << bands[0]
                            << " 行、第 2 页 " << bands[1] << " 行\n";
    }

    /*!
     * 二、像真的打字那样：一边打一边看"纸与纸之间那道缝"
     *     —— 第 2 页上冒出第 1 页的字，必然先表现为**缝里有墨**。
     *
     * 量法：把视口滚到"第 1 页的下边距 + 纸缝"都看得见的位置，
     * 然后在**缝那几行**上找墨。纸缝里除了桌面什么都没有，
     * 一旦出现非桌面色的像素，就说明有东西被画到纸外面去了
     * （老代码里第 2 页画的是文档开头，正是从缝里开始冒出来的）。
     */
    {
        editor->setZoom(1.0);
        editor->verticalScrollBar()->setValue(0);
        editor->setCurrentCharFormat(QTextCharFormat());
        QStringList lines;
        for (int i = 0; i < 30; ++i)
            lines << QStringLiteral("第一页第%1行").arg(i + 1);
        editor->setPlainText(lines.join(QLatin1Char('\n')));
        editor->moveCursor(QTextCursor::Start);
        settle();

        const double nowPaperH = editor->paperViewHeightPx();
        // 让"第 1 页的底边"落在视口中间：滚动值 = 纸高 - 视口高/2
        const int want = int(std::lround(nowPaperH - editor->viewport()->height() / 2.0));
        editor->verticalScrollBar()->setValue(qBound(0, want, editor->verticalScrollBar()->maximum()));
        settle();

        const QImage shot = grabViewport();
        const double dprNow = window.devicePixelRatioF();
        const QPoint paperAt = editor->paperOriginInViewport().toPoint();
        const double paperTopInView = paperAt.y(); // 第 1 页纸顶在视口里的 y
        const QColor desk(0x3a, 0x3d, 0x42);

        int gapInk = 0;
        int widest = 0;
        {
            // 纸缝：第 1 页纸底 .. 第 2 页纸顶（含第 2 页纸顶以上的桌面）
            const int gapTop = int(std::lround((paperTopInView + nowPaperH) * dprNow)) + 6;
            const int gapBottom =
                int(std::lround((paperTopInView + 2 * nowPaperH) * dprNow)) - 2;
            const int x0 = qMax(0, int(std::lround((paperAt.x() + 10) * dprNow)));
            const int x1 = qMin(shot.width() - 1,
                                int(std::lround((paperAt.x() + editor->paperViewWidthPx() - 10)
                                                * dprNow)));
            for (int y = qMax(0, gapTop); y <= qMin(shot.height() - 1, gapBottom); ++y) {
                int row = 0;
                for (int x = x0; x <= x1; ++x) {
                    const QRgb c = shot.pixel(x, y);
                    // 桌面色（含阴影）之外的都算"不该出现在缝里的东西"
                    if (qAbs(qRed(c) - desk.red()) > 12 || qAbs(qGreen(c) - desk.green()) > 12
                        || qAbs(qBlue(c) - desk.blue()) > 12)
                        ++row;
                }
                if (row > 0) {
                    gapInk += row;
                    widest = qMax(widest, row);
                }
            }
            QTextStream(stdout) << "pagefix: 纸缝检查 滚动=" << editor->verticalScrollBar()->value()
                                << " 纸顶=" << paperTopInView << " 缝(设备像素)=" << gapTop << ".."
                                << gapBottom << " 缝里的非桌面像素=" << gapInk
                                << "（最宽一行 " << widest << "）\n";
            shot.save(outDir + QStringLiteral("/uitest_pagefix_gap.png"));
        }
        if (gapInk > 200) {
            QTextStream(stderr) << "pagefix 失败：两张纸之间的缝里有 " << gapInk
                                << " 个像素不是桌面色 —— 有内容被画到纸外面去了"
                                   "（第 2 页上又冒出第 1 页的字，就是这个症状）\n";
            return 66;
        }
    }

    /*!
     * 二、100%：一页纸比视口高，只能看一部分，判据是"墨别越出正文区"。
     */
    editor->setZoom(1.0);
    editor->verticalScrollBar()->setValue(0);
    settle();
    {
        const PageMeasure m = measurePage(grabViewport(), 0, editor->paperViewHeightPx());
        QTextStream(stdout) << "pagefix: 100% 第 1 页 墨=" << m.firstInk << ".." << m.lastInk
                            << " 行数=" << m.bands << "\n";
        const int rc = checkMargins(0, m, "100%");
        if (rc != 0)
            return rc;
    }

    /**
     * 二、一直往下打字：文档从"一页装不满"长到"好几页"，边打边量，
     * 不许崩、不许把内容画到页边距外面去。
     *
     * 这条对应的是"写满了字就一定要崩"那个报告：只要文档高度
     * 还有一点没被页高钉住、或者每页的切片窗口和纸对不齐，
     * 打字打到跨页就会踩到。
     */
    {
        editor->setZoom(1.0);
        editor->verticalScrollBar()->setValue(0);
        settle();
        editor->setCurrentCharFormat(QTextCharFormat());
        editor->setPlainText(QString());
        editor->moveCursor(QTextCursor::Start);
        settle();

        const int batches = 8;
        for (int b = 0; b < batches; ++b) {
            for (int k = 0; k < 40; ++k) {
                QTextCursor c = editor->textCursor();
                c.movePosition(QTextCursor::End);
                c.insertText(QStringLiteral("打字管%1 ").arg(b * 40 + k + 1));
                c.insertBlock();
                editor->setTextCursor(c);
            }
            /*!
             * 量**最后一页**，不量第 1 页：打字打到第 n 页时，视口在文档末尾，
             * 第 1 页的正文区早就滚到屏幕外面去了（那里量出来当然是空的）。
             * 量最后一页才是"打字的人此刻看到的那张纸"。
             */
            editor->ensureCaretVisible();
            settle();
            const QImage shot = grabViewport();
            /*!
             * 纸高、纸宽都要**当场取**：这个自检中途换过比例尺，
             * 循环外面抓的那个 `paperH` 还是 40% 时的值（449），
             * 拿它去算"第 p 页在哪"会整段偏掉 —— 于是"看得见的那一页"
             * 永远算成第 1 页，量出来一片空白（踩过）。
             */
            const double nowPaperH = editor->paperViewHeightPx();
            const int pages = qMax(1, int(std::ceil(editor->document()->size().height()
                                                    / nowPaperH)));
            /*!
             * 挑"当前屏幕上**最完整**的那张纸"来量。
             *
             * 不能挑"最后一个能量的"：滚到底时视口里往往同时有上一页的一小截
             * 和这一页的大半张 —— 按顺序取最后那个会取到只有几个像素露出来的
             * 那一页，量出来一片空白（踩过两次）。按"可见高度"挑最稳。
             */
            int top = 0;
            double bestVisible = -1.0;
            for (int p = 0; p < pages; ++p) {
                const PageMeasure cand = measurePage(shot, p, nowPaperH);
                const double visible = qMin(nowPaperH,
                                            qMin(nowPaperH + cand.paperTop,
                                                 double(editor->viewport()->height())
                                                     - cand.paperTop));
                if (visible > bestVisible) {
                    bestVisible = visible;
                    top = p;
                }
            }
            const PageMeasure m = measurePage(shot, top, nowPaperH);
            QTextStream(stdout) << "pagefix: 键入 " << (b + 1) * 40 << " 段 文档高="
                                << editor->document()->size().height() << " 页数=" << pages
                                << " 滚动=" << editor->verticalScrollBar()->value() << "/"
                                << editor->verticalScrollBar()->maximum() << " 看得见的第 "
                                << top + 1 << " 页(可见 " << bestVisible << "px) 墨=" << m.firstInk
                                << ".." << m.lastInk << " 行数=" << m.bands << "\n";
            const int rc = checkMargins(top, m, "打字");
            if (rc != 0)
                return rc;
        }
    }

    /*!
     * 三、缩放：满页文档来回缩放不许崩。
     * Qt 的断言失败是**直接 abort**，所以"能走到下一行"本身就是判据。
     */
    const double zooms[] = {1.0, 0.25, 4.0, 1.5, 0.5, 1.0};
    for (double z : zooms) {
        editor->setZoom(z);
        for (int i = 0; i < 10; ++i) {
            QApplication::processEvents();
            QThread::msleep(2);
        }
        const QImage shot = window.grab().toImage();
        shot.save(QStringLiteral("%1/uitest_pagefix_zoom%2.png").arg(outDir).arg(int(z * 100)));
        QTextStream(stdout) << "pagefix: 缩放 " << int(z * 100)
                            << "% 绘制完成，纸宽=" << editor->paperViewWidthPx() << "\n";
    }

    editor->setZoom(1.0);
    editor->verticalScrollBar()->setValue(0);
    for (int i = 0; i < 10; ++i)
        QApplication::processEvents();

    QTextStream(stdout) << "pagefix: 分页自检通过（每页一份内容、四边页边距都管用、缩放不崩）\n";
    return 0;
}
















/*!
 * \brief 折腾自检（soak）：像真人那样一直打字 + 拉窗口 + 缩放，边折腾边记日志。
 *
 * 这一组不是"断言某个数对不对"，而是**把用户报的"打字/缩放时随机崩"
 * 搬到自检里来**：每种操作都做成一个函数，一轮一轮随机组合着跑；
 * 每步之前写一条面包屑、每步之后写一条日志。只要它崩了，
 * `--log=debug` 的日志 + 崩溃 dump 里就能看到崩在哪种组合上。
 *
 * 跑法：`tripa.exe --uitest <目录> light soak --log=debug`
 *
 * \a rounds 默认 200 轮；每轮都做"打字 1~8 个字符"，再按轮次轮换做
 * 改窗口大小 / 改缩放 / 翻到文档头尾 / 改页面设置。
 */
static int runSoakProbe(MainWindow &window, const QString &outDir, int rounds)
{
    TextEditor *editor = window.findChild<TextEditor *>();
    if (!editor) {
        QTextStream(stderr) << "soak 失败：找不到编辑区\n";
        return 70;
    }

    QTextStream(stdout) << "soak: 开始折腾 " << rounds << " 轮（日志级别见 --log）\n";

    // 起手先放一段有中文有英文的文字，别用空白文档——那种文档什么都测不出来
    editor->setCurrentCharFormat(QTextCharFormat());
    editor->setPlainText(QStringLiteral("折腾自检起点 abc 123 排版\n"));
    editor->moveCursor(QTextCursor::End);
    editor->setFocus(Qt::OtherFocusReason);
    editor->setZoom(1.0);

    auto pump = [](int times = 3) {
        for (int i = 0; i < times; ++i) {
            QApplication::processEvents();
            QThread::msleep(1);
        }
    };

    const int widths[] = {1360, 900, 1600, 700, 1200};
    const double zooms[] = {1.0, 0.5, 1.5, 0.25, 2.0, 0.75, 4.0, 1.0};
    int typed = 0;
    int maxPages = 1;

    for (int round = 0; round < rounds; ++round) {
        tripalog::breadcrumb(QStringLiteral("soak 第 %1 轮：开始").arg(round + 1));

        // --- 打字：1~8 个字符，偶尔回车换段
        {
            const int count = 1 + (round * 7) % 8;
            tripalog::breadcrumb(QStringLiteral("soak 第 %1 轮：打字 %2 个").arg(round + 1).arg(count));
            QTextCursor c = editor->textCursor();
            c.movePosition(QTextCursor::End);
            for (int k = 0; k < count; ++k) {
                if (k == 3 && round % 3 == 0)
                    c.insertBlock();
                c.insertText(QStringLiteral("管%1").arg(typed + k + 1));
            }
            editor->setTextCursor(c);
            typed += count;
            pump();
        }

        // --- 每轮都动的东西：光标滚进视口（打字的人眼睛盯着光标）
        editor->ensureCaretVisible();
        pump(2);

        // --- 轮换：改窗口大小（最容易踩到重排 + 滚动范围重算）
        if (round % 4 == 1) {
            const int w = widths[(round / 4) % 5];
            const int h = 620 + (round % 5) * 70;
            tripalog::breadcrumb(QStringLiteral("soak 第 %1 轮：改窗口 %2x%3")
                                     .arg(round + 1).arg(w).arg(h));
            window.resize(w, h);
            pump(4);
        }

        // --- 轮换：改缩放（每页几何都要重算）
        if (round % 5 == 2) {
            const double z = zooms[(round / 5) % 8];
            tripalog::breadcrumb(QStringLiteral("soak 第 %1 轮：缩放 %2%")
                                     .arg(round + 1).arg(int(z * 100)));
            editor->setZoom(z);
            pump(4);
        }

        // --- 轮换：在文档里乱跳（光标、滚动条、选区一起动）
        if (round % 7 == 3) {
            tripalog::breadcrumb(QStringLiteral("soak 第 %1 轮：跳光标/滚动").arg(round + 1));
            const int max = qMax(1, editor->document()->characterCount() - 1);
            const int pos = (round * 37) % max;
            QTextCursor c(editor->document());
            c.setPosition(pos);
            if (round % 2 == 0) {
                const int end = qMin(max, pos + 20 + round % 50);
                c.setPosition(end, QTextCursor::KeepAnchor);
            }
            editor->setTextCursor(c);
            editor->ensureCaretVisible();
            const int range = editor->verticalScrollBar()->maximum();
            editor->verticalScrollBar()->setValue(range > 0 ? (round * 13) % range : 0);
            pump(3);
        }

        /*!
         * --- 轮换：连续缩放（一路放大再一路缩小）
         *
         * 这一条对着"一缩放就崩"那个报告：缩放会同时改绘制变换、
         * 纸张居中量、滚动范围、光标矩形，还要重排一遍 ——
         * 是一堆状态里最容易互相踩的一项。
         */
        if (round % 6 == 4) {
            tripalog::breadcrumb(QStringLiteral("soak 第 %1 轮：连续缩放").arg(round + 1));
            for (int step = 0; step < 6; ++step) {
                const double z = 0.25 + 0.125 * step; // 25%..87.5%
                editor->setZoom(z);
                pump(1);
            }
            for (int step = 5; step >= 0; --step) {
                const double z = 0.25 + 0.125 * step;
                editor->setZoom(z);
                pump(1);
            }
        }

        // --- 轮换：改页面设置（换纸、换边距 —— 分页模型整个重算）
        if (round % 11 == 5) {
            tripalog::breadcrumb(QStringLiteral("soak 第 %1 轮：改页面设置").arg(round + 1));
            PageSetup setup = editor->pageSetup();
            if (round % 22 == 5) {
                setup.presetName = QStringLiteral("A5");
                setup.widthMm = 148.0;
                setup.heightMm = 210.0;
            } else {
                setup.presetName = QStringLiteral("A4");
                setup.widthMm = 210.0;
                setup.heightMm = 297.0;
            }
            setup.landscape = (round % 44 == 27);
            setup.marginTopMm = 10.0 + (round % 4) * 7.0;
            setup.marginBottomMm = 12.0 + (round % 3) * 9.0;
            editor->setPageSetup(setup);
            pump(4);
        }

        // --- 每轮顺手画一次（把绘制路径也拖进来），偶尔存一张图留证
        editor->viewport()->repaint();
        pump(1);

        const double pageH = editor->paperViewHeightPx();
        const int pages =
            qMax(1, int(std::ceil(editor->document()->size().height() / qMax(1.0, pageH))));
        maxPages = qMax(maxPages, pages);
        if (round % 20 == 19) {
            TRIPA_INFO("soak",
                       QStringLiteral("进度：第 %1/%2 轮 字符=%3 页数=%4 缩放=%5% 窗口=%6x%7")
                           .arg(round + 1)
                           .arg(rounds)
                           .arg(editor->document()->characterCount() - 1)
                           .arg(pages)
                           .arg(int(std::lround(editor->zoom() * 100.0)))
                           .arg(window.width())
                           .arg(window.height()));
            if (round % 40 == 19)
                window.grab().save(QStringLiteral("%1/uitest_soak_%2.png")
                                       .arg(outDir)
                                       .arg(round + 1));
        }
    }

    tripalog::breadcrumb(QStringLiteral("soak 结束"));
    QTextStream(stdout) << "soak: " << rounds << " 轮跑完，没崩；共键入 " << typed << " 个字符，"
                        << "最多 " << maxPages << " 页\n";
    TRIPA_INFO("soak", QStringLiteral("跑完 %1 轮没崩（字符 %2，最多 %3 页）")
                           .arg(rounds)
                           .arg(typed)
                           .arg(maxPages));
    return 0;
}

/*!
 * \brief 崩溃报告自检：**故意崩一次**，然后看有没有 dump 留下来。
 *
 * 崩溃处理这条路"平时不走"，所以必须能主动测 —— 否则等用户真的崩了，
 * 才发现 dump 里什么都没有，那就白做了。用 `RaiseException` 造一个
 * 访问冲突（不是真空指针解引用：那样编译器可能优化掉，也没法在自检里
 * 稳定复现），异常码固定成 `0xE0000001` 好认。
 *
 * 跑法：`tripa.exe --crashtest`（子进程用），
 * 或 `--uitest <目录> light crashtest`（父进程拉起子进程再检查 dump）。
 */
static int runCrashChild()
{
#if defined(Q_OS_WIN)
    tripalog::breadcrumb(QStringLiteral("崩溃自检：故意制造一次访问冲突"));
    TRIPA_WARN("crashtest", "日志系统自检 —— 下面这行之后进程会故意崩溃，这是预期的");
    TRIPA_INFO("crashtest", QStringLiteral("崩溃前日志（应该能在 dump 里看到这一行）"));
    fprintf(stderr, "crashtest: 即将故意崩溃，dump 应当写到 %s\n",
            qPrintable(tripalog::dumpDirectory()));
    fflush(stderr);
    ::RaiseException(0xE0000001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    fprintf(stderr, "crashtest: 居然没崩（RaiseException 被吞了？）\n");
    return 90;
#else
    fprintf(stderr, "crashtest: 这个平台没做崩溃处理\n");
    return 90;
#endif
}

/*!
 * \brief 在自检里验证崩溃 dump：拉起一个子进程让它崩，再检查 dump 文件。
 */
static int runCrashDumpProbe(const QString &outDir)
{
#if defined(Q_OS_WIN)
    const QString exe = QCoreApplication::applicationFilePath();

    /*!
     * dump 固定写到**自检输出目录**旁边：默认位置是
     * `%LOCALAPPDATA%/tripa`，而受限沙箱里那个目录可能根本写不进去 ——
     * 那样测的就成了"沙箱让不让写"，而不是"崩溃处理器对不对"。
     * 用**命令行参数**（不是环境变量：环境变量在子进程里未必拿得到，
     * 踩过一次）把子进程的报告目录指到当前工作目录下的临时目录；
     * 现场（用户机器）用的仍然是默认位置。
     */
    const QString logDir = QDir::currentPath() + QStringLiteral("/crashtest_logs");
    QDir().mkpath(logDir);

    /*!
     * 用 `_spawnl` 而不是 `QProcess`：`QProcess` 抓子进程输出要开**命名管道**，
     * 而受限沙箱里那是被拒的（实测报 "pipe: 系统找不到指定的文件"）——
     * 自检在自己的开发沙箱里跑不起来，就没法验证这条路。子进程不需要
     * 回传输出（它要写的东西全在 dump 里），所以直接 spawn 最省事。
     */
    const QByteArray exeUtf8 = QDir::toNativeSeparators(exe).toLocal8Bit();
    const QByteArray dirUtf8 = QDir::toNativeSeparators(logDir).toLocal8Bit();
    errno = 0;
    const intptr_t pid = _spawnl(_P_NOWAIT, exeUtf8.constData(), exeUtf8.constData(),
                                 "--crashtest", "--crashdir", dirUtf8.constData(), nullptr);
    if (pid == -1) {
        QTextStream(stderr) << "crashtest 失败：起不了子进程（errno=" << errno << "）\n";
        return 91;
    }
    QTextStream(stdout) << "crashtest: 子进程 pid=" << qint64(pid) << "，dump 目录="
                        << QDir::toNativeSeparators(logDir) << "，等它崩完…\n";

    QDir dir(logDir);
    QString path;
    for (int i = 0; i < 100 && path.isEmpty(); ++i) {
        QThread::msleep(100);
        const QStringList found =
            dir.entryList({QStringLiteral("tripa-crash-*.txt")}, QDir::Files, QDir::Time);
        if (!found.isEmpty())
            path = dir.absoluteFilePath(found.first());
    }
    if (path.isEmpty()) {
        QTextStream(stderr) << "crashtest 失败：" << QDir::toNativeSeparators(logDir)
                            << " 里没有 tripa-crash-*.txt（子进程崩了但没写出报告？）\n";
        return 92;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream(stderr) << "crashtest 失败：打不开 dump " << path << "\n";
        return 93;
    }
    const QString text = QString::fromUtf8(file.readAll());
    file.close();
    QTextStream(stdout) << "crashtest: dump=" << path << "（" << text.size() << " 字）\n";

    // 把 dump 复制一份到自检输出目录，方便直接看
    QFile::remove(outDir + QStringLiteral("/uitest_crash_dump.txt"));
    QFile::copy(path, outDir + QStringLiteral("/uitest_crash_dump.txt"));

    struct Check
    {
        const char *what;
        bool ok;
    };
    const QVector<Check> checks = {
        {"里面有异常码", text.contains(QStringLiteral("异常码"))},
        {"异常码是自检造的那个 0xE0000001",
         text.contains(QStringLiteral("E0000001"), Qt::CaseInsensitive)},
        {"里面有面包屑", text.contains(QStringLiteral("面包屑"))},
        {"面包屑写着是崩溃自检", text.contains(QStringLiteral("崩溃自检"))},
        {"里面有最近的日志", text.contains(QStringLiteral("最近的日志"))},
        {"日志里能看到崩溃前那一条",
         text.contains(QStringLiteral("下面这行之后进程会故意崩溃"))},
        {"里面有寄存器",
         text.contains(QStringLiteral("RIP=")) || text.contains(QStringLiteral("EIP="))},
        {"里面写出错模块", text.contains(QStringLiteral("出错模块"))},
    };
    int failed = 0;
    for (const Check &c : checks) {
        QTextStream(stdout) << "crashtest: " << (c.ok ? "ok   " : "FAIL ") << c.what << "\n";
        if (!c.ok)
            ++failed;
    }
    if (failed > 0) {
        QTextStream(stderr) << "crashtest 失败：" << failed << " 项没通过，dump 内容：\n"
                            << text.left(2000) << "\n";
        return 94;
    }
    QTextStream(stdout) << "crashtest: 崩溃报告自检通过（子进程崩了、报告写全了）\n";
    return 0;
#else
    Q_UNUSED(outDir);
    return 0;
#endif
}

/*!
 * 界面冒烟测试：真正建出主窗口、跑几个回合的事件循环、截图后退出。
 * 用来验证排版控件、效果层、工具栏这些没法用纯逻辑测的部分。
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
 *   - 精确拖选：锚点和终点必须等于"按下点/松手点的命中结果"（只断行号不够，
 *     列才是关键）；
 *   - 连续拖选：按下后分多步移动，左拖右拖都测，锚点不许被上一帧的终点顶掉，
 *     两头都必须能停在行中间；
 *   - Shift+方向键扩选之后，画面上必须真有高亮像素；
 *   - 扭曲替换：正文行内的原字黑墨必须消失、出现扭曲色墨，且全选后
 *     选区色像素不减少（效果层不许再刷底色盖原字）。
 *
 * 另有两个独立的参数：
 *   - zoom：视图缩放自检（见 runZoomProbe）；
 *   - hwnoise：给手写字加噪声的自检（见 runHandwritingNoiseProbe）；
 *   - pagefix：分页 / 页边距自检（见 runPageLayoutProbe）。
 */
static int runUiTest(const QStringList &args)
{
    const QString outDir = args.size() > 1 ? args.at(1) : QStringLiteral(".");
    QString theme = QStringLiteral("system");
    bool probeMouse = false;
    bool probeCursor = false;
    bool probeBaseline = false;
    bool probeZoom = false;
    bool probeHwNoise = false;
    bool probePageFix = false;
    bool probeProgress = false;
    bool probeSoak = false;
    bool probeCrash = false;
    QString baselineDir;
    for (int i = 2; i < args.size(); ++i) {
        const QString a = args.at(i).toLower();
        if (a == QStringLiteral("dark") || a == QStringLiteral("light"))
            theme = a;
        else if (a == QStringLiteral("probe"))
            probeMouse = probeCursor = true;
        else if (a == QStringLiteral("cursor"))
            probeCursor = true;
        else if (a == QStringLiteral("zoom"))
            probeZoom = true;
        else if (a == QStringLiteral("hwnoise"))
            probeHwNoise = true;
        else if (a == QStringLiteral("pagefix"))
            probePageFix = true;
        else if (a == QStringLiteral("progress"))
            probeProgress = true;
        else if (a == QStringLiteral("soak"))
            probeSoak = true;
        else if (a == QStringLiteral("crashtest"))
            probeCrash = true;
        else if (a == QStringLiteral("baseline"))
            probeBaseline = true;
        else if (a == QStringLiteral("baseline-dir") && i + 1 < args.size())
            baselineDir = args.at(++i); // 用原始大小写的参数：这是路径
    }

    /*!
     * 基线调整对话框的实测放在建主窗口之前跑：它自带一个临时 CSV 和一份库，
     * 和窗口里的状态互不干扰，也省得"对话框是模态的、被主窗口挡住"。
     */
    if (probeBaseline) {
        const int rc = runBaselineDialogProbe(outDir, baselineDir);
        if (rc != 0)
            return rc;
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
     * 手写层的默认状态必须是"遮住正文"。
     *
     * 手写数据是用来顶替机打字体的：默认半透明叠加的话，笔迹底下永远透出
     * 一层机打字，看着就是两套字重影。这里直接查工具栏上那个勾选框 ——
     * 渲染参数就是从它来的（见 MainWindow::buildRenderOptions）。
     */
    {
        QCheckBox *replaceBox = nullptr;
        for (QCheckBox *box : window.findChildren<QCheckBox *>()) {
            if (box->text().contains(QStringLiteral("手写遮住正文"))) {
                replaceBox = box;
                break;
            }
        }
        const bool checked = replaceBox && replaceBox->isChecked();
        QTextStream(stdout) << "ui-test: 工具栏「手写遮住正文」默认勾选=" << (checked ? "是" : "否")
                            << "\n";
        if (!checked) {
            QTextStream(stderr) << "ui-test 失败：手写层默认应当遮住正文（机打字不该透出来）\n";
            return 34;
        }
    }

    /*!
     * 崩溃报告自检放在最前面：它会拉起一个子进程故意崩掉，
     * 和主进程里的状态互不干扰，也不受后面那些体检的影响。
     */
    if (probeCrash) {
        const int rc = runCrashDumpProbe(outDir);
        if (rc != 0)
            return rc;
    }

    /*!
     * 分页体检放在光标/选区体检**之前**：它要自己写满一篇文档，
     * 而后面那些体检各自会重设正文（谁先设谁说了算）。
     */
    if (probePageFix) {
        const int rc = runPageLayoutProbe(window, outDir);
        if (rc != 0)
            return rc;
    }

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
            /*!
             * 这里只把**数值**（行顶、行高）存进 bands，不留 QTextLine。
             *
             * QTextLine 是个"指向布局内部"的轻量句柄：布局一旦被 invalidate
             * （重排、setFormats、改页宽……），之前拿到的句柄就变成悬垂引用，
             * 再调 line.y() 会直接段错误 —— 实测就在 QTextLine::y() 里崩过一次，
             * 而且崩的位置离真正的原因很远。要用的那一刻现取，取完只留数。
             */
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
             * **只认一套坐标：viewport 局部坐标**，也就是 documentToViewport()
             * 的值域、QMouseEvent::pos() 那一套。靶点用 documentToViewport()
             * 从文档坐标算出来，直接喂进来，中间不做任何加减。
             *
             * 这里踩过一个大坑：早期版本让本函数收"排版坐标"（= 局部 + viewport
             * 在编辑区里的偏移）再自己减一次，结果同一个自检文件里两套约定并存
             * —— 靶点是局部坐标、事件却按排版坐标处理，点击整段偏一个
             * "纸张居中量 + 工具栏高度"。偏一点点还不容易被发现：
             * 断言只看"点中哪一行"，而光标贴到下一行时行号照样对，
             * 只有列会悄悄变成行尾。现在把这一段偏移彻底删掉，
             * 让"坐标空间不匹配"这类错误无处藏身。
             *
             * 直接调用处理函数而不是往事件队列里塞：拖选需要 Qt 内部的
             * 按键状态跟着变，QTest::mouseMove 又要求窗口真的激活
             * （自检环境里没有），手搓事件 sendEvent 会被当成悬停丢掉。
             * "事件怎么送进控件"是框架的事，"收到之后算得对不对"才是断言对象。
             */
            auto sendMouse = [editor](QEvent::Type type, const QPoint &viewportPos,
                                      Qt::MouseButton button, Qt::MouseButtons buttons) {
                QMouseEvent ev(type, QPointF(viewportPos),
                               editor->viewport()->mapToGlobal(viewportPos), button, buttons,
                               Qt::NoModifier);
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

            //! 数一数画面上有多少像素是钉死的选区色 #3399ff（±30）
            auto countSelectionPixels = [](const QImage &img) {
                int n = 0;
                for (int y = 0; y < img.height(); ++y) {
                    const QRgb *scan = reinterpret_cast<const QRgb *>(img.constScanLine(y));
                    for (int x = 0; x < img.width(); ++x) {
                        const QRgb px = scan[x];
                        if (qAbs(qRed(px) - 0x33) <= 30 && qAbs(qGreen(px) - 0x99) <= 30
                            && qAbs(qBlue(px) - 0xff) <= 30)
                            ++n;
                    }
                }
                return n;
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
                 * 标定：靶点是从文档坐标经 documentToViewport() 算出来的，
                 * 也就是 **viewport 局部坐标**（event->pos() 那一套），
                 * 直接喂回 documentCursorAt() 必须回到同一个字符。
                 *
                 * 这条断言把两个坐标空间钉死：文档坐标 <-> viewport 局部坐标。
                 * （曾经这里多减了一次 viewport 原点，把"viewport 局部"当成
                 * "排版坐标"再换回去，于是整段偏掉，点哪儿都落在纸外面。）
                 */
                const Target &t0 = targets.first();
                const int back = editor->documentCursorAt(t0.vp).position() - t0.docPos;
                QTextStream(stdout) << "ui-test: 点击标定 viewport 局部(" << t0.vp.x() << ","
                                    << t0.vp.y() << ") -> 回到字符偏差 " << back << "\n";
                if (qAbs(back) > 2) {
                    QTextStream(stderr)
                        << "ui-test 失败：坐标换算对不上（偏差 " << back
                        << " 个字符）—— 点击定位会整体错位\n";
                    return 26;
                }
            }

            /*!
             * 冒烟标定：拿第 0 行第 0 个字当控制点，走一遍"文档坐标 ->
             * documentToViewport() -> documentCursorAt()"，必须回到同一个字符。
             */
            {
                const QTextBlock b0 = doc->firstBlock();
                const QTextLine l0 = b0.layout()->lineForTextPosition(0);
                const QRectF br0 = doc->documentLayout()->blockBoundingRect(b0);
                const QPointF docPt(br0.left() + l0.x() + 1.0, br0.top() + l0.y() + 2.0);
                const QPoint vpPt = editor->documentToViewport().map(docPt).toPoint();
                const int got = editor->documentCursorAt(vpPt).position() - b0.position();
                QTextStream(stdout) << "ui-test: 命中测试标定 文档点(" << docPt.x() << ","
                                    << docPt.y() << ") -> viewport 局部(" << vpPt.x() << ","
                                    << vpPt.y() << ") -> 给第 " << got << " 个字符（应为 0）\n";
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
                /*!
                 * 靶点直接取**命中测试的结果**当期望值，不写死"应该是第 5 格"。
                 *
                 * charVp(5) 落在第 5 格的**左边界**上，像素取整之后正好压线，
                 * 由 FuzzyHit 落在哪一边是 Qt 的自由。写死期望值就是在赌取整方向，
                 * 断言会随 dpr / 字体而飘。这里改成"松手点命中谁就选到谁"，
                 * 断言的是**拖选忠实执行了命中测试**，那才是这个用例要管的。
                 */
                const QPoint from = charVp(0);
                const QPoint to = charVp(5);
                const int anchorWanted = editor->documentCursorAt(from).position();
                const int endWanted = editor->documentCursorAt(to).position();
                sendMouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
                QApplication::processEvents();
                sendMouse(QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton);
                QApplication::processEvents();
                sendMouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
                sendMouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
                for (int i = 0; i < 5; ++i)
                    QApplication::processEvents();

                const QTextCursor c = editor->textCursor();
                const bool ok = c.hasSelection() && c.anchor() == anchorWanted
                                && c.position() == endWanted
                                && (c.selectionEnd() - c.selectionStart()) >= 4;
                QTextStream(stdout) << "ui-test: 精确拖选 " << from.x() << "," << from.y() << " -> "
                                    << to.x() << "," << to.y() << " ：选中 ["
                                    << c.selectedText() << "] " << c.anchor() << ".."
                                    << c.position() << "，应为 " << anchorWanted << ".." << endWanted
                                    << (ok ? " OK" : " **不对**") << "\n";
                if (!ok) {
                    QTextStream(stderr) << "ui-test 失败：拖选的起止列不准（选到 \"" << c.selectedText()
                                        << "\"，应为 " << anchorWanted << ".." << endWanted << "）\n";
                    return 27;
                }
            }

            /*!
             * 真实拖选：按下之后一连串 MouseMove，最后停在行的**中间**。
             *
             * 上面那个"精确拖选"只送了一次移动，正好绕过了这个 bug：
             * mouseMoveEvent 里如果读 textCursor().anchor()，那个值在每次扩选后
             * 就被推到上一次的终点上了 —— 于是
             *   - 往左拖：选区变成"行首..起点"，终点永远贴在一行的开头；
             *   - 往右拖：选区塌成一个点，看着像"选不动"。
             * 用户报的"起点和终点一定在一行的开始或末尾，不能选到中间就停"
             * 就是往左拖的情况。所以这里两个方向都要走多次移动。
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
                Q_UNUSED(line);
                Q_UNUSED(blockRect);
                /*!
                 * 靶点取"字符格**中心**"，不是格边界。
                 *
                 * 格边界上的点在像素取整之后可能落到前一格，命中测试就会给出
                 * 差一个字符的结果 —— 那是靶子本身没瞄准，不是选择逻辑错。
                 * 中间点离两边都有一格的一半，怎么取整都还是这一格。
                 *
                 * 每次调用都**重新取一遍布局**：QTextLine 是布局内部句柄，
                 * 布局一旦 invalidate 旧句柄就悬垂了，实测第二次调用就返回垃圾
                 * （所有 idx 都算出同一个 x）。要用的那一刻现取。
                 */
                auto charVp = [&](int idx) {
                    const QTextBlock blk = editor->document()->firstBlock();
                    const QTextLine ln = blk.layout()->lineForTextPosition(0);
                    const QRectF br =
                        editor->document()->documentLayout()->blockBoundingRect(blk);
                    const double x0 = br.left() + ln.x() + ln.cursorToX(idx);
                    const double x1 = br.left() + ln.x() + ln.cursorToX(idx + 1);
                    const double y = br.top() + ln.y() + ln.height() / 2.0;
                    return editor->documentToViewport().map(QPointF((x0 + x1) / 2.0, y)).toPoint();
                };
                // 分 steps 段走到终点，模拟真实的连续拖动
                auto drag = [&](int fromIdx, int toIdx, int steps, int *wantFrom, int *wantTo) {
                    editor->setTextCursor(QTextCursor(editor->document()));
                    const QPoint from = charVp(fromIdx);
                    const QPoint to = charVp(toIdx);
                    /*!
                     * 期望值必须在**拖动之前**算出来：拖完再算的话，
                     * textCursor() 已经变了，量到的是"拖完之后命中"的结果，
                     * 两边必然相等 —— 这条断言就退化成了恒真式（踩过一次）。
                     */
                    *wantFrom = editor->documentCursorAt(from).position();
                    *wantTo = editor->documentCursorAt(to).position();
                    sendMouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
                    QApplication::processEvents();
                    for (int i = 1; i <= steps; ++i)
                        sendMouse(QEvent::MouseMove, from + (to - from) * i / steps,
                                  Qt::NoButton, Qt::LeftButton);
                    sendMouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
                    QApplication::processEvents();
                    return editor->textCursor();
                };

                struct DragCase { int from; int to; int steps; const char *what; };
                const QVector<DragCase> cases = {
                    {0, 6, 10, "向右多次移动"},
                    {6, 2, 6, "向左多次移动（原来会退化成行首..起点）"},
                    {9, 3, 8, "长距离向左"},
                    {8, 1, 12, "向左跨半个词"},
                };
                const int lineChars = editor->document()->firstBlock().length() - 1;
                int bad = 0;
                for (const DragCase &dc : cases) {
                    int wantFrom = -1;
                    int wantTo = -1;
                    const QTextCursor c = drag(dc.from, dc.to, dc.steps, &wantFrom, &wantTo);
                    /*!
                     * 断言三条，都不依赖"靶点像素取整后落在第几格"：
                     *   1. 锚点 == 按下点算出来的位置 —— 也就是锚点**没有被**顶到
                     *      上一帧的终点上（老代码就是这里错，往左拖会退化成
                     *      "行首..按下点"，往右拖会塌成一个点）；
                     *   2. 终点 == 松手点算出来的位置 —— 松手在哪就停在哪；
                     *   3. 两头都在行的**中间**，谁也没贴到行首或行尾
                     *      —— 这正是用户要的"能选到中间就停"。
                     */
                    const bool anchorOk = (c.anchor() == wantFrom);
                    const bool endOk = (c.position() == wantTo);
                    const bool midLine = c.anchor() > 0 && c.anchor() < lineChars
                                         && c.position() > 0 && c.position() < lineChars;
                    const bool ok = anchorOk && endOk && midLine;
                    if (!ok)
                        ++bad;
                    QTextStream(stdout) << "ui-test: 连续拖选 " << dc.what << "：" << dc.from
                                        << " -> " << dc.to << " 选中 [" << c.selectedText() << "] "
                                        << c.anchor() << ".." << c.position() << "，应为 "
                                        << wantFrom << ".." << wantTo << "（都必须在行中间）"
                                        << (ok ? " OK" : " **不对**") << "\n";
                }
                if (bad > 0) {
                    QTextStream(stderr)
                        << "ui-test 失败：连续拖选 " << bad
                        << " 处锚点/终点不对（锚点被上一帧的终点顶掉了）\n";
                    return 30;
                }
                editor->setTextCursor(QTextCursor(editor->document()));
            }

            /*!
             * 扭曲层的"透明底"：原字必须被隐藏，而不是被一块纸色刷掉。
             *
             * 用户报的是"变形后的文字有一个白色的底，会挡住选框的蓝色，只留约 1px 边"。
             * 根子是效果层画在正文（含选区高亮）之上，它想盖原字就只能刷底色，
             * 那块底色连选区一起盖。现在改成绘制期把那一格的前景设成透明。
             *
             * 断言用像素做，不看代码：
             *   - 关掉效果时先记下正文的墨迹；
             *   - 开启"扭曲 + 替换 + 半透明对照关闭"再截一张；
             *   - 原来那些墨迹像素一个都不许还是黑的（原字真的没了）；
             *   - 纸上一个白块都不许多出来（没有底色），且纸面颜色只有白纸 + 扭曲色。
             */
            {
                editor->setPlainText(QStringLiteral("HH HH HH"));
                editor->moveCursor(QTextCursor::Start);
                for (int i = 0; i < 10; ++i)
                    QApplication::processEvents();

                QTextCursor all(editor->document());
                all.select(QTextCursor::Document);
                QTextCharFormat fx;
                EffectStyle st;
                st.kind = EffectKind::Distortion;
                st.seed = 20240925u;
                setEffectStyle(&fx, st);
                all.mergeCharFormat(fx);
                editor->setTextCursor(QTextCursor(editor->document()));
                for (int i = 0; i < 10; ++i)
                    QApplication::processEvents();

                EffectRenderOptions fxOptions = editor->effectOptions();
                NoiseWave fxWave;
                fxWave.reseed(4711u);
                fxOptions.wave = &fxWave;
                fxOptions.showDistortion = true;
                fxOptions.showHandwriting = false;
                fxOptions.distortionReplaceText = true;
                fxOptions.amplitudePt = 2.5;
                fxOptions.waveScale = 2.0;
                fxOptions.distortionColor = QColor(0x14, 0x1e, 0x78);
                editor->setEffectOptions(fxOptions);
                editor->setEffectsVisible(false);
                for (int i = 0; i < 10; ++i)
                    QApplication::processEvents();

                const QImage plain = window.grab().toImage();
                plain.save(outDir + QStringLiteral("/uitest_distort_off.png"));

                editor->setEffectsVisible(true);
                /*!
                 * 等几何算好再截图。
                 *
                 * 效果层的几何是**后台分片算**的（见 effectplanner.h）：前几帧
                 * 原字照旧画着（那时还没有"洞"），算好了才换成扭曲字形。
                 * 不耐心等的话，量到的就是"原字还在"—— 而那不是缺陷，是进度。
                 */
                windowSettleEffects(window);
                for (int i = 0; i < 15; ++i)
                    QApplication::processEvents();
                const QImage fxShot = window.grab().toImage();
                fxShot.save(outDir + QStringLiteral("/uitest_distort_bg.png"));

                /*!
                 * 判据全部**只量正文那两行**（窗口坐标），不量整屏。
                 *
                 * 整屏差分踩过两次坑：界面自己的深色部件在两张图里都黑，
                 * 会把"消失的黑字"淹掉；而"新墨"那条判据一度恒为 0 ——
                 * 因为我拿来比的 plain 图根本不是"关效果"那张。
                 * 只量正文行就没这些事：那里除了纸就是字。
                 */
                const QPoint vpInWindow = editor->viewport()->mapTo(&window, QPoint(0, 0));
                const double dprS = window.devicePixelRatioF();
                /*!
                 * 只量**正文第一行那一横条**，不量整屏。
                 *
                 * 整屏统计踩过两次坑：界面自己的深色部件（工具栏图标、桌面底、
                 * 状态栏）在两张图里都是黑的，会把"消失的黑字"淹掉；
                 * 而"新墨"那条判据一度恒为 0，因为拿来比的图根本不是"关效果"那张。
                 * 只量正文行就没这些事：那里除了纸就是字。
                 */
                const QTextBlock bandBlock = editor->document()->firstBlock();
                const QRectF bandBlockRect =
                    editor->document()->documentLayout()->blockBoundingRect(bandBlock);
                /*!
                 * 取样条的位置走 documentToViewport()（文档坐标 -> viewport），
                 * 不要“文档原点 + 页边距”手算：文档原点现在就是**纸的左上角**，
                 * 手算会把量到的范围落到纸边的空白上，结果就是“一个字都没量到”。
                 */
                const QPointF bandView = editor->documentToViewport().map(
                    QPointF(bandBlockRect.left(), bandBlockRect.top()));
                const QRect textBandWin(QPoint(int(vpInWindow.x() + bandView.x()),
                                               int(vpInWindow.y() + bandView.y())),
                                        QSize(240, 40));
                const QRect textBand(int(textBandWin.left() * dprS),
                                     int(textBandWin.top() * dprS),
                                     int(textBandWin.width() * dprS),
                                     int(textBandWin.height() * dprS));
                struct BandCount { int dark = 0; int blue = 0; };
                const auto countBand = [&](const QImage &img) {
                    BandCount c;
                    for (int y = textBand.top(); y <= textBand.bottom(); ++y) {
                        const QRgb *scan = reinterpret_cast<const QRgb *>(img.constScanLine(y));
                        for (int x = textBand.left(); x <= textBand.right(); ++x) {
                            const QRgb px = scan[x];
                            if (qRed(px) < 90 && qGreen(px) < 90 && qBlue(px) < 90)
                                ++c.dark;
                            // 扭曲层的颜色是 #141e78：红绿很低、蓝明显高
                            if (qBlue(px) > 90 && qBlue(px) > qRed(px) + 40
                                && qBlue(px) > qGreen(px) + 30)
                                ++c.blue;
                        }
                    }
                    return c;
                };
                const BandCount plainBand = countBand(plain);
                const BandCount fxBand = countBand(fxShot);
                QTextStream(stdout) << "ui-test: 扭曲替换 正文行内 关效果 黑墨=" << plainBand.dark
                                    << " 蓝墨=" << plainBand.blue << "；开效果后 黑墨=" << fxBand.dark
                                    << " 蓝墨=" << fxBand.blue << "（范围 " << textBand.left() << ","
                                    << textBand.top() << " " << textBand.width() << "x"
                                    << textBand.height() << "）\n";
                if (plainBand.dark < 50) {
                    QTextStream(stderr) << "ui-test 失败：对照组（关效果）就没量到黑字，断言没有意义\n";
                    return 31;
                }
                if (fxBand.dark > plainBand.dark / 5) {
                    QTextStream(stderr) << "ui-test 失败：开启扭曲替换后原字还在（黑墨 " << fxBand.dark
                                        << " 个，关效果时 " << plainBand.dark
                                        << " 个）—— 原字没被裁掉，是靠刷底色盖的\n";
                    return 31;
                }
                if (fxBand.blue < 200) {
                    QTextStream(stderr) << "ui-test 失败：原字是没了，但扭曲层没画出来（蓝墨只有 "
                                        << fxBand.blue << " 个）\n";
                    return 31;
                }

                /*!
                 * 多块文档也要对：只在第 2 块上套扭曲，第 1 块必须原样留着。
                 *
                 * "挖洞裁剪"是按块画的，块的位置一旦算错（少加块顶、或者自己累加 y），
                 * 第 2 块起就会整体叠到第 1 块上 —— 只测单行文档看不出来。
                 */
                {
                    editor->setEffectsVisible(false);
                    editor->setPlainText(QStringLiteral("AAAAAAAAAAAAAAAAAAA\nBBBBBBBBBBBBBBBBBBB"));
                    /*!
                     * 先把整篇的效果清掉再只给第 2 行套。
                     *
                     * 不能指望 setPlainText 清干净：它只重置字符内容，
                     * 光标位置的**字符格式是带过去的**（QTextEdit 会把当前格式
                     * 应用给新文本），于是上一段测试套在"HH HH HH"上的扭曲
                     * 会跟着跑到新文本上，两行都被替换 —— 断言里就成了
                     * "没套效果的那一块被改动了"，看着像绘制逻辑错，其实是测试没摆干净。
                     */
                    {
                        QTextCursor wipe(editor->document());
                        wipe.select(QTextCursor::Document);
                        clearEffects(&wipe);
                        editor->setTextCursor(QTextCursor(editor->document()));
                    }
                    for (int i = 0; i < 5; ++i)
                        QApplication::processEvents();
                    for (int i = 0; i < 10; ++i)
                        QApplication::processEvents();

                    QTextCursor second(editor->document());
                    // 必须先 setPosition 再 movePosition：不带锚点的 movePosition
                    // 是从光标当前位置（默认 0）**扩选**过去的，
                    // 会把第 1 行也一起选上（踩过，效果项数直接翻倍）
                    second.setPosition(editor->document()->findBlockByNumber(1).position());
                    second.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                    QTextCharFormat f2;
                    EffectStyle s2;
                    s2.kind = EffectKind::Distortion;
                    s2.seed = 777u;
                    setEffectStyle(&f2, s2);
                    second.mergeCharFormat(f2);
                    editor->setTextCursor(QTextCursor(editor->document()));
                    for (int i = 0; i < 10; ++i)
                        QApplication::processEvents();

                    const QImage off = window.grab().toImage();
                    editor->setEffectsVisible(true);
                    // 同样要等几何算好：没算好的格子还画着原字（见 windowSettleEffects）
                    windowSettleEffects(window);
                    for (int i = 0; i < 15; ++i)
                        QApplication::processEvents();
                    const QImage on = window.grab().toImage();
                    on.save(outDir + QStringLiteral("/uitest_distort_multiblock.png"));

                    // 两块各自的窗口坐标矩形（正文第 1 行 / 第 2 行）
                    const auto blockBand = [&](int row) {
                        const QRectF br = editor->document()->documentLayout()->blockBoundingRect(
                            editor->document()->findBlockByNumber(row));
                        const QPointF topLeft = editor->documentToViewport().map(
                            QPointF(br.left(), br.top())); // 文档坐标 -> viewport 局部
                        return QRect(int((vpInWindow.x() + topLeft.x()) * dprS),
                                     int((vpInWindow.y() + topLeft.y()) * dprS),
                                     int(br.width() * dprS), int(br.height() * dprS));
                    };
                    const auto countIn = [](const QImage &img, const QRect &band, bool wantBlue) {
                        int n = 0;
                        for (int y = qMax(0, band.top()); y <= band.bottom(); ++y) {
                            const QRgb *scan =
                                reinterpret_cast<const QRgb *>(img.constScanLine(y));
                            for (int x = qMax(0, band.left()); x <= band.right(); ++x) {
                                const QRgb px = scan[x];
                                const bool blue = qBlue(px) > 90 && qBlue(px) > qRed(px) + 40
                                                  && qBlue(px) > qGreen(px) + 30;
                                const bool dark = qRed(px) < 90 && qGreen(px) < 90
                                                  && qBlue(px) < 90;
                                if (wantBlue ? blue : dark)
                                    ++n;
                            }
                        }
                        return n;
                    };
                    const QRect row0 = blockBand(0);
                    const QRect row1 = blockBand(1);
                    // 第 1 行：开效果前后都要有黑字（它没被套效果）
                    const int row0Off = countIn(off, row0, false);
                    const int row0On = countIn(on, row0, false);
                    // 第 2 行：开效果后黑字消失、出现扭曲墨
                    const int row1Off = countIn(off, row1, false);
                    const int row1On = countIn(on, row1, false);
                    const int row1Blue = countIn(on, row1, true);
                    QTextStream(stdout) << "ui-test: 多块 第1行黑墨 " << row0Off << " -> " << row0On
                                        << "（应保持）；第2行黑墨 " << row1Off << " -> " << row1On
                                        << "、扭曲墨 " << row1Blue << "（应换掉）\n";
                    if (row0Off < 50 || row0On < row0Off / 2) {
                        QTextStream(stderr) << "ui-test 失败：没有套效果的那一块被改动了（" << row0Off
                                            << " -> " << row0On << "）—— 逐块绘制的位置算错了\n";
                        return 33;
                    }
                    if (row1On > row1Off / 5 || row1Blue < 100) {
                        QTextStream(stderr) << "ui-test 失败：第 2 块的原字没被换掉（黑墨 " << row1On
                                            << "，扭曲墨 " << row1Blue << "）\n";
                        return 33;
                    }

                    editor->setEffectsVisible(false);
                    // 换回上一段测试的文本（顺便把效果带过去，下一段测试自己会清）
                    editor->setPlainText(QStringLiteral("HH HH HH"));
                    editor->moveCursor(QTextCursor::Start);
                    for (int i = 0; i < 10; ++i)
                        QApplication::processEvents();
                }
                editor->setEffectsVisible(false);
                {
                    QTextCursor wipe(editor->document());
                    wipe.select(QTextCursor::Document);
                    clearEffects(&wipe);
                }

                /*!
                 * 再来一条"选框不能被底色挡住"的断言：
                 * 选中全部文字，量选区色像素。效果层要是还在刷纸色底，
                 * 蓝色会被啃掉一大块。
                 */
                editor->setEffectsVisible(false);
                QTextCursor sel(editor->document());
                sel.select(QTextCursor::Document);
                editor->setTextCursor(sel);
                for (int i = 0; i < 10; ++i)
                    QApplication::processEvents();
                const int hiNoFx = countSelectionPixels(window.grab().toImage());
                editor->setEffectsVisible(true);
                for (int i = 0; i < 15; ++i)
                    QApplication::processEvents();
                const int hiFx = countSelectionPixels(window.grab().toImage());
                window.grab().save(outDir + QStringLiteral("/uitest_distort_sel.png"));
                QTextStream(stdout) << "ui-test: 选区高亮像素 关效果 " << hiNoFx << " -> 开效果 "
                                    << hiFx << "\n";
                if (hiNoFx < 500 || hiFx < hiNoFx * 4 / 5) {
                    QTextStream(stderr) << "ui-test 失败：开效果后选区被盖掉（" << hiNoFx
                                        << " -> " << hiFx << "）—— 效果层还在刷底色\n";
                    return 32;
                }

                editor->setEffectsVisible(false);
                editor->setEffectOptions(EffectRenderOptions());
                editor->clear();
                editor->setTextCursor(QTextCursor(editor->document()));
                for (int i = 0; i < 5; ++i)
                    QApplication::processEvents();
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

    /*!
     * 手写加噪声、缩放这两项放在最后：它们都会换掉正文内容
     * （量纸宽要在空白处量、铺手写要有确定的字符），
     * 前面那些和内容有关的体检（光标、选区、扭曲）都已经跑完了。
     */
    if (probeHwNoise) {
        const int rc = runHandwritingNoiseProbe(window, outDir);
        if (rc != 0)
            return rc;
    }

    /*!
     * 长活儿自检放最后：它会写一篇长稿子、套一遍效果再取消，
     * 现场和前面那些体检完全不兼容。
     */
    if (probeProgress) {
        const int rc = runProgressProbe(window, outDir);
        if (rc != 0)
            return rc;
    }

    if (probeZoom) {
        const int rc = runZoomProbe(window, outDir);
        if (rc != 0)
            return rc;
    }

    /*!
     * 折腾自检放最后：它会一直改文档/窗口/缩放，把前面那些体检的现场都搅乱。
     */
    if (probeSoak) {
        const int rc = runSoakProbe(window, outDir, 400);
        if (rc != 0)
            return rc;
    }

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
            "act_lib",       "act_baseline",
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

/*!
 * 屏蔽掉 DirectWrite 那条"点阵字体建面失败"的警告。
 *
 *   qt.qpa.fonts: DirectWrite: CreateFontFaceFromHDC() failed (...) for
 *   QFontDef(Family="MS Serif", ...)
 *
 * 系统里 MS Serif / MS Sans Serif / Small Fonts 这些是 GDI 点阵字体，
 * DirectWrite 建不出字体面；Qt 随后会自己回退到可缩放的替代字体，
 * 所以它只是**噪音**，不影响排版结果。
 *
 * 字体候选列表那边已经把点阵字体过滤掉了（见 MainWindow::buildToolBars 与
 * allFontFamilies），正常操作不会再触发；这里再兜一道，是因为文档里可能
 * 残留着别人机器上的老字体名，一旦命中就会在控制台反复刷屏。
 * 只丢这一条，别的 qt.qpa.fonts 消息原样放行。
 */
void installFontNoiseFilter()
{
    // 取出当前处理器（第一次调用时把默认处理器换回来，不会丢东西）
    static const QtMessageHandler previous = qInstallMessageHandler(nullptr);
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &context,
                              const QString &message) {
        if (message.contains(QLatin1String("CreateFontFaceFromHDC")))
            return;
        if (previous) {
            previous(type, context, message);
            return;
        }
        // 没有旧处理器：按 Qt 默认格式 <category>: <message> 打到 stderr
        const char *category = context.category ? context.category : "default";
        fprintf(stderr, "%s: %s\n", category, qPrintable(message));
        if (type == QtFatalMsg)
            abort();
    });
}

/*!
 * 版本号：只在这一个地方写死（CMake 的 `project(tripa VERSION 0.2)` 是另一份，
 * 版本号写两遍迟早会不一致；这里这份是**日志和诊断信息**用的，改版本时一起改）。
 */
static constexpr const char *TRIPA_VERSION_STRING = "0.2";

/*!
 * \brief 把日志相关的参数从命令行里摘掉。
 *
 * 自检是按**位置**认参数的（`--uitest <目录> <主题> <组>`），
 * 多一个认不出来的参数就会让整组自检跑不起来 —— 而排查问题时
 * 十有八九是要"带日志再跑一次自检"，所以这两件事必须能共存。
 */
static QStringList stripLogArguments(const QStringList &arguments)
{
    QStringList out;
    for (int i = 0; i < arguments.size(); ++i) {
        const QString &a = arguments.at(i);
        if (a == QLatin1String("--log-file")) {
            ++i; // 连值一起摘掉
            continue;
        }
        if (a == QLatin1String("--log") || a == QLatin1String("--no-log")
            || a == QLatin1String("--log-stderr") || a == QLatin1String("--no-crash-dump")
            || a.startsWith(QLatin1String("--log="))
            || a.startsWith(QLatin1String("--log-file="))) {
            continue;
        }
        out.append(a);
    }
    return out;
}

int main(int argc, char *argv[])
{
    /*!
     * 日志必须在**最前面**起来：它要在 QApplication 构造之前就装好消息处理器，
     * 这样连"Qt 起不来 / 插件加载失败"这种事都能留下记录。
     * `parseCommandLine` 特意不依赖 Qt（只用 qgetenv / fromLocal8Bit）。
     */
    tripalog::Options logOptions;
    tripalog::parseCommandLine(argc, argv, &logOptions);
    /*!
     * `--crashdir <目录>`：把日志/崩溃报告都放到指定目录（自检用）。
     * 现场不需要这个参数，默认位置（`%LOCALAPPDATA%/tripa`）就好。
     */
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--crashdir") && i + 1 < argc) {
            const QString dir = QString::fromLocal8Bit(argv[i + 1]);
            logOptions.filePath = dir + QStringLiteral("/tripa.log");
            break;
        }
    }
    tripalog::start(logOptions);
    tripalog::breadcrumb(QStringLiteral("程序启动"));
    {
        // 把"启动现场"记下来：排查时第一件要问的就是这些
        QStringList args;
        for (int i = 1; i < argc; ++i)
            args << QString::fromLocal8Bit(argv[i]);
        TRIPA_INFO("app",
                   QStringLiteral("启动：版本=%1 Qt=%2 系统=%3 参数=[%4] 工作目录=%5")
                       .arg(QLatin1String(TRIPA_VERSION_STRING))
                       .arg(QLatin1String(qVersion()))
                       .arg(QSysInfo::prettyProductName(), args.join(QLatin1Char(' ')),
                            QDir::currentPath()));
        TRIPA_INFO("app",
                   QStringLiteral("日志文件=%1（级别 %2）")
                       .arg(tripalog::logFilePath().isEmpty() ? QStringLiteral("（关闭）")
                                                              : tripalog::logFilePath())
                       .arg(int(logOptions.level)));
    }

    installFontNoiseFilter();

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("tripa"));
    QApplication::setApplicationVersion(QLatin1String(TRIPA_VERSION_STRING));
    QApplication::setApplicationDisplayName(QStringLiteral("tripa 排版器"));
    QApplication::setOrganizationName(QStringLiteral("mywrite"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/tripa.png")));
    TRIPA_INFO("app", QStringLiteral("QApplication 建好了，平台插件=%1 屏幕=%2")
                                          .arg(QApplication::platformName())
                                          .arg(QStringLiteral("%1x%2 @%3dpi")
                                                   .arg(app.primaryScreen()
                                                            ? app.primaryScreen()->geometry().width()
                                                            : 0)
                                                   .arg(app.primaryScreen()
                                                            ? app.primaryScreen()->geometry().height()
                                                            : 0)
                                                   .arg(app.primaryScreen()
                                                            ? qRound(app.primaryScreen()->logicalDotsPerInch())
                                                            : 0)));

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = QStringLiteral("tripa_") + QLocale(locale).name();
        if (translator.load(QStringLiteral(":/i18n/") + baseName)) {
            QApplication::installTranslator(&translator);
            break;
        }
    }

    /*!
     * 自检那条路要把 `--log` 之类的参数摘掉再往下传：
     * `--uitest <目录> <主题> <组>` 是按位置认参数的，
     * 多一个认不出来的参数就会让整组自检跑不起来。
     * （QApplication 已经吃过 argv 了，它只挑自己认识的，不受影响。）
     */
    QStringList args = QApplication::arguments();
    args = stripLogArguments(args);
    if (args.size() > 1 && args.at(1) == QStringLiteral("--selftest")) {
        const int rc = runSelfTest(args.mid(1));
        TRIPA_INFO("app", QStringLiteral("--selftest 结束，返回码=%1").arg(rc));
        tripalog::shutdown();
        return rc;
    }
    if (args.size() > 1 && args.at(1) == QStringLiteral("--crashtest")) {
        // 故意崩：崩溃处理器会写出 dump（见 runCrashDumpProbe）
        return runCrashChild();
    }
    if (args.size() > 1 && args.at(1) == QStringLiteral("--uitest")) {
        const int rc = runUiTest(args.mid(1));
        TRIPA_INFO("app", QStringLiteral("--uitest 结束，返回码=%1").arg(rc));
        tripalog::shutdown();
        return rc;
    }
    MainWindow window;
    window.show();
    TRIPA_INFO("app", QStringLiteral("主窗口已显示，进入事件循环"));
    const int rc = QApplication::exec();
    TRIPA_INFO("app", QStringLiteral("事件循环退出，返回码=%1").arg(rc));
    tripalog::shutdown();
    return rc;
}
