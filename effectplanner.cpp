#include "effectplanner.h"

#include "noise.h"
#include "tripalog.h"

#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>

#include <algorithm>

namespace {

/*!
 * 显示表缓存最多留几段。
 *
 * 一段话大约几十个字，带上算好的几何（填充路径 / 手写折线）也就几百 KB ——
 * 96 段够铺满好几屏，滚回去不用重算；再往上内存就开始白白涨了
 * （几何的大头本来另有一份全局缓存，见 effectsrenderer.cpp 里的 distortCache）。
 */
constexpr int kMaxCachedBlocks = 96;

//! 噪声波的指纹：波一变，手写笔迹的抖动就不一样了，几何必须重算
quint64 waveHash(const NoiseWave *wave)
{
    if (!wave || !wave->isValid())
        return 0;
    quint64 h = 1469598103934665603ull;
    for (const NoiseWave::Term &t : wave->terms()) {
        const quint64 parts[3] = {quint64(qRound64(t.amplitude * 1e6)),
                                  quint64(qRound64(t.frequency * 1e6)),
                                  quint64(qRound64(t.phase * 1e6))};
        for (quint64 v : parts) {
            h ^= v;
            h *= 1099511628211ull;
        }
    }
    return h;
}

//! 把影响几何 / 挖洞的字段揉成一个指纹（颜色、笔宽不在里面，见 setOptions）
quint64 geometryKeyOf(const EffectRenderOptions &o)
{
    quint64 h = 1469598103934665603ull;
    auto mix = [&h](quint64 v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    mix(o.showHandwriting ? 1u : 0u);
    mix(o.showDistortion ? 2u : 0u);
    mix(o.handwritingReplaceText ? 3u : 0u);
    mix(o.distortionReplaceText ? 4u : 0u);
    mix(quint64(reinterpret_cast<quintptr>(o.library)));
    mix(quint64(qRound64(o.amplitudePt * 1e6)));
    mix(quint64(qRound64(o.waveScale * 1e6)));
    mix(waveHash(o.wave));
    return h;
}

//! 这一段里还有多少项没算几何
int unpreparedCount(const QVector<EffectDrawItem> &items)
{
    int n = 0;
    for (const EffectDrawItem &item : items) {
        if (!item.geometryDone)
            ++n;
    }
    return n;
}

} // namespace

EffectPlanner::EffectPlanner(QTextDocument *document, PaginatingLayout *layout, QObject *parent)
    : QObject(parent)
    , m_document(document)
    , m_layout(layout)
{
    m_geometryKey = geometryKeyOf(m_options);
    if (m_layout)
        m_metrics = m_layout->metrics();

    /*!
     * "等安静"的定时器：`requestRange` 只记下想看哪几段，
     * 真正把几何排进队列是这里 —— 打字、拖选、连续滚动都不会让它一直重算。
     */
    m_settleTimer = new QTimer(this);
    m_settleTimer->setSingleShot(true);
    m_settleTimer->setInterval(150);
    connect(m_settleTimer, &QTimer::timeout, this, &EffectPlanner::requestGeometry);

    /*!
     * 正文一变，哪一段的显示表就作废 —— 但**只作废受影响的那些段**。
     *
     * `contentsChange` 给的 `position` 是精确的：插入删除只影响这一段和它**后面**
     * 的段落（前面的段落一个字没动、位置也没动）。所以是 `invalidateFrom(position)`，
     * 不是"整篇重来" —— 否则打一个字就得把看得见的那几屏重新规划一遍。
     *
     * 版面几何（纸张、页边距）不走这条路，由 `ensureMetricsUpToDate()` 兜底。
     */
    if (m_document) {
        connect(m_document, &QTextDocument::contentsChange, this,
                [this](int position, int charsRemoved, int charsAdded) {
                    Q_UNUSED(charsRemoved);
                    Q_UNUSED(charsAdded);
                    invalidateFrom(position);
                });
    }
}

void EffectPlanner::setOptions(const EffectRenderOptions &options)
{
    /*!
     * 选项分两类，只有第一类会让人重新算：
     *   - **影响几何**：显示哪一层、是否替换、噪声幅度、波数、噪声波、手写库 ——
     *     变了就是"画的不是同一个东西了"，缓存必须扔；
     *   - **只在落笔时用**：颜色、笔宽（pressureToWidth）。
     *     这两个拖滑块时每动一下就重算几千个字的几何是不可接受的。
     */
    const quint64 key = geometryKeyOf(options);
    m_options = options;
    if (key != m_geometryKey) {
        m_geometryKey = key;
        invalidateAll();
    }
}

void EffectPlanner::invalidateAll()
{
    for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it)
        delete it.value();
    m_blocks.clear();
    m_createOrder.clear();
    m_queue.clear();
    m_queued.clear();
    m_wanted.clear();
    m_settleTimer->stop();
    m_pendingItems = 0;
    TRIPA_DEBUG("effects", QStringLiteral("效果显示表整篇作废（段落缓存已清空）"));
    emit planChanged();
}

void EffectPlanner::invalidateFrom(int position)
{
    if (m_blocks.isEmpty())
        return;

    int firstBlock = 0;
    if (m_document)
        firstBlock = m_document->findBlock(qMax(0, position)).blockNumber();

    QVector<int> doomed;
    for (auto it = m_blocks.constBegin(); it != m_blocks.constEnd(); ++it) {
        if (it.key() >= firstBlock)
            doomed.append(it.key());
    }
    for (int n : doomed) {
        BlockPlan *plan = m_blocks.take(n);
        if (plan->queued)
            m_pendingItems -= unpreparedCount(plan->items);
        m_queued.remove(n);
        delete plan;
    }
    if (!doomed.isEmpty()) {
        // 队列里可能有已经被删掉的段落（清掉，免得 work() 每片都白跑一次）
        m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                     [this](int n) { return !m_blocks.contains(n); }),
                      m_queue.end());
        emit planChanged();
    }
}

void EffectPlanner::beginFrame()
{
    ++m_frame;
    /*!
     * 帧号是"引用保护"：绘制手里握着 `items()` 返回的引用，
     * 缓存淘汰**只能动不是这一帧用过的段落**（见 trimCache）。
     */
}

void EffectPlanner::requestRange(int firstBlock, int lastBlock)
{
    if (!m_document || !m_options.anyLayer())
        return;
    ensureMetricsUpToDate();
    if (!m_layout)
        return;
    if (firstBlock > lastBlock)
        std::swap(firstBlock, lastBlock);

    bool wanted = false;
    for (int n = qMax(0, firstBlock); n <= lastBlock; ++n) {
        BlockPlan &plan = planOf(n);
        plan.lastUsedFrame = m_frame;
        if (plan.queued || !needsGeometry(plan))
            continue;
        if (!m_wanted.contains(n)) {
            m_wanted.insert(n);
            wanted = true;
        }
    }
    /*!
     * 只记下"想要"，不马上算 —— 等文档安静下来再统一排队（见头文件里的说明）。
     */
    if (wanted)
        m_settleTimer->start();
}

void EffectPlanner::requestGeometry()
{
    m_settleTimer->stop();
    if (m_wanted.isEmpty())
        return;

    /*!
     * "我正看着的这一页先算"：插到队首，而且**保持段落顺序**
     * （倒着 prepend 就是正着插进去了）。
     */
    QVector<int> urgent;
    urgent.reserve(m_wanted.size());
    for (int n : std::as_const(m_wanted))
        urgent.append(n);
    m_wanted.clear();
    std::sort(urgent.begin(), urgent.end());
    for (int i = urgent.size() - 1; i >= 0; --i)
        enqueueGeometry(urgent.at(i), true);

    if (hasPendingWork())
        emit workPending();
}

const QVector<EffectDrawItem> &EffectPlanner::items(int blockNumber)
{
    static const QVector<EffectDrawItem> empty;
    if (!m_document)
        return empty;
    BlockPlan &plan = planOf(blockNumber);
    plan.lastUsedFrame = m_frame;
    return plan.items;
}

int EffectPlanner::plannedBlockCount() const
{
    int n = 0;
    for (auto it = m_blocks.constBegin(); it != m_blocks.constEnd(); ++it) {
        if (it.value()->planned)
            ++n;
    }
    return n;
}

bool EffectPlanner::work(int maxItems)
{
    if (m_queue.isEmpty())
        return false;
    ensureMetricsUpToDate();
    trimCache();

    const int budget = qMax(1, maxItems);
    int done = 0;
    bool changed = false;

    while (!m_queue.isEmpty() && done < budget) {
        const int number = m_queue.first();
        BlockPlan *plan = m_blocks.value(number, nullptr);
        if (!plan) {
            m_queue.removeFirst();
            m_queued.remove(number);
            continue;
        }

        bool allDone = true;
        for (EffectDrawItem &item : plan->items) {
            if (item.geometryDone)
                continue;
            if (done >= budget) {
                allDone = false;
                break;
            }
            prepareEffectItem(&item, m_options);
            ++done;
            ++m_preparedItems;
            --m_pendingItems;
            changed = true;
        }

        if (allDone) {
            m_queue.removeFirst();
            m_queued.remove(number);
            plan->queued = false;
        }
    }

    if (changed)
        emit planChanged();
    return !m_queue.isEmpty();
}

void EffectPlanner::prepareAll()
{
    if (!m_document || !m_options.anyLayer() || !m_layout)
        return;
    ensureMetricsUpToDate();
    const int blocks = m_document->blockCount();
    for (int n = 0; n < blocks; ++n) {
        BlockPlan &plan = planOf(n);
        if (needsGeometry(plan) && !plan.queued)
            enqueueGeometry(n, false);
    }
    while (work(256)) {
        // 打印 / 导出是模态的：一次算到底（这里卡一下没关系，怕的是每帧重算）
    }
}

EffectPlanner::BlockPlan &EffectPlanner::planOf(int blockNumber)
{
    auto it = m_blocks.find(blockNumber);
    if (it != m_blocks.end())
        return **it;

    auto *plan = new BlockPlan;
    plan->lastUsedFrame = m_frame;
    if (m_document && m_options.anyLayer()) {
        // 分页/断行得先算出来：格子位置就是从渲染层问的
        if (m_layout)
            (void)m_layout->pageCount();
        const QTextBlock block = m_document->findBlockByNumber(blockNumber);
        if (block.isValid()) {
            plan->items = planEffectsForBlock(m_document, block, m_options, nullptr);
            plan->planned = true;
        }
    }
    m_blocks.insert(blockNumber, plan);
    m_createOrder.append(blockNumber);
    return *plan;
}

bool EffectPlanner::needsGeometry(const BlockPlan &plan) const
{
    return plan.planned && unpreparedCount(plan.items) > 0;
}

void EffectPlanner::enqueueGeometry(int blockNumber, bool urgent)
{
    BlockPlan *plan = m_blocks.value(blockNumber, nullptr);
    if (!plan || plan->queued || !needsGeometry(*plan))
        return;

    plan->queued = true;
    m_pendingItems += unpreparedCount(plan->items);
    m_queued.insert(blockNumber);
    if (urgent)
        m_queue.prepend(blockNumber);
    else
        m_queue.append(blockNumber);
}

void EffectPlanner::dropFromQueue(int blockNumber)
{
    BlockPlan *plan = m_blocks.value(blockNumber, nullptr);
    if (plan && plan->queued) {
        m_pendingItems -= unpreparedCount(plan->items);
        plan->queued = false;
    }
    m_queued.remove(blockNumber);
    m_queue.removeAll(blockNumber);
}

void EffectPlanner::trimCache()
{
    while (m_blocks.size() > kMaxCachedBlocks) {
        bool removed = false;
        for (int i = 0; i < m_createOrder.size(); ++i) {
            const int number = m_createOrder.at(i);
            BlockPlan *plan = m_blocks.value(number, nullptr);
            if (!plan) {
                m_createOrder.removeAt(i);
                --i;
                continue;
            }
            // 这一帧正在用的段落一根汗毛都不能动：绘制手里就是它的引用
            if (plan->lastUsedFrame == m_frame)
                continue;
            m_createOrder.removeAt(i);
            if (plan->queued)
                m_pendingItems -= unpreparedCount(plan->items);
            m_queued.remove(number);
            m_queue.removeAll(number);
            m_blocks.remove(number);
            delete plan;
            removed = true;
            break;
        }
        // 全是本帧在用的：宁可多占一点内存，也不能让引用悬空
        if (!removed)
            break;
    }
}

void EffectPlanner::ensureMetricsUpToDate()
{
    if (!m_layout)
        return;
    const RenderMetrics current = m_layout->metrics();
    if (current == m_metrics)
        return;
    /*!
     * 纸张 / 页边距变了：每个格子的位置都变了，显示表整篇作废。
     * 这条检查放在这里而不是靠信号：版面几何进渲染层有好几个入口
     * （页面设置、窗口尺寸、打印借用版面……），漏一个就是"效果画错位置"，
     * 而比较两个 RenderMetrics 便宜到可以每帧做。
     */
    m_metrics = current;
    invalidateAll();
}

// ---------------------------------------------------------------- 后台任务

EffectWorkJob::EffectWorkJob(EffectPlanner *planner, int itemsPerStep)
    : m_planner(planner)
    , m_itemsPerStep(qMax(1, itemsPerStep))
{
    if (planner) {
        m_doneAtStart = planner->preparedItemCount();
        m_totalAtStart = m_doneAtStart + planner->pendingItemCount();
    }
}

QString EffectWorkJob::title() const
{
    return QObject::tr("计算字形几何");
}

QString EffectWorkJob::tag() const
{
    // 同标签的任务只排一个：滚动一次申请一次，队列里不该堆出一串
    return QStringLiteral("tripa.effect-geometry");
}

int EffectWorkJob::total() const
{
    EffectPlanner *planner = m_planner.data();
    if (!planner)
        return m_totalAtStart;
    return planner->preparedItemCount() + planner->pendingItemCount();
}

int EffectWorkJob::done() const
{
    EffectPlanner *planner = m_planner.data();
    return planner ? planner->preparedItemCount() : m_doneAtStart;
}

QString EffectWorkJob::detail() const
{
    EffectPlanner *planner = m_planner.data();
    if (!planner)
        return QString();
    return QObject::tr("还剩 %1 个字符").arg(planner->pendingItemCount());
}

bool EffectWorkJob::step()
{
    EffectPlanner *planner = m_planner.data();
    if (!planner)
        return false;
    return planner->work(m_itemsPerStep);
}
