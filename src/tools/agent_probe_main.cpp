#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QMetaType>
#include <QTcpSocket>
#include <QTextStream>

#include <master.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "recordlab/backend/agent_manager_process.h"
#include "recordlab/core/qt_json_compat.h"

namespace {

bool isLocalMasterReachable(int timeoutMs = 200)
{
    // 用短连接探测本地 echo master 是否可达，避免重复启动。
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), 5590);
    const bool connected = socket.waitForConnected(timeoutMs);
    if (connected) {
        socket.disconnectFromHost();
    }
    return connected;
}

class EchoMasterRuntime {
public:
    // 析构时停止内部启动的 master 线程，避免 probe 进程退出后仍留后台服务。
    ~EchoMasterRuntime()
    {
        stop();
    }

    void ensureStarted()
    {
        // 优先复用外部 master；若本地不存在再临时拉起嵌入式实例。
        if (startedInternally_ || usingExternal_) {
            return;
        }

        if (isLocalMasterReachable()) {
            usingExternal_ = true;
            std::cout << "[agent_probe] Reusing existing master on tcp://127.0.0.1:5590"
                      << std::endl;
            return;
        }

        master_ = std::make_unique<echo::Master>(5590);
        masterThread_ = std::thread([this]() {
            try {
                master_->start();
            } catch (const std::exception& e) {
                std::cerr << "[agent_probe] Master thread failed: " << e.what() << std::endl;
            }
        });

        for (int attempt = 0; attempt < 20; ++attempt) {
            if (isLocalMasterReachable()) {
                startedInternally_ = true;
                std::cout << "[agent_probe] Started embedded master on tcp://127.0.0.1:5590"
                          << std::endl;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::cerr << "[agent_probe] Embedded master did not become reachable in time"
                  << std::endl;
    }

private:
    void stop()
    {
        // 停止并 join master 线程，确保资源按顺序释放。
        if (master_) {
            master_->stop();
        }
        if (masterThread_.joinable()) {
            masterThread_.join();
        }
        master_.reset();
    }

    std::unique_ptr<echo::Master> master_;
    std::thread masterThread_;
    bool startedInternally_ = false;
    bool usingExternal_ = false;
};

QString resolveAppRoot()
{
    // 解析 RecordLabC 工程根目录，优先使用环境变量覆盖。
    QString appRoot = qEnvironmentVariable("RECORDLABC_ROOT");
    if (appRoot.isEmpty()) {
        QDir appDir(QCoreApplication::applicationDirPath());
        if (appDir.dirName() == QStringLiteral("build")) {
            appDir.cdUp();
        }
        appRoot = appDir.absolutePath();
    }
    return QDir::cleanPath(appRoot);
}

void printJson(const nlohmann::json& value)
{
    // 以格式化 JSON 输出 probe 结果，便于终端和脚本消费。
    QTextStream(stdout) << QString::fromStdString(value.dump(2)) << Qt::endl;
}

}  // namespace

int main(int argc, char* argv[])
{
    // CLI 负责初始化本地 probe 运行时，并按命令执行一组 manager 动作。
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("recordlabc_agent_probe"));
    qRegisterMetaType<nlohmann::json>("nlohmann::json");

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("RecordLabC agent probe"));
    parser.addHelpOption();

    const QCommandLineOption agentOption(
        QStringList{QStringLiteral("a"), QStringLiteral("agent")},
        QStringLiteral("Agent name"),
        QStringLiteral("agent"),
        QStringLiteral("glasses_bsp_node"));
    const QCommandLineOption commandOption(
        QStringList{QStringLiteral("c"), QStringLiteral("command")},
        QStringLiteral("Probe command: init_agent | check | init_device | start_device | observe_start | release_agent"),
        QStringLiteral("command"),
        QStringLiteral("init_agent"));
    const QCommandLineOption timeoutOption(
        QStringList{QStringLiteral("t"), QStringLiteral("timeout")},
        QStringLiteral("Timeout in seconds"),
        QStringLiteral("seconds"),
        QStringLiteral("20"));
    const QCommandLineOption observeSecondsOption(
        QStringList{QStringLiteral("observe-seconds")},
        QStringLiteral("Observe duration in seconds for observe_start"),
        QStringLiteral("seconds"),
        QStringLiteral("12"));

    parser.addOption(agentOption);
    parser.addOption(commandOption);
    parser.addOption(timeoutOption);
    parser.addOption(observeSecondsOption);
    parser.process(application);

    const QString agentName = parser.value(agentOption).trimmed();
    const QString commandName = parser.value(commandOption).trimmed();
    const double timeoutSec = parser.value(timeoutOption).toDouble();
    const double observeSeconds = parser.value(observeSecondsOption).toDouble();
    const QString appRoot = resolveAppRoot();
    const QString configPath = QDir(appRoot).filePath(QStringLiteral("config/agents_config.json"));

    std::cout << "[agent_probe] app_root=" << appRoot.toStdString() << std::endl;
    std::cout << "[agent_probe] config=" << configPath.toStdString() << std::endl;
    std::cout << "[agent_probe] agent=" << agentName.toStdString()
              << " command=" << commandName.toStdString()
              << " timeout=" << timeoutSec << "s" << std::endl;

    EchoMasterRuntime masterRuntime;
    masterRuntime.ensureStarted();

    recordlab::backend::AgentManagerProcess process(configPath, nullptr);
    process.start();
    QObject::connect(&process, &recordlab::backend::AgentManagerProcess::statusUpdate,
                     [](const nlohmann::json& status) {
                         std::cout << "[agent_probe] status_update: "
                                   << status.dump() << std::endl;
                     });

    auto runManagerAction = [&](const nlohmann::json& command) {
        std::cout << "[agent_probe] sending: " << command.dump() << std::endl;
        const auto result = process.sendCommandSync(command, timeoutSec);
        printJson(result);
        return result;
    };

    int exitCode = 0;

    if (commandName == QStringLiteral("init_agent")) {
        const auto result = runManagerAction(
            {{"action", "init_agent"}, {"agent_name", agentName.toStdString()}});
        exitCode = result.value("success", false) ? 0 : 1;
    } else if (commandName == QStringLiteral("release_agent")) {
        const auto initResult = runManagerAction(
            {{"action", "init_agent"}, {"agent_name", agentName.toStdString()}});
        if (!initResult.value("success", false)) {
            exitCode = 1;
        } else {
            const auto releaseResult = runManagerAction(
                {{"action", "release_agent"}, {"agent_name", agentName.toStdString()}});
            exitCode = releaseResult.value("success", false) ? 0 : 1;
        }
    } else if (commandName == QStringLiteral("observe_start")) {
        const auto initResult = runManagerAction(
            {{"action", "init_agent"}, {"agent_name", agentName.toStdString()}});
        if (!initResult.value("success", false)) {
            exitCode = 1;
        } else {
            const auto initDeviceResult = runManagerAction(
                {{"action", "send_agent_command"},
                 {"agent_name", agentName.toStdString()},
                 {"cmd_name", std::string("init_device")},
                 {"params", nlohmann::json::object()}});
            if (!initDeviceResult.value("success", false)) {
                exitCode = 1;
            } else {
                const auto startDeviceResult = runManagerAction(
                    {{"action", "send_agent_command"},
                     {"agent_name", agentName.toStdString()},
                     {"cmd_name", std::string("start_device")},
                     {"params", nlohmann::json::object()}});
                exitCode = startDeviceResult.value("success", false) ? 0 : 1;
                const auto observeDeadline =
                    std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(
                        static_cast<int>(observeSeconds * 1000.0));
                int checkIndex = 0;
                while (std::chrono::steady_clock::now() < observeDeadline) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    const auto remainingMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            observeDeadline - std::chrono::steady_clock::now())
                            .count();
                    if (remainingMs <= 0) {
                        break;
                    }
                    if (++checkIndex % 5 == 0) {
                        const auto checkResult = runManagerAction(
                            {{"action", "send_agent_command"},
                             {"agent_name", agentName.toStdString()},
                             {"cmd_name", std::string("check")},
                             {"params", nlohmann::json::object()}});
                        if (!checkResult.value("success", false)) {
                            exitCode = 1;
                            break;
                        }
                    }
                }
            }
            runManagerAction(
                {{"action", "release_agent"}, {"agent_name", agentName.toStdString()}});
        }
    } else {
        const auto initResult = runManagerAction(
            {{"action", "init_agent"}, {"agent_name", agentName.toStdString()}});
        if (!initResult.value("success", false)) {
            exitCode = 1;
        } else {
            const auto commandResult = runManagerAction(
                {{"action", "send_agent_command"},
                 {"agent_name", agentName.toStdString()},
                 {"cmd_name", commandName.toStdString()},
                 {"params", nlohmann::json::object()}});
            exitCode = commandResult.value("success", false) ? 0 : 1;
            runManagerAction(
                {{"action", "release_agent"}, {"agent_name", agentName.toStdString()}});
        }
    }

    process.stop();
    return exitCode;
}
