#ifndef TRIPALOG_H
#define TRIPALOG_H

#include <QString>

/*!
 * \brief tripa 自己的日志（排查现场用）。
 *
 * 目的只有一个：**用户那边出问题时，能把现场留下来。**
 * 所以这套东西是围绕"事后看日志"设计的，不是围绕"打印调试信息"：
 *
 *   - 落盘（`%LOCALAPPDATA%/tripa/tripa.log`，必要时自定义路径），
 *     崩溃之后文件还在；单个文件超过上限自动轮转到 `.1`，不会涨到几个 G；
 *   - **崩溃时写一份 dump**（`tripa-crash-<时间>.txt`）：异常码、地址、
 *     模块基址、线程 id、以及崩溃前最后几百条日志 + 面包屑（"最后在做什么"）；
 *   - 进程活着的时候用**环形缓冲**记住最近的日志行，所以 dump 里一定有上下文；
 *   - Qt 自己的 `qDebug/qWarning/qCritical` 也进同一个文件（那些警告经常
 *     直接指出问题，比如 QPainter 的 "Painter not active"）；
 *   - 级别、文件路径都能用命令行或环境变量改：现场没法重编译，
 *     所以"让用户加个参数再跑一次"必须是可行的。
 *
 * 用法（放在 main() 最前面）：
 * \code
 *   tripalog::Options options;
 *   tripalog::parseCommandLine(argc, argv, &options);
 *   tripalog::start(options);
 *   ...
 *   tripalog::shutdown();
 * \endcode
 *
 * 打印一律用宏（会自动带上函数名和行号）：
 * \code
 *   TRIPA_INFO(QStringLiteral("layout"), QStringLiteral("页高=%1").arg(h));
 * \endcode
 */
namespace tripalog {

/*!
 * 级别。数字越大越啰嗦：
 *   Error   —— 出错了，但程序还活着
 *   Warning —— 可疑但不一定是错
 *   Info    —— 用户看得懂的动作（打开/保存/改页面设置/缩放）
 *   Debug   —— 排查缺陷用的细节（分页、几何、重排）
 *   Trace   —— 每次绘制这种高频事件（只在让人专门开的时候用）
 */
enum class Level { Error = 0, Warning = 1, Info = 2, Debug = 3, Trace = 4 };

struct Options
{
    //! 日志文件路径；空 = 默认位置（%LOCALAPPDATA%/tripa/tripa.log）；"-" = 不落盘
    QString filePath;
    //! 记录到这个级别（含）
    Level level = Level::Info;
    //! 单个日志文件的字节上限，超过就轮转到 `<文件>.1`（0 = 不轮转）
    qint64 maxFileBytes = 4 * 1024 * 1024;
    //! 崩溃时是否写 dump（默认写）
    bool crashDump = true;
    //! 把日志同时打到 stderr（命令行下的自检用得上）
    bool toStderr = false;

    //! 钳过界的上限：太小的值会让日志一两秒就轮转一次，没意义
    qint64 maxBytes() const
    {
        return maxFileBytes < 64 * 1024 ? qint64(4 * 1024 * 1024) : maxFileBytes;
    }
};

/*!
 * 从命令行/环境变量里读日志相关的开关（**不依赖 Qt**，可以在 QApplication 之前调）。
 *
 *   --log            打开日志（Info 级）
 *   --log=debug      指定级别（error/warning/info/debug/trace）
 *   --log-file=<路径> 指定日志文件
 *   --no-log         关掉日志
 *   --log-stderr     同时打到 stderr
 *
 * 环境变量（优先级低于命令行）：
 *   TRIPA_LOG=1|debug|trace...   TRIPA_LOG_FILE=<路径>
 */
void parseCommandLine(int argc, char *const argv[], Options *options);

//! 启动日志：开文件、装 Qt 消息处理器、装崩溃处理器
bool start(const Options &options);
//! 收尾：写"正常退出"、卸载处理器、关文件
void shutdown();
//! 日志系统起来了没
bool isRunning();
//! 当前日志文件路径（没起来时是空串）
QString logFilePath();
//! 崩溃 dump 的目录（没起来时是空串）
QString dumpDirectory();

//! 直接写一条（宏最终走这里）。\a function / \a line 由宏填。
void write(Level level, const char *category, const QString &message, const char *function,
           int line);

/*!
 * \brief 面包屑：记"此刻正在干什么"。
 *
 * 崩溃 dump 里只留**最后一条**面包屑 —— 每一条都是一句"接下来要做的危险动作"，
 * 崩在哪儿一目了然，而且不用往日志文件里灌几百行。
 * 用法：在危险动作**之前**调一次。
 */
void breadcrumb(const QString &what);
//! 当前面包屑（诊断用）
QString currentBreadcrumb();

/*!
 * \brief "文档快照"：崩溃 dump 里带上它，就能知道崩的时候在编辑什么。
 *
 * 只留前若干字符（默认 500），自己别把整篇文档塞进来。
 */
void setDocumentSnapshot(const QString &text);
QString documentSnapshot();

//! 日志/面包屑里用的当前时间（本地时间，毫秒精度）
QString nowStamp();

/*!
 * \brief "求救报告"：把日志尾部 + 面包屑 + 环境信息拼成一段**可以直接粘贴**的文本。
 *
 * 现场没法调试，只能靠"用户把这段贴回来"。所以内容要选得刚好：
 * 程序版本 / Qt 版本 / 系统 / 日志路径（含 dump 目录）+ 最后 100 条日志 + 面包屑。
 * 用法（见「帮助 → 复制诊断信息」）。
 */
QString helpReport(int tailLines = 100);

/*!
 * \brief 作用域计时/进出记录（`TRIPA_SCOPE` 用）。
 *
 * 只在 Debug/Trace 级下写日志，所以常驻代码里也可以放。
 */
class Scope
{
public:
    Scope(Level level, const char *category, const char *function, const QString &what);
    ~Scope();

private:
    Level m_level;
    const char *m_category;
    const char *m_function;
    QString m_what;
};

} // namespace tripalog

//! 记一条日志（带上函数名/行号）
#define TRIPA_LOG_AT(level, category, message)                                                   \
    ::tripalog::write((level), (category), (message), __func__, __LINE__)

#define TRIPA_ERROR(category, message) TRIPA_LOG_AT(::tripalog::Level::Error, category, message)
#define TRIPA_WARN(category, message) TRIPA_LOG_AT(::tripalog::Level::Warning, category, message)
#define TRIPA_INFO(category, message) TRIPA_LOG_AT(::tripalog::Level::Info, category, message)
#define TRIPA_DEBUG(category, message) TRIPA_LOG_AT(::tripalog::Level::Debug, category, message)
#define TRIPA_TRACE(category, message) TRIPA_LOG_AT(::tripalog::Level::Trace, category, message)

/*!
 * 记"进入 / 离开 某某"，离开时带上耗时。
 * 里面会拼字符串，放在**高频**路径上不合适（绘制里请用 TRIPA_TRACE 并按计数节流）。
 */
#define TRIPA_SCOPE(level, category, what)                                                       \
    ::tripalog::Scope tripaScopeGuard_((level), (category), __func__, (what))

#endif // TRIPALOG_H
