#include "mainwindow.h"

#include "baselineadjust.h"
#include "effectsrenderer.h"
#include "handwriting.h"
#include "pagesetup.h"
#include "proofsheet.h"
#include "texteditor.h"

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
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPageLayout>
#include <QPainterPath>
#include <QPdfWriter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSlider>
#include <QStringList>
#include <QStyle>
#include <QTableWidget>
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

    // 设备像素 / 96dpi 像素（96dpi 下 1mm = 25.4 px）
    const double zoom = (double(device->width()) / paperMm.width()) * 25.4 / PageSetup::kDpi;
    const double pageHeightPx = paperMm.height() * PageSetup::kDpi / 25.4;
    const int pages = qMax(1, int(std::ceil(document->size().height() / cardPx.height())));

    /*!
     * 会被效果"整格替换掉"的字符：正文这一格必须**不画**。
     *
     * 以前是让效果层往上刷一块纸色去盖原字，那块底色在屏幕上还会盖掉
     * 选区高亮（用户看到的就是"变形后的字有个白底挡住选框"）。
     * 现在改成绘制期把这一格裁掉，屏幕和纸面走同一份计划，谁也不欠谁一块底色。
     *
     * 用裁剪路径而不是给这一格设透明前景：设格式会 invalidate 布局，
     * 让别处已经拿到的 QTextLine 变成悬垂引用（实测会直接段错误）。
     * 裁剪是纯绘制期的，不碰布局。
     *
     * 该挖哪些格子一律看 EffectDrawItem::hidden —— 手写层和扭曲层一个口径。
     * （这里以前只处理扭曲，于是"手写遮住正文"在屏幕上是遮住的、
     *   导出 PDF 时原字又冒出来了，所见非所得。）
     */
    const QVector<EffectDrawItem> items = planEffects(document, options);
    /*!
     * 正文窗口（文档坐标下的第一页正文区）减去那些洞。
     *
     * 奇偶填充：窗口是实、洞是空。洞必须挑**落在窗口里**的那些 ——
     * 别的页的洞在原坐标里和窗口不相交，直接加进奇偶路径会变成一块
     * "实心岛"，平白多给出一片可绘制区域。
     */
    QPainterPath pageClip;
    bool anyHidden = false;
    {
        const QRectF window(QPointF(0.0, 0.0), cardPx);
        pageClip.setFillRule(Qt::OddEvenFill);
        pageClip.addRect(window);
        for (const EffectDrawItem &item : items) {
            if (item.hidden && item.charRect.intersects(window)) {
                pageClip.addRect(item.charRect);
                anyHidden = true;
            }
        }
    }

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
        /*!
         * 有字要被替换时才设这个裁剪（它同时充当"这一页的正文窗口"）；
         * 一个洞都没有时保持原样不设裁剪，免得平白改变无效果文档的输出。
         */
        if (anyHidden)
            painter->setClipPath(pageClip, Qt::IntersectClip);
        document->documentLayout()->draw(painter, context);

        if (options.anyLayer()) {
            /*!
             * 画效果层之前**必须先去掉"挖洞"的裁剪**。
             *
             * 洞的位置就是会被效果字形替换掉的那些格子 —— 也就是效果层要画的地方。
             * 带着这套裁剪去画效果层，等于把新字形也一起裁掉：原字没了、新字也没了。
             * 屏幕那条路径早就这么处理了（见 TextEditor::paintEvent 里的
             * setClipping(false) 与那段注释），导出这一路漏了这一步，
             * 结果是**屏幕上好好的、导出的 PDF / 图片里手写笔迹几乎全没了**。
             * 保留下来的只有恰好越过格子边界的那几笔，看着像几个碎竖条。
             *
             * 换成普通的"正文窗口"裁剪：效果层不该画到页边距外面去。
             */
            painter->setClipping(false);
            painter->setClipRect(QRectF(QPointF(0.0, 0.0), cardPx));
            EffectRenderOptions local = options;
            local.missing.clear();
            renderEffects(painter, document, local);
        }

        painter->restore();
    }

    painter->restore();
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
                const QMarginsF marginsPx = setup.bodyMarginsPx();
                const double toDevice = dpi / PageSetup::kDpi;
                const QRect dev(int((cell.left() + marginsPx.left()) * toDevice),
                                int((cell.top() + marginsPx.top()) * toDevice),
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
        editor->setFocus();
        editor->verticalScrollBar()->setValue(200);
        for (int i = 0; i < 20; ++i) {
            QApplication::processEvents();
            QThread::msleep(5);
        }

        const double dpr = window.devicePixelRatioF();
        const QPoint vpInWindow = editor->viewport()->mapTo(&window, QPoint(0, 0));
        const QPointF docOrigin = editor->documentOriginInViewport();
        const double docY = editor->document()->documentLayout()->blockBoundingRect(block).top();
        const double wantTop = vpInWindow.y() + docOrigin.y() + docY;
        const double wantLeft = vpInWindow.x() + docOrigin.x();

        int foundTop = -1;
        int foundLeft = -1;
        int foundBottom = -1;
        for (int attempt = 0; attempt < 40 && foundTop < 0; ++attempt) {
            QApplication::processEvents();
            QThread::msleep(20); // 光标会闪：暗相位抓不到，多抓几次
            const QImage win = window.grab().toImage();
            const int y0 = qMax(0, int((wantTop - 8) * dpr));
            const int y1 = qMin(win.height(), int((wantTop + 40) * dpr));
            const int x0 = qMax(0, int(wantLeft * dpr));
            const int x1 = qMin(win.width(), int((wantLeft + 60) * dpr));
            for (int x = x0; x < x1 && foundTop < 0; ++x) {
                int top = -1;
                int bottom = -1;
                for (int y = y0; y < y1; ++y) {
                    const QRgb px = win.pixel(x, y);
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
        || qAbs(editor->documentOriginInViewport().y() - origin0.y()) > 1.0) {
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
        const QPointF origin = QPointF(vpInWindow) + editor->documentOriginInViewport();
        const double dpr = window.devicePixelRatioF();
        const QRectF band(origin.x() - 4.0, origin.y() + blockRect.top() - 2.0,
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
 *   - hwnoise：给手写字加噪声的自检（见 runHandwritingNoiseProbe）。
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
                const QPointF docOrigin = editor->documentOriginInViewport();
                const double dprS = window.devicePixelRatioF();
                /*!
                 * 只量**正文第一行那一横条**，不量整屏。
                 *
                 * 整屏统计踩过两次坑：界面自己的深色部件（工具栏图标、桌面底、
                 * 状态栏）在两张图里都是黑的，会把"消失的黑字"淹掉；
                 * 而"新墨"那条判据一度恒为 0，因为拿来比的图根本不是"关效果"那张。
                 * 只量正文行就没这些事：那里除了纸就是字。
                 */
                const QRect textBandWin(
                    QPoint(int(vpInWindow.x() + docOrigin.x()), int(vpInWindow.y() + docOrigin.y())),
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
                    for (int i = 0; i < 15; ++i)
                        QApplication::processEvents();
                    const QImage on = window.grab().toImage();
                    on.save(outDir + QStringLiteral("/uitest_distort_multiblock.png"));

                    // 两块各自的窗口坐标矩形（正文第 1 行 / 第 2 行）
                    const auto blockBand = [&](int row) {
                        const QRectF br = editor->document()->documentLayout()->blockBoundingRect(
                            editor->document()->findBlockByNumber(row));
                        return QRect(int((vpInWindow.x() + docOrigin.x() + br.left()) * dprS),
                                     int((vpInWindow.y() + docOrigin.y() + br.top()) * dprS),
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

    if (probeZoom) {
        const int rc = runZoomProbe(window, outDir);
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

int main(int argc, char *argv[])
{
    installFontNoiseFilter();

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
