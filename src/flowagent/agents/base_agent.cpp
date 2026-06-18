/*
 * BaseAgent - Agent 基类 实现
 *
 */

#include "recordlab/flowagent/agents/base_agent.h"

#include <action.h>     // echo::ActionClient
#include <master.h>
#include <subscriber.h> // echo::Subscriber

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QThread>

#include <chrono>
#include <iostream>
#include <thread>

#include "recordlab/bsp/xreal_runtime_locator.h"
#include "recordlab/core/compatibility_contract.h"
#include "recordlab/flowagent/agents/legacy_remote_action_client.h"

namespace recordlab::flowagent::agents {

namespace {

QString cleanedOrEmpty(const QString& value)
{
  // 统一清理路径字符串；空白输入返回空字符串而不是当前目录。
  return value.trimmed().isEmpty() ? QString() : QDir::cleanPath(value.trimmed());
}

}  // namespace

// ============================================================================
// CmdStorage 实现
// ============================================================================

void CmdStorage::initGoal(uint32_t goalId) {
  // 为一个新的 action goal 预留反馈和结果槽位。
  QMutexLocker locker(&lock_);
  feedbacks_.emplace(goalId, std::vector<nlohmann::json>{});
  results_.emplace(goalId, std::nullopt);
}

void CmdStorage::addFeedback(uint32_t goalId, const nlohmann::json &feedback) {
  // 追加某个 goal 的反馈消息，供调试或上层轮询查看。
  QMutexLocker locker(&lock_);
  feedbacks_[goalId].push_back(feedback);
}

void CmdStorage::setResult(uint32_t goalId, const nlohmann::json &result,
                           bool success) {
  // 记录最终结果及成功标志，标记该 goal 已完成。
  QMutexLocker locker(&lock_);
  results_[goalId] = ResultEntry{result, success};
}

std::optional<CmdStorage::ResultEntry>
CmdStorage::getResult(uint32_t goalId) const {
  // 读取某个 goal 的最终结果；未完成时返回空。
  QMutexLocker locker(&lock_);
  auto it = results_.find(goalId);
  if (it == results_.end())
    return std::nullopt;
  return it->second;
}

std::vector<nlohmann::json> CmdStorage::getFeedbacks(uint32_t goalId) const {
  // 返回反馈列表副本，避免调用方持有内部容器引用。
  QMutexLocker locker(&lock_);
  auto it = feedbacks_.find(goalId);
  if (it == feedbacks_.end())
    return {};
  return it->second; // 返回副本
}

bool CmdStorage::hasGoal(uint32_t goalId) const {
  // 判断指定 goal 是否已经建立过存储槽位。
  QMutexLocker locker(&lock_);
  return results_.count(goalId) > 0;
}

void CmdStorage::clearGoal(uint32_t goalId) {
  // 删除某个 goal 的反馈和结果缓存，释放长期运行中的积累数据。
  QMutexLocker locker(&lock_);
  feedbacks_.erase(goalId);
  results_.erase(goalId);
}

void CmdStorage::reset() {
  // 一次性清空所有命令结果缓存，通常在 agent reset 时调用。
  QMutexLocker locker(&lock_);
  feedbacks_.clear();
  results_.clear();
}

// ============================================================================
// BaseAgent 实现
// ============================================================================

BaseAgent::BaseAgent(const QString &name, const QString &subnodePath,
                     const QString &subnodeHost, int goalPort, int feedbackPort,
                     const QString &rootPath, const QVariantMap &customParams,
                     QObject *parent)
    : QObject(parent), name_(name), subnodePath_(subnodePath),
      subnodeHost_(subnodeHost), goalPort_(goalPort),
      feedbackPort_(feedbackPort), rootPath_(rootPath),
      customParams_(customParams) {
  // 构造阶段只保存连接参数和自定义配置，不做任何 IPC 建连操作。
  std::cout << "[" << name_.toStdString() << "] Agent initialized" << std::endl;
}

BaseAgent::~BaseAgent() {
  // 析构时若仍处于连接态，统一走 disconnect 做完整收尾。
  if (isConnected_) {
    disconnect();
  }
}

bool BaseAgent::isRemoteMode() const {
  // 没有本地 subnodePath 时视为远程模式，表示子节点已在外部机器上运行。
  return subnodePath_.trimmed().isEmpty();
}

// ==================== connect / disconnect ====================

CmdResult BaseAgent::connect() {
  // 建立 agent 连接：必要时拉起本地子节点，然后创建 ActionClient。
  std::cout << "[" << name_.toStdString() << "] Connecting to SubNode..."
            << std::endl;

  if (isConnected_) {
    return {true, "Already connected"};
  }

  auto cleanupAndFail = [this](const QString &message) -> CmdResult {
    std::cerr << "[" << name_.toStdString() << "] " << message.toStdString()
              << std::endl;
    disconnect();
    return {false, message};
  };

  try {
    if (isRemoteMode()) {
      std::cout << "[" << name_.toStdString()
                << "] Remote mode: subnode running on external machine"
                << std::endl;
    } else {
      auto result = launchSubnode();
      if (!result.success) {
        return cleanupAndFail("Failed to launch subnode: " + result.message);
      }
    }

    auto result = createActionClient();
    if (!result.success) {
      return cleanupAndFail("Failed to create ActionClient: " + result.message);
    }

    isConnected_ = true;
    QString modeStr = isRemoteMode() ? " (remote mode)" : "";
    std::cout << "[" << name_.toStdString() << "] Connected successfully"
              << modeStr.toStdString() << std::endl;
    return {true, "Connected" + modeStr};

  } catch (const std::exception &e) {
    return cleanupAndFail(QString("Connection error: %1").arg(e.what()));
  }
}

CmdResult BaseAgent::disconnect() {
  // 断开连接时尽量先优雅释放设备，再回收 action client、订阅器和子进程。
  std::cout << "[" << name_.toStdString() << "] Disconnecting..." << std::endl;

  try {
    try {
      gracefulReleaseBeforeDisconnect();
    } catch (...) {
      // 忽略
    }

    try {
      closeActionClient();
    } catch (...) {
      // 忽略
    }

    try {
      closeAllSubscribers();
    } catch (...) {
      // 忽略
    }

    if (!isRemoteMode()) {
      try {
        terminateSubnode();
      } catch (...) {
        // 忽略
      }
    }

    isConnected_ = false;
    std::cout << "[" << name_.toStdString() << "] Disconnected successfully"
              << std::endl;
    return {true, "Disconnected"};

  } catch (...) {
    isConnected_ = false;
    return {true, "Disconnected with warnings"};
  }
}

// ==================== cmd ====================

CmdResult BaseAgent::cmd(const QString &cmdName, const nlohmann::json &params,
                         bool waitForResult, double timeout) {
  // 通过 ActionClient 发送命令；可选择同步等待最终结果或仅发送 goal。
  QMutexLocker commandLocker(&commandLock_);
  if (!isConnected_) {
    return {false, "Not connected"};
  }

  const bool verboseCommand = cmdName != QStringLiteral("check");
  if (verboseCommand) {
    std::cout << "[" << name_.toStdString() << "] Sending command '"
              << cmdName.toStdString() << "'" << std::endl;
  }

  try {
    nlohmann::json goal = {{"cmd", cmdName.toStdString()}, {"params", params}};

    uint32_t goalId = 0;
    auto feedbackCallback = [this](uint32_t goalId,
                                   const nlohmann::json &feedback) {
      cmdStorage_.addFeedback(goalId, feedback);
    };
    auto resultCallback = [this](uint32_t goalId,
                                 const nlohmann::json &result, bool success) {
      cmdStorage_.setResult(goalId, result, success);
    };
    if (legacyRemoteActionClient_) {
      goalId = legacyRemoteActionClient_->sendGoal(goal, feedbackCallback,
                                                   resultCallback);
    } else {
      goalId = actionClient_->sendGoal(goal, feedbackCallback, resultCallback);
    }
    if (!cmdStorage_.hasGoal(goalId)) {
      cmdStorage_.initGoal(goalId);
    }

    if (!waitForResult) {
      return {true,
              QString("Command sent (non-blocking), goal_id: %1").arg(goalId)};
    }

    // 同步等待结果
    auto startTime = std::chrono::steady_clock::now();
    constexpr int checkIntervalMs = 100;

    while (true) {
      if (isCmdDone(goalId)) {
        auto optResult = cmdStorage_.getResult(goalId);
        if (optResult && optResult->success) {
          if (verboseCommand) {
            std::cout << "[" << name_.toStdString() << "] Command '"
                      << cmdName.toStdString() << "' succeeded" << std::endl;
          }
          return {true, "Success", optResult->result};
        } else {
          QString errorMsg = "Unknown error";
          if (optResult) {
            try {
              if (optResult->result.is_object() &&
                  optResult->result.contains("message")) {
                errorMsg = QString::fromStdString(
                    optResult->result["message"].get<std::string>());
              } else {
                errorMsg = QString::fromStdString(optResult->result.dump());
              }
            } catch (...) {
              errorMsg = "Error parsing result";
            }
          }
          std::cerr << "[" << name_.toStdString() << "] Command '"
                    << cmdName.toStdString()
                    << "' failed: " << errorMsg.toStdString() << std::endl;
          return {false, errorMsg,
                  optResult ? optResult->result : nlohmann::json{}};
        }
      }

      auto elapsed = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - startTime)
                         .count();
      if (elapsed >= timeout) {
        std::cerr << "[" << name_.toStdString() << "] Command '"
                  << cmdName.toStdString() << "' timeout" << std::endl;
        return {false, "Timeout"};
      }

      QCoreApplication::processEvents(QEventLoop::AllEvents, checkIntervalMs);
      std::this_thread::sleep_for(std::chrono::milliseconds(checkIntervalMs));
    }

  } catch (const std::exception &e) {
    std::cerr << "[" << name_.toStdString() << "] Command error: " << e.what()
              << std::endl;
    return {false, QString::fromUtf8(e.what())};
  }
}

CmdStatus BaseAgent::getCmdStatus(uint32_t goalId) {
  // 查询某个 goal 当前是否完成以及最终状态。
  if (!cmdStorage_.hasGoal(goalId)) {
    return {false, {}, {}};
  }

  auto optResult = cmdStorage_.getResult(goalId);
  if (!optResult.has_value()) {
    return {false, {}, {}};
  }

  return {true, optResult->success ? "SUCCEEDED" : "FAILED", optResult->result};
}

void BaseAgent::reset() {
  // 清空 topic 缓存和命令结果，不影响当前连接本身。
  std::cout << "[" << name_.toStdString() << "] Resetting state..."
            << std::endl;
  {
    QMutexLocker locker(&topicDataLock_);
    topicData_.clear();
    topicReceiveCount_.clear();
  }
  cmdStorage_.reset();
}

CmdResult BaseAgent::getRootPathSync() {
  // 尝试从子节点同步获取真实 root_path；失败时回退到本地配置值。
  if (!isConnected_) {
    return {false, "Not connected to subnode",
            nlohmann::json{{"root_path", rootPath_.toStdString()}}};
  }

  auto result = cmd("get_root_path", {}, true, 5.0);
  if (result.success) {
    try {
      if (result.result.is_object() && result.result.contains("root_path")) {
        return {true, "Root path retrieved from subnode", result.result};
      }
    } catch (...) {
    }
  }

  return {false, "Failed to retrieve root_path, using local config",
          nlohmann::json{{"root_path", rootPath_.toStdString()}}};
}

// ==================== copy_folder_from_remote ====================

CmdResult BaseAgent::copyFolderFromRemote(
    const QString &remotePath, const QString &localPath, const QString &method,
    const QString &remoteHost, const QString &remoteUser,
    const QString &remotePassword, const QString &adbDevice) {
  // 通过 scp 或 adb 将远端目录拉回本地，主要用于录制产物回收。
  std::cout << "[" << name_.toStdString()
            << "] Copying folder from remote: " << remotePath.toStdString()
            << " -> " << localPath.toStdString()
            << " (method: " << method.toStdString() << ")" << std::endl;

  try {
    // 确保本地目标目录存在
    QDir().mkpath(QFileInfo(localPath).absolutePath());

    if (method.toLower() == "scp") {
      QString host = remoteHost.isEmpty() ? subnodeHost_ : remoteHost;
      QString user = remoteUser;
      if (user.isEmpty()) {
        user = customParams_.value("remote_user").toString();
        if (user.isEmpty()) {
          user = qEnvironmentVariable("USER", "root");
        }
      }
      QString password = remotePassword;
      if (password.isEmpty()) {
        password = customParams_.value("remote_password").toString();
      }

      QStringList args;
      if (!password.isEmpty()) {
        args << "-p" << password << "scp" << "-r"
             << "-o" << "StrictHostKeyChecking=no"
             << "-o" << "UserKnownHostsFile=/dev/null"
             << QString("%1@%2:%3").arg(user, host, remotePath) << localPath;

        QProcess proc;
        proc.start("sshpass", args);
        proc.waitForFinished(300000);
        if (proc.exitCode() != 0) {
          return {false,
                  QString::fromUtf8(proc.readAllStandardError()).trimmed()};
        }
      } else {
        args << "-r"
             << "-o" << "StrictHostKeyChecking=no"
             << "-o" << "UserKnownHostsFile=/dev/null"
             << QString("%1@%2:%3").arg(user, host, remotePath) << localPath;

        QProcess proc;
        proc.start("scp", args);
        proc.waitForFinished(300000);
        if (proc.exitCode() != 0) {
          return {false,
                  QString::fromUtf8(proc.readAllStandardError()).trimmed()};
        }
      }

      return {true, "Folder copied via SCP"};
    }

    if (method.toLower() == "adb") {
      QStringList args;
      if (!adbDevice.isEmpty()) {
        args << "-s" << adbDevice;
      }
      args << "pull" << remotePath << localPath;

      QProcess proc;
      proc.start("adb", args);
      proc.waitForFinished(300000);
      if (proc.exitCode() != 0) {
        return {false,
                QString::fromUtf8(proc.readAllStandardError()).trimmed()};
      }
      return {true, "Folder copied via ADB"};
    }

    return {false,
            QString("Unsupported method: %1. Use 'scp' or 'adb'").arg(method)};

  } catch (const std::exception &e) {
    return {false, QString("Copy error: %1").arg(e.what())};
  }
}

// ==================== Subscriber ====================

void BaseAgent::createSubscriber(int port, const QString &topic,
                                 const QString & /*encoding*/) {
  // 为指定端口创建一个订阅器，并可选记录 topicName 到端口的映射。
  if (subscribers_.count(port) > 0) {
    return;
  }

  try {
    std::string topicStr = topic.isEmpty() ? "" : topic.toStdString();

    // 使用 echo::Subscriber 的 RawCallback 模式
    auto sub = std::make_unique<echo::Subscriber>(
        topicStr,
        [this, port](const std::string &data) {
          this->onTopicData(port, data);
        },
        true // raw mode
    );

    subscribers_[port] = std::move(sub);

    if (!topic.isEmpty()) {
      QMutexLocker locker(&topicDataLock_);
      topicToPort_[topic.toStdString()] = port;
    }

    std::cout << "[" << name_.toStdString() << "] Subscribed to port " << port;
    if (!topic.isEmpty()) {
      std::cout << " (topic: " << topic.toStdString() << ")";
    }
    std::cout << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "[" << name_.toStdString() << "] Failed to subscribe port "
              << port << ": " << e.what() << std::endl;
  }
}

void BaseAgent::onTopicData(int port, const std::string &rawData) {
  // 收到 topic 数据后优先尝试解析 JSON；失败时退回为原始字符串缓存。
  try {
    auto data = nlohmann::json::parse(rawData);
    QMutexLocker locker(&topicDataLock_);
    topicData_[port] = std::move(data);
    topicReceiveCount_[port]++;
  } catch (...) {
    // 非JSON数据，存储为字符串
    QMutexLocker locker(&topicDataLock_);
    topicData_[port] = rawData;
    topicReceiveCount_[port]++;
  }
}

nlohmann::json BaseAgent::getTopicData(int port) {
  // 返回指定端口最近一条收到的 topic 数据。
  QMutexLocker locker(&topicDataLock_);
  auto it = topicData_.find(port);
  if (it != topicData_.end())
    return it->second;
  return nullptr;
}

nlohmann::json BaseAgent::getTopicDataByName(const QString &topicName) {
  // 先通过 topicName 找到端口，再返回对应缓存的最新消息。
  QMutexLocker locker(&topicDataLock_);
  auto it = topicToPort_.find(topicName.toStdString());
  if (it == topicToPort_.end())
    return nullptr;
  auto dataIt = topicData_.find(it->second);
  if (dataIt != topicData_.end())
    return dataIt->second;
  return nullptr;
}

int BaseAgent::getTopicReceiveCount(int port) {
  // 返回指定端口累计收到的消息条数。
  QMutexLocker locker(&topicDataLock_);
  auto it = topicReceiveCount_.find(port);
  return (it != topicReceiveCount_.end()) ? it->second : 0;
}

// ==================== 内部方法 ====================

bool BaseAgent::isCmdDone(uint32_t goalId) {
  // 只要结果槽位里已经出现最终结果，就认为该命令完成。
  if (!cmdStorage_.hasGoal(goalId))
    return false;
  return cmdStorage_.getResult(goalId).has_value();
}

void BaseAgent::gracefulReleaseBeforeDisconnect() {
  // 对本地设备型 agent，在断开前尝试 stop/release 以减少资源泄漏。
  if (isRemoteMode() || !isConnected_ || !actionClient_)
    return;

  struct CmdPair {
    QString name;
    double timeout;
  };
  for (const auto &cp :
       {CmdPair{"stop_device", 3.0}, CmdPair{"release_device", 5.0}}) {
    auto result = sendCommandSyncDirect(cp.name, {}, cp.timeout);
    if (result.success) {
      std::cout << "[" << name_.toStdString() << "] Graceful "
                << cp.name.toStdString() << " succeeded before disconnect"
                << std::endl;
    }
  }
}

CmdResult BaseAgent::sendCommandSyncDirect(const QString &cmdName,
                                           const nlohmann::json &params,
                                           double timeout) {
  // disconnect 路径使用的简化同步命令发送，不再打印额外命令日志。
  QMutexLocker commandLocker(&commandLock_);
  if (!isConnected_ || (!actionClient_ && !legacyRemoteActionClient_)) {
    return {false, "Not connected"};
  }

  try {
    nlohmann::json goal = {{"cmd", cmdName.toStdString()}, {"params", params}};

    uint32_t goalId = 0;
    auto feedbackCallback = [this](uint32_t goalId,
                                   const nlohmann::json &feedback) {
      cmdStorage_.addFeedback(goalId, feedback);
    };
    auto resultCallback = [this](uint32_t goalId,
                                 const nlohmann::json &result, bool success) {
      cmdStorage_.setResult(goalId, result, success);
    };
    if (legacyRemoteActionClient_) {
      goalId = legacyRemoteActionClient_->sendGoal(goal, feedbackCallback,
                                                   resultCallback);
    } else {
      goalId = actionClient_->sendGoal(goal, feedbackCallback, resultCallback);
    }
    if (!cmdStorage_.hasGoal(goalId)) {
      cmdStorage_.initGoal(goalId);
    }

    auto startTime = std::chrono::steady_clock::now();
    while (true) {
      if (isCmdDone(goalId)) {
        auto optResult = cmdStorage_.getResult(goalId);
        if (optResult && optResult->success) {
          return {true, "Success", optResult->result};
        }
        QString msg = "Unknown error";
        if (optResult) {
          try {
            if (optResult->result.is_object() &&
                optResult->result.contains("message")) {
              msg = QString::fromStdString(
                  optResult->result["message"].get<std::string>());
            }
          } catch (...) {
          }
        }
        return {false, msg};
      }

      auto elapsed = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - startTime)
                         .count();
      if (elapsed >= timeout) {
        return {false, QString("Timeout waiting for %1").arg(cmdName)};
      }

      QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  } catch (const std::exception &e) {
    return {false, QString::fromUtf8(e.what())};
  }
}

void BaseAgent::cleanupStaleSubnodes() {
  // 启动前扫描并清理同一入口/端口遗留的旧子节点进程，避免端口冲突。
  if (subnodePath_.isEmpty())
    return;

  try {
    QProcess psProc;
    psProc.start("ps", {"-eo", "pid=,args="});
    if (!psProc.waitForFinished(2000))
      return;
    if (psProc.exitCode() != 0)
      return;

    QString output = QString::fromUtf8(psProc.readAllStandardOutput());
    qint64 currentPid = QCoreApplication::applicationPid();
    QString targetPath = QFileInfo(subnodePath_).absoluteFilePath();
    QString goalPortFlag = QString("--goal-port %1").arg(goalPort_);

    QList<qint64> stalePids;
    for (const auto &line : output.split('\n', Qt::SkipEmptyParts)) {
      auto parts =
          line.trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
      if (parts.size() < 2)
        continue;

      bool ok;
      qint64 pid = parts[0].toLongLong(&ok);
      if (!ok || pid == currentPid)
        continue;

      QString cmdline = parts.mid(1).join(' ');
      if (!cmdline.contains(targetPath) || !cmdline.contains(goalPortFlag))
        continue;
      stalePids.append(pid);
    }

    if (stalePids.isEmpty())
      return;

    std::cout << "[" << name_.toStdString()
              << "] Found stale subnode processes, cleaning up..." << std::endl;

    // SIGTERM
    for (qint64 pid : stalePids) {
      QProcess::execute("kill", {QString::number(pid)});
    }

    // 等待2秒
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // SIGKILL 还存活的进程
    for (qint64 pid : stalePids) {
      if (QFileInfo::exists(QString("/proc/%1").arg(pid))) {
        QProcess::execute("kill", {"-9", QString::number(pid)});
      }
    }
  } catch (...) {
    // 忽略清理失败
  }
}

CmdResult BaseAgent::launchSubnode() {
  // 根据入口文件类型选择合适启动程序，并注入需要的运行时环境。
  if (isRemoteMode()) {
    return {true, "Remote subnode, no local process to launch"};
  }

  if (!QFileInfo::exists(subnodePath_)) {
    return {false, QString("Subnode entry not found: %1").arg(subnodePath_)};
  }

  std::cout << "[" << name_.toStdString()
            << "] Launching subnode from: " << subnodePath_.toStdString()
            << std::endl;

  cleanupStaleSubnodes();

  QString program;
  QStringList args;

  // 当前工程既支持旧版 Python 子节点，也支持新的 C++ 可执行子节点。
  // 因此这里不能再一律用 python3 启动，而要根据入口文件类型选择程序。
  if (subnodePath_.endsWith(".py")) {
    program = QStringLiteral("python3");
    args << subnodePath_;
  } else if (subnodePath_.endsWith(".sh")) {
    program = QStringLiteral("bash");
    args << subnodePath_;
  } else {
    program = subnodePath_;
  }

  args << "--name"
       << name_
       << "--host"
       << subnodeHost_
       << "--goal-port"
       << QString::number(goalPort_)
       << "--feedback-port"
       << QString::number(feedbackPort_)
       << "--root-path"
       << rootPath_;

  // 添加自定义参数
  for (auto it = customParams_.constBegin(); it != customParams_.constEnd();
       ++it) {
    if (it.value().typeId() == QMetaType::Bool) {
      if (it.value().toBool()) {
        args << ("--" + it.key());
      }
    } else {
      args << ("--" + it.key()) << it.value().toString();
    }
  }

  std::cout << "[" << name_.toStdString()
            << "][LAUNCH] program=" << program.toStdString()
            << " args=" << args.join(QLatin1Char(' ')).toStdString()
            << " subnodePath=" << subnodePath_.toStdString()
            << " rootPath=" << rootPath_.toStdString() << std::endl;

  subnodeProcess_ = std::make_unique<QProcess>();
  auto environment = QProcessEnvironment::systemEnvironment();
  const bool usesXrealRuntime =
      name_ == QString::fromUtf8(recordlab::core::compat::kPrimaryBspAgent);
  if (usesXrealRuntime) {
    const QString appRoot = cleanedOrEmpty(qEnvironmentVariable("RECORDLABC_ROOT")).isEmpty()
        ? QStringLiteral(RECORDLABC_SOURCE_DIR)
        : QDir::cleanPath(qEnvironmentVariable("RECORDLABC_ROOT"));
    const auto runtime = recordlab::bsp::XrealRuntimeLocator::probe(appRoot);
    if (runtime.projectLocalRuntimeAvailable()) {
      QStringList libraryPaths;
      libraryPaths << runtime.nativeLibDir << runtime.qtLibDir << runtime.shibokenDir;
      const QString existingLdLibraryPath = environment.value(QStringLiteral("LD_LIBRARY_PATH"));
      if (!existingLdLibraryPath.trimmed().isEmpty()) {
        libraryPaths << existingLdLibraryPath;
      }
      environment.insert(QStringLiteral("LD_LIBRARY_PATH"), libraryPaths.join(QLatin1Char(':')));
      environment.insert(QStringLiteral("QT_PLUGIN_PATH"), runtime.qtPluginsDir);
      environment.insert(
          QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
          QDir(runtime.qtPluginsDir).filePath(QStringLiteral("platforms")));
      environment.insert(QStringLiteral("RECORDLABC_XREAL_RUNTIME_ROOT"), runtime.runtimeRoot);
      environment.insert(QStringLiteral("RECORDLABC_XREAL_SITE_PACKAGES"), runtime.sitePackagesPath);
      environment.insert(QStringLiteral("RECORDLABC_XREAL_GLASSES_SERVER"), runtime.glassesServerPath);
      std::cout << "[" << name_.toStdString()
                << "] Using project-local XREAL runtime: "
                << runtime.summary().toStdString() << std::endl;
    } else {
      std::cerr << "[" << name_.toStdString()
                << "] Project-local XREAL runtime not ready: "
                << runtime.blockers.join(QStringLiteral(" | ")).toStdString()
                << std::endl;
    }
  }
  subnodeProcess_->setProcessEnvironment(environment);
  const QString explicitProjectRoot =
      cleanedOrEmpty(environment.value(QStringLiteral("RECORDLABC_ROOT")));
  if (!explicitProjectRoot.isEmpty() && QFileInfo(explicitProjectRoot).isDir()) {
    subnodeProcess_->setWorkingDirectory(explicitProjectRoot);
  } else {
#ifdef RECORDLABC_SOURCE_DIR
    const QString compiledProjectRoot =
        cleanedOrEmpty(QString::fromUtf8(RECORDLABC_SOURCE_DIR));
    if (!compiledProjectRoot.isEmpty() && QFileInfo(compiledProjectRoot).isDir()) {
      subnodeProcess_->setWorkingDirectory(compiledProjectRoot);
    }
#endif
  }
  std::cout << "[" << name_.toStdString()
            << "][LAUNCH] cwd="
            << subnodeProcess_->workingDirectory().toStdString()
            << std::endl;
  QObject::connect(subnodeProcess_.get(), &QProcess::readyReadStandardOutput, this,
                   [this]() {
                     const auto data = subnodeProcess_->readAllStandardOutput();
                     if (!data.trimmed().isEmpty()) {
                       std::cout << "[" << name_.toStdString()
                                 << " subnode stdout] " << data.toStdString();
                     }
                   });
  QObject::connect(subnodeProcess_.get(), &QProcess::readyReadStandardError, this,
                   [this]() {
                     const auto data = subnodeProcess_->readAllStandardError();
                     if (!data.trimmed().isEmpty()) {
                       std::cerr << "[" << name_.toStdString()
                                 << " subnode stderr] " << data.toStdString();
                     }
                   });
  QObject::connect(
      subnodeProcess_.get(),
      qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
      [this](int exitCode, QProcess::ExitStatus exitStatus) {
        std::cerr << "[" << name_.toStdString()
                  << " subnode finished] exitCode=" << exitCode
                  << " exitStatus=" << static_cast<int>(exitStatus)
                  << std::endl;
      });
  QObject::connect(subnodeProcess_.get(), &QProcess::errorOccurred, this,
                   [this](QProcess::ProcessError error) {
                     std::cerr << "[" << name_.toStdString()
                               << " subnode process error] "
                               << static_cast<int>(error) << ": "
                               << subnodeProcess_->errorString().toStdString()
                               << std::endl;
                   });
  subnodeProcess_->start(program, args);

  if (!subnodeProcess_->waitForStarted(5000)) {
    return {false, "SubNode failed to start"};
  }

  std::cout << "[" << name_.toStdString()
            << "] Subnode process created (PID=" << subnodeProcess_->processId()
            << ")" << std::endl;

  // 等待1秒确认进程存活
  std::this_thread::sleep_for(std::chrono::seconds(1));

  if (subnodeProcess_->state() == QProcess::NotRunning) {
    return {false, "SubNode exited immediately"};
  }

  return {true,
          QString("Launched (PID: %1)").arg(subnodeProcess_->processId())};
}

CmdResult BaseAgent::terminateSubnode() {
  // 优先 terminate，超时仍未退出时再强制 kill 本地子节点进程。
  if (!subnodeProcess_) {
    return {true, "Subnode not running"};
  }

  qint64 pid = subnodeProcess_->processId();
  std::cout << "[" << name_.toStdString()
            << "] Terminating subnode (PID: " << pid << ")..." << std::endl;

  subnodeProcess_->terminate();

  if (!subnodeProcess_->waitForFinished(10000)) {
    std::cerr << "[" << name_.toStdString()
              << "] Forcing kill of subnode (PID: " << pid << ")" << std::endl;
    subnodeProcess_->kill();
    subnodeProcess_->waitForFinished();
  }

  subnodeProcess_.reset();
  return {true, QString("Terminated (PID: %1)").arg(pid)};
}

CmdResult BaseAgent::createActionClient() {
  // 等待 action server 注册完成后创建 ActionClient，避免过早连接失败。
  if (actionClient_ || legacyRemoteActionClient_) {
    return {true, "ActionClient already exists"};
  }

  try {
    std::string actionName = name_.toStdString() + "_actions";
    int timeoutMs = isRemoteMode() ? 10000 : 5000;

    if (isRemoteMode()) {
      std::cout << "[" << name_.toStdString()
                << "] Creating legacy remote ActionClient for tcp://"
                << subnodeHost_.toStdString() << ":" << goalPort_ << " / :"
                << feedbackPort_ << std::endl;
      legacyRemoteActionClient_ =
          std::make_unique<LegacyRemoteActionClient>(
              subnodeHost_.toStdString(), goalPort_, feedbackPort_, 5000);
      std::string errorMessage;
      if (!legacyRemoteActionClient_->waitForServer(timeoutMs, &errorMessage)) {
        legacyRemoteActionClient_.reset();
        return {false,
                QStringLiteral("Remote ActionServer not reachable: %1")
                    .arg(QString::fromStdString(errorMessage))};
      }
      return {true, "Legacy remote ActionClient created"};
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const std::string sendGoalService = actionName + "/send_goal";
    const std::string cancelService = actionName + "/cancel";

    std::cout << "[" << name_.toStdString()
              << "] Waiting for ActionServer registration for '" << actionName
              << "' (timeout: " << timeoutMs << "ms)..." << std::endl;

    while (std::chrono::steady_clock::now() < deadline) {
      const auto goalServices =
          echo::MasterClient::getInstance().queryService(sendGoalService);
      const auto cancelServices =
          echo::MasterClient::getInstance().queryService(cancelService);
      if (!goalServices.empty() && !cancelServices.empty()) {
        break;
      }

      if (subnodeProcess_ && subnodeProcess_->state() == QProcess::NotRunning) {
        return {false,
                QStringLiteral("SubNode exited before ActionServer became ready")};
      }

      QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const auto goalServices =
        echo::MasterClient::getInstance().queryService(sendGoalService);
    const auto cancelServices =
        echo::MasterClient::getInstance().queryService(cancelService);
    if (goalServices.empty() || cancelServices.empty()) {
      return {false,
              QStringLiteral("ActionServer not registered in time: %1 / %2")
                  .arg(QString::fromStdString(sendGoalService),
                       QString::fromStdString(cancelService))};
    }

    std::cout << "[" << name_.toStdString() << "] Creating ActionClient..."
              << std::endl;
    actionClient_ = std::make_unique<echo::ActionClient>(actionName);

    // ZMQ PUB-SUB slow joiner 缓解
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "[" << name_.toStdString() << "] ActionClient created"
              << std::endl;
    return {true, "ActionClient created"};

  } catch (const std::exception &e) {
    std::cerr << "[" << name_.toStdString()
              << "] Error creating ActionClient: " << e.what() << std::endl;
    actionClient_.reset();
    return {false, QString::fromUtf8(e.what())};
  }
}

void BaseAgent::closeActionClient() {
  // 释放 ActionClient，关闭后续命令通道。
  if (!actionClient_ && !legacyRemoteActionClient_)
    return;

  std::cout << "[" << name_.toStdString() << "] Closing ActionClient..."
            << std::endl;
  actionClient_.reset();
  legacyRemoteActionClient_.reset();
  std::cout << "[" << name_.toStdString() << "] ActionClient closed"
            << std::endl;
}

void BaseAgent::closeAllSubscribers() {
  // 释放全部 topic 订阅器，断开数据回流路径。
  if (subscribers_.empty())
    return;

  std::cout << "[" << name_.toStdString() << "] Closing all subscribers..."
            << std::endl;
  subscribers_.clear();
  std::cout << "[" << name_.toStdString() << "] All subscribers closed"
            << std::endl;
}

} // namespace recordlab::flowagent::agents
