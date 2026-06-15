#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTcpSocket>

#include <action.h>
#include <logger.h>
#include <master.h>
#include <subscriber.h>

#include <chrono>
#include <iostream>
#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

#include "recordlab/core/qt_json_compat.h"
#include "recordlab/flowagent/agents/legacy_remote_action_client.h"

namespace {

bool isLocalMasterReachable(int timeoutMs = 200)
{
    // 用短连接探测本地 echo master 是否已运行。
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), 5590);
    const bool connected = socket.waitForConnected(timeoutMs);
    if (connected) {
        socket.disconnectFromHost();
    }
    return connected;
}

bool waitForActionServer(const std::string& actionName, int timeoutMs)
{
    // 轮询 action server 所需的两个服务是否都已向 master 注册。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const std::string sendGoalService = actionName + "/send_goal";
    const std::string cancelService = actionName + "/cancel";

    while (std::chrono::steady_clock::now() < deadline) {
        const auto goalServices =
            echo::MasterClient::getInstance().queryService(sendGoalService);
        const auto cancelServices =
            echo::MasterClient::getInstance().queryService(cancelService);
        if (!goalServices.empty() && !cancelServices.empty()) {
            return true;
        }

        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
}

void writeResultFile(const QString& path, const nlohmann::json& result)
{
    // 将命令结果持久化到 JSON 文件，供外部脚本或 UI 侧桥接读取。
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        throw std::runtime_error(
            QStringLiteral("无法写入结果文件: %1").arg(path).toStdString());
    }
    file.write(QByteArray::fromStdString(result.dump()));
    file.close();
}

struct RemoteAgentEndpoint {
    QString host;
    int goalPort = 0;
    int feedbackPort = 0;
};

QString defaultConfigPath()
{
    const QString appRoot = qEnvironmentVariable("RECORDLABC_ROOT").trimmed();
    if (!appRoot.isEmpty()) {
        const QString path = QDir(appRoot).filePath(QStringLiteral("config/agents_config.json"));
        if (QFileInfo::exists(path)) {
            return path;
        }
    }

    const QString cwdPath = QDir::current().filePath(QStringLiteral("config/agents_config.json"));
    if (QFileInfo::exists(cwdPath)) {
        return cwdPath;
    }

#ifdef RECORDLABC_SOURCE_DIR
    const QString sourcePath = QDir(QStringLiteral(RECORDLABC_SOURCE_DIR))
                                   .filePath(QStringLiteral("config/agents_config.json"));
    if (QFileInfo::exists(sourcePath)) {
        return sourcePath;
    }
#endif

    return cwdPath;
}

std::optional<RemoteAgentEndpoint> loadRemoteEndpoint(const QString& agentName)
{
    QFile file(defaultConfigPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    const auto doc = QJsonDocument::fromJson(file.readAll());
    const auto agents = doc.object().value(QStringLiteral("agents")).toObject();
    const auto cfg = agents.value(agentName).toObject();
    if (cfg.isEmpty()) {
        return std::nullopt;
    }

    const QString subnodePath = cfg.value(QStringLiteral("subnode_path")).toString().trimmed();
    if (!subnodePath.isEmpty()) {
        return std::nullopt;
    }

    RemoteAgentEndpoint endpoint;
    endpoint.host = cfg.value(QStringLiteral("subnode_host")).toString(QStringLiteral("localhost"));
    endpoint.goalPort = cfg.value(QStringLiteral("goal_port")).toInt();
    endpoint.feedbackPort = cfg.value(QStringLiteral("feedback_port")).toInt();
    if (endpoint.host.trimmed().isEmpty() || endpoint.goalPort <= 0 ||
        endpoint.feedbackPort <= 0) {
        return std::nullopt;
    }
    return endpoint;
}

}  // namespace

int main(int argc, char* argv[])
{
    // CLI 负责桥接 agent/action command，并把结果写入指定 JSON 文件。
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("recordlabc_agent_cmd"));
    qRegisterMetaType<nlohmann::json>("nlohmann::json");

    echo::Logger::getInstance().setConsoleOutput(false);
    echo::Logger::getInstance().setLogLevel(echo::LogLevel::ERROR);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("RecordLabC agent command bridge"));
    parser.addHelpOption();

    const QCommandLineOption agentOption(
        QStringList{QStringLiteral("a"), QStringLiteral("agent")},
        QStringLiteral("Agent name"),
        QStringLiteral("agent"));
    const QCommandLineOption cmdOption(
        QStringList{QStringLiteral("c"), QStringLiteral("cmd")},
        QStringLiteral("Command name"),
        QStringLiteral("cmd"));
    const QCommandLineOption paramsOption(
        QStringList{QStringLiteral("p"), QStringLiteral("params-json")},
        QStringLiteral("JSON params"),
        QStringLiteral("json"),
        QStringLiteral("{}"));
    const QCommandLineOption timeoutOption(
        QStringList{QStringLiteral("t"), QStringLiteral("timeout")},
        QStringLiteral("Timeout in seconds"),
        QStringLiteral("seconds"),
        QStringLiteral("30"));
    const QCommandLineOption resultFileOption(
        QStringList{QStringLiteral("result-file")},
        QStringLiteral("Path to output JSON result file"),
        QStringLiteral("path"));
    const QCommandLineOption stateKeyOption(
        QStringList{QStringLiteral("state-key")},
        QStringLiteral("Read global state key: record_timer | time_delay | motion_status"),
        QStringLiteral("state"));
    const QCommandLineOption noWaitOption(
        QStringList{QStringLiteral("no-wait")},
        QStringLiteral("Send command without waiting for result"));
    const QCommandLineOption probeOption(
        QStringList{QStringLiteral("probe")},
        QStringLiteral("Only probe action server availability"));

    parser.addOption(agentOption);
    parser.addOption(cmdOption);
    parser.addOption(paramsOption);
    parser.addOption(timeoutOption);
    parser.addOption(resultFileOption);
    parser.addOption(stateKeyOption);
    parser.addOption(noWaitOption);
    parser.addOption(probeOption);
    parser.process(application);

    nlohmann::json payload = {
        {"success", false},
        {"message", std::string("unknown error")},
    };

    try {
        const QString agentName = parser.value(agentOption).trimmed();
        const QString cmdName = parser.value(cmdOption).trimmed();
        const QString resultFile = parser.value(resultFileOption).trimmed();
        const QString stateKey = parser.value(stateKeyOption).trimmed();
        const bool probeOnly = parser.isSet(probeOption);
        const bool waitForResult = !parser.isSet(noWaitOption);
        const int timeoutMs =
            static_cast<int>(parser.value(timeoutOption).toDouble() * 1000.0);
        const int actionServerTimeoutMs = timeoutMs > 0 ? timeoutMs : 5000;
        const int resultTimeoutMs = timeoutMs > 0 ? timeoutMs : std::numeric_limits<int>::max();

        if (agentName.isEmpty() && stateKey.isEmpty()) {
            throw std::runtime_error("agent 和 state-key 至少需要提供一个");
        }
        if (!stateKey.isEmpty() && (!agentName.isEmpty() || !cmdName.isEmpty() || probeOnly)) {
            // state mode ignores agent/cmd/probe, but reject mixed usage to keep semantics explicit.
        }
        if (agentName.isEmpty() && stateKey.isEmpty()) {
            throw std::runtime_error("agent 不能为空");
        }
        if (resultFile.isEmpty()) {
            throw std::runtime_error("result-file 不能为空");
        }
        if (stateKey.isEmpty() && !probeOnly && cmdName.isEmpty()) {
            throw std::runtime_error("cmd 不能为空");
        }

        const auto remoteEndpoint =
            stateKey.isEmpty() ? loadRemoteEndpoint(agentName) : std::nullopt;
        if (remoteEndpoint.has_value()) {
            using recordlab::flowagent::agents::LegacyRemoteActionClient;
            const int requestTimeoutMs = std::clamp(timeoutMs, 200, 5000);
            LegacyRemoteActionClient client(
                remoteEndpoint->host.toStdString(), remoteEndpoint->goalPort,
                remoteEndpoint->feedbackPort, requestTimeoutMs);

            std::string remoteError;
            if (!client.waitForServer(actionServerTimeoutMs, &remoteError)) {
                throw std::runtime_error(
                    QStringLiteral("远程 ActionServer 未就绪: %1 (%2:%3/%4)")
                        .arg(agentName, remoteEndpoint->host)
                        .arg(remoteEndpoint->goalPort)
                        .arg(remoteEndpoint->feedbackPort)
                        .toStdString() +
                    (remoteError.empty() ? std::string()
                                         : std::string(" - ") + remoteError));
            }

            if (probeOnly) {
                payload = {
                    {"success", true},
                    {"message", "Remote ActionServer available"},
                    {"status", "AVAILABLE"},
                };
                writeResultFile(resultFile, payload);
                return 0;
            }

            nlohmann::json params = nlohmann::json::object();
            const auto paramsJson = parser.value(paramsOption).trimmed();
            if (!paramsJson.isEmpty()) {
                params = nlohmann::json::parse(paramsJson.toStdString());
                if (!params.is_object()) {
                    params = nlohmann::json::object();
                }
            }

            std::mutex resultLock;
            nlohmann::json resultValue = nlohmann::json::object();
            nlohmann::json lastFeedback = nlohmann::json::object();
            bool callbackSuccess = false;

            const uint32_t goalId = client.sendGoal(
                {{"cmd", cmdName.toStdString()}, {"params", params}},
                [&resultLock, &lastFeedback](uint32_t,
                                             const nlohmann::json& feedback) {
                    std::lock_guard<std::mutex> locker(resultLock);
                    lastFeedback = feedback;
                },
                [&resultLock, &resultValue, &callbackSuccess](
                    uint32_t, const nlohmann::json& result, bool success) {
                    std::lock_guard<std::mutex> locker(resultLock);
                    resultValue = result;
                    callbackSuccess = success;
                });

            if (!waitForResult) {
                payload = {
                    {"success", true},
                    {"message", "Command sent"},
                    {"goal_id", goalId},
                    {"status", "ACCEPTED"},
                };
                writeResultFile(resultFile, payload);
                return 0;
            }

            if (!client.waitForResult(goalId, resultTimeoutMs)) {
                payload = {
                    {"success", false},
                    {"message", "Timeout waiting for result"},
                    {"goal_id", goalId},
                    {"status", "TIMEOUT"},
                };
                writeResultFile(resultFile, payload);
                return 1;
            }

            {
                std::lock_guard<std::mutex> locker(resultLock);
                if (resultValue.is_object()) {
                    payload = resultValue;
                } else {
                    payload = {
                        {"success", callbackSuccess},
                        {"message", resultValue.dump()},
                    };
                }
                if (!lastFeedback.is_null() && !lastFeedback.empty()) {
                    payload["feedback"] = lastFeedback;
                }
                payload["goal_id"] = goalId;
                payload["status"] = callbackSuccess ? "SUCCEEDED" : "FAILED";
                if (!payload.contains("success")) {
                    payload["success"] = callbackSuccess;
                }
                if (!payload.contains("message")) {
                    payload["message"] =
                        callbackSuccess ? "Success" : "Command failed";
                }
            }

            writeResultFile(resultFile, payload);
            return payload.value("success", false) ? 0 : 1;
        }

        if (!isLocalMasterReachable()) {
            throw std::runtime_error(
                "本地 echo master 未运行: tcp://127.0.0.1:5590");
        }

        if (!stateKey.isEmpty()) {
            QString topicName;
            if (stateKey == QStringLiteral("record_timer")) {
                topicName = QStringLiteral("record_timer");
            } else if (stateKey == QStringLiteral("time_delay")) {
                topicName = QStringLiteral("time_delay");
            } else if (stateKey == QStringLiteral("motion_status")) {
                topicName = QStringLiteral("motion_status");
            } else {
                throw std::runtime_error(
                    QStringLiteral("未知 state-key: %1").arg(stateKey).toStdString());
            }

            std::mutex stateLock;
            nlohmann::json latest = nlohmann::json::object();
            bool received = false;
            echo::Subscriber subscriber(
                topicName.toStdString(),
                [&stateLock, &latest, &received](const std::string& raw) {
                    try {
                        auto payload = nlohmann::json::parse(raw);
                        std::lock_guard<std::mutex> locker(stateLock);
                        latest = payload;
                        received = true;
                    } catch (...) {
                    }
                },
                true);

            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(timeoutMs);
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::lock_guard<std::mutex> locker(stateLock);
                    if (received) {
                        break;
                    }
                }
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            nlohmann::json value = nullptr;
            {
                std::lock_guard<std::mutex> locker(stateLock);
                if (received) {
                    if (stateKey == QStringLiteral("record_timer")) {
                        value = latest.value("duration_ns", 0.0) / 1'000'000'000.0;
                    } else if (stateKey == QStringLiteral("time_delay")) {
                        value = latest.value("time_delay_ns", 0.0) / 1'000'000.0;
                    } else if (stateKey == QStringLiteral("motion_status")) {
                        if (latest.contains("status") && latest["status"].is_string()) {
                            value = latest["status"];
                        }
                    }
                }
            }

            payload = {
                {"success", true},
                {"message", received ? "State snapshot read" : "No state data received"},
                {"status", received ? "SUCCEEDED" : "NO_DATA"},
                {"value", value},
            };
            writeResultFile(resultFile, payload);
            return 0;
        }

        const std::string actionName =
            agentName.toStdString() + std::string("_actions");
        if (!waitForActionServer(actionName, actionServerTimeoutMs)) {
            throw std::runtime_error(
                QStringLiteral("ActionServer 未就绪: %1").arg(agentName)
                    .toStdString());
        }

        if (probeOnly) {
            payload = {
                {"success", true},
                {"message", "ActionServer available"},
                {"status", "AVAILABLE"},
            };
            writeResultFile(resultFile, payload);
            return 0;
        }

        nlohmann::json params = nlohmann::json::object();
        const auto paramsJson = parser.value(paramsOption).trimmed();
        if (!paramsJson.isEmpty()) {
            params = nlohmann::json::parse(paramsJson.toStdString());
            if (!params.is_object()) {
                params = nlohmann::json::object();
            }
        }

        echo::ActionClient client(actionName);
        std::this_thread::sleep_for(std::chrono::seconds(1));

        std::mutex resultLock;
        nlohmann::json resultValue = nlohmann::json::object();
        nlohmann::json lastFeedback = nlohmann::json::object();
        bool callbackSuccess = false;

        const uint32_t goalId = client.sendGoal(
            {{"cmd", cmdName.toStdString()}, {"params", params}},
            [&resultLock, &lastFeedback](uint32_t, const nlohmann::json& feedback) {
                std::lock_guard<std::mutex> locker(resultLock);
                lastFeedback = feedback;
            },
            [&resultLock, &resultValue, &callbackSuccess](
                uint32_t, const nlohmann::json& result, bool success) {
                std::lock_guard<std::mutex> locker(resultLock);
                resultValue = result;
                callbackSuccess = success;
            });

        if (!waitForResult) {
            payload = {
                {"success", true},
                {"message", "Command sent"},
                {"goal_id", goalId},
                {"status", "ACCEPTED"},
            };
            writeResultFile(resultFile, payload);
            return 0;
        }

        if (!client.waitForResult(goalId, resultTimeoutMs)) {
            payload = {
                {"success", false},
                {"message", "Timeout waiting for result"},
                {"goal_id", goalId},
                {"status", "TIMEOUT"},
            };
            writeResultFile(resultFile, payload);
            return 1;
        }

        {
            std::lock_guard<std::mutex> locker(resultLock);
            if (resultValue.is_object()) {
                payload = resultValue;
            } else {
                payload = {
                    {"success", callbackSuccess},
                    {"message", resultValue.dump()},
                };
            }
            if (!lastFeedback.is_null() && !lastFeedback.empty()) {
                payload["feedback"] = lastFeedback;
            }
            payload["goal_id"] = goalId;
            payload["status"] = callbackSuccess ? "SUCCEEDED" : "FAILED";
            if (!payload.contains("success")) {
                payload["success"] = callbackSuccess;
            }
            if (!payload.contains("message")) {
                payload["message"] = callbackSuccess ? "Success" : "Command failed";
            }
        }

        writeResultFile(resultFile, payload);
        return payload.value("success", false) ? 0 : 1;
    } catch (const std::exception& e) {
        payload = {
            {"success", false},
            {"message", std::string(e.what())},
            {"status", "ERROR"},
        };
        const QString resultFile = parser.value(resultFileOption).trimmed();
        if (!resultFile.isEmpty()) {
            try {
                writeResultFile(resultFile, payload);
            } catch (...) {
            }
        }
        return 1;
    }
}
