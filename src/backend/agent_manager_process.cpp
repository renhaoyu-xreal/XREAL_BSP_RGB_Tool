/*
 * AgentManagerProcess 实现
 *
 * C++ 版本使用 QThread 替代 multiprocessing.Process，
 * 命令通过线程安全队列传递，信号用于跨线程通知。
 */
#include "recordlab/backend/agent_manager_process.h"
#include "recordlab/common/commands.h"
#include "recordlab/core/compatibility_contract.h"
#include "recordlab/flowagent/agents/base_agent.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <iostream>
#include <thread>

namespace recordlab::backend {

using namespace recordlab::common;

namespace {

double commandTimeoutSeconds(const std::string& cmdName)
{
  // 为不同命令配置更符合实际耗时的同步超时，避免 init/start 过早误判失败。
  if (cmdName == "init_device") {
    return 120.0;
  }
  if (cmdName == "start_device") {
    // BSP start_device can run startSensors + configureGlasses, each with its
    // own 30s guard, and may need rollback on failure. Keep the outer action
    // timeout comfortably above the inner device-operation budget.
    return 120.0;
  }
  if (cmdName == "restart_device") {
    return 240.0;
  }
  if (cmdName == "stop_record") {
    return 300.0;
  }
  if (cmdName == "stop_device" || cmdName == "release_device") {
    return 45.0;
  }
  if (cmdName == "capture_raw_frame") {
    return 180.0;
  }
  if (cmdName == "get_bsp_runtime_state") {
    return 10.0;
  }
  return 30.0;
}

bool messageContains(const nlohmann::json& result, const QString& needle)
{
  // 同时检查 message/error 字段，统一兼容不同命令返回体的文本格式。
  QString message;
  if (result.contains("message") && result["message"].is_string()) {
    message = QString::fromStdString(result["message"].get<std::string>());
  } else if (result.contains("error") && result["error"].is_string()) {
    message = QString::fromStdString(result["error"].get<std::string>());
  }
  return message.contains(needle, Qt::CaseInsensitive);
}

bool jsonBoolField(const nlohmann::json& object, const char* key)
{
  try {
    return object.is_object() && object.contains(key) &&
           object.at(key).is_boolean() && object.at(key).get<bool>();
  } catch (...) {
    return false;
  }
}

bool runtimeStateStarted(flowagent::agents::BaseAgent* agent)
{
  // start_device 前先问运行态。BSP/Helen 支持 get_bsp_runtime_state；
  // 不支持该命令的 agent 会失败，调用方继续走原来的 start_device。
  if (!agent) {
    return false;
  }

  const auto state =
      agent->cmd(QStringLiteral("get_bsp_runtime_state"),
                 nlohmann::json::object(), true, commandTimeoutSeconds("get_bsp_runtime_state"));
  if (!state.success) {
    return false;
  }

  const auto& payload = state.result;
  if (jsonBoolField(payload, "started")) {
    return true;
  }
  if (payload.is_object() && payload.contains("device") &&
      jsonBoolField(payload.at("device"), "started")) {
    return true;
  }
  return false;
}

bool shouldAutoRestorePrimaryRgb(const std::string& agentName)
{
  return agentName == recordlab::core::compat::kPrimaryBspAgent;
}

nlohmann::json defaultWatchdogStartDeviceParams(const std::string& agentName)
{
  if (!shouldAutoRestorePrimaryRgb(agentName)) {
    return nlohmann::json::object();
  }
  return {{"camera_mode", std::string("rgb")}};
}

class ScopedExpectedDisconnect {
 public:
  ScopedExpectedDisconnect(AgentWatchdog* watchdog, bool enabled,
                           const std::string& agentName,
                           const std::string& reason)
      : watchdog_(enabled ? watchdog : nullptr), agentName_(agentName) {
    if (!watchdog_) {
      return;
    }
    watchdog_->beginExpectedTransientDisconnect(agentName_, reason);
    active_ = true;
  }

  ~ScopedExpectedDisconnect() {
    if (active_ && watchdog_) {
      watchdog_->endExpectedTransientDisconnect(agentName_);
    }
  }

  ScopedExpectedDisconnect(const ScopedExpectedDisconnect&) = delete;
  ScopedExpectedDisconnect& operator=(const ScopedExpectedDisconnect&) = delete;

 private:
  AgentWatchdog* watchdog_ = nullptr;
  std::string agentName_;
  bool active_ = false;
};

}  // namespace

AgentManagerProcess::AgentManagerProcess(const QString &configPath,
                                         QObject *parent)
    : QObject(parent) {
  // 构造阶段建立 AgentManager 与 Watchdog，并把二者的回调关系接起来。
  agentManager_ = std::make_unique<flowagent::core::AgentManager>(configPath);
  watchdog_ = std::make_unique<AgentWatchdog>();
  applyWatchdogConfig(configPath);

  // Watchdog callbacks
  watchdog_->setCheckCallback(
      [this](const std::string &agentName,
             const std::string & /*cmd*/) -> nlohmann::json {
        return sendCommandSync(
            {{"action", ManagerAction::SEND_AGENT_COMMAND},
             {"agent_name", agentName},
             {"cmd_name", std::string("check")},
             {"params", nlohmann::json::object()}},
            watchdog_->checkTimeout(), true);
      });

  watchdog_->setEstopCallback([this](const std::string &reason) {
    std::cerr << "[AgentManagerProcess] ESTOP: " << reason << std::endl;
    sendCommand({{"action", ManagerAction::EMERGENCY_STOP},
                 {"reason", reason}},
                true);
  });

  watchdog_->setInitDeviceCallback(
      [this](const std::string &agentName,
             const nlohmann::json &params) -> nlohmann::json {
        auto initResult = sendCommandSync(
            {{"action", ManagerAction::SEND_AGENT_COMMAND},
             {"agent_name", agentName},
             {"cmd_name", std::string("init_device")},
             {"params", params}},
            120.0, true);
        if (!initResult.value("success", false)) {
          return initResult;
        }

        const bool shouldRestoreStart =
            restoreStartDeviceAfterInit_.count(agentName) > 0 &&
            restoreStartDeviceAfterInit_[agentName];
        if (!shouldRestoreStart) {
          return initResult;
        }

        const auto startParamsIt = lastStartDeviceParams_.find(agentName);
        const auto startParams =
            startParamsIt == lastStartDeviceParams_.end()
                ? nlohmann::json::object()
                : startParamsIt->second;
        std::cout << "[AgentManagerProcess] Restoring start_device for "
                  << agentName << " params=" << startParams.dump()
                  << std::endl;
        auto startResult = sendCommandSync(
            {{"action", ManagerAction::SEND_AGENT_COMMAND},
             {"agent_name", agentName},
             {"cmd_name", std::string("start_device")},
             {"params", startParams}},
            120.0, true);
        if (!startResult.value("success", false) &&
            messageContains(startResult, QStringLiteral("Already started"))) {
          startResult["success"] = true;
        }
        return startResult.value("success", false) ? initResult : startResult;
      });

  QObject::connect(watchdog_.get(), &AgentWatchdog::statusUpdate, this,
                   &AgentManagerProcess::statusUpdate);

  std::cout << "[AgentManagerProcess] Initialized" << std::endl;
}

AgentManagerProcess::~AgentManagerProcess() { stop(); }

void AgentManagerProcess::pauseWatchdogChecks(const std::string &reason) {
  if (watchdog_) {
    watchdog_->pauseChecks(reason);
  }
}

void AgentManagerProcess::resumeWatchdogChecks(const std::string &reason) {
  if (watchdog_) {
    watchdog_->resumeChecks(reason);
  }
}

void AgentManagerProcess::start() {
  // 启动后台工作线程、全局订阅和 watchdog，让命令循环进入可用状态。
  if (running_)
    return;
  running_ = true;

  workerThread_ = std::make_unique<QThread>();
  this->moveToThread(workerThread_.get());
  QObject::connect(workerThread_.get(), &QThread::started,
                   [this]() { processLoop(); });
  workerThread_->start();

  agentManager_->startGlobalStateSubscriptions();
  agentManager_->autoInitializeRemoteAgentsAsync();
  watchdog_->start();

  std::cout << "[AgentManagerProcess] Started" << std::endl;
}

void AgentManagerProcess::stop() {
  // 按 watchdog -> 订阅 -> agent 资源 -> 线程 的顺序平滑停机。
  if (!running_)
    return;

  watchdog_->stopWatchdog();
  agentManager_->stopGlobalStateSubscriptions();

  if (workerThread_ && workerThread_->isRunning() &&
      QThread::currentThread() != workerThread_.get()) {
    sendCommandSync({{"action", ManagerAction::SHUTDOWN}}, 30.0, true);
  } else {
    agentManager_->releaseAll();
    running_ = false;
  }

  commandReady_.wakeAll();
  if (workerThread_ && workerThread_->isRunning()) {
    workerThread_->quit();
    workerThread_->wait(5000);
  }

  std::cout << "[AgentManagerProcess] Stopped" << std::endl;
}

void AgentManagerProcess::sendCommand(const nlohmann::json &command,
                                      bool urgent) {
  // 将命令投入普通或紧急队列；紧急命令会在 processLoop 中优先处理。
  QMutexLocker locker(&queueLock_);
  if (urgent) {
    urgentCommandQueue_.enqueue(command);
  } else {
    commandQueue_.enqueue(command);
  }
  commandReady_.wakeOne();
}

nlohmann::json
AgentManagerProcess::sendCommandSync(const nlohmann::json &command,
                                     double timeout, bool urgent) {
  // 为同步命令分配 request_id，并阻塞等待对应结果返回或超时。
  auto commandWithId = command;
  const auto requestId = nextRequestId();
  commandWithId["request_id"] = requestId.toStdString();

  sendCommand(commandWithId, urgent);

  QMutexLocker locker(&syncLock_);
  const auto timeoutMs = static_cast<unsigned long>(timeout * 1000.0);
  QElapsedTimer timer;
  timer.start();
  const auto requestIdStd = requestId.toStdString();
  while (!syncResults_.count(requestIdStd)) {
    const auto elapsed = static_cast<unsigned long>(timer.elapsed());
    const auto remainingMs =
        elapsed >= timeoutMs ? 0UL : timeoutMs - elapsed;
    if (remainingMs == 0 || !syncReady_.wait(&syncLock_, remainingMs)) {
      return {{"success", false},
              {"error", "Timeout"},
              {"request_id", requestIdStd}};
    }
  }

  auto it = syncResults_.find(requestIdStd);
  if (it == syncResults_.end()) {
    return {{"success", false},
            {"error", "Timeout"},
            {"request_id", requestIdStd}};
  }

  const auto result = it->second;
  syncResults_.erase(it);
  return result;
}

void AgentManagerProcess::processLoop() {
  // 后台循环持续从紧急队列和普通队列取命令，并按优先级串行执行。
  while (running_) {
    nlohmann::json cmd;
    {
      QMutexLocker locker(&queueLock_);
      if (urgentCommandQueue_.isEmpty() && commandQueue_.isEmpty()) {
        commandReady_.wait(&queueLock_, 100);
        if (urgentCommandQueue_.isEmpty() && commandQueue_.isEmpty())
          continue;
      }
      if (!urgentCommandQueue_.isEmpty()) {
        cmd = urgentCommandQueue_.dequeue();
      } else {
        cmd = commandQueue_.dequeue();
      }
    }
    processCommand(cmd);
  }
}

void AgentManagerProcess::processCommand(const nlohmann::json &command) {
  // 统一解释 ManagerAction，并在执行后补齐标准化结果结构。
  std::string action = command.value("action", std::string());
  nlohmann::json result;
  const std::string requestId = command.value("request_id", std::string());

  try {
    if (action == ManagerAction::INIT_AGENT) {
      const auto agentName = command.value("agent_name", std::string());
      result = agentManager_->initializeAgent(agentName);
      if (result.value("success", false) && isPrimaryAgentName(agentName) && agentName != "android") {
        auto initParams = agentManager_->getInitDeviceParams(agentName);
        auto overrideIt = watchdogInitParamOverrides_.find(agentName);
        if (overrideIt != watchdogInitParamOverrides_.end()) {
          initParams = overrideIt->second;
        }
        std::cout << "[AgentManagerProcess] Registering watchdog primary "
                  << agentName << " init_params=" << initParams.dump()
                  << std::endl;
        watchdog_->registerPrimaryAgent(agentName, initParams);

        // 从 agents_config.json 加载 supported_devices 映射和热插拔标志
        auto agentConfig = agentManager_->getAgentConfig(agentName);
        std::unordered_map<int, std::string> supportedDevices;
        bool suppressDialog = false;
        if (agentConfig.contains("supported_devices") &&
            agentConfig["supported_devices"].is_object()) {
          for (auto &[pidStr, nameVal] :
               agentConfig["supported_devices"].items()) {
            try {
              int pid = std::stoi(pidStr);
              std::string name = nameVal.get<std::string>();
              supportedDevices[pid] = name;
            } catch (...) {
            }
          }
        }
        if (agentConfig.contains("suppress_disconnect_dialog")) {
          suppressDialog =
              agentConfig.value("suppress_disconnect_dialog", false);
        }
        watchdog_->setAgentDeviceConfig(agentName, supportedDevices,
                                        suppressDialog);
        if (shouldAutoRestorePrimaryRgb(agentName)) {
          lastStartDeviceParams_[agentName] =
              defaultWatchdogStartDeviceParams(agentName);
          restoreStartDeviceAfterInit_[agentName] = true;
        }
      }

    } else if (action == ManagerAction::SET_WATCHDOG_INIT_PARAMS) {
      const auto agentName = command.value("agent_name", std::string());
      auto params = command.value("params", nlohmann::json::object());
      if (agentName.empty() || !isPrimaryAgentName(agentName)) {
        result = {{"success", false}, {"message", "primary agent_name required"}};
      } else {
        watchdogInitParamOverrides_[agentName] = params;
        std::cout << "[AgentManagerProcess] Watchdog init params override for "
                  << agentName << ": " << params.dump() << std::endl;
        watchdog_->updatePrimaryAgentInitDeviceParams(agentName, params);
        result = {{"success", true}, {"message", "watchdog init params updated"}};
      }

    } else if (action == ManagerAction::RELEASE_AGENT) {
      const auto agentName = command.value("agent_name", std::string());
      if (isPrimaryAgentName(agentName)) {
        watchdog_->unregisterPrimaryAgent();
        restoreStartDeviceAfterInit_[agentName] = false;
      }
      result = agentManager_->releaseAgent(agentName);

    } else if (action == ManagerAction::STOP_ALL_AGENTS) {
      watchdog_->unregisterPrimaryAgent();
      for (auto &[name, _] : restoreStartDeviceAfterInit_) {
        restoreStartDeviceAfterInit_[name] = false;
      }
      result = agentManager_->stopAllAgents();

    } else if (action == ManagerAction::STOP_AGENTS) {
      auto names = command.value("agent_names", std::vector<std::string>{});
      for (const auto &name : names) {
        if (isPrimaryAgentName(name)) {
          watchdog_->unregisterPrimaryAgent();
          restoreStartDeviceAfterInit_[name] = false;
          break;
        }
      }
      result = agentManager_->stopAgents(names);

    } else if (action == ManagerAction::SEND_AGENT_COMMAND) {
      auto agentName = command.value("agent_name", std::string());
      auto args = command.value("args", nlohmann::json::object());
      auto cmdName = command.value("cmd_name", std::string());
      if (cmdName.empty()) {
        cmdName = args.value("cmd_name", std::string());
      }
      auto params = command.value("params", nlohmann::json::object());
      if (params.empty()) {
        params = args.value("params", nlohmann::json::object());
      }
      if (cmdName.empty()) {
        result = {{"success", false}, {"error", "cmd_name required"}};
      } else {
        auto *agent = agentManager_->getAgent(agentName);
        if (!agent) {
          result = {{"success", false},
                    {"error", agentName + " not initialized"}};
        } else {
          const bool isPrimaryAgent = isPrimaryAgentName(agentName);
          if (isPrimaryAgent && cmdName == "init_device") {
            watchdog_->markAgentInitializing(agentName,
                                             "Explicit init_device in progress");
          }

          if (isPrimaryAgent && cmdName == "start_device" &&
              runtimeStateStarted(agent)) {
            result = {{"success", true},
                      {"message", "Already started"},
                      {"result", {{"started", true}}},
                      {"feedback", nlohmann::json::object()},
                      {"status_value", 0},
                      {"data", {{"agent", agentName}}}};
          } else {
            ScopedExpectedDisconnect expectedDisconnect(
                watchdog_.get(),
                isPrimaryAgent && cmdName == "capture_raw_frame",
                agentName,
                "capture_raw_frame in progress");
            auto r = agent->cmd(QString::fromStdString(cmdName), params, true,
                                commandTimeoutSeconds(cmdName));
            result = {{"success", r.success},
                      {"message", r.message.toStdString()},
                      {"result", r.result},
                      {"feedback", r.feedback},
                      {"status_value", r.statusValue},
                      {"data", {{"agent", agentName}}}};
          }

          if (isPrimaryAgent) {
            const bool success = result.value("success", false);
            const auto commandPayload =
                result.value("result", nlohmann::json::object());
            if (cmdName == "init_device") {
              if (success) {
                watchdog_->markAgentHealthy(agentName, true);
              } else {
                watchdog_->markAgentDisconnected(
                    agentName,
                    result.value("message",
                                 result.value("error", std::string())));
              }
            } else if (cmdName == "start_device" || cmdName == "restart_device") {
              if (success ||
                  messageContains(result, QStringLiteral("Already started"))) {
                if (shouldAutoRestorePrimaryRgb(agentName)) {
                  lastStartDeviceParams_[agentName] =
                      params.empty() ? defaultWatchdogStartDeviceParams(agentName)
                                     : params;
                  restoreStartDeviceAfterInit_[agentName] = true;
                } else {
                  lastStartDeviceParams_[agentName] = params;
                  restoreStartDeviceAfterInit_[agentName] = true;
                }
                watchdog_->markAgentHealthy(agentName);
              }
            } else if (cmdName == "capture_raw_frame") {
              // RAW 抓取内部会主动 release/reboot/restore RGB。只要本次命令结束后
              // 设备已经恢复，就把 watchdog 状态拉回 healthy，避免后续 check
              // 把这次临时断开误判为“重新插入后需要再 init/start 一次”。
              if (success ||
                  commandPayload.value("rgb_restore_success", false)) {
                watchdog_->markAgentHealthy(agentName);
              }
            } else if ((cmdName == "stop_device" || cmdName == "release_device")
                       && success) {
              if (shouldAutoRestorePrimaryRgb(agentName)) {
                lastStartDeviceParams_[agentName] =
                    defaultWatchdogStartDeviceParams(agentName);
                restoreStartDeviceAfterInit_[agentName] = true;
              } else {
                restoreStartDeviceAfterInit_[agentName] = false;
              }
              watchdog_->markAgentDisconnected(agentName,
                                               cmdName + " completed");
            }
          }
        }
      }

    } else if (action == ManagerAction::GET_STATUS) {
      auto agents = agentManager_->getConnectedAgents();
      result = {{"success", true}, {"connected_agents", agents}};

    } else if (action == ManagerAction::GET_AVAILABLE_AGENTS) {
      auto avail = agentManager_->getAvailableAgents();
      result = {{"success", true}, {"data", {{"available_agents", avail}}}};

    } else if (action == ManagerAction::GET_PRIMARY_AGENTS) {
      auto primary = agentManager_->getPrimaryAgents();
      result = {{"success", true}, {"data", {{"primary_agents", primary}}}};

    } else if (action == ManagerAction::SHOW_DIALOG) {
      nlohmann::json req = command;
      emit dialogRequest(req);
      result = {{"success", true}};

    } else if (action == ManagerAction::EMERGENCY_STOP) {
      auto reason = command.value("reason", std::string("Unknown"));
      std::cerr << "[AgentManagerProcess] EMERGENCY STOP: " << reason
                << std::endl;
      agentManager_->stopAllAgents();
      watchdog_->unregisterPrimaryAgent();
      for (auto &[name, _] : restoreStartDeviceAfterInit_) {
        restoreStartDeviceAfterInit_[name] = false;
      }
      result = {{"success", true}};

    } else if (action == ManagerAction::SHUTDOWN) {
      watchdog_->unregisterPrimaryAgent();
      agentManager_->releaseAll();
      running_ = false;
      result = {{"success", true}};

    } else {
      result = {{"success", false}, {"error", "Unknown action: " + action}};
    }

  } catch (const std::exception &e) {
    result = {{"success", false}, {"error", e.what()}};
  }

  result["action"] = action;
  if (command.contains("agent_name")) {
    result["agent_name"] = command.value("agent_name", std::string());
  }
  if (command.contains("cmd_name")) {
    result["cmd_name"] = command.value("cmd_name", std::string());
  } else if (command.contains("args") && command["args"].contains("cmd_name")) {
    result["cmd_name"] = command["args"].value("cmd_name", std::string());
  }
  if (!requestId.empty()) {
    result["request_id"] = requestId;
    QMutexLocker locker(&syncLock_);
    syncResults_[requestId] = result;
    syncReady_.wakeAll();
  }

  emit commandResult(result);
}

bool AgentManagerProcess::isPrimaryAgentName(const std::string &agentName) const {
  // 判断给定 agent 是否属于 primary_agents，用于决定是否纳入 watchdog 管控。
  return agentManager_ && agentManager_->isPrimaryAgent(agentName);
}

void AgentManagerProcess::applyWatchdogConfig(const QString &configPath) {
  // 从配置文件读取 watchdog 参数，让 UI 与后台共享同一套超时策略。
  QFile file(configPath);
  if (!file.open(QIODevice::ReadOnly)) {
    return;
  }

  const auto document = QJsonDocument::fromJson(file.readAll());
  const auto root = document.object();
  const auto watchdogConfig = root.value(QStringLiteral("watchdog")).toObject();
  if (watchdogConfig.isEmpty()) {
    return;
  }

  if (watchdogConfig.contains(QStringLiteral("check_interval"))) {
    watchdog_->setCheckInterval(
        watchdogConfig.value(QStringLiteral("check_interval")).toDouble(3.0));
  }
  if (watchdogConfig.contains(QStringLiteral("startup_delay"))) {
    watchdog_->setStartupDelay(
        watchdogConfig.value(QStringLiteral("startup_delay")).toDouble(10.0));
  }
  if (watchdogConfig.contains(QStringLiteral("check_timeout"))) {
    watchdog_->setCheckTimeout(
        watchdogConfig.value(QStringLiteral("check_timeout")).toDouble(10.0));
  }
}

QString AgentManagerProcess::nextRequestId() {
  // 生成单调递增的请求 ID，供同步命令匹配返回结果。
  const auto nextId = requestCounter_.fetch_add(1) + 1;
  return QStringLiteral("req_%1").arg(nextId);
}

} // namespace recordlab::backend
