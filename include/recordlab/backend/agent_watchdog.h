/*
 * AgentWatchdog - Agent 守护进程
 *
 *
 * 功能：
 * 1. 注册管理：主agent默认注册
 * 2. 健康检查：周期性 check 主agent
 * 3. 自动恢复：check 恢复时自动 init_device
 * 4. 故障处理：check 失败时 estop
 * 5. 状态通知：通过信号向UI发送状态更新
 */
#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

#include "recordlab/core/qt_json_compat.h"

namespace recordlab::backend {

// ============================================================================
// AgentStatus
// ============================================================================

struct AgentStatus {
  static constexpr const char *STATE_HEALTHY = "healthy";
  static constexpr const char *STATE_DISCONNECTED = "disconnected";
  static constexpr const char *STATE_INITIALIZING = "initializing";

  std::string agentName;
  bool isPrimary = false;
  std::string state = STATE_DISCONNECTED;
  int consecutiveFailures = 0;
  double lastCheckTime = 0.0;
  std::string lastError;

  // Primary agent only
  nlohmann::json initDeviceParams;
  double lastInitTime = 0.0;

  // 设备信息（由 check 结果更新）
  int productId = -1;                                        // 当前连接的产品 ID
  std::string productDisplayId;                              // 面向 UI 的产品 ID（如 USB PID: 0x043a）
  std::string productName;                                   // 解析后的型号名称
  std::string fsn;                                           // 眼镜 FSN（若 SDK 可提供）
  std::vector<int> connectedProductIds;                      // 所有连接的设备 ID

  // 配置（从 agents_config.json 加载）
  std::unordered_map<int, std::string> supportedDevices;     // product_id → 名称映射
  bool suppressDisconnectDialog = false;                     // 是否支持热插拔（不弹窗）
};

// ============================================================================
// AgentWatchdog
// ============================================================================

class AgentWatchdog : public QThread {
  Q_OBJECT

public:
  explicit AgentWatchdog(QObject *parent = nullptr);
  ~AgentWatchdog() override;

  void stopWatchdog();

  // ========== 注册 ==========
  void registerPrimaryAgent(const std::string &agentName,
                            const nlohmann::json &initDeviceParams = {});
  void updatePrimaryAgentInitDeviceParams(
      const std::string &agentName, const nlohmann::json &initDeviceParams);
  void unregisterPrimaryAgent();
  void registerScriptAgents(const std::vector<std::string> &agentNames);
  void unregisterScriptAgents(const std::vector<std::string> &agentNames);
  void markAgentInitializing(const std::string &agentName,
                             const std::string &message = {});
  void markAgentHealthy(const std::string &agentName,
                        bool updateInitTime = false);
  void markAgentDisconnected(const std::string &agentName,
                             const std::string &error = {});

  /// 设置 agent 的设备配置（supported_devices 映射和热插拔标志），
  /// 通常在 registerPrimaryAgent 后从 agents_config.json 加载。
  void setAgentDeviceConfig(
      const std::string &agentName,
      const std::unordered_map<int, std::string> &supportedDevices,
      bool suppressDisconnectDialog);

  // ========== 查询 ==========
  QString getStatusSummary() const;

  // ========== 配置 ==========
  void setCheckInterval(double seconds) { checkInterval_ = seconds; }
  void setStartupDelay(double seconds) { startupDelay_ = seconds; }
  void setCheckTimeout(double seconds) { checkTimeout_ = seconds; }
  double checkTimeout() const { return checkTimeout_; }

  // ========== 检查命令执行回调 ==========
  using CheckCallback = std::function<nlohmann::json(
      const std::string &agentName, const std::string &cmd)>;
  void setCheckCallback(CheckCallback cb) { checkCallback_ = std::move(cb); }

  using EstopCallback = std::function<void(const std::string &reason)>;
  void setEstopCallback(EstopCallback cb) { estopCallback_ = std::move(cb); }

  using InitDeviceCallback = std::function<nlohmann::json(
      const std::string &agentName, const nlohmann::json &params)>;
  void setInitDeviceCallback(InitDeviceCallback cb) {
    initDeviceCallback_ = std::move(cb);
  }

signals:
  void statusUpdate(const nlohmann::json &statusData);
  void agentFailure(const QString &agentName, const QString &error);

protected:
  void run() override;

private:
  void watchdogLoop();
  void checkAllAgents();
  void checkAgent(const std::string &agentName);
  void handleCheckSuccess(const std::string &agentName);
  void handleCheckFailure(const std::string &agentName,
                          const std::string &error);
  void initDevice(const std::string &agentName,
                  const nlohmann::json &initDeviceParams);
  void executeEstop(const std::string &agentName, const std::string &reason);
  void notifyStatusUpdate();

  mutable QMutex lock_;
  std::unordered_map<std::string, AgentStatus> registeredAgents_;
  std::string primaryAgent_;

  std::atomic<bool> running_{false};

  // Config
  double checkInterval_ = 3.0;
  double startupDelay_ = 10.0;
  double checkTimeout_ = 10.0;

  // Callbacks
  CheckCallback checkCallback_;
  EstopCallback estopCallback_;
  InitDeviceCallback initDeviceCallback_;
};

} // namespace recordlab::backend
