/*
 * 长活儿调度与效果计算层的自检（jobrunner.h / effectplanner.h）。
 *
 * 这一组断言盯的是**这次重构的契约**，不是"顺手加的覆盖率"：
 *   - 长活儿必须真的分片、真的让出事件循环（不然"界面不卡"就是空话）；
 *   - 进度必须单调、总数/已完成必须自洽（状态栏照着它画进度条）；
 *   - 取消必须停在分片边界、并且收尾钩子只被调一次（界面状态要恢复）；
 *   - 效果显示表必须**按段落惰性算**、几何必须只在计算侧算
 *     —— 绘制路径一旦开始算几何，"点一下加噪声白一秒"就会回来。
 *
 * 跑法： cmake --build build --target test_jobs && ./build/test_jobs.exe
 */

#include "effect.h"
#include "effectplanner.h"
#include "handwriting.h"
#include "jobrunner.h"
#include "noise.h"
#include "paginatinglayout.h"
#include "pagesetup.h"
#include "richdocument.h"

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>
#include <QTimer>

#include <cmath>

static int g_failures = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        if (cond) {                                                              \
            qInfo().noquote() << "  ok   " << (what);                             \
        } else {                                                                 \
            ++g_failures;                                                        \
            qWarning().noquote() << "  FAIL " << (what);                          \
        }                                                                        \
    } while (false)

namespace {

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

//! 跑事件循环，直到调度器空下来（任务都在事件循环里推进）
void runUntilIdle(JobRunner *runner)
{
    if (runner->idle())
        return;
    QEventLoop loop;
    QObject::connect(runner, &JobRunner::idled, &loop, &QEventLoop::quit);
    loop.exec();
}

//! 造 20 段、每段 20 个字（共 400 字），并在整篇上挂一种效果
void fillWithEffects(QTextDocument *doc, EffectKind kind, quint32 seed)
{
    QStringList lines;
    for (int i = 0; i < 20; ++i) {
        QString line;
        for (int j = 0; j < 20; ++j)
            line.append(QChar(0x4E00 + (i * 20 + j) % 500));
        lines << line;
    }
    doc->setPlainText(lines.join(QLatin1Char('\n')));

    QTextCursor work(doc);
    work.beginEditBlock();
    for (int pos = 0; pos < doc->characterCount() - 1; ++pos) {
        QTextCursor one(doc);
        one.setPosition(pos);
        one.setPosition(pos + 1, QTextCursor::KeepAnchor);
        if (one.selectedText().trimmed().isEmpty())
            continue;
        EffectStyle style;
        style.kind = kind;
        style.seed = seed + quint32(pos) * 2654435761u;
        QTextCharFormat fmt;
        setEffectStyle(&fmt, style);
        one.mergeCharFormat(fmt);
    }
    work.endEditBlock();
}

//! 一份"三层就位"的文档（富文本 + 分页渲染 + 计算层）
struct Fixture
{
    QTextDocument doc;
    PaginatingLayout *layout = nullptr;

    Fixture()
    {
        QFont font(QStringLiteral("Microsoft YaHei"));
        font.setPointSizeF(12.0);
        doc.setDefaultFont(font);
        doc.setDocumentMargin(0);
        layout = installPaginatingLayout(&doc);
        layout->setMetrics(RenderMetrics::fromPageSetup(a4Setup()));
    }
};

// ---------------------------------------------------------------- 调度器

/*!
 * 分片是真的发生了吗。
 *
 * 判据不是"step() 被调了几次"（那只能说明循环在跑），而是
 * **事件循环在任务进行期间还能派发事件** —— 那才是"界面不卡"的定义。
 * 所以这里挂一个 0 间隔定时器数心跳，再看时间预算有没有被遵守。
 */
void checkJobSlicing()
{
    qInfo().noquote() << "== 调度器：分片、让出、进度 ==";

    JobRunner runner;
    runner.setSliceBudgetMs(5);

    int ticks = 0;
    QTimer heartbeat;
    heartbeat.setInterval(0);
    QObject::connect(&heartbeat, &QTimer::timeout, [&ticks] { ++ticks; });
    heartbeat.start();

    int steps = 0;
    auto *job = new LoopJob(QStringLiteral("数数"), 4000, [&steps](int) {
        // 每项花一点点时间：让一片里能装下若干项，但整件事跨很多片
        QElapsedTimer spin;
        spin.start();
        while (spin.nsecsElapsed() < 20000) { }
        ++steps;
    });
    job->setChunkSize(20);

    QVector<JobProgress> snapshots;
    QObject::connect(&runner, &JobRunner::progressed,
                     [&snapshots](const JobProgress &p) { snapshots.append(p); });

    runner.enqueue(job);
    runUntilIdle(&runner);
    heartbeat.stop();

    CHECK(steps == 4000, QStringLiteral("4000 项全干完（实际 %1）").arg(steps));
    CHECK(ticks > 5, QStringLiteral("任务期间事件循环还在转（心跳 %1 次）").arg(ticks));
    CHECK(!snapshots.isEmpty(), QStringLiteral("中途报告过进度（%1 次）").arg(snapshots.size()));

    bool monotonic = true;
    bool sane = true;
    int previous = -1;
    for (const JobProgress &p : snapshots) {
        if (p.done < previous)
            monotonic = false;
        previous = p.done;
        if (p.total != 4000 || p.done < 0 || p.done > p.total || !p.running)
            sane = false;
    }
    CHECK(monotonic, QStringLiteral("进度单调不回退"));
    CHECK(sane, QStringLiteral("进度自洽（总数 4000、已完成在 0..总数 之间）"));
}

//! 进度条的"看得见"部分：开始 / 进展 / 结束三条信号必须成对出现
void checkProgressSignals()
{
    qInfo().noquote() << "== 调度器：信号与快照 ==";

    JobRunner runner;
    int started = 0;
    int finished = 0;
    bool lastCancelled = true;
    JobProgress last;

    QObject::connect(&runner, &JobRunner::started,
                     [&started](const JobProgress &) { ++started; });
    QObject::connect(&runner, &JobRunner::finished,
                     [&finished, &lastCancelled, &last](const JobProgress &p, bool cancelled) {
                         ++finished;
                         lastCancelled = cancelled;
                         last = p;
                     });

    runner.enqueue(new LoopJob(QStringLiteral("小活"), 50, [](int) { }));
    runUntilIdle(&runner);

    CHECK(started == 1 && finished == 1, QStringLiteral("开始 / 结束各一次"));
    CHECK(!lastCancelled, QStringLiteral("正常跑完不算取消"));
    CHECK(last.title == QStringLiteral("小活") && last.done == 50 && last.total == 50,
          QStringLiteral("结束时的快照带任务名与最终进度（%1 %2/%3）")
              .arg(last.title)
              .arg(last.done)
              .arg(last.total));

    // 同一个标签只排一个：滚动一次申请一次"算几何"的场合靠它去重
    int ran = 0;
    auto makeJob = [&ran] {
        auto *job = new LoopJob(QStringLiteral("重复"), 100, [&ran](int) { ++ran; });
        job->setTag(QStringLiteral("同一个标签"));
        return job;
    };
    runner.enqueue(makeJob());
    runner.enqueue(makeJob());
    runner.enqueue(makeJob());
    CHECK(runner.queuedCount() + (runner.busy() ? 1 : 0) == 1,
          QStringLiteral("同标签的重复任务只排了一个"));
    runUntilIdle(&runner);
    CHECK(ran == 100, QStringLiteral("只跑了一遍（实际处理 %1 项）").arg(ran));
}

/*!
 * 取消：停在分片边界、收尾只调一次、检查点收尾（恢复界面状态）。
 */
void checkCancel()
{
    qInfo().noquote() << "== 调度器：取消与收尾 ==";

    JobRunner runner;
    runner.setSliceBudgetMs(5);

    int processed = 0;
    int finishCalls = 0;
    bool finishedCancelled = false;
    auto *job = new LoopJob(QStringLiteral("长活"), 100000, [&processed](int) { ++processed; });
    job->setChunkSize(200);
    job->setFinishHook([&finishCalls, &finishedCancelled](bool cancelled, int) {
        ++finishCalls;
        finishedCancelled = cancelled;
    });

    QObject::connect(&runner, &JobRunner::progressed, [&runner](const JobProgress &p) {
        if (p.done > 4000)
            runner.cancelCurrent();
    });

    runner.enqueue(job);
    runUntilIdle(&runner);

    CHECK(finishedCancelled, QStringLiteral("被取消的任务在 finished 里报 cancelled=true"));
    CHECK(finishCalls == 1, QStringLiteral("收尾钩子只调一次（实际 %1 次）").arg(finishCalls));
    CHECK(processed > 0 && processed < 100000,
          QStringLiteral("取消是停在半路，不是跑完（处理了 %1/100000）").arg(processed));
    CHECK(!runner.busy(), QStringLiteral("取消后调度器空出来了"));
}

//! 排队：一次只跑一个；先来的先跑
void checkQueue()
{
    qInfo().noquote() << "== 调度器：排队（一次只跑一个） ==";

    JobRunner runner;
    int active = 0;
    int maxActive = 0;
    QString order;

    auto makeJob = [&](const QString &name) {
        auto *job = new LoopJob(name, 200, [&active, &maxActive, name, &order](int index) {
            ++active;
            maxActive = qMax(maxActive, active);
            if (index == 0)
                order += name;
            --active;
        });
        job->setChunkSize(50);
        return job;
    };

    runner.enqueue(makeJob(QStringLiteral("A")));
    runner.enqueue(makeJob(QStringLiteral("B")));
    runner.enqueue(makeJob(QStringLiteral("C")));
    runUntilIdle(&runner);

    CHECK(maxActive == 1, QStringLiteral("任何时刻只有一个任务在动（峰值 %1）").arg(maxActive));
    CHECK(order == QStringLiteral("ABC"), QStringLiteral("按排队顺序执行（实际 %1）").arg(order));
}

/*!
 * 调度器被销毁时，没跑完的任务也必须收尾。
 *
 * 这条是给"任务握着界面状态"用的：编辑器临时只读、编辑块还开着 ——
 * 直接 delete 会把它们永远留在原地（下一次打开文档就不能打字了）。
 */
void checkDestructorFinishesJobs()
{
    qInfo().noquote() << "== 调度器：析构时收尾 ==";

    int finishCalls = 0;
    bool cancelled = false;
    {
        JobRunner runner;
        auto *job = new LoopJob(QStringLiteral("没跑完"), 100000, [](int) { });
        job->setChunkSize(100);
        job->setFinishHook([&finishCalls, &cancelled](bool wasCancelled, int) {
            ++finishCalls;
            cancelled = wasCancelled;
        });
        runner.enqueue(job);
        // 队列里那个还没轮到它：析构时也要走收尾
    }
    CHECK(finishCalls == 1 && cancelled,
          QStringLiteral("析构时队列里的任务被收尾并标记为取消（%1 次）").arg(finishCalls));
}

// ---------------------------------------------------------------- 计算层

/*!
 * 显示表**按段落惰性算**：只碰要的那几段，不是全篇。
 */
void checkPlannerLazyPerBlock()
{
    qInfo().noquote() << "== 计算层：按段落惰性算 ==";

    Fixture fx;
    fillWithEffects(&fx.doc, EffectKind::Distortion, 0x1234u);

    EffectPlanner planner(&fx.doc, fx.layout);
    EffectRenderOptions options;
    options.showDistortion = true;
    options.distortionReplaceText = true;
    NoiseWave wave;
    wave.reseed(0x1234u);
    options.wave = &wave;
    planner.setOptions(options);

    planner.beginFrame();
    planner.requestRange(0, 2);

    CHECK(planner.plannedBlockCount() == 3,
          QStringLiteral("只要了 3 段就只规划 3 段（实际 %1，全篇 %2 段）")
              .arg(planner.plannedBlockCount())
              .arg(fx.doc.blockCount()));

    const int items = planner.items(0).size() + planner.items(1).size() + planner.items(2).size();
    CHECK(items > 0, QStringLiteral("要的那几段确实有内容（%1 项）").arg(items));
    CHECK(planner.items(0).isEmpty() == false, QStringLiteral("第 0 段有显示表"));

    bool allUnprepared = true;
    for (int n = 0; n <= 2; ++n) {
        for (const EffectDrawItem &item : planner.items(n)) {
            if (item.geometryDone || item.hidden || item.art.ready)
                allUnprepared = false;
        }
    }
    CHECK(allUnprepared,
          QStringLiteral("读显示表**不算几何**（geometryDone / hidden / art.ready 全为假）"));

    // 没算好的格子不许挖洞：否则屏幕上是"原字没了、新字还没来"
    QImage image(200, 200, QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        renderPreparedEffects(&painter, planner.items(0), options);
    }
    int ink = 0;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *scan = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb c = scan[x];
            if (qRed(c) < 200 || qGreen(c) < 200 || qBlue(c) < 200)
                ++ink;
        }
    }
    CHECK(ink == 0, QStringLiteral("几何没算好时效果层什么都不画（墨点 %1）").arg(ink));
}

/*!
 * 几何分片算：算一批、亮一批，算好的格子才挖洞。
 */
void checkPlannerProgressive()
{
    qInfo().noquote() << "== 计算层：分片算几何 + 算好才挖洞 ==";

    Fixture fx;
    fillWithEffects(&fx.doc, EffectKind::Distortion, 0x5678u);

    EffectPlanner planner(&fx.doc, fx.layout);
    EffectRenderOptions options;
    options.showDistortion = true;
    options.distortionReplaceText = true;
    NoiseWave wave;
    wave.reseed(0x5678u);
    options.wave = &wave;
    planner.setOptions(options);

    int changes = 0;
    int requests = 0;
    QObject::connect(&planner, &EffectPlanner::planChanged, [&changes] { ++changes; });
    QObject::connect(&planner, &EffectPlanner::workPending, [&requests] { ++requests; });

    planner.beginFrame();
    planner.requestRange(0, 0);
    /*!
     * `requestRange` 只记下"想要"：**不当场排队**。
     * 打字的人每敲一个字都会让显示表作废，立刻重算就是白白烧 CPU ——
     * 所以几何要等文档安静下来（150ms）才统一排队（见 requestGeometry）。
     */
    CHECK(requests == 0 && !planner.hasPendingWork(),
          QStringLiteral("刚画完一帧还不排几何（等文档安静）"));

    planner.requestGeometry();
    CHECK(requests == 1, QStringLiteral("申请几何时后台任务被叫了一次"));
    CHECK(planner.hasPendingWork(), QStringLiteral("现在有待算的活了"));

    const int firstBatch = planner.preparedItemCount();
    planner.work(1); // 一次只算一项
    CHECK(planner.preparedItemCount() == firstBatch + 1,
          QStringLiteral("work(1) 只算一项（%1 -> %2）")
              .arg(firstBatch)
              .arg(planner.preparedItemCount()));

    int hiddenAfterOne = 0;
    for (const EffectDrawItem &item : planner.items(0)) {
        if (item.hidden)
            ++hiddenAfterOne;
    }
    CHECK(hiddenAfterOne == 1,
          QStringLiteral("算好一项就只挖一个洞（%1）").arg(hiddenAfterOne));
    CHECK(changes == 1, QStringLiteral("算完一片发一次 planChanged（视图据此重画）"));

    while (planner.hasPendingWork())
        planner.work(64);
    CHECK(planner.preparedItemCount() > firstBatch, QStringLiteral("接着把这一段的几何算完了"));
    CHECK(planner.pendingItemCount() == 0, QStringLiteral("待算数归零"));

    int hiddenAll = 0;
    int readyAll = 0;
    for (const EffectDrawItem &item : planner.items(0)) {
        if (item.hidden)
            ++hiddenAll;
        if (item.art.ready)
            ++readyAll;
    }
    CHECK(hiddenAll == readyAll && readyAll > 0,
          QStringLiteral("挖洞数 == 几何就绪数（%1）").arg(hiddenAll));
}

/*!
 * 失效：正文一改，只有**改到的位置之后**的段落作废；版面一变，整篇作废。
 */
void checkPlannerInvalidation()
{
    qInfo().noquote() << "== 计算层：失效范围 ==";

    Fixture fx;
    fillWithEffects(&fx.doc, EffectKind::Distortion, 0x9ABCu);

    EffectPlanner planner(&fx.doc, fx.layout);
    EffectRenderOptions options;
    options.showDistortion = true;
    NoiseWave wave;
    wave.reseed(0x9ABCu);
    options.wave = &wave;
    planner.setOptions(options);

    planner.beginFrame();
    planner.requestRange(0, 5);
    const int planned = planner.plannedBlockCount();
    CHECK(planned == 6, QStringLiteral("先规划前 6 段（%1）").arg(planned));

    // 在第 3 段插入一个字：第 3 段及之后的显示表作废，前面两段留着
    const int pos = fx.doc.findBlockByNumber(3).position() + 1;
    {
        QTextCursor cursor(&fx.doc);
        cursor.setPosition(pos);
        cursor.insertText(QStringLiteral("字"));
    }
    CHECK(planner.plannedBlockCount() <= 3,
          QStringLiteral("改动位置之后的段落都作废了（还剩 %1 段）").arg(planner.plannedBlockCount()));

    planner.beginFrame();
    planner.requestRange(0, 5);
    CHECK(planner.plannedBlockCount() == 6, QStringLiteral("重新要一遍就重新建好了"));

    // 版面几何变了（改页边距）：格子的位置全变，整篇作废
    PageSetup wider = a4Setup();
    wider.marginLeftMm = 30.0;
    fx.layout->setMetrics(RenderMetrics::fromPageSetup(wider));
    planner.beginFrame();
    planner.requestRange(0, 5);
    const int afterMetrics = planner.plannedBlockCount();
    CHECK(afterMetrics <= 6,
          QStringLiteral("换页边距后重新规划（%1 段）").arg(afterMetrics));
    CHECK(!planner.items(0).isEmpty() && planner.items(0).first().charRect.left()
                                               > fx.layout->metrics().marginLeftPx - 0.5,
          QStringLiteral("新格子跟着新页边距走（left=%1，页边距=%2）")
              .arg(planner.items(0).first().charRect.left(), 0, 'f', 1)
              .arg(fx.layout->metrics().marginLeftPx, 0, 'f', 1));
}

/*!
 * 后台任务把几何算完；几何算完之后画出来必须有墨。
 */
void checkWorkJobDrawsEffects()
{
    qInfo().noquote() << "== 计算层：后台任务算完 -> 画得出来 ==";

    Fixture fx;
    fillWithEffects(&fx.doc, EffectKind::Handwriting, 0x2468u);

    HandwritingLibrary library;
    EffectRenderOptions options;
    options.library = &library;
    options.showHandwriting = true;
    options.handwritingReplaceText = true;

    // 给整篇用到的字都造一份手写数据（一个方形笔画就够量墨了）
    for (int pos = 0; pos < fx.doc.characterCount() - 1; ++pos) {
        const QString ch = fx.doc.characterAt(pos);
        if (ch.trimmed().isEmpty())
            continue;
        library.addCsvText(QStringLiteral("%1\n0,0,0.5\n10,0,0.5\n10,10,0.5\n0,10,0.5\n").arg(ch),
                           QStringLiteral("probe.csv"));
    }

    EffectPlanner planner(&fx.doc, fx.layout);
    planner.setOptions(options);

    JobRunner runner;
    runner.enqueue(new EffectWorkJob(&planner, 16));

    planner.beginFrame();
    planner.requestRange(0, 0);
    planner.requestGeometry();
    runUntilIdle(&runner);

    int ready = 0;
    for (const EffectDrawItem &item : planner.items(0)) {
        if (item.art.ready)
            ++ready;
    }
    CHECK(ready > 0, QStringLiteral("后台任务把第 0 段的几何算好了（%1 项）").arg(ready));
    CHECK(!planner.items(0).isEmpty() && planner.items(0).first().hidden,
          QStringLiteral("算好的格子进入\"挖洞\"状态（原字不画）"));

    // 画一次：整篇的效果层必须有墨
    QImage image(1240, 1754, QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        for (int n = 0; n < fx.doc.blockCount(); ++n)
            renderPreparedEffects(&painter, planner.items(n), options);
    }
    int ink = 0;
    for (int y = 0; y < image.height(); y += 2) {
        const QRgb *scan = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x += 2) {
            const QRgb c = scan[x];
            if (qRed(c) < 200 || qGreen(c) < 200 || qBlue(c) < 200)
                ++ink;
        }
    }
    CHECK(ink > 200, QStringLiteral("画出来的效果层有墨（隔点采样 %1 点）").arg(ink));
}
/*!
 * "等安静"是真的在生效吗：
 * 连续敲字不应该把几何排队（_requestGeometry 不被自动调），
 * 而停下来一会儿就该自动开始算。
 */
void checkSettleDelay()
{
    qInfo().noquote() << "== 计算层：等安静 150ms 再算几何 ==";

    Fixture fx;
    fillWithEffects(&fx.doc, EffectKind::Distortion, 0x1357u);

    EffectPlanner planner(&fx.doc, fx.layout);
    EffectRenderOptions options;
    options.showDistortion = true;
    NoiseWave wave;
    wave.reseed(0x1357u);
    options.wave = &wave;
    planner.setOptions(options);

    int requests = 0;
    QObject::connect(&planner, &EffectPlanner::workPending, [&requests] { ++requests; });

    // 连续改了三次（就像连续敲字）：每次都会让显示表作废，但都不该排队
    for (int i = 0; i < 3; ++i) {
        planner.beginFrame();
        planner.requestRange(0, 2);
        QTextCursor cursor(&fx.doc);
        cursor.setPosition(fx.doc.findBlockByNumber(0).position() + 1);
        cursor.insertText(QStringLiteral("字"));
        QApplication::processEvents();
    }
    CHECK(requests == 0, QStringLiteral("连续改动期间不排几何（实际排了 %1 次）").arg(requests));

    planner.beginFrame();
    planner.requestRange(0, 2);
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 2000 && requests == 0) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    CHECK(requests > 0, QStringLiteral("安静下来之后自动排队（用了 %1ms）").arg(clock.elapsed()));
    if (requests > 0) {
        planner.work(1);
        CHECK(planner.preparedItemCount() > 0, QStringLiteral("排了队就真的能算"));
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    checkJobSlicing();
    checkProgressSignals();
    checkCancel();
    checkQueue();
    checkDestructorFinishesJobs();
    checkPlannerLazyPerBlock();
    checkPlannerProgressive();
    checkPlannerInvalidation();
    checkWorkJobDrawsEffects();
    checkSettleDelay();

    if (g_failures == 0) {
        qInfo().noquote() << "全部通过";
        return 0;
    }
    qWarning().noquote() << QStringLiteral("%1 项失败").arg(g_failures);
    return 1;
}
