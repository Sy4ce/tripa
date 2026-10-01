#include "jobrunner.h"

#include "tripalog.h"

#include <QElapsedTimer>
#include <QTimer>

#include <algorithm>

bool JobProgress::sameAs(const JobProgress &other) const
{
    return done == other.done && total == other.total && title == other.title
           && detail == other.detail && running == other.running
           && cancellable == other.cancellable;
}

Job::~Job() = default;

LoopJob::LoopJob(const QString &title, int count, Body body)
    : m_title(title)
    , m_count(qMax(0, count))
    , m_body(std::move(body))
{
    /*!
     * 分片粒度默认按总数自适应：进度条要动得起来（至少 100 格），
     * 但也不能细到"每处理一个字就跳一次事件循环"。
     */
    m_chunk = qBound(1, m_count / 100, 4096);
}

void LoopJob::setChunkSize(int size)
{
    m_chunk = qMax(1, size);
}

void LoopJob::setDetailProvider(std::function<QString(int index)> provider)
{
    m_detail = std::move(provider);
}

void LoopJob::setFinishHook(std::function<void(bool cancelled, int processed)> hook)
{
    m_finishHook = std::move(hook);
}

QString LoopJob::detail() const
{
    return m_detail ? m_detail(m_index) : QString();
}

bool LoopJob::step()
{
    if (m_index >= m_count)
        return false;

    const int end = qMin(m_count, m_index + m_chunk);
    for (; m_index < end; ++m_index) {
        if (m_body)
            m_body(m_index);
    }
    return m_index < m_count;
}

void LoopJob::finish(bool cancelled)
{
    // 收尾只做一次：调度器析构和正常结束两条路都可能走到这里
    if (m_finished)
        return;
    m_finished = true;
    if (m_finishHook)
        m_finishHook(cancelled, m_index);
}

JobRunner::JobRunner(QObject *parent)
    : QObject(parent)
{
}

JobRunner::~JobRunner()
{
    /*!
     * 析构时把没跑完的任务也**收尾**掉：
     * 任务多半握着"编辑器暂时只读""编辑块还开着"这类状态，
     * 直接 delete 会把它们永远留在原地（下一次打开文档就不能打字了）。
     */
    m_cancelRequested = true;
    if (m_current) {
        Job *job = m_current;
        m_current = nullptr;
        job->finish(true);
        delete job;
    }
    for (Job *job : std::as_const(m_queue)) {
        job->finish(true);
        delete job;
    }
    m_queue.clear();
}

void JobRunner::enqueue(Job *job)
{
    if (!job)
        return;
    // 同标签的任务已经在跑或在排队：这一个没必要再排（滚动一次申请一次"算几何"就是这种）
    const QString tag = job->tag();
    if (!tag.isEmpty() && hasJobWithTag(tag)) {
        TRIPA_DEBUG("jobs", QStringLiteral("任务 [%1] 与队列里的同标签任务重复，丢弃").arg(tag));
        delete job;
        return;
    }
    m_queue.append(job);
    schedule();
}

void JobRunner::enqueueFront(Job *job)
{
    if (!job)
        return;
    const QString tag = job->tag();
    if (!tag.isEmpty() && hasJobWithTag(tag)) {
        delete job;
        return;
    }
    m_queue.prepend(job);
    schedule();
}

bool JobRunner::hasJobWithTag(const QString &tag) const
{
    if (tag.isEmpty())
        return false;
    if (m_current && m_current->tag() == tag)
        return true;
    for (const Job *job : m_queue) {
        if (job->tag() == tag)
            return true;
    }
    return false;
}

JobProgress JobRunner::progress() const
{
    JobProgress p;
    if (!m_current)
        return p;
    p.title = m_current->title();
    p.detail = m_current->detail();
    p.done = m_current->done();
    p.total = m_current->total();
    p.cancellable = m_current->cancellable();
    p.running = true;
    return p;
}

void JobRunner::cancelCurrent()
{
    if (!m_current)
        return;
    TRIPA_DEBUG("jobs", QStringLiteral("请求取消任务：[%1]").arg(m_current->title()));
    m_cancelRequested = true;
}

void JobRunner::cancelAll()
{
    m_cancelRequested = true;
    // 队列里还没开跑的：收尾 + 销毁，它们的 finish(true) 也要走到
    const QVector<Job *> queued = m_queue;
    m_queue.clear();
    for (Job *job : queued) {
        job->finish(true);
        delete job;
    }
    if (!m_current && !queued.isEmpty())
        emit idled();
}

void JobRunner::setSliceBudgetMs(int ms)
{
    m_budgetMs = qBound(1, ms, 1000);
}

void JobRunner::setMaxStepsPerSlice(int steps)
{
    m_maxSteps = qBound(1, steps, 100000);
}

void JobRunner::schedule()
{
    if (m_scheduled || m_finishing)
        return;
    m_scheduled = true;
    /*!
     * 排到事件循环的下一轮，**不是**直接调用 `runSlice()`。
     *
     * 这一行就是"界面不卡"的全部秘密：一次 `step()` 之后立刻让出，
     * Qt 才有机会把积压的绘制、输入、定时器事件派发出去。
     * 直接同步跑完整个任务，等于什么都没改。
     */
    QTimer::singleShot(0, this, &JobRunner::runSlice);
}

void JobRunner::startNext()
{
    if (m_current || m_queue.isEmpty())
        return;
    m_current = m_queue.takeFirst();
    m_cancelRequested = false;
    emit started(progress());
    schedule();
}

void JobRunner::runSlice()
{
    m_scheduled = false;
    if (m_finishing)
        return;

    if (!m_current) {
        startNext();
        if (!m_current) {
            emit idled();
            return;
        }
    }

    if (m_cancelRequested) {
        settleCurrent(true);
        return;
    }

    // 一片：最多跑 budget 毫秒、最多 maxSteps 次 step()
    QElapsedTimer clock;
    clock.start();
    bool more = false;
    int steps = 0;
    for (;;) {
        more = m_current->step();
        ++steps;
        if (m_cancelRequested || !more)
            break;
        if (steps >= m_maxSteps || clock.elapsed() >= m_budgetMs)
            break;
    }

    /*!
     * 一片花超了就要报出来。
     *
     * 分片调度的全部价值就在"每片短"上：某个任务一次干太多（比如一片里改了几千
     * 个字符），从外面看就是"界面又卡了一下"——而任务自己不会知道。
     * 这条 WARN 和 `updateScrollRange()` 里那个活锁警告是同一个思路：
     * 把"卡"变成日志里一眼能看见的数。
     */
    if (const qint64 spent = clock.elapsed(); spent > qMax<qint64>(50, qint64(m_budgetMs) * 4))
        TRIPA_WARN("jobs", QStringLiteral("任务「%1」一片花了 %2ms（预算 %3ms，%4 次 step）")
                              .arg(m_current->title())
                              .arg(spent)
                              .arg(m_budgetMs)
                              .arg(steps));

    if (m_cancelRequested || !more) {
        settleCurrent(m_cancelRequested);
        return;
    }

    emit progressed(progress());
    schedule();
}

void JobRunner::settleCurrent(bool cancelled)
{
    Job *job = m_current;
    m_current = nullptr;
    m_cancelRequested = false;
    if (!job) {
        schedule();
        return;
    }

    /*!
     * `finish()` 里任务会恢复界面状态（取消只读、关掉编辑块、把选项写回去），
     * 这些动作可能**又**触发一轮排版 —— 所以这里先把 `m_finishing` 竖起来，
     * 期间进来的 enqueue/schedule 排到收尾之后再说，免得重入。
     */
    m_finishing = true;
    const JobProgress snapshot = [&] {
        JobProgress p;
        p.title = job->title();
        p.detail = job->detail();
        p.done = job->done();
        p.total = job->total();
        p.cancellable = job->cancellable();
        p.running = true;
        return p;
    }();
    job->finish(cancelled);
    delete job;
    m_finishing = false;

    emit finished(snapshot, cancelled);

    if (!m_queue.isEmpty())
        schedule();
    else
        emit idled();
}
