/*
 * AgentManager 实现
 */
#include "recordlab/flowagent/core/agent_manager.h"
#include "recordlab/common/topics.h"
#include "recordlab/core/global_state.h"
#include "recordlab/flowagent/agents/base_agent.h"
#include "recordlab/flowagent/agents/localhost_agent.h"

#include <subscriber.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

namespace recordlab::flowagent::core {

using namespace recordlab::flowagent::agents;

AgentManager::AgentManager(const QString &configPath, QObject *parent)
    : QObject(parent) {
  // 初始化 agent 类型映射、全局状态指针并加载本地配置。
  // Agent 类映射
  agentClasses_ = {{"glasses_bsp_node", "BaseAgent"},
                   {"localhost", "LocalhostAgent"}};

  globalState_ = &recordlab::core::GlobalState::instance();

  QString cfgPath = configPath;
  if (cfgPath.isEmpty()) {
    const QString appRoot = qEnvironmentVariable("RECORDLABC_ROOT").trimmed();
    if (!appRoot.isEmpty()) {
      cfgPath = QDir(appRoot).filePath("config/agents_config.json");
    } else {
      cfgPath = QDir(QStringLiteral(RECORDLABC_SOURCE_DIR))
                    .filePath("config/agents_config.json");
    }
  }
  loadConfigFromJson(cfgPath);

  std::cout << "[AgentManager] Initialized with " << agentClasses_.size()
            << " agent types" << std::endl;
}

AgentManager::~AgentManager() { releaseAll(); }

// ==================== Config loading ====================

void AgentManager::loadConfigFromJson(const QString &configPath) {
  // 从 JSON 配置读取 agent 定义，并把相对路径转成项目内绝对路径。
  QFile file(configPath);
  if (!file.open(QIODevice::ReadOnly)) {
    std::cerr << "[AgentManager] Config not found: " << configPath.toStdString()
              << std::endl;
    return;
  }

  auto doc = QJsonDocument::fromJson(file.readAll());
  auto root = doc.object();
  projectRoot_ = QFileInfo(configPath).absolutePath() + "/..";

  // Parse agents
  auto agentsObj = root["agents"].toObject();
  for (auto it = agentsObj.begin(); it != agentsObj.end(); ++it) {
    std::string name = it.key().toStdString();
    auto cfg = it.value().toObject();
    nlohmann::json jcfg;

    for (auto ci = cfg.begin(); ci != cfg.end(); ++ci) {
      std::string key = ci.key().toStdString();
      if (ci.value().isString()) {
        std::string val = ci.value().toString().toStdString();
        // Resolve relative paths
        if ((key == "subnode_path" || key == "root_path") && !val.empty()) {
          if (!QFileInfo(QString::fromStdString(val)).isAbsolute()) {
            val = QDir(projectRoot_)
                      .filePath(QString::fromStdString(val))
                      .toStdString();
          }
        }
        jcfg[key] = val;
      } else if (ci.value().isDouble()) {
        jcfg[key] = ci.value().toInt();
      } else if (ci.value().isBool()) {
        jcfg[key] = ci.value().toBool();
      } else if (ci.value().isObject()) {
        // custom_params etc — store as nested json
        auto subObj = ci.value().toObject();
        nlohmann::json sub;
        for (auto si = subObj.begin(); si != subObj.end(); ++si) {
          if (si.value().isString())
            sub[si.key().toStdString()] = si.value().toString().toStdString();
          else if (si.value().isDouble())
            sub[si.key().toStdString()] = si.value().toInt();
          else if (si.value().isBool())
            sub[si.key().toStdString()] = si.value().toBool();
        }
        jcfg[key] = sub;
      }
    }

    // Move remote_user/remote_password into custom_params
    for (const auto &k : {"remote_user", "remote_password"}) {
      if (jcfg.contains(k)) {
        jcfg["custom_params"][k] = jcfg[k];
        jcfg.erase(k);
      }
    }

    agentConfigs_[name] = jcfg;
  }

  // Primary agents
  auto primaryArr = root["primary_agents"].toArray();
  for (const auto &v : primaryArr) {
    primaryAgents_.push_back(v.toString().toStdString());
  }

  std::cout << "[AgentManager] Loaded config: " << agentConfigs_.size()
            << " agents, " << primaryAgents_.size() << " primary" << std::endl;

  autoInitializeLocalhostOnly();
}

void AgentManager::autoInitializeLocalhostOnly() {
  // 本机 agent 不依赖远程子节点，启动时优先自动初始化。
  for (auto &[name, cfg] : agentConfigs_) {
    if (agentClasses_.count(name) && agentClasses_[name] == "LocalhostAgent") {
      initializeAgent(name);
    }
  }
}

void AgentManager::autoInitializeRemoteAgentsAsync() {
  // 后台异步初始化远程 primary agent，避免阻塞主线程启动。
  auto t = std::thread([this]() {
    for (auto &[name, cfg] : agentConfigs_) {
      auto sp = cfg.value("subnode_path", std::string());
      bool isRemote = sp.empty();
      bool isLocalhost =
          agentClasses_.count(name) && agentClasses_[name] == "LocalhostAgent";
      const bool isPrimary =
          std::find(primaryAgents_.begin(), primaryAgents_.end(), name) !=
          primaryAgents_.end();
      if (isRemote && !isLocalhost && isPrimary) {
        initializeAgent(name);
      }
    }
  });
  t.detach();
}

// ==================== Lifecycle ====================

nlohmann::json AgentManager::initializeAgent(const std::string &agentName) {
  // 创建并连接指定 agent；成功后纳入 connectedAgents_ 生命周期管理。
  if (connectedAgents_.count(agentName)) {
    return {{"success", true},
            {"message", agentName + " already initialized"},
            {"agent_name", agentName}};
  }
  if (!agentClasses_.count(agentName)) {
    return {{"success", false},
            {"message", "Unknown agent: " + agentName},
            {"agent_name", agentName}};
  }

  try {
    auto *agent = createAgent(agentName);
    if (!agent) {
      return {{"success", false},
              {"message", "Failed to create agent"},
              {"agent_name", agentName}};
    }

    auto result = agent->connect();
    if (result.success) {
      connectedAgents_[agentName] = std::unique_ptr<BaseAgent>(agent);
      emit agentStatusChanged(QString::fromStdString(agentName), true);
      return {{"success", true},
              {"message", result.message.toStdString()},
              {"agent_name", agentName}};
    } else {
      delete agent;
      return {{"success", false},
              {"message", result.message.toStdString()},
              {"agent_name", agentName}};
    }
  } catch (const std::exception &e) {
    return {
        {"success", false}, {"message", e.what()}, {"agent_name", agentName}};
  }
}

BaseAgent *AgentManager::createAgent(const std::string &agentName) {
  // 根据配置和 agentClasses_ 选择具体 agent 类型并装配构造参数。
  auto cfg = agentConfigs_.count(agentName) ? agentConfigs_[agentName]
                                            : nlohmann::json{};
  auto className =
      agentClasses_.count(agentName) ? agentClasses_[agentName] : "BaseAgent";

  if (className == "LocalhostAgent") {
    return new LocalhostAgent(
        QString::fromStdString(cfg.value("name", agentName)),
        QString::fromStdString(
            cfg.value("scripts_dir", std::string("./scripts"))));
  }

  // Default: BaseAgent
  QVariantMap customParams;
  if (cfg.contains("custom_params") && cfg["custom_params"].is_object()) {
    for (auto &[k, v] : cfg["custom_params"].items()) {
      if (v.is_string())
        customParams[QString::fromStdString(k)] =
            QString::fromStdString(v.get<std::string>());
      else if (v.is_number_integer())
        customParams[QString::fromStdString(k)] = v.get<int>();
      else if (v.is_boolean())
        customParams[QString::fromStdString(k)] = v.get<bool>();
    }
  }

  return new BaseAgent(
      QString::fromStdString(cfg.value("name", agentName)),
      QString::fromStdString(cfg.value("subnode_path", std::string())),
      QString::fromStdString(
          cfg.value("subnode_host", std::string("127.0.0.1"))),
      cfg.value("goal_port", 5690), cfg.value("feedback_port", 5691),
      QString::fromStdString(cfg.value("root_path", std::string("./output"))),
      customParams);
}

nlohmann::json AgentManager::releaseAgent(const std::string &agentName) {
  // 断开并移除单个 agent，供切换主 agent 或局部回收时调用。
  auto it = connectedAgents_.find(agentName);
  if (it == connectedAgents_.end()) {
    return {{"success", true}, {"message", agentName + " not initialized"}};
  }
  try {
    it->second->disconnect();
    connectedAgents_.erase(it);
    emit agentStatusChanged(QString::fromStdString(agentName), false);
    return {{"success", true}, {"message", "Released"}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

nlohmann::json AgentManager::releaseAll() {
  // 逐个释放全部已连接 agent，并返回成败统计。
  std::vector<std::string> released, failed;
  auto names = getConnectedAgents();
  for (auto &n : names) {
    auto r = releaseAgent(n);
    (r.value("success", false) ? released : failed).push_back(n);
  }
  return {
      {"success", failed.empty()}, {"released", released}, {"failed", failed}};
}

nlohmann::json AgentManager::stopAllAgents() {
  // 对所有已连接 agent 发送 estop，用于全局停机或急停。
  std::vector<std::string> stopped, failed;
  for (auto &[name, agent] : connectedAgents_) {
    if (!agent->isConnected())
      continue;
    auto r = agent->cmd("estop", {}, false, 5.0);
    (r.success ? stopped : failed).push_back(name);
  }
  return {
      {"success", failed.empty()}, {"stopped", stopped}, {"failed", failed}};
}

nlohmann::json
AgentManager::stopAgents(const std::vector<std::string> &agentNames) {
  // 对指定 agent 子集发送 estop，避免一次性影响全部节点。
  std::vector<std::string> stopped, failed;
  for (auto &name : agentNames) {
    auto it = connectedAgents_.find(name);
    if (it == connectedAgents_.end() || !it->second->isConnected())
      continue;
    auto r = it->second->cmd("estop", {}, false, 5.0);
    (r.success ? stopped : failed).push_back(name);
  }
  return {
      {"success", failed.empty()}, {"stopped", stopped}, {"failed", failed}};
}

// ==================== Query ====================

BaseAgent *AgentManager::getAgent(const std::string &agentName) {
  // 返回已初始化 agent 的裸指针，供上层直接发送命令。
  auto it = connectedAgents_.find(agentName);
  return it != connectedAgents_.end() ? it->second.get() : nullptr;
}

bool AgentManager::isInitialized(const std::string &agentName) const {
  // 判断目标 agent 是否已经在 connectedAgents_ 中完成初始化。
  return connectedAgents_.count(agentName) > 0;
}

std::vector<std::string> AgentManager::getConnectedAgents() const {
  // 返回当前所有已连接 agent 的名称列表。
  std::vector<std::string> r;
  for (auto &[k, _] : connectedAgents_)
    r.push_back(k);
  return r;
}

std::vector<std::string> AgentManager::getAvailableAgents() const {
  // 返回类型映射中声明可用的 agent 名称集合。
  std::vector<std::string> r;
  for (auto &[k, _] : agentClasses_)
    r.push_back(k);
  return r;
}

std::vector<std::string> AgentManager::getPrimaryAgents() const {
  // 返回配置文件里定义的主 agent 列表。
  return primaryAgents_;
}

bool AgentManager::isPrimaryAgent(const std::string &agentName) const {
  // 判断某个 agent 是否属于主 agent 集合。
  return std::find(primaryAgents_.begin(), primaryAgents_.end(), agentName) !=
         primaryAgents_.end();
}

nlohmann::json AgentManager::getAgentConfig(const std::string &agentName) const {
  // 返回指定 agent 的原始配置快照；缺失时回退为空对象。
  auto it = agentConfigs_.find(agentName);
  return it != agentConfigs_.end() ? it->second : nlohmann::json::object();
}

nlohmann::json
AgentManager::getInitDeviceParams(const std::string &agentName) const {
  // 读取配置中的 init_device_params，供 watchdog 自动恢复时复用。
  const auto config = getAgentConfig(agentName);
  if (!config.is_object()) {
    return nlohmann::json::object();
  }

  if (config.contains("init_device_params") &&
      config["init_device_params"].is_object()) {
    return config["init_device_params"];
  }

  return nlohmann::json::object();
}

void AgentManager::setAgentConfig(const std::string &agentName,
                                  const nlohmann::json &config) {
  // 仅允许在 agent 尚未初始化时替换配置，避免运行期参数漂移。
  if (connectedAgents_.count(agentName))
    return;
  agentConfigs_[agentName] = config;
}

nlohmann::json AgentManager::showDialog(const std::string & /*dialogType*/,
                                        const std::string & /*title*/,
                                        const std::string & /*message*/,
                                        const nlohmann::json & /*kwargs*/) {
  // 当前版本尚未把 agent 弹窗接入 UIMessageBridge，这里先返回显式失败。
  // TODO: integrate with UIMessageBridge
  return {{"success", false},
          {"error", "UI queues not configured"},
          {"cancelled", true}};
}

// ==================== Global State Subscriptions ====================

void AgentManager::startGlobalStateSubscriptions(const std::string &host) {
  // 订阅全局状态 topic，并把结果写回 GlobalState 供 UI 顶栏统一读取。
  using namespace recordlab::common;
  (void)host;

  struct TopicInfo {
    const char *name;
    int port;
  };
  TopicInfo topics[] = {{TOPIC_RECORD_TIMER, PORT_RECORD_TIMER},
                        {TOPIC_TIME_DELAY, PORT_TIME_DELAY},
                        {TOPIC_MOTION_STATUS, PORT_MOTION_STATUS}};

  for (auto &t : topics) {
    if (subscribers_.count(t.name))
      continue;
    try {
      auto sub = std::make_unique<echo::Subscriber>(
          t.name,
          [this, topicName = std::string(t.name)](const std::string &raw) {
            try {
              auto data = nlohmann::json::parse(raw);
              onGlobalStateData(topicName, data);
            } catch (...) {
            }
          },
          true);
      subscribers_[t.name] = std::move(sub);
    } catch (const std::exception &e) {
      std::cerr << "[AgentManager] Failed to subscribe " << t.name << ": "
                << e.what() << std::endl;
    }
  }
}

void AgentManager::stopGlobalStateSubscriptions() { subscribers_.clear(); }

void AgentManager::onGlobalStateData(const std::string &topicName,
                                     const nlohmann::json &data) {
  // 以固定节流间隔消费全局状态，避免高频 topic 反复抢锁。
  double now = std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();
  if (now - lastUpdateTime_[topicName] < updateInterval_)
    return;
  lastUpdateTime_[topicName] = now;

  {
    QMutexLocker locker(&dataLock_);
    topicData_[topicName] = data;
  }
  updateGlobalState(topicName, data);
}

void AgentManager::updateGlobalState(const std::string &topicName,
                                     const nlohmann::json &data) {
  // 将 topic 数据映射到 GlobalState 的统一字段，供多页面共享。
  if (topicName == "record_timer") {
    if (data.contains("duration_ns")) {
      globalState_->setRecordTimer(data["duration_ns"].get<double>() / 1e9);
    }
  } else if (topicName == "time_delay") {
    if (data.contains("time_delay_ns")) {
      globalState_->setTimeDelay(data["time_delay_ns"].get<double>() / 1e6);
    }
  } else if (topicName == "motion_status") {
    if (data.contains("status")) {
      globalState_->setMotionStatus(
          QString::fromStdString(data["status"].get<std::string>()));
    }
  }
}

} // namespace recordlab::flowagent::core
