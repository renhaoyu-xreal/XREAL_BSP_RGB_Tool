#include "recordlab/core/logger.h"

#include <QDateTime>
#include <QDir>
#include <QMessageLogContext>
#include <QByteArray>
#include <QByteArrayView>
#include <QtGlobal>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

/*
 * logger.cpp
 *
 * 当前日志实现故意保持简洁：
 * - 不引入复杂日志框架
 * - 先把 Qt 自己的日志统一接住
 * - 保证 stderr 上能看到稳定、可定位的输出
 *
 * 这对迁移早期非常重要，因为后面一旦接入 IPC、设备层、外部库，
 * 如果日志入口不统一，排障会非常痛苦。
 */
namespace recordlab::core {

namespace {

class TerminalLogTee {
public:
    explicit TerminalLogTee(const QString& appRoot)
    {
        const QDir rootDir(appRoot);
        QDir logDir(rootDir.filePath(QStringLiteral("log")));
        if (!logDir.exists()) {
            QDir().mkpath(logDir.absolutePath());
        }

        const QString fileName = QStringLiteral("recordlabc_terminal_%1.log")
                                     .arg(QDateTime::currentDateTime().toString(
                                         QStringLiteral("yyyyMMdd_HHmmss_zzz")));
        logFile_ = std::fopen(logDir.filePath(fileName).toUtf8().constData(), "ab");
        if (!logFile_) {
            const QByteArray message =
                QStringLiteral("[RecordLabC] failed to open terminal log file: %1\n")
                    .arg(QString::fromLocal8Bit(std::strerror(errno)))
                    .toUtf8();
            writeAll(STDERR_FILENO, message.constData(),
                     static_cast<size_t>(message.size()));
            return;
        }

        originalStdout_ = ::dup(STDOUT_FILENO);
        originalStderr_ = ::dup(STDERR_FILENO);
        if (originalStdout_ < 0 || originalStderr_ < 0) {
            writeOriginal(STDERR_FILENO,
                          "[RecordLabC] failed to duplicate terminal streams\n");
            return;
        }

        setupPipe(STDOUT_FILENO, originalStdout_, "stdout");
        setupPipe(STDERR_FILENO, originalStderr_, "stderr");

        const QByteArray started =
            QStringLiteral("[RecordLabC] terminal log: %1\n")
                .arg(logDir.filePath(fileName))
                .toUtf8();
        writeChunk(originalStderr_, QByteArrayView(started), true);
    }

    ~TerminalLogTee()
    {
        running_.store(false);
        if (logFile_) {
            std::fflush(logFile_);
            std::fclose(logFile_);
        }
        if (originalStdout_ >= 0) {
            ::close(originalStdout_);
        }
        if (originalStderr_ >= 0) {
            ::close(originalStderr_);
        }
    }

    TerminalLogTee(const TerminalLogTee&) = delete;
    TerminalLogTee& operator=(const TerminalLogTee&) = delete;

private:
    void setupPipe(int targetFd, int originalFd, const char* streamName)
    {
        int pipeFds[2] = {-1, -1};
        if (::pipe(pipeFds) != 0) {
            writeOriginal(originalFd, "[RecordLabC] failed to create log pipe\n");
            return;
        }
        if (::dup2(pipeFds[1], targetFd) < 0) {
            writeOriginal(originalFd, "[RecordLabC] failed to redirect terminal stream\n");
            ::close(pipeFds[0]);
            ::close(pipeFds[1]);
            return;
        }
        ::close(pipeFds[1]);

        std::thread([this, readFd = pipeFds[0], originalFd, streamName]() {
            char buffer[4096];
            while (running_.load()) {
                const ssize_t count = ::read(readFd, buffer, sizeof(buffer));
                if (count > 0) {
                    writeChunk(originalFd,
                               QByteArrayView(buffer, static_cast<qsizetype>(count)),
                               true);
                    continue;
                }
                if (count == 0 || errno != EINTR) {
                    break;
                }
            }
            ::close(readFd);
            const std::string stopped =
                std::string("[RecordLabC] terminal tee stopped for ") +
                streamName + "\n";
            writeOriginal(originalFd, stopped.c_str());
        }).detach();
    }

    void writeChunk(int originalFd, QByteArrayView data, bool mirrorToTerminal)
    {
        if (mirrorToTerminal && originalFd >= 0) {
            writeAll(originalFd, data.data(), static_cast<size_t>(data.size()));
        }
        if (!logFile_) {
            return;
        }
        std::lock_guard<std::mutex> locker(logMutex_);
        std::fwrite(data.data(), 1, static_cast<size_t>(data.size()), logFile_);
        std::fflush(logFile_);
    }

    static void writeOriginal(int fd, const char* message)
    {
        if (fd >= 0 && message) {
            writeAll(fd, message, std::strlen(message));
        }
    }

    static void writeAll(int fd, const char* data, size_t size)
    {
        size_t written = 0;
        while (written < size) {
            const ssize_t count = ::write(fd, data + written, size - written);
            if (count > 0) {
                written += static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

    std::atomic_bool running_{true};
    std::FILE* logFile_ = nullptr;
    int originalStdout_ = -1;
    int originalStderr_ = -1;
    std::mutex logMutex_;
};

TerminalLogTee* terminalLogTee = nullptr;

// 将 Qt 日志级别映射成简短文本，便于 stderr 输出阅读。
const char* messageTypeName(QtMsgType type)
{
    // 使用短标签而不是枚举名，方便终端里快速辨认日志级别。
    switch (type) {
    case QtDebugMsg:
        return "DEBUG";
    case QtInfoMsg:
        return "INFO";
    case QtWarningMsg:
        return "WARN";
    case QtCriticalMsg:
        return "ERROR";
    case QtFatalMsg:
        return "FATAL";
    }

    return "LOG";
}

// 当前日志策略非常直接：
// - 所有 Qt message 统一写到 stderr
// - 输出时间、级别、文件、行号
// - fatal 直接 abort
void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    // 统一拦截 Qt 日志并写到 stderr，保证终端、脚本和 IDE 都能看到一致输出。
    const auto timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    std::fprintf(
        stderr,
        "[%s] [%s] [%s:%u] %s\n",
        timestamp.toUtf8().constData(),
        messageTypeName(type),
        context.file ? context.file : "<unknown>",
        context.line,
        message.toUtf8().constData());
    std::fflush(stderr);

    if (type == QtFatalMsg) {
        std::abort();
    }
}

}  // namespace

void installQtLogger()
{
    // 安装进程级 Qt message handler，让后续所有 Qt 日志都走统一入口。
    qInstallMessageHandler(qtMessageHandler);
}

void installTerminalLogTee(const QString& appRoot)
{
    if (!terminalLogTee) {
        terminalLogTee = new TerminalLogTee(appRoot);
    }
}

}  // namespace recordlab::core
