/*
 * AgentManager - Agent 管理器
 *
 * 持有所有 Agent 实例，根据配置初始化/释放 Agent
 *
 */
#pragma once

#include <QMutex>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace echo {
class Subscriber;
}

namespace recordlab::flowagent::agents {
class BaseAgent;
}

namespace recordlab::core {
class GlobalState;
class UIMessageBridge;
} // namespace recordlab::core

namespace recordlab::flowagent::core {

class AgentManager : public QObject {
  Q_OBJECT

public:
  explicit AgentManager(const QString &configPath = {},
                        QObject *parent = nullptr);
  ~AgentManager() override;

  // ========== Agent 生命周期 ==========

  nlohmann::json initializeAgent(const std::string &agentName);
  nlohmann::json releaseAgent(const std::string &agentName);
  nlohmann::json releaseAll();
  nlohmann::json stopAllAgents();
  nlohmann::json stopAgents(const std::vector<std::string> &agentNames);

  // ========== 查询 ==========

  agents::BaseAgent *getAgent(const std::string &agentName);
  bool isInitialized(const std::string &agentName) const;
  std::vector<std::string> getConnectedAgents() const;
  std::vector<std::string> getAvailableAgents() const;
  std::vector<std::string> getPrimaryAgents() const;
  bool isPrimaryAgent(const std::string &agentName) const;
  nlohmann::json getAgentConfig(const std::string &agentName) const;
  nlohmann::json getInitDeviceParams(const std::string &agentName) const;

  // ========== 配置 ==========

  void setAgentConfig(const std::string &agentName,
                      const nlohmann::json &config);

  // ========== UI 弹窗 ==========

  nlohmann::json showDialog(const std::string &dialogType,
                            const std::string &title,
                            const std::string &message,
                            const nlohmann::json &kwargs = {});

  // ========== 全局状态订阅 ==========

  void startGlobalStateSubscriptions(const std::string &host = "127.0.0.1");
  void stopGlobalStateSubscriptions();

  // ========== 异步初始化 ==========

  void autoInitializeRemoteAgentsAsync();

signals:
  void agentStatusChanged(const QString &agentName, bool connected);

private:
  void loadConfigFromJson(const QString &configPath);
  void autoInitializeLocalhostOnly();
  agents::BaseAgent *createAgent(const std::string &agentName);

  // 全局状态回调
  void onGlobalStateData(const std::string &topicName,
                         const nlohmann::json &data);
  void updateGlobalState(const std::string &topicName,
                         const nlohmann::json &data);

  // Agent 类映射
  std::unordered_map<std::string, std::string>
      agentClasses_; // name -> className

  // 已连接 Agent
  std::unordered_map<std::string, std::unique_ptr<agents::BaseAgent>>
      connectedAgents_;

  // Agent 配置
  std::unordered_map<std::string, nlohmann::json> agentConfigs_;
  std::vector<std::string> primaryAgents_;

  // 全局状态
  recordlab::core::GlobalState *globalState_ = nullptr;

  // 全局状态订阅者
  std::unordered_map<std::string, std::unique_ptr<echo::Subscriber>>
      subscribers_;
  mutable QMutex dataLock_;
  std::unordered_map<std::string, nlohmann::json> topicData_;
  std::unordered_map<std::string, double> lastUpdateTime_;
  double updateInterval_ = 0.1; // 100ms

  QString projectRoot_;
};

} // namespace recordlab::flowagent::core
