/*
 * AgentWatchdog 实现
 */
#include "recordlab/backend/agent_watchdog.h"
#include <chrono>
#include <iostream>
#include <thread>

namespace recordlab::backend {

static double nowSec() {
  // 使用单调时钟记录健康检查时间，避免系统时间跳变干扰状态判断。
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static nlohmann::json checkDevicePayload(const nlohmann::json &checkResult) {
  // AgentManagerProcess 会把子节点原始结果包在 result 字段里；watchdog
  // 需要读取里面的 product_id/product_ids 才能显示具体眼镜型号。
  if (checkResult.contains("result") && checkResult["result"].is_object()) {
    return checkResult["result"];
  }
  return checkResult;
}

static QString glassesDeviceLabel(const AgentStatus &status) {
  QString name = QString::fromStdString(status.productName);
  const QString displayId = QString::fromStdString(status.productDisplayId);
  if (!displayId.isEmpty() && !name.isEmpty()) {
    return QStringLiteral("%1-%2").arg(displayId, name);
  }
  if (!displayId.isEmpty()) {
    return displayId;
  }
  if (!name.isEmpty()) {
    return name;
  }
  if (status.productId > 0) {
    return QStringLiteral("(%1)").arg(status.productId);
  }
  return QStringLiteral("眼镜");
}

AgentWatchdog::AgentWatchdog(QObject *parent) : QThread(parent) {}

// 析构时等待 watchdog 线程退出，确保不会在对象释放后继续检查 agent。
AgentWatchdog::~AgentWatchdog() { stopWatchdog(); }

void AgentWatchdog::stopWatchdog() {
  // 请求 watchdog 循环退出，并在有限时间内等待线程收尾完成。
  running_ = false;
  if (isRunning())
    wait(5000);
}

// ==================== Registration ====================

void AgentWatchdog::registerPrimaryAgent(
    const std::string &agentName, const nlohmann::json &initDeviceParams) {
  // 当前 watchdog 只强监控一个主 agent；注册新主 agent 时会覆盖旧主目标。
  {
    QMutexLocker locker(&lock_);
    for (auto &[name, status] : registeredAgents_) {
      status.isPrimary = (name == agentName);
    }

    auto &status = registeredAgents_[agentName];
    if (status.agentName.empty()) {
      status.agentName = agentName;
    }
    status.isPrimary = true;
    status.initDeviceParams = initDeviceParams;
    primaryAgent_ = agentName;
  }
  notifyStatusUpdate();
}

void AgentWatchdog::updatePrimaryAgentInitDeviceParams(
    const std::string &agentName, const nlohmann::json &initDeviceParams) {
  {
    QMutexLocker locker(&lock_);
    auto it = registeredAgents_.find(agentName);
    if (it == registeredAgents_.end() || !it->second.isPrimary) {
      return;
    }
    it->second.initDeviceParams = initDeviceParams;
  }
  notifyStatusUpdate();
}

void AgentWatchdog::unregisterPrimaryAgent() {
  // 移除当前主 agent 的监控状态，常用于切换主 agent 或整体停机。
  bool changed = false;
  {
    QMutexLocker locker(&lock_);
    if (!primaryAgent_.empty()) {
      registeredAgents_.erase(primaryAgent_);
      primaryAgent_.clear();
      changed = true;
    }
  }
  if (changed) {
    notifyStatusUpdate();
  }
}

void AgentWatchdog::registerScriptAgents(
    const std::vector<std::string> & /*agentNames*/) {
  // 目前脚本 agent 不纳入 watchdog；保留接口用于未来扩展。
  // Watchdog only monitors primary agent
}

void AgentWatchdog::unregisterScriptAgents(
    const std::vector<std::string> &agentNames) {
  // 清理非主 agent 的监控记录，为脚本批量执行结束留出回收入口。
  bool changed = false;
  {
    QMutexLocker locker(&lock_);
    for (auto &name : agentNames) {
      auto it = registeredAgents_.find(name);
      if (it != registeredAgents_.end() && !it->second.isPrimary) {
        registeredAgents_.erase(it);
        changed = true;
      }
    }
  }
  if (changed) {
    notifyStatusUpdate();
  }
}

void AgentWatchdog::markAgentInitializing(const std::string &agentName,
                                          const std::string &message) {
  // 显式标记 agent 正在 init_device，期间暂停自动健康检查触发的状态切换。
  bool changed = false;
  {
    QMutexLocker locker(&lock_);
    auto it = registeredAgents_.find(agentName);
    if (it != registeredAgents_.end()) {
      it->second.state = AgentStatus::STATE_INITIALIZING;
      it->second.lastError = message;
      changed = true;
    }
  }
  if (changed) {
    notifyStatusUpdate();
  }
}

void AgentWatchdog::markAgentHealthy(const std::string &agentName,
                                     bool updateInitTime) {
  // 将 agent 恢复为 healthy，并在需要时刷新最近初始化完成时间。
  bool changed = false;
  {
    QMutexLocker locker(&lock_);
    auto it = registeredAgents_.find(agentName);
    if (it != registeredAgents_.end()) {
      it->second.state = AgentStatus::STATE_HEALTHY;
      it->second.lastError.clear();
      it->second.consecutiveFailures = 0;
      if (updateInitTime) {
        it->second.lastInitTime = nowSec();
      }
      changed = true;
    }
  }
  if (changed) {
    notifyStatusUpdate();
  }
}

void AgentWatchdog::markAgentDisconnected(const std::string &agentName,
                                          const std::string &error) {
  // 主动把 agent 标记为断开，通常在命令执行失败或 stop/release 后调用。
  bool changed = false;
  {
    QMutexLocker locker(&lock_);
    auto it = registeredAgents_.find(agentName);
    if (it != registeredAgents_.end()) {
      it->second.state = AgentStatus::STATE_DISCONNECTED;
      it->second.lastError = error;
      changed = true;
    }
  }
  if (changed) {
    notifyStatusUpdate();
  }
}

void AgentWatchdog::setAgentDeviceConfig(
    const std::string &agentName,
    const std::unordered_map<int, std::string> &supportedDevices,
    bool suppressDisconnectDialog) {
  // 从 agents_config.json 加载的 supported_devices 映射和热插拔标志写入 AgentStatus。
  QMutexLocker locker(&lock_);
  auto it = registeredAgents_.find(agentName);
  if (it != registeredAgents_.end()) {
    it->second.supportedDevices = supportedDevices;
    it->second.suppressDisconnectDialog = suppressDisconnectDialog;
    std::cout << "[Watchdog] Device config for " << agentName
              << ": " << supportedDevices.size() << " supported device(s)"
              << ", suppress_dialog=" << (suppressDisconnectDialog ? "true" : "false")
              << std::endl;
  }
}

void AgentWatchdog::pauseChecks(const std::string &reason) {
  pausedCheckCount_.fetch_add(1);
  {
    QMutexLocker locker(&lock_);
    if (!reason.empty()) {
      pauseReason_ = reason;
    }
  }
  std::cout << "[Watchdog] Checks paused";
  if (!reason.empty()) {
    std::cout << ": " << reason;
  }
  std::cout << std::endl;
  notifyStatusUpdate();
}

void AgentWatchdog::resumeChecks(const std::string &reason) {
  const int previous = pausedCheckCount_.load();
  if (previous <= 0) {
    return;
  }
  const int remaining = pausedCheckCount_.fetch_sub(1) - 1;
  bool notify = false;
  {
    QMutexLocker locker(&lock_);
    if (remaining <= 0) {
      pausedCheckCount_.store(0);
      pauseReason_.clear();
      notify = true;
    } else if (!reason.empty()) {
      pauseReason_ = reason;
      notify = true;
    }
  }
  std::cout << "[Watchdog] Checks resumed";
  if (!reason.empty()) {
    std::cout << ": " << reason;
  }
  std::cout << std::endl;
  if (notify) {
    notifyStatusUpdate();
  }
}

// ==================== Thread ====================

void AgentWatchdog::run() {
  // QThread 入口只负责设置运行标志并转入实际 watchdog 循环。
  running_ = true;
  watchdogLoop();
}

void AgentWatchdog::watchdogLoop() {
  // 按设定间隔周期性检查主 agent，必要时触发自动恢复或急停。
  std::cout << "[Watchdog] Starting, delay " << startupDelay_ << "s..."
            << std::endl;
  std::this_thread::sleep_for(std::chrono::duration<double>(startupDelay_));
  std::cout << "[Watchdog] Health checks active" << std::endl;

  int checkCount = 0;
  while (running_) {
    try {
      bool hasRegisteredAgents = false;
      int registeredCount = 0;
      {
        QMutexLocker locker(&lock_);
        hasRegisteredAgents = !registeredAgents_.empty();
        registeredCount = static_cast<int>(registeredAgents_.size());
      }

      if (hasRegisteredAgents) {
        if (checksPaused()) {
          if (checkCount % 60 == 0) {
            std::cout << "[Watchdog] Checks paused, skip health check" << std::endl;
          }
          checkCount++;
        } else {
          checkCount++;
          if (checkCount % 60 == 1) {
            std::cout << "[Watchdog] Checking " << registeredCount
                      << " agents..." << std::endl;
          }
          checkAllAgents();
        }
      }
    } catch (const std::exception &e) {
      std::cerr << "[Watchdog] Error: " << e.what() << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::duration<double>(checkInterval_));
  }
  std::cout << "[Watchdog] Stopped" << std::endl;
}

void AgentWatchdog::checkAllAgents() {
  // 当前只真正检查 primary agent，但保留集合语义便于后续扩展到多 agent。
  std::string primaryAgent;
  {
    QMutexLocker locker(&lock_);
    if (!primaryAgent_.empty() && registeredAgents_.count(primaryAgent_)) {
      primaryAgent = primaryAgent_;
    }
  }
  if (!primaryAgent.empty()) {
    checkAgent(primaryAgent);
  }
}

void AgentWatchdog::checkAgent(const std::string &agentName) {
  // 对单个 agent 执行 check，并据返回结果更新状态机、设备信息与失败计数。
  {
    QMutexLocker locker(&lock_);
    auto it = registeredAgents_.find(agentName);
    if (it == registeredAgents_.end() ||
        it->second.state == AgentStatus::STATE_INITIALIZING) {
      return;
    }
  }

  bool success = false;
  std::string errorMessage;
  nlohmann::json checkResult;
  try {
    if (checkCallback_) {
      checkResult = checkCallback_(agentName, "check");
    } else {
      checkResult = {{"success", false}, {"error", "No check callback set"}};
    }

    success = checkResult.value("success", false);
    if (success) {
      handleCheckSuccess(agentName);
    } else {
      errorMessage = checkResult.value("error",
                         checkResult.value("message", std::string("Unknown error")));
      handleCheckFailure(agentName, errorMessage);
    }
  } catch (const std::exception &e) {
    success = false;
    errorMessage = e.what();
    handleCheckFailure(agentName, errorMessage);
  }

  {
    QMutexLocker locker(&lock_);
    auto it = registeredAgents_.find(agentName);
    if (it != registeredAgents_.end()) {
      it->second.lastCheckTime = nowSec();
      if (it->second.state != AgentStatus::STATE_INITIALIZING) {
        it->second.consecutiveFailures =
            success ? 0 : it->second.consecutiveFailures + 1;
        if (!success && !errorMessage.empty()) {
          it->second.lastError = errorMessage;
        }
      }

      // 从 check 结果中提取设备信息。
      const auto devicePayload = checkDevicePayload(checkResult);
      if (success && devicePayload.contains("product_ids") &&
          devicePayload["product_ids"].is_array()) {
        it->second.connectedProductIds.clear();
        for (const auto &pid : devicePayload["product_ids"]) {
          if (pid.is_number_integer()) {
            it->second.connectedProductIds.push_back(pid.get<int>());
          }
        }
        const int firstPid = devicePayload.value("product_id", -1);
        it->second.productId = firstPid;
        it->second.productDisplayId =
            devicePayload.value("product_display_id", std::string());

        const std::string reportedName =
            devicePayload.value("product_name", std::string());
        if (!reportedName.empty()) {
          it->second.productName = reportedName;
        } else if (firstPid > 0) {
          // 从 supportedDevices 映射中解析型号名称
          auto nameIt = it->second.supportedDevices.find(firstPid);
          if (nameIt != it->second.supportedDevices.end()) {
            it->second.productName = nameIt->second;
          } else {
            it->second.productName = std::to_string(firstPid);
          }
        } else {
          it->second.productName.clear();
        }
        it->second.fsn = devicePayload.value("fsn", std::string());
      } else if (!success) {
        it->second.connectedProductIds.clear();
        it->second.productId = -1;
        it->second.productDisplayId.clear();
        it->second.fsn.clear();
        // 保留 productName，断开时仍显示"Helen 已断开"而不是清空
      }
    }
  }
  notifyStatusUpdate();
}

void AgentWatchdog::handleCheckSuccess(const std::string &agentName) {
  // check 成功时，如果 agent 刚从断开恢复，则为主 agent 自动补一次 init_device。
  nlohmann::json initParams;
  bool shouldInit = false;

  {
    QMutexLocker locker(&lock_);
    auto it = registeredAgents_.find(agentName);
    if (it == registeredAgents_.end()) {
      return;
    }

    auto &status = it->second;
    const std::string oldState = status.state;
    status.lastError.clear();

    if (oldState == AgentStatus::STATE_DISCONNECTED) {
      if (status.isPrimary) {
        std::cout << "[Watchdog] " << agentName
                  << " reconnected, initializing..." << std::endl;
        status.state = AgentStatus::STATE_INITIALIZING;
        initParams = status.initDeviceParams;
        shouldInit = true;
      } else {
        status.state = AgentStatus::STATE_HEALTHY;
      }
    } else if (oldState == AgentStatus::STATE_INITIALIZING) {
      return;
    } else if (oldState == AgentStatus::STATE_HEALTHY) {
      status.state = AgentStatus::STATE_HEALTHY;
    }
  }

  if (shouldInit) {
    notifyStatusUpdate();
    initDevice(agentName, initParams);
    return;
  }
}

void AgentWatchdog::handleCheckFailure(const std::string &agentName,
                                       const std::string &error) {
  // 首次从 healthy 退化到失败时触发断开状态。支持热插拔的 agent 保持
  // watchdog 注册关系，继续轮询直到设备重新出现，再自动 init_device。
  bool emitFailure = false;
  bool shouldEstop = false;
  bool suppressDialog = false;

  {
    QMutexLocker locker(&lock_);
    auto it = registeredAgents_.find(agentName);
    if (it == registeredAgents_.end()) {
      return;
    }

    auto &status = it->second;
    const std::string oldState = status.state;
    status.lastError = error;
    suppressDialog = status.suppressDisconnectDialog;

    if (oldState == AgentStatus::STATE_HEALTHY) {
      status.state = AgentStatus::STATE_DISCONNECTED;
      emitFailure = true;
      shouldEstop = !suppressDialog;
      std::cout << "[Watchdog] " << agentName << " disconnected"
                << (suppressDialog ? " (hot-plug, no dialog)" : "")
                << ": " << error << std::endl;
    } else if (oldState == AgentStatus::STATE_INITIALIZING) {
      return;
    }
  }

  // 支持热插拔的 agent 不弹窗，Watchdog 自动等待重连
  if (emitFailure && !suppressDialog) {
    emit agentFailure(QString::fromStdString(agentName),
                      QString::fromStdString(error));
  }
  if (shouldEstop) {
    executeEstop(agentName, error);
  }
}

void AgentWatchdog::initDevice(const std::string &agentName,
                               const nlohmann::json &initDeviceParams) {
  // 通过外部回调重新执行 init_device，作为主 agent 恢复后的自动补救动作。
  nlohmann::json result;
  if (initDeviceCallback_) {
    try {
      result = initDeviceCallback_(agentName, initDeviceParams);
    } catch (...) {
      result = {{"success", false}, {"error", "Init device exception"}};
    }
  } else {
    result = {{"success", false}, {"error", "No init callback"}};
  }

  {
    QMutexLocker locker(&lock_);
    auto it = registeredAgents_.find(agentName);
    if (it == registeredAgents_.end()) {
      return;
    }

    auto &status = it->second;
    if (result.value("success", false)) {
      status.state = AgentStatus::STATE_HEALTHY;
      status.lastInitTime = nowSec();
      status.lastError.clear();
    } else {
      status.state = AgentStatus::STATE_DISCONNECTED;
      if (result.contains("message") && result["message"].is_string()) {
        status.lastError = result["message"].get<std::string>();
      } else if (result.contains("error") && result["error"].is_string()) {
        status.lastError = result["error"].get<std::string>();
      } else {
        status.lastError = "Init device failed";
      }
    }
  }

  notifyStatusUpdate();
}

void AgentWatchdog::executeEstop(const std::string &agentName,
                                 const std::string &reason) {
  // 将 watchdog 发现的硬故障统一升级为紧急停止请求。
  if (estopCallback_) {
    estopCallback_("Agent " + agentName + " disconnected: " + reason);
  }
}

void AgentWatchdog::notifyStatusUpdate() {
  // 汇总全部监控状态并向 UI / 控制器发出统一状态快照。
  nlohmann::json agents;
  int healthy = 0;
  int disconnected = 0;
  int initializing = 0;
  QString summary = QStringLiteral("无监控");
  std::string primaryAgent;
  const bool paused = checksPaused();
  QString pauseReason;

  {
    QMutexLocker locker(&lock_);
    pauseReason = QString::fromStdString(pauseReason_);
    for (auto &[name, s] : registeredAgents_) {
      if (s.state == AgentStatus::STATE_HEALTHY) {
        healthy++;
      } else if (s.state == AgentStatus::STATE_DISCONNECTED) {
        disconnected++;
      } else if (s.state == AgentStatus::STATE_INITIALIZING) {
        initializing++;
      }

      nlohmann::json productIdsJson = nlohmann::json::array();
      for (int pid : s.connectedProductIds) {
        productIdsJson.push_back(pid);
      }

      agents[name] = {{"is_primary", s.isPrimary},
                      {"state", s.state},
                      {"last_error", s.lastError},
                      {"last_check_time", s.lastCheckTime},
                      {"last_init_time", s.lastInitTime},
                      {"consecutive_failures", s.consecutiveFailures},
                      {"product_id", s.productId},
                      {"product_display_id", s.productDisplayId},
                      {"product_name", s.productName},
                      {"fsn", s.fsn},
                      {"connected_product_ids", productIdsJson}};
    }

    // 生成带型号信息的摘要
    primaryAgent = primaryAgent_;
    auto primaryIt = registeredAgents_.find(primaryAgent);

    if (primaryIt != registeredAgents_.end()) {
      const auto &ps = primaryIt->second;
      const QString deviceLabel = glassesDeviceLabel(ps);

      if (paused) {
        summary = QStringLiteral("%1脚本执行中，暂停检查").arg(deviceLabel);
      } else if (ps.state == AgentStatus::STATE_HEALTHY) {
        summary = QStringLiteral("%1已连接").arg(deviceLabel);
      } else if (ps.state == AgentStatus::STATE_INITIALIZING) {
        summary = QStringLiteral("%1正在初始化...").arg(deviceLabel);
      } else {
        if (ps.suppressDisconnectDialog) {
          summary = QStringLiteral("%1已断开，等待重连...").arg(deviceLabel);
        } else {
          summary = QStringLiteral("%1已断开").arg(deviceLabel);
        }
      }
    } else if (!registeredAgents_.empty()) {
      const int total = static_cast<int>(registeredAgents_.size());
      if (initializing > 0) {
        summary = QStringLiteral("%1个Agent | 初始化中").arg(total);
      } else if (disconnected > 0) {
        summary = QStringLiteral("%1个Agent | %2正常 %3断开")
                      .arg(total)
                      .arg(healthy)
                      .arg(disconnected);
      } else {
        summary = QStringLiteral("%1个Agent | 全部正常").arg(total);
      }
    }
  }

  emit statusUpdate({{"type", "status_update"},
                     {"summary", summary.toStdString()},
                     {"checks_paused", paused},
                     {"pause_reason", pauseReason.toStdString()},
                     {"primary_agent", primaryAgent},
                     {"agents", agents}});
}

QString AgentWatchdog::getStatusSummary() const {
  // 生成面向顶部状态栏的简洁 watchdog 摘要文案（含型号信息）。
  QMutexLocker locker(&lock_);
  if (registeredAgents_.empty())
    return QStringLiteral("无监控");

  auto primaryIt = registeredAgents_.find(primaryAgent_);
  if (primaryIt != registeredAgents_.end()) {
    const auto &ps = primaryIt->second;
    const QString deviceLabel = glassesDeviceLabel(ps);

    if (ps.state == AgentStatus::STATE_HEALTHY) {
      return QStringLiteral("%1已连接").arg(deviceLabel);
    } else if (ps.state == AgentStatus::STATE_INITIALIZING) {
      return QStringLiteral("%1正在初始化...").arg(deviceLabel);
    } else {
      return QStringLiteral("%1已断开").arg(deviceLabel);
    }
  }

  int healthy = 0, disconnected = 0, initializing = 0;
  for (auto &[_, s] : registeredAgents_) {
    if (s.state == AgentStatus::STATE_HEALTHY)
      healthy++;
    else if (s.state == AgentStatus::STATE_DISCONNECTED)
      disconnected++;
    else if (s.state == AgentStatus::STATE_INITIALIZING)
      initializing++;
  }

  int total = static_cast<int>(registeredAgents_.size());
  if (initializing > 0)
    return QStringLiteral("%1个Agent | 初始化中").arg(total);
  if (disconnected > 0)
    return QStringLiteral("%1个Agent | %2正常 %3断开")
        .arg(total)
        .arg(healthy)
        .arg(disconnected);
  return QStringLiteral("%1个Agent | 全部正常").arg(total);
}

} // namespace recordlab::backend
