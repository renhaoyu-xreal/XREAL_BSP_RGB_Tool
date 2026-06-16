/*
 * AgentManagerProcess - AgentManager 包装器
 *
 * 在 Python 版本中通过 multiprocessing.Process 运行 AgentManager。
 * 在 C++ 版本中使用 QThread 包装，提供相同的命令队列接口。
 *
 */
#pragma once

#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>

#include <atomic>
#include <memory>
#include <unordered_map>

#include "recordlab/backend/agent_watchdog.h"
#include "recordlab/core/qt_json_compat.h"
#include "recordlab/flowagent/core/agent_manager.h"

namespace recordlab::backend {

/*
 * AgentManagerProcess
 *
 * 持有 AgentManager 和 AgentWatchdog 实例，运行在独立 QThread。
 * 从 commandQueue 接收命令，
 * 结果通过信号返回。
 */
class AgentManagerProcess : public QObject {
  Q_OBJECT

public:
  explicit AgentManagerProcess(const QString &configPath = {},
                               QObject *parent = nullptr);
  ~AgentManagerProcess() override;

  // ========== 线程控制 ==========
  void start();
  void stop();

  // ========== 命令发送 ==========
  void sendCommand(const nlohmann::json &command, bool urgent = false);
  nlohmann::json sendCommandSync(const nlohmann::json &command,
                                 double timeout = 30.0,
                                 bool urgent = false);

  // ========== 直接访问 ==========
  flowagent::core::AgentManager *agentManager() { return agentManager_.get(); }
  AgentWatchdog *watchdog() { return watchdog_.get(); }
  void pauseWatchdogChecks(const std::string &reason = {});
  void resumeWatchdogChecks(const std::string &reason = {});

signals:
  void commandResult(const nlohmann::json &result);
  void statusUpdate(const nlohmann::json &status);
  void dialogRequest(const nlohmann::json &request);

private slots:
  void processCommand(const nlohmann::json &command);

private:
  void processLoop();
  bool isPrimaryAgentName(const std::string &agentName) const;
  void applyWatchdogConfig(const QString &configPath);
  QString nextRequestId();

  std::unique_ptr<flowagent::core::AgentManager> agentManager_;
  std::unique_ptr<AgentWatchdog> watchdog_;
  std::unordered_map<std::string, nlohmann::json> watchdogInitParamOverrides_;
  std::unordered_map<std::string, nlohmann::json> lastStartDeviceParams_;
  std::unordered_map<std::string, bool> restoreStartDeviceAfterInit_;

  std::unique_ptr<QThread> workerThread_;

  QMutex queueLock_;
  QQueue<nlohmann::json> urgentCommandQueue_;
  QQueue<nlohmann::json> commandQueue_;
  QWaitCondition commandReady_;

  QMutex syncLock_;
  QWaitCondition syncReady_;
  std::unordered_map<std::string, nlohmann::json> syncResults_;
  std::atomic<uint64_t> requestCounter_{0};

  bool running_ = false;
};

} // namespace recordlab::backend
