#include "tripalog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QStringList>
#include <QSysInfo>
#include <QTextStream>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(Q_OS_WIN)
#  include <windows.h>
#endif

namespace tripalog {
namespace {

/*!
 * 现场状态。用 `std::atomic` 而不是 QAtomic*：崩溃处理器里读这些值，
 * 越简单越好（别在异常处理里再去碰 malloc / Qt 的锁）。
 */
QFile *g_file = nullptr;
QTextStream *g_stream = nullptr;
QMutex g_mutex;
std::atomic<Level> g_level{Level::Info};
std::atomic<bool> g_running{false};
std::atomic<bool> g_toStderr{false};
std::atomic<bool> g_crashDump{true};
std::atomic<qint64> g_written{0};
std::atomic<qint64> g_maxBytes{4 * 1024 * 1024};
QString g_path;          //!< 日志文件路径（只在启动/收尾时改）
QString g_dumpDir;       //!< dump 目录
QElapsedTimer g_clock;   //!< 进程启动到现在的相对时间
std::atomic<qint64> g_sequence{0};

/*!
 * 环形缓冲：留着最近这些行，崩溃时倒进 dump。
 * 只存已经格式化好的字符串 —— 崩溃处理器里不能再去格式化。
 */
constexpr int kRingLines = 400;
constexpr int kRingLineChars = 512;
QMutex g_ringMutex;
QStringList g_ring;

/*! 面包屑（"最后在做什么"）：一条短字符串，崩溃处理器里直接写出去 */
QMutex g_crumbMutex;
QString g_breadcrumb;

/*! 文档快照：崩的时候在编辑什么（截断过的纯文本） */
QMutex g_docMutex;
QString g_document;

const char *levelName(Level level)
{
    switch (level) {
    case Level::Error:
        return "ERROR";
    case Level::Warning:
        return "WARN ";
    case Level::Info:
        return "INFO ";
    case Level::Debug:
        return "DEBUG";
    case Level::Trace:
        return "TRACE";
    }
    return "?????";
}

//! 名字 -> 级别；认不出来返回 fallback
Level levelFromName(const QString &text, Level fallback)
{
    const QString name = text.trimmed().toLower();
    if (name.isEmpty())
        return fallback;
    if (name == QLatin1String("error") || name == QLatin1String("0"))
        return Level::Error;
    if (name == QLatin1String("warn") || name == QLatin1String("warning")
        || name == QLatin1String("1"))
        return Level::Warning;
    if (name == QLatin1String("info") || name == QLatin1String("2"))
        return Level::Info;
    if (name == QLatin1String("debug") || name == QLatin1String("3"))
        return Level::Debug;
    if (name == QLatin1String("trace") || name == QLatin1String("4"))
        return Level::Trace;
    // "1"/"yes"/"on" 之类：当作 Info；"0"/"off" 当作"不记"
    if (name == QLatin1String("on") || name == QLatin1String("yes")
        || name == QLatin1String("true"))
        return Level::Info;
    return fallback;
}

QString defaultDirectory()
{
    /*!
     * 默认放 `%LOCALAPPDATA%/tripa`（Windows）/ `~/.local/share/tripa`（其它）。
     * 用 AppLocalDataLocation 而不是程序目录：程序可能装在 Program Files 里，
     * 那里普通用户没有写权限 —— 日志写不出去就等于没有日志。
     */
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath() + QStringLiteral("/tripa");
    return dir;
}

QString defaultFilePath()
{
    return defaultDirectory() + QStringLiteral("/tripa.log");
}

/*!
 * 文件超过上限就轮转：`tripa.log` -> `tripa.log.1`（旧的 .1 直接删）。
 * 只在写入之后检查，且只转一次 —— 现场磁盘满 / 权限不对时不能把程序拖死。
 */
void rotateIfNeeded()
{
    if (g_maxBytes.load() <= 0 || !g_file)
        return;
    if (g_written.load() < g_maxBytes.load())
        return;
    const QString path = g_file->fileName();
    g_stream->flush();
    g_file->close();
    QFile::remove(path + QStringLiteral(".1"));
    QFile::rename(path, path + QStringLiteral(".1"));
    if (g_file->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        g_written.store(0);
        *g_stream << QStringLiteral("=== 日志轮转：上一个文件已改名为 ")
                         + QFileInfo(path + QStringLiteral(".1")).fileName()
                         + QStringLiteral(" ===\n");
        g_stream->flush();
    }
}

//! 真正往文件（和 stderr）写一行；调用方持有 g_mutex
void emitLine(const QString &line)
{
    // 环形缓冲（崩溃 dump 用）
    {
        QMutexLocker lock(&g_ringMutex);
        QString kept = line;
        if (kept.size() > kRingLineChars)
            kept = kept.left(kRingLineChars) + QStringLiteral("…");
        g_ring.append(kept);
        while (g_ring.size() > kRingLines)
            g_ring.removeFirst();
    }

    if (g_toStderr.load()) {
        const QByteArray utf8 = line.toUtf8();
        fwrite(utf8.constData(), 1, size_t(utf8.size()), stderr);
        fputc('\n', stderr);
        fflush(stderr);
    }

    if (!g_stream)
        return;
    *g_stream << line << '\n';
    g_stream->flush(); // 崩溃丢日志是最气人的事，宁可慢一点
    g_written.fetch_add(line.size() + 1);
    rotateIfNeeded();
}

//! 组装一行：[相对时间] [级别] [线程] [类别] 消息 (函数:行)
QString formatLine(Level level, const char *category, const QString &message,
                   const char *function, int line)
{
    const qint64 ms = g_clock.isValid() ? g_clock.elapsed() : 0;
    const qint64 seq = g_sequence.fetch_add(1) + 1;
    /*!
     * 线程名：Qt 的 QThread::currentThread() 在**非** Qt 线程里也安全，
     * 名字拿不到就退化成指针后缀，够区分了。
     */
    const Qt::HANDLE handle = QThread::currentThreadId();
    const QString threadName = QThread::currentThread()
                                   ? (QThread::currentThread()->objectName().isEmpty()
                                          ? QStringLiteral("t%1").arg(quintptr(handle) % 10000)
                                          : QThread::currentThread()->objectName())
                                   : QStringLiteral("t%1").arg(quintptr(handle) % 10000);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    QString out = QStringLiteral("%1 +%2ms #%3 [%4] [%5] [%6] %7")
                      .arg(stamp)
                      .arg(ms, 7, 10, QLatin1Char(' '))
                      .arg(seq, 6, 10, QLatin1Char(' '))
                      .arg(QLatin1String(levelName(level)))
                      .arg(threadName)
                      .arg(QLatin1String(category ? category : "app"))
                      .arg(message);
    if (function)
        out += QStringLiteral("  (%1:%2)").arg(QLatin1String(function)).arg(line);
    return out;
}

/* ------------------------------------------------------------------ Qt 消息 */

QtMessageHandler g_previousHandler = nullptr;

/*!
 * Qt 自己的消息也进日志。
 *
 * `qDebug/qWarning/qCritical` 里经常有 Qt 直接指出的问题
 * （"QPainter::begin: Painter already active"、"QTextCursor::setPosition:
 * Position out of range" 之类），这些恰恰是排查崩溃最有用的线索。
 * 级别映射：Debug->Debug、Info->Info、Warning->Warning、Critical/Fatal->Error。
 */
void qtMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    Level level = Level::Info;
    switch (type) {
    case QtDebugMsg:
        level = Level::Debug;
        break;
    case QtInfoMsg:
        level = Level::Info;
        break;
    case QtWarningMsg:
        level = Level::Warning;
        break;
    case QtCriticalMsg:
    case QtFatalMsg:
        level = Level::Error;
        break;
    }
    write(level, context.category ? context.category : "qt", message, context.function,
          context.line);

    /*!
     * **不要**把 Qt 的致命消息吞掉：Qt 自己会 abort()，而我们需要那个
     * "进程被 abort 了"的信号（崩溃处理器会写出 dump）。
     */
    if (g_previousHandler && type == QtFatalMsg)
        g_previousHandler(type, context, message);
}

#if defined(Q_OS_WIN)

/* ------------------------------------------------------------------ 崩溃处理 */

LONG WINAPI exceptionFilter(EXCEPTION_POINTERS *info)
{
    /*!
     * 这个函数在**异常上下文里**跑，能做和不能做的事必须分清楚：
     *   - 能：写文件、读已经准备好的字符串、读 CONTEXT 里的寄存器；
     *   - 不能：分配内存（malloc / QString 拼接）、加可能死锁的锁、
     *          调 Qt 的字符串格式化（也可能会分配）。
     * 所以下面用**固定的 char 缓冲 + 裸 fwrite**，环形缓冲和面包屑
     * 在这里用 tryLock：拿不到锁也照样出 dump，绝不在这里挂住。
     */
    const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    const void *address =
        info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;

    char line[1024];
    char path[MAX_PATH * 2];
    const QString dir = dumpDirectory();
    const QByteArray dirUtf8 = dir.toUtf8();
    snprintf(path, sizeof(path), "%s/tripa-crash-%s.txt", dirUtf8.constData(),
             qPrintable(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));

    FILE *fp = fopen(path, "wb");
    if (!fp)
        return EXCEPTION_EXECUTE_HANDLER; // 写不出 dump 也不能在这儿卡住

    auto put = [&](const char *text) { fwrite(text, 1, strlen(text), fp); };

    /*!
     * 面包屑和环形缓冲都用 **tryLock**，不用 QMutexLocker：
     * 崩溃时另一个线程很可能正握着那把锁（它可能也快死了），
     * 在异常处理器里死等就是"程序卡住、连 dump 都写不出来"，比崩溃还糟。
     */
    auto processSnapshot = [&] {
        if (g_crumbMutex.tryLock(100)) {
            const QByteArray utf8 = g_breadcrumb.toUtf8();
            fwrite(utf8.constData(), 1, size_t(utf8.size()), fp);
            g_crumbMutex.unlock();
        } else {
            put("(取不到：锁被别的线程占着)\n");
        }
    };

    put("==== tripa 崩溃报告 ====\n");
    snprintf(line, sizeof(line), "时间: %s\n",
             qPrintable(QDateTime::currentDateTime().toString(Qt::ISODate)));
    put(line);
    snprintf(line, sizeof(line), "异常码: 0x%08lX  %s\n", (unsigned long)code,
             code == EXCEPTION_ACCESS_VIOLATION  ? "(访问冲突 / 空指针？)"
             : code == EXCEPTION_STACK_OVERFLOW ? "(栈溢出 —— 递归没收敛？)"
             : code == EXCEPTION_ILLEGAL_INSTRUCTION ? "(非法指令)"
             : code == EXCEPTION_INT_DIVIDE_BY_ZERO  ? "(除以零)"
                                                     : "");
    put(line);
    snprintf(line, sizeof(line), "异常地址: %p\n", address);
    put(line);
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        snprintf(line, sizeof(line), "访问类型: %s 目标地址: %p\n",
                 info->ExceptionRecord->ExceptionInformation[0] ? "写" : "读",
                 (void *)info->ExceptionRecord->ExceptionInformation[1]);
        put(line);
    }
    snprintf(line, sizeof(line), "线程 id: %lu\n", (unsigned long)GetCurrentThreadId());
    put(line);

    // 出错的模块（哪个 dll 的地址）
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module)) {
        char modulePath[MAX_PATH] = {0};
        GetModuleFileNameA(module, modulePath, MAX_PATH);
        snprintf(line, sizeof(line), "出错模块: %s (基址 %p)\n", modulePath, (void *)module);
        put(line);
    }

    // CPU 寄存器（配合 .map / addr2line 能定位到具体指令）
    if (info && info->ContextRecord) {
#if defined(_M_X64) || defined(__x86_64__)
        const CONTEXT *c = info->ContextRecord;
        snprintf(line, sizeof(line), "RIP=%p RSP=%p RBP=%p RAX=%p RBX=%p RCX=%p RDX=%p\n",
                 (void *)c->Rip, (void *)c->Rsp, (void *)c->Rbp, (void *)c->Rax,
                 (void *)c->Rbx, (void *)c->Rcx, (void *)c->Rdx);
        put(line);
#else
        const CONTEXT *c = info->ContextRecord;
        snprintf(line, sizeof(line), "EIP=%p ESP=%p EBP=%p EAX=%p EBX=%p ECX=%p EDX=%p\n",
                 (void *)c->Eip, (void *)c->Esp, (void *)c->Ebp, (void *)c->Eax,
                 (void *)c->Ebx, (void *)c->Ecx, (void *)c->Edx);
        put(line);
#endif
    }

    // 面包屑：崩溃前最后想做的那件事
    put("\n---- 最后在做什么（面包屑）----\n");
    processSnapshot();
    put("\n---- 崩溃时的文档快照（前 500 字）----\n");
    {
        if (g_docMutex.tryLock(100)) {
            const QByteArray utf8 = g_document.toUtf8();
            fwrite(utf8.constData(), 1, size_t(utf8.size()), fp);
            g_docMutex.unlock();
        } else {
            put("(取不到：锁被别的线程占着)\n");
        }
    }

    // 最近的日志
    put("\n---- 最近的日志 ----\n");
    {
        /*!
         * **必须 tryLock**：`QMutexLocker` 会一直等，而崩溃发生时
         * 另一个线程很可能正握着这把锁（它可能也快死了）—— 在异常处理器里
         * 死等就是"程序卡死、连 dump 都写不出来"，比崩溃还糟。
         */
        if (g_ringMutex.tryLock(200)) {
            for (const QString &entry : std::as_const(g_ring)) {
                const QByteArray utf8 = entry.toUtf8();
                fwrite(utf8.constData(), 1, size_t(utf8.size()), fp);
                fputc('\n', fp);
            }
            g_ringMutex.unlock();
        } else {
            put("(取不到：锁被别的线程占着)\n");
        }
    }

    put("\n==== 报告结束 ====\n");
    fclose(fp);

    /*!
     * 让默认处理器继续跑（Windows 错误报告 / 调试器），
     * 这样"程序自己退出"的行为和以前一致，只是磁盘上多了一份报告。
     */
    return EXCEPTION_EXECUTE_HANDLER;
}

void installCrashHandlers()
{
    SetUnhandledExceptionFilter(exceptionFilter);
}

#else // !Q_OS_WIN

void installCrashHandlers()
{
    // 其它平台先不做（这个项目只在 Windows 上验证过）
}

#endif

} // namespace

/* ---------------------------------------------------------------- 对外接口 */

void parseCommandLine(int argc, char *const argv[], Options *options)
{
    if (!options)
        return;

    // 先看环境变量，命令行再覆盖
    const QByteArray envLevel = qgetenv("TRIPA_LOG");
    if (!envLevel.isEmpty()) {
        options->level = levelFromName(QString::fromLocal8Bit(envLevel), Level::Info);
    }
    const QByteArray envFile = qgetenv("TRIPA_LOG_FILE");
    if (!envFile.isEmpty())
        options->filePath = QString::fromLocal8Bit(envFile);

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--log")) {
            options->level = Level::Info;
        } else if (arg.startsWith(QLatin1String("--log="))) {
            options->level = levelFromName(arg.mid(6), Level::Info);
        } else if (arg == QLatin1String("--log-file") && i + 1 < argc) {
            options->filePath = QString::fromLocal8Bit(argv[++i]);
        } else if (arg.startsWith(QLatin1String("--log-file="))) {
            options->filePath = arg.mid(11);
        } else if (arg == QLatin1String("--no-log")) {
            options->level = Level::Error;
            options->filePath = QStringLiteral("-"); // 明确关掉：不落盘
        } else if (arg == QLatin1String("--log-stderr")) {
            options->toStderr = true;
        } else if (arg == QLatin1String("--no-crash-dump")) {
            options->crashDump = false;
        }
    }
}

bool start(const Options &options)
{
    if (g_running.load())
        return true;

    g_clock.start();
    g_level.store(options.level);
    g_toStderr.store(options.toStderr);
    g_crashDump.store(options.crashDump);
    g_maxBytes.store(options.maxBytes());

    /*!
     * dump 目录**先定下来**：哪怕日志文件打不开（权限不对、磁盘满），
     * 崩溃报告也要有个地方写 —— "日志关着"和"崩了什么都没留下"是两件事。
     */
    g_dumpDir = options.filePath.isEmpty() || options.filePath == QLatin1String("-")
                    ? defaultDirectory()
                    : QFileInfo(options.filePath).absolutePath();
    QDir().mkpath(g_dumpDir);

    const bool disabled = options.filePath == QLatin1String("-");
    if (!disabled) {
        g_path = options.filePath.isEmpty() ? defaultFilePath() : options.filePath;
        g_file = new QFile(g_path);
        if (g_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            g_stream = new QTextStream(g_file);
            g_stream->setEncoding(QStringConverter::Utf8);
            g_written.store(g_file->size());
        } else {
            fprintf(stderr, "tripalog: 打不开日志文件 %s（%s），崩溃报告仍然会写到 %s\n",
                    qPrintable(g_path), qPrintable(g_file->errorString()),
                    qPrintable(g_dumpDir));
            delete g_file;
            g_file = nullptr;
            g_path.clear();
        }
    } else {
        g_path.clear();
    }

    // 环形缓冲先清空，崩溃 dump 里的"最近日志"就不会混进上一次的
    {
        QMutexLocker lock(&g_ringMutex);
        g_ring.clear();
    }

    g_running.store(true);

    // Qt 的消息也进同一个文件
    g_previousHandler = qInstallMessageHandler(qtMessageHandler);
    if (options.crashDump)
        installCrashHandlers();

    TRIPA_INFO("log",
               QStringLiteral("日志启动：文件=%1 级别=%2 崩溃 dump=%3")
                   .arg(g_path.isEmpty() ? QStringLiteral("（关闭）") : g_path)
                   .arg(int(options.level))
                   .arg(options.crashDump ? QStringLiteral("开") : QStringLiteral("关")));
    return true;
}

void shutdown()
{
    if (!g_running.load())
        return;
    TRIPA_INFO("log", QStringLiteral("正常退出"));
    qInstallMessageHandler(g_previousHandler);
    g_previousHandler = nullptr;
    g_running.store(false);

    QMutexLocker lock(&g_mutex);
    if (g_stream) {
        g_stream->flush();
        delete g_stream;
        g_stream = nullptr;
    }
    if (g_file) {
        g_file->close();
        delete g_file;
        g_file = nullptr;
    }
}

bool isRunning()
{
    return g_running.load();
}

QString logFilePath()
{
    return g_path;
}

QString dumpDirectory()
{
    return g_dumpDir;
}

void write(Level level, const char *category, const QString &message, const char *function,
           int line)
{
    if (!g_running.load())
        return;
    if (int(level) > int(g_level.load()))
        return;

    QMutexLocker lock(&g_mutex);
    emitLine(formatLine(level, category, message, function, line));
}

void breadcrumb(const QString &what)
{
    QMutexLocker lock(&g_crumbMutex);
    g_breadcrumb = QStringLiteral("%1  %2")
                       .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                       .arg(what);
}

QString currentBreadcrumb()
{
    QMutexLocker lock(&g_crumbMutex);
    return g_breadcrumb;
}

void setDocumentSnapshot(const QString &text)
{
    QMutexLocker lock(&g_docMutex);
    g_document = text.left(500);
}

QString documentSnapshot()
{
    QMutexLocker lock(&g_docMutex);
    return g_document;
}

QString nowStamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
}

QString helpReport(int tailLines)
{
    QString out;
    QTextStream text(&out);
    text << QStringLiteral("==== tripa 诊断信息 ====\n");
    text << QStringLiteral("时间: ") << QDateTime::currentDateTime().toString(Qt::ISODate) << '\n';
    text << QStringLiteral("程序: tripa ") << QCoreApplication::applicationVersion() << '\n';
    text << QStringLiteral("Qt: ") << QLatin1String(qVersion()) << '\n';
    text << QStringLiteral("构建: ") << QLatin1String(__DATE__) << ' ' << QLatin1String(__TIME__)
         << '\n';
    text << QStringLiteral("系统: ") << QSysInfo::prettyProductName() << " / "
         << QSysInfo::currentCpuArchitecture() << '\n';
    text << QStringLiteral("日志: ") << (g_path.isEmpty() ? QStringLiteral("（未启用）") : g_path)
         << '\n';
    text << QStringLiteral("崩溃报告目录: ") << (g_dumpDir.isEmpty() ? QStringLiteral("（无）")
                                                                     : g_dumpDir)
         << '\n';
    text << QStringLiteral("日志级别: ") << int(g_level.load()) << QStringLiteral("（0=错误 4=全部）")
         << '\n';
    text << QStringLiteral("最后在做什么: ") << currentBreadcrumb() << '\n';
    text << QStringLiteral("文档快照: ") << documentSnapshot() << '\n';

    text << QStringLiteral("\n---- 最近 %1 条日志 ----\n").arg(tailLines);
    {
        QMutexLocker lock(&g_ringMutex);
        const int from = qMax(0, g_ring.size() - qMax(1, tailLines));
        for (int i = from; i < g_ring.size(); ++i)
            text << g_ring.at(i) << '\n';
    }
    text << QStringLiteral("==== 结束（把上面这些整段贴回去即可）====\n");
    return out;
}

Scope::Scope(Level level, const char *category, const char *function, const QString &what)
    : m_level(level)
    , m_category(category)
    , m_function(function)
    , m_what(what)
{
    if (int(level) <= int(g_level.load()))
        write(level, category, QStringLiteral("进入 %1").arg(what), function, 0);
}

Scope::~Scope()
{
    if (int(m_level) <= int(g_level.load()))
        write(m_level, m_category, QStringLiteral("离开 %1").arg(m_what), m_function, 0);
}

} // namespace tripalog
