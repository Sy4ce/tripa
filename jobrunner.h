#ifndef JOBRUNNER_H
#define JOBRUNNER_H

#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

class QElapsedTimer;

/*!
 * \file jobrunner.h
 * \brief 长活儿的**分片调度**：把"一秒卡死"变成"界面照常动 + 状态栏有进度"。
 *
 * 为什么需要这一层（实测数据，见 tests/bench_effects.cpp）：
 * 给 2050 个字符套上噪声音效，光是"写格式"就要 78ms，而**第一次重绘**要把每个字的
 * 字形轮廓取出来、按噪声变形、再拼成填充路径 —— 585ms；文档再长一点就是好几秒。
 * 这些计算原本都发生在两个"必须立刻返回"的地方：
 *   - 菜单动作里（点一下，窗口白掉，标题栏写"无响应"）；
 *   - `paintEvent` 里（每一次滚动、每一次打字都在重算全篇）。
 *
 * 这个文件解决"谁来算"的问题：把长活儿写成**可以随时停下来的分片任务**（Job），
 * 由一个调度器（JobRunner）在事件循环里一片一片地喂给它。
 * 每片之后主动让出，事件循环就能继续派发绘制和输入 —— 界面不白，
 * 取消按钮点得动，状态栏的进度条动得起来。
 *
 * 三条铁律：
 *   1. **一片要短**：默认 12ms，60fps 下给绘制和输入留足时间；
 *   2. **任务自己记状态**：`step()` 每次从上次停下的地方接着干，不许从头再来；
 *   3. **同一时刻只有一个任务**：`JobRunner` 排队串行执行 —— 两个任务同时改文档
 *      是灾难，而"进度条只有一个"也决定了这一点。
 *
 * 取消是**协作式**的：调度器不会从背后把任务掐断（那会留下改了一半的文档），
 * 它只是停止再喂 `step()`，然后调 `finish(true)` 让任务自己收尾
 * （回滚、关掉编辑块、恢复界面状态）。
 */

//! 任务当前的进度快照（状态栏照着它显示）
struct JobProgress
{
    QString title;         //!< 任务名，例如"套用笔画扭曲"
    QString detail;        //!< 现在在干什么，例如"第 512/2050 个字"
    int done = 0;          //!< 已完成步数
    int total = 0;         //!< 总步数；<= 0 = 未知（进度条走"忙"状态，不显示百分比）
    bool cancellable = true;
    bool running = false;

    //! 完成比例（0..1）；总数未知时返回 0
    double fraction() const { return total > 0 ? double(done) / double(total) : 0.0; }
    //! 只比"看得见的字段"：内容没变就不用往状态栏推一次
    bool sameAs(const JobProgress &other) const;
};

/*!
 * \brief 一个可以分片做的长活儿。
 *
 * 生命周期：`JobRunner::enqueue()` 接管所有权，任务跑完（或被取消）之后
 * 由调度器 `delete`。任务里**不要**再 delete 自己。
 */
class Job
{
public:
    virtual ~Job();

    //! 状态栏上显示的名字
    virtual QString title() const = 0;
    /*!
     * 去重用的标签：同标签的任务已经在队列里时，`JobRunner::enqueue()` 会丢掉新的那个。
     * 典型用途是"把文档里所有效果几何算一遍"——滚动一次就申请一次，
     * 但真正需要排队的只有第一次。
     */
    virtual QString tag() const { return QString(); }
    //! 总步数；<= 0 = 不知道（进度条显示忙）
    virtual int total() const { return 0; }
    virtual int done() const { return 0; }
    //! 现在在干什么（状态栏第二行 / 工具提示）
    virtual QString detail() const { return QString(); }
    virtual bool cancellable() const { return true; }

    /*!
     * \brief 干一小片活。
     * \return true = 还有活没干完；false = 这份活干完了。
     *
     * 这个函数**每次都必须很快返回**（一片 10ms 量级）：它是在事件循环里被调的，
     * 这里耗多久，界面就僵多久。
     */
    virtual bool step() = 0;

    /*!
     * \brief 收尾：不管成功、被取消还是调度器析构，都会调一次（而且只调一次）。
     * \param cancelled 是不是被取消的（false = 正常干完了）
     */
    virtual void finish(bool cancelled) { Q_UNUSED(cancelled); }
};

/*!
 * \brief 把"按下标推进的循环"包成分片任务。
 *
 * 绝大多数长活儿都是这个形状：遍历一批东西（字符、段落、页、文件），每项做一件小事。
 * 为了这点事写一个虚函数族太啰嗦，这里直接用回调。
 *
 * 注意 \a body 里**不要**再套一层大循环 —— 那等于把分片又取消了。
 */
class LoopJob : public Job
{
public:
    using Body = std::function<void(int index)>;

    LoopJob(const QString &title, int count, Body body);

    //! 一片最多处理多少项（默认自动：总数的 1/100，最少 1 项）
    void setChunkSize(int size);
    int chunkSize() const { return m_chunk; }

    //! 进度说明文本（每片问一次，参数是"下一次要处理的下标"）
    void setDetailProvider(std::function<QString(int index)> provider);
    /*!
     * \brief 收尾钩子。
     * \param cancelled 是不是被取消的
     * \param processed 已经处理了多少项
     */
    void setFinishHook(std::function<void(bool cancelled, int processed)> hook);

    QString title() const override { return m_title; }
    QString tag() const override { return m_tag; }
    void setTag(const QString &tag) { m_tag = tag; }
    int total() const override { return m_count; }
    int done() const override { return m_index; }
    QString detail() const override;
    bool cancellable() const override { return m_cancellable; }
    void setCancellable(bool cancellable) { m_cancellable = cancellable; }
    int processed() const { return m_index; }

    bool step() override;
    void finish(bool cancelled) override;

private:
    QString m_title;
    QString m_tag;
    int m_count = 0;
    int m_index = 0;
    int m_chunk = 1;
    bool m_cancellable = true;
    bool m_finished = false;
    Body m_body;
    std::function<QString(int index)> m_detail;
    std::function<void(bool cancelled, int processed)> m_finishHook;
};

/*!
 * \brief 任务调度器：一次只跑一个任务，每片之后把事件循环让出来。
 *
 * 用法：`runner->enqueue(new LoopJob(...))`。同标签的任务已经在跑/在排队时，
 * 新的会被直接删掉（见 `Job::tag()`）—— 滚动一次就申请一次"算几何"的场合，
 * 靠它避免队列里堆几百个一模一样的任务。
 */
class JobRunner : public QObject
{
    Q_OBJECT
public:
    explicit JobRunner(QObject *parent = nullptr);
    ~JobRunner() override;

    //! 排到队尾（接管所有权）
    void enqueue(Job *job);
    //! 插到队首："我正在看的那一页先算"
    void enqueueFront(Job *job);

    //! 队列里（含正在跑的）有没有这个标签的任务
    bool hasJobWithTag(const QString &tag) const;
    bool busy() const { return m_current != nullptr; }
    bool idle() const { return m_current == nullptr && m_queue.isEmpty(); }
    int queuedCount() const { return m_queue.size(); }
    Job *current() const { return m_current; }
    //! 当前任务的进度快照（没有任务时 running = false）
    JobProgress progress() const;

    //! 请求取消当前任务（下一片边界生效，任务自己收尾）
    void cancelCurrent();
    //! 请求取消当前任务，并清空队列
    void cancelAll();

    /*!
     * \brief 一片最多占用多少毫秒（默认 12ms）。
     *
     * 太小（比如 1ms）会让一次计算被切成几千次事件循环往返，反而更慢；
     * 太大（比如 100ms）界面就开始有顿挫感。12ms ≈ 60fps 下留出 4ms 给绘制。
     */
    void setSliceBudgetMs(int ms);
    int sliceBudgetMs() const { return m_budgetMs; }

    /*!
     * \brief 一片最多调多少次 `step()`（默认 256）。
     *
     * 这是**兜底**：万一某个 `step()` 不守时间预算（比如一次处理了太多项），
     * 至少不会让事件循环永远轮不到。
     */
    void setMaxStepsPerSlice(int steps);
    int maxStepsPerSlice() const { return m_maxSteps; }

signals:
    void started(const JobProgress &progress);
    void progressed(const JobProgress &progress);
    //! \a cancelled 为真表示任务是被取消的（进度停在取消的那一刻）
    void finished(const JobProgress &progress, bool cancelled);
    //! 队列空了
    void idled();

private:
    void schedule();
    void runSlice();
    void startNext();
    void settleCurrent(bool cancelled);

    QVector<Job *> m_queue;
    Job *m_current = nullptr;
    bool m_cancelRequested = false;
    bool m_scheduled = false;
    bool m_finishing = false;
    int m_budgetMs = 12;
    int m_maxSteps = 256;
};

#endif // JOBRUNNER_H
