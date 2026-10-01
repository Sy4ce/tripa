/*
 * 临时基准：量清楚"给文字加噪声"这条路上每一步各花多久。
 *
 * 目的只有一个：把"卡"归因到具体的函数，而不是凭感觉优化。
 * 量完这个文件就删掉（结论会写进 README 与正式自检）。
 *
 * 跑法： tools/build-manual.ps1 -Targets bench_effects  &&  ./build/bench_effects.exe
 */

#include "effect.h"
#include "effectsrenderer.h"
#include "handwriting.h"
#include "noise.h"
#include "paginatinglayout.h"
#include "pagesetup.h"
#include "richdocument.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>

#include <cstdio>

namespace {

double ms(const QElapsedTimer &t)
{
    return t.nsecsElapsed() / 1e6;
}

//! 造 \a count 个"互不相同"的汉字（取 U+4E00 起的一段，保证每个字都要单独算轮廓）
QString makeText(int count)
{
    QString text;
    text.reserve(count + count / 20);
    for (int i = 0; i < count; ++i) {
        text.append(QChar(0x4E00 + (i % 20000)));
        if ((i + 1) % 40 == 0)
            text.append(QLatin1Char('\n'));
    }
    return text;
}

void benchApply(QTextDocument *doc, const QVector<QPair<int, int>> &ranges, quint32 seed)
{
    QElapsedTimer t;
    t.start();
    QTextCursor work(doc);
    work.beginEditBlock();
    for (const auto &range : ranges) {
        for (int pos = range.first; pos < range.second; ++pos) {
            QTextCursor one(doc);
            one.setPosition(pos);
            one.setPosition(pos + 1, QTextCursor::KeepAnchor);
            EffectStyle style;
            style.kind = EffectKind::Distortion;
            style.seed = seed + quint32(pos) * 2654435761u;
            QTextCharFormat fmt;
            setEffectStyle(&fmt, style);
            one.mergeCharFormat(fmt);
        }
    }
    work.endEditBlock();
    std::printf("  逐字套效果（%d 字）              %8.1f ms\n", doc->characterCount() - 1, ms(t));
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const int charCount = argc > 1 ? QString::fromLatin1(argv[1]).toInt() : 400;

    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPointSizeF(12.0);

    QTextDocument doc;
    doc.setDefaultFont(font);
    doc.setDocumentMargin(0);

    QElapsedTimer t;
    t.start();
    doc.setPlainText(makeText(charCount));
    std::printf("填正文（%d 字）                    %8.1f ms\n", doc.characterCount() - 1, ms(t));

    PaginatingLayout *layout = installPaginatingLayout(&doc);
    PageSetup setup;
    setup.presetName = QStringLiteral("A4");
    setup.widthMm = 210.0;
    setup.heightMm = 297.0;
    setup.marginLeftMm = 20.0;
    setup.marginTopMm = 20.0;
    setup.marginRightMm = 20.0;
    setup.marginBottomMm = 20.0;
    layout->setMetrics(RenderMetrics::fromPageSetup(setup));
    t.restart();
    layout->pageCount();
    std::printf("首次分页排版（%d 页）             %8.1f ms\n", layout->pageCount(), ms(t));

    const QVector<QPair<int, int>> ranges{{0, doc.characterCount() - 1}};
    benchApply(&doc, ranges, 0x12345678u);

    t.restart();
    layout->invalidateAll();
    const int pages = layout->pageCount();
    std::printf("套完效果后重新分页（%d 页）        %8.1f ms\n", pages, ms(t));

    // 噪声波
    NoiseWave wave;
    wave.reseed(0x12345678u);

    // 手写库（内置数据可能没有，没有就只量"缺字统计"）
    HandwritingLibrary library;
    library.loadResourceDir(QStringLiteral(":/handwrite"));
    std::printf("手写库：%d 字 / %d 样本\n", library.characterCount(), library.sampleCount());

    EffectRenderOptions options;
    options.library = &library;
    options.wave = &wave;
    options.amplitudePt = 1.6;
    options.waveScale = 2.0;
    options.showDistortion = true;
    options.showHandwriting = false;

    t.restart();
    const int effectChars = doc.characterCount() - 1;
    QVector<QPair<int, int>> hidden;
    const QVector<EffectDrawItem> items = planEffects(&doc, options, &hidden);
    std::printf("planEffects（%d 项）              %8.1f ms  -> 每项 %.3f ms\n",
                items.size(), ms(t), items.isEmpty() ? 0.0 : ms(t) / items.size());

    t.restart();
    planEffects(&doc, options, nullptr);
    std::printf("planEffects 第二次（无缓存）       %8.1f ms\n", ms(t));

    // --- 单字几何：冷缓存下每个字多少钱
    const int probe = qMin(argc > 2 ? QString::fromLatin1(argv[2]).toInt() : 30, items.size());
    double outlineMs = 0.0;
    double worstMs = 0.0;
    QString worstChar;
    for (int i = 0; i < probe; ++i) {
        const EffectDrawItem &item = items.at(i);
        const QString one(doc.characterAt(item.position));
        QElapsedTimer one_clock;
        one_clock.start();
        const QVector<QVector<QPointF>> o =
            distortedGlyphOutlines(one, item.font, wave, item.seed, item.fit.scale,
                                   qMax(0.05, options.amplitudePt * item.fit.scale),
                                   options.waveScale);
        const double cost = ms(one_clock);
        outlineMs += cost;
        int points = 0;
        for (const QVector<QPointF> &sub : o)
            points += sub.size();
        if (cost > worstMs) {
            worstMs = cost;
            worstChar = QStringLiteral("%1（%2 条轮廓 %3 点）").arg(one).arg(o.size()).arg(points);
        }
    }
    std::printf("扭曲单字轮廓（冷缓存 %d 字）       %8.1f ms  -> 每字 %.2f ms，最慢 %.2f ms %s\n",
                probe, outlineMs, probe ? outlineMs / probe : 0.0, worstMs, qPrintable(worstChar));
    std::printf("   => 全篇 %d 字外推            %8.1f ms\n", effectChars,
                probe ? outlineMs / probe * effectChars : 0.0);

    // --- 整段绘制（冷缓存）
    QImage image(1240, 1754, QImage::Format_ARGB32_Premultiplied);
    t.restart();
    {
        QPainter painter(&image);
        renderEffects(&painter, &doc, options);
    }
    std::printf("renderEffects 冷缓存（%d 字）      %8.1f ms\n", effectChars, ms(t));

    // --- 整段绘制（热缓存）
    t.restart();
    {
        QPainter painter(&image);
        renderEffects(&painter, &doc, options);
    }
    std::printf("renderEffects 热缓存               %8.1f ms\n", ms(t));

    // --- 效果层：手写笔迹的"准备 -> 绘制"链路（导出路径靠它）
    {
        QTextDocument probe;
        probe.setDefaultFont(font);
        probe.setDocumentMargin(0);
        probe.setPlainText(QStringLiteral("a"));
        PaginatingLayout *probeLayout = installPaginatingLayout(&probe);
        probeLayout->setMetrics(RenderMetrics::fromPageSetup(setup));

        HandwritingLibrary hw;
        hw.addCsvText(QStringLiteral("a\n0,0,0.5\n10,0,0.5\n10,10,0.5\n0,10,0.5\n"),
                      QStringLiteral("probe.csv"));
        QTextCursor one(&probe);
        one.setPosition(0);
        one.setPosition(1, QTextCursor::KeepAnchor);
        QTextCharFormat fmt;
        EffectStyle style;
        style.kind = EffectKind::Handwriting;
        style.seed = 4242u;
        setEffectStyle(&fmt, style);
        one.mergeCharFormat(fmt);

        EffectRenderOptions hwOptions;
        hwOptions.library = &hw;
        hwOptions.showHandwriting = true;
        hwOptions.handwritingReplaceText = true;

        QVector<EffectDrawItem> hwItems = planEffects(&probe, hwOptions);
        std::printf("手写探针：计划 %d 项\n", hwItems.size());
        prepareEffects(&hwItems, hwOptions);
        if (!hwItems.isEmpty()) {
            const EffectDrawItem &it = hwItems.first();
            std::printf("  ready=%d missing=%d hidden=%d 笔画=%d 格子=%s\n", it.art.ready ? 1 : 0,
                        it.art.missingData ? 1 : 0, it.hidden ? 1 : 0, it.art.strokes.size(),
                        qPrintable(QStringLiteral("%1,%2 %3x%4")
                                       .arg(it.charRect.left(), 0, 'f', 1)
                                       .arg(it.charRect.top(), 0, 'f', 1)
                                       .arg(it.charRect.width(), 0, 'f', 1)
                                       .arg(it.charRect.height(), 0, 'f', 1)));
        }
        QImage hwImage(600, 200, QImage::Format_RGB32);
        hwImage.fill(Qt::white);
        {
            QPainter painter(&hwImage);
            renderPreparedEffects(&painter, hwItems, hwOptions);
        }
        int ink = 0;
        for (int y = 0; y < hwImage.height(); ++y) {
            const QRgb *scan = reinterpret_cast<const QRgb *>(hwImage.constScanLine(y));
            for (int x = 0; x < hwImage.width(); ++x) {
                const QRgb c = scan[x];
                if (qRed(c) < 200 || qGreen(c) < 200 || qBlue(c) < 200)
                    ++ink;
            }
        }
        std::printf("  prepare+draw 后全图墨点=%d（应为 0 才对？—— 只画效果层，没有正文）\n", ink);
    }

    // --- 缺字统计
    t.restart();
    const QStringList missing = findMissingHandwriting(&doc, &library);
    std::printf("缺字统计（%d 个缺）               %8.1f ms\n", missing.size(), ms(t));
    // --- 只画正文（不含效果层）
    t.restart();
    {
        QPainter painter(&image);
        QAbstractTextDocumentLayout::PaintContext ctx;
        layout->draw(&painter, ctx);
    }
    std::printf("只画正文（layout->draw）         %8.1f ms\n", ms(t));

    // --- 完整绘制一轮（正文 + 效果），模拟一次 paintEvent
    for (int round = 0; round < 3; ++round) {
        t.restart();
        {
            QPainter painter(&image);
            QAbstractTextDocumentLayout::PaintContext ctx;
            layout->draw(&painter, ctx);
            renderEffects(&painter, &doc, options);
        }
        std::printf("一次完整重绘（正文+效果，热）#%d    %8.1f ms\n", round + 1, ms(t));
    }


    return 0;
}
