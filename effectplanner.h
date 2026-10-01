#ifndef EFFECTPLANNER_H
#define EFFECTPLANNER_H

#include "effectsrenderer.h"
#include "jobrunner.h"
#include "paginatinglayout.h"

#include <QHash>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QVector>

class QTextDocument;

/*!
 * \file effectplanner.h
 * \brief 效果层的**计算层**：把"每个字符该画成什么样"提前算好，绘制只读结果。
 *
 * 为什么要有这一层（都是实测出来的，见 tests/bench_effects.cpp）：
 *
 *   1. `paintEvent` 原来每帧都把**全篇**重新过一遍（`planEffects`），而且一次绘制
 *      过**两遍**（挖洞一遍、画效果一遍）—— 2000 字的稿子每帧 15ms，20000 字就是
 *      150ms，"滚动一下卡一下"就是从这来的；
 *   2. 更贵的是几何：每个字都要取字形轮廓、按噪声变形、拼填充路径；手写层还要
 *      逐点换算、重采样、平滑。2000 字冷缓存 **585ms**，而这发生在 `paintEvent` 里
 *      —— 点一下"加噪声"，窗口白将近一秒。
 *
 * 这一层把这两件事挪出绘制路径：
 *
 *   - **按段落算、按段落缓存**：一帧只碰"看得见的那几段"，跟稿子多长无关；
 *   - **几何在后台任务里分片算**（`JobRunner` 驱动 `work()`），算好之前那一格
 *     照常画原字，算好了自然就变 —— 屏幕上永远是完整的字，不会先空一片。
 *
 * 分工写死在这里：
 *   - 渲染侧（`TextEditor::paintEvent`）只调 `beginFrame()` / `requestRange()` /
 *     `items()`，**不做任何几何运算**；
 *   - 计算侧（`EffectWorkJob`）只调 `work()`；
 *   - 两边唯一的交接物是 `EffectDrawItem` 里的 `art`（见 effectsrenderer.h）。
 */
class EffectPlanner : public QObject
{
    Q_OBJECT
public:
    /*!
     * \param document 富文本层的文档（效果数据在字符格式里）
     * \param layout   渲染层的分页排版（单元格子在哪、行在哪，全靠它）
     */
    EffectPlanner(QTextDocument *document, PaginatingLayout *layout, QObject *parent = nullptr);

    QTextDocument *document() const { return m_document; }
    PaginatingLayout *layout() const { return m_layout; }

    /*!
     * \brief 绘制选项。
     *
     * 只有**影响几何或"要不要挖洞"**的那些字段变了才整篇失效
     * （显示哪层、是否替换、幅度、波数、噪声波、手写库）；
     * 颜色、笔宽这类"只在落笔时用"的字段改了**不重算** ——
     * 拖一下颜色滑块就把几千个字的几何全扔掉，谁也不愿意。
     */
    void setOptions(const EffectRenderOptions &options);
    const EffectRenderOptions &options() const { return m_options; }

    //! 文档 / 版面变了：显示表和待算队列全部作废
    void invalidateAll();
    //! 只有 \a position 之后的内容变了（插入删除只影响它后面的段落）
    void invalidateFrom(int position);

    // ---------------------------------------------------------------- 渲染侧

    /*!
     * \brief 开始新的一帧（渲染侧每帧调一次）。
     *
     * 作用是给"正在用的段落"盖一个帧号：缓存满了要淘汰时，只能淘汰
     * **不是这一帧**用过的段落 —— 否则绘制手里那个引用就悬空了。
     */
    void beginFrame();
    /*!
     * \brief 要屏幕上那几段的效果（渲染侧每帧调一次，幂等）。
     *
     * 段落还没有显示表的当场算一张（很便宜：只读格式和格子位置），
     * 然后把这些段落记成"想要几何"。**几何不在这里算**，也不一定马上排队 ——
     * 见 `requestGeometry()` 里的"等安静"。
     */
    void requestRange(int firstBlock, int lastBlock);

    /*!
     * \brief 把"想要几何"的那些段落排进待算队列（并通知调用方去开任务）。
     *
     * 什么时候调很讲究：正在打字的人每敲一个字都会让显示表作废，
     * 如果每次都马上重算几何，那就是"一边打字一边烧 CPU"（实测过：
     * 一千多字的稿子会一直重算，一秒几十毫秒白花）。
     * 所以 `requestRange` 只是把段落记下来，等文档**安静** 150ms 之后
     * 由这里的定时器统一排队。
     */
    void requestGeometry();

    /*!
     * \brief 第 \a blockNumber 段的显示表。
     *
     * 返回的引用在 **下一次 `invalidate*()`** 之前有效；本帧申请过的段落
     * 不会在 `work()` 里被淘汰。没算过 / 没有效果时返回空表。
     */
    const QVector<EffectDrawItem> &items(int blockNumber);

    // ---------------------------------------------------------------- 计算侧

    //! 还有多少项等着算几何（状态栏进度用）
    int pendingItemCount() const { return m_pendingItems; }
    //! 累计算好了多少项（同一代内单调递增）
    int preparedItemCount() const { return m_preparedItems; }
    //! 有活等着算吗（渲染侧据此申请后台任务）
    bool hasPendingWork() const { return !m_queue.isEmpty(); }
    //! 显示表已经建好的段落数（诊断用）
    int plannedBlockCount() const;

    /*!
     * \brief 干一小片：最多算 \a maxItems 项的几何。
     * \return true = 还有活（调用方接着喂）
     *
     * 一次只算几项是有意的：一项平均 0.27ms，24 项大约 6.5ms ——
     * 加上调度器 12ms 的时间预算，界面每片之间都还能绘制。
     */
    bool work(int maxItems);

    //! 把整篇都算完（**同步**，给打印 / 导出 / 自检用；交互式别调）
    void prepareAll();

signals:
    //! 显示表变了（有新的格子算好了）：视图要重画
    void planChanged();
    //! 有待算的活：调用方把它交给 JobRunner
    void workPending();

private:
    //! 一个段落的显示表 + 它的状态
    struct BlockPlan
    {
        QVector<EffectDrawItem> items;
        bool planned = false;
        bool queued = false;      //!< 已经排在待算队列里
        quint64 lastUsedFrame = 0;
    };

    BlockPlan &planOf(int blockNumber);
    bool needsGeometry(const BlockPlan &plan) const;
    void enqueueGeometry(int blockNumber, bool urgent);
    void dropFromQueue(int blockNumber);
    //! 缓存满了：淘汰最久没用过、且不是本帧用过的段落
    void trimCache();
    //! 版面几何变了（纸张/页边距）：整篇失效
    void ensureMetricsUpToDate();

    QTextDocument *m_document = nullptr;
    PaginatingLayout *m_layout = nullptr;

    EffectRenderOptions m_options;
    //! 只包含"影响几何/隐藏"的字段指纹（见 setOptions 的说明）
    quint64 m_geometryKey = 0;
    RenderMetrics m_metrics;

    //! 段落号 -> 显示表（堆上分配：`items()` 返回的引用必须稳定）
    QHash<int, BlockPlan *> m_blocks;
    //! 想要几何、还在"等安静"的段落
    QSet<int> m_wanted;
    //! 安静下来的定时器（见 requestGeometry 的说明）
    QTimer *m_settleTimer = nullptr;
    //! 创建顺序（缓存淘汰时从头找最久没用过的）
    QVector<int> m_createOrder;
    QVector<int> m_queue;              //!< 待算几何的段落（队首最优先）
    QSet<int> m_queued;
    int m_pendingItems = 0;
    int m_preparedItems = 0;
    quint64 m_frame = 0;
};

/*!
 * \brief 把"算效果几何"这件事交给调度器分片做（见 jobrunner.h）。
 *
 * 这个任务自己不含逻辑：它只是按片喊 `EffectPlanner::work()`，
 * 把进度报给状态栏。真正的优先级（先算看得见的）在 planner 的队列里。
 */
class EffectWorkJob : public Job
{
public:
    explicit EffectWorkJob(EffectPlanner *planner, int itemsPerStep = 24);

    QString title() const override;
    QString tag() const override;
    int total() const override;
    int done() const override;
    QString detail() const override;
    bool step() override;

private:
    QPointer<EffectPlanner> m_planner;
    int m_itemsPerStep = 24;
    int m_doneAtStart = 0;
    int m_totalAtStart = 0;
};

#endif // EFFECTPLANNER_H
