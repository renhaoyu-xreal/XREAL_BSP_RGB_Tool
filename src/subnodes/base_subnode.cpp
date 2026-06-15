/*
 * BaseSubnode - 子节点基类 实现
 *
 */

#include "recordlab/subnodes/base_subnode.h"

#include <action.h>    // echo::ActionServer
#include <publisher.h> // echo::Publisher

#include <QCoreApplication>

#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

namespace recordlab::subnodes {

// ============================================================================
// BaseSubnode
// ============================================================================

BaseSubnode::BaseSubnode(const QString &name, const QString &subnodeHost,
                         int goalPort, int feedbackPort,
                         const QString &rootPath, QObject *parent)
    : QObject(parent), name_(name), subnodeHost_(subnodeHost),
      goalPort_(goalPort), feedbackPort_(feedbackPort), rootPath_(rootPath) {
  // 初始化数据注册客户端
  try {
    registryClient_ = std::make_unique<recordlab::common::DataRegistryClient>();
    std::cout << "[" << name_.toStdString()
              << "] DataRegistryClient initialized" << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "[" << name_.toStdString()
              << "] Failed to initialize DataRegistryClient: " << e.what()
              << std::endl;
  }

  registerBuiltinCommands();
  std::cout << "[" << name_.toStdString() << "] SubNode initialized"
            << std::endl;
}

BaseSubnode::~BaseSubnode() {
  // 析构时若仍处于连接态，统一走 disconnect 做完整收尾。
  if (isConnected_) {
    disconnect();
  }
}

// ==================== 内置命令注册 ====================

void BaseSubnode::registerBuiltinCommands() {
  // 注册所有子节点都必须支持的基础命令，保持与上层 agent 协议一致。
  // check
  registerCmd("check",
              [this](uint32_t, const std::string &, const nlohmann::json &) {
                return this->check();
              });

  // estop
  registerCmd("estop",
              [this](uint32_t, const std::string &, const nlohmann::json &) {
                return this->estop();
              });

  // sleep
  registerCmd("sleep", [this](uint32_t, const std::string &,
                              const nlohmann::json &params) {
    int duration = params.value("time", 1);
    int elapsed = 0;
    while (elapsed < duration) {
      if (estopFlag_) {
        return nlohmann::json{
            {"success", false},
            {"message", "Sleep interrupted by emergency stop"}};
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
      elapsed++;
    }
    return nlohmann::json{
        {"success", true},
        {"message", "Slept for " + std::to_string(duration) + " seconds"}};
  });

  // get_agent_topics
  registerCmd("get_agent_topics",
              [this](uint32_t, const std::string &, const nlohmann::json &) {
                nlohmann::json topics;
                for (const auto &[port, topicName] : agentTopics_) {
                  topics[std::to_string(port)] = topicName;
                }
                return nlohmann::json{{"success", true},
                                      {"message", "Agent topics retrieved"},
                                      {"topics", topics}};
              });

  // get_root_path
  registerCmd("get_root_path",
              [this](uint32_t, const std::string &, const nlohmann::json &) {
                return nlohmann::json{{"success", true},
                                      {"message", "Root path retrieved"},
                                      {"root_path", rootPath_.toStdString()}};
              });
}

// ==================== 生命周期 ====================

nlohmann::json BaseSubnode::connect() {
  // 创建 ActionServer，使外部 agent 可以通过统一 action 通道驱动子节点。
  std::cout << "[" << name_.toStdString() << "] Connecting SubNode..."
            << std::endl;
  estopFlag_ = false;

  try {
    auto result = createActionServer();
    if (!result.value("success", false)) {
      return result;
    }

    isConnected_ = true;
    std::cout << "[" << name_.toStdString() << "] Connected successfully"
              << std::endl;
    return {{"success", true}, {"message", "Connected"}};

  } catch (const std::exception &e) {
    std::cerr << "[" << name_.toStdString()
              << "] Connection error: " << e.what() << std::endl;
    return {{"success", false}, {"message", e.what()}};
  }
}

nlohmann::json BaseSubnode::disconnect() {
  // 进入关闭态后回收 action server 和全部 publisher。
  std::cout << "[" << name_.toStdString() << "] Disconnecting..." << std::endl;

  try {
    isShuttingDown_ = true;
    closeActionServer();
    closeAllPublishers();
    isConnected_ = false;

    std::cout << "[" << name_.toStdString() << "] Disconnected successfully"
              << std::endl;
    return {{"success", true}, {"message", "Disconnected"}};

  } catch (const std::exception &e) {
    std::cerr << "[" << name_.toStdString()
              << "] Error during disconnect: " << e.what() << std::endl;
    return {{"success", false}, {"message", e.what()}};
  }
}

nlohmann::json BaseSubnode::release() {
  // release 作为派生类资源收尾入口，默认只调用虚函数 onRelease。
  std::cout << "[" << name_.toStdString() << "] Releasing resources..."
            << std::endl;
  auto result = onRelease();
  if (!result.value("success", false))
    return result;
  return {{"success", true}, {"message", ""}};
}

nlohmann::json BaseSubnode::onRelease() {
  // 基类默认不做额外释放，具体设备逻辑由派生类覆盖。
  return {{"success", true}, {"message", "on_release executed"}};
}

nlohmann::json BaseSubnode::estop() {
  // 急停会立刻置位 estopFlag_，让长任务可尽快中断。
  std::cout << "[" << name_.toStdString() << "] Emergency stop triggered!"
            << std::endl;
  estopFlag_ = true;
  auto result = onEstop();
  if (!result.value("success", false))
    return result;
  return {{"success", true}, {"message", ""}};
}

nlohmann::json BaseSubnode::onEstop() {
  // 基类默认只确认急停已处理，派生类可扩展具体设备停机逻辑。
  return {{"success", true}, {"message", "on_estop executed"}};
}

nlohmann::json BaseSubnode::check() {
  // check 统一走虚函数 onCheck，供各设备报告自身健康状态。
  std::cout << "[" << name_.toStdString() << "] Checking SubNode..."
            << std::endl;
  auto result = onCheck();
  if (!result.value("success", false))
    return result;
  if (!result.contains("success")) {
    result["success"] = true;
  }
  if (!result.contains("message")) {
    result["message"] = "";
  }
  return result;
}

nlohmann::json BaseSubnode::onCheck() {
  // 基类默认返回成功，真实健康检查由派生类补充。
  return {{"success", true}, {"message", "on_check executed"}};
}

// ==================== 命令注册 ====================

void BaseSubnode::registerCmd(const std::string &cmdName, CmdHandler handler) {
  // 将命令名映射到处理函数，供 actionCallback 动态分发。
  cmdHandlers_[cmdName] = std::move(handler);
  std::cout << "[" << name_.toStdString() << "] Registered command: " << cmdName
            << std::endl;
}

// ==================== ActionServer 回调 ====================

void BaseSubnode::actionCallback(
    uint32_t goalId, const nlohmann::json &goalData,
    std::function<void(const nlohmann::json &)> sendFb,
    std::atomic<bool> & /*shouldCancel*/) {
  // 统一解析 action goal，串行执行命令并把结果回传给客户端。
  Q_UNUSED(sendFb);
  try {
    estopFlag_ = false;

    std::string cmdName = goalData.value("cmd", std::string());
    nlohmann::json params = goalData.value("params", nlohmann::json::object());

    std::cout << "[" << name_.toStdString() << "] Received command '" << cmdName
              << "' (goal_id=" << goalId << ") params=" << params.dump()
              << std::endl;

    nlohmann::json result;

    auto it = cmdHandlers_.find(cmdName);
    if (it != cmdHandlers_.end()) {
      QMutexLocker commandLocker(&commandExecutionLock_);
      result = it->second(goalId, cmdName, params);
    } else {
      std::cerr << "[" << name_.toStdString() << "] Command '" << cmdName
                << "' not found" << std::endl;
      result = {{"success", false},
                {"message", "Command '" + cmdName + "' not registered"}};
    }

    // 发送结果
    bool success = result.value("success", true);
    std::cout << "[" << name_.toStdString() << "] Command result '" << cmdName
              << "' success=" << success << " result=" << result.dump()
              << std::endl;
    sendResult(goalId, result, success);

  } catch (const std::exception &e) {
    std::cerr << "[" << name_.toStdString()
              << "] Error in action callback: " << e.what() << std::endl;
    sendResult(goalId, {{"success", false}, {"message", e.what()}}, false);
  }
}

void BaseSubnode::sendResult(uint32_t goalId, const nlohmann::json &result,
                             bool success) {
  // 将最终结果发布到 action server；若正处于关闭态则直接跳过。
  if (isShuttingDown_)
    return;
  if (!actionServer_) {
    std::cerr << "[" << name_.toStdString()
              << "] Cannot send result, ActionServer not initialized"
              << std::endl;
    return;
  }

  try {
    actionServer_->publishResult(goalId, result, success);
  } catch (const std::exception &e) {
    if (!isShuttingDown_) {
      std::cerr << "[" << name_.toStdString()
                << "] Failed to send result: " << e.what() << std::endl;
    }
  }
}

void BaseSubnode::sendFeedback(uint32_t goalId,
                               const nlohmann::json &feedback) {
  // 向 action client 推送过程反馈，供长任务显示进度或中间状态。
  if (isShuttingDown_)
    return;
  if (!actionServer_)
    return;

  try {
    actionServer_->publishFeedback(goalId, feedback);
  } catch (const std::exception &e) {
    if (!isShuttingDown_) {
      std::cerr << "[" << name_.toStdString()
                << "] Failed to send feedback: " << e.what() << std::endl;
    }
  }
}

// ==================== Publisher 管理 ====================

bool BaseSubnode::createPublisher(int port, const std::string &topicName,
                                  const std::string & /*encoding*/,
                                  double frequency, bool publishToAgent) {
  // 创建一个带频率限制器的 publisher，并可选择暴露给 agent 查询 topic。
  if (publishers_.count(port) > 0) {
    return false;
  }

  try {
    auto pub = std::make_unique<echo::Publisher>(topicName);
    publishers_[port] = std::move(pub);

    rateLimiters_.emplace(port, recordlab::common::RateLimiter(frequency));

    if (publishToAgent && !topicName.empty()) {
      agentTopics_[port] = topicName;
    }

    std::cout << "[" << name_.toStdString() << "] Publisher created on port "
              << port << " (topic: " << topicName << ", freq: " << frequency
              << "Hz)" << std::endl;

    return true;

  } catch (const std::exception &e) {
    std::cerr << "[" << name_.toStdString()
              << "] Failed to create publisher on port " << port << ": "
              << e.what() << std::endl;
    return false;
  }
}

void BaseSubnode::publish(int port, const nlohmann::json &data) {
  // 按端口找到对应 publisher 并在通过频率限制后发布 JSON 数据。
  if (isShuttingDown_)
    return;

  auto pubIt = publishers_.find(port);
  if (pubIt == publishers_.end()) {
    if (!isShuttingDown_) {
      std::cerr << "[" << name_.toStdString() << "] Publisher on port " << port
                << " not found" << std::endl;
    }
    return;
  }

  // 频率限制
  auto rlIt = rateLimiters_.find(port);
  if (rlIt != rateLimiters_.end() && !rlIt->second.check(data)) {
    return;
  }

  try {
    static std::mutex publishLogMutex;
    static std::map<int, uint64_t> publishCounts;
    uint64_t count = 0;
    {
      std::lock_guard<std::mutex> locker(publishLogMutex);
      count = ++publishCounts[port];
    }
    if (count <= 5 || count % 10000 == 0) {
      std::cout << "[" << name_.toStdString() << "][PUBLISH] port=" << port
                << " count=" << count << std::endl;
    }
    pubIt->second->publishRaw(data.dump());
  } catch (const std::exception &e) {
    if (!isShuttingDown_) {
      std::cerr << "[" << name_.toStdString() << "] Failed to publish to port "
                << port << ": " << e.what() << std::endl;
    }
  }
}

bool BaseSubnode::registerCustomData(const std::string &dataName,
                                     const std::string &dataType, int port) {
  // 把自定义数据主题登记给注册中心，方便接收端动态发现。
  if (!registryClient_) {
    std::cerr << "[" << name_.toStdString()
              << "] DataRegistryClient not available" << std::endl;
    return false;
  }

  auto result = registryClient_->registerData(dataName, dataType, port,
                                              name_.toStdString());
  return result.success;
}

// ==================== 内部方法 ====================

nlohmann::json BaseSubnode::createActionServer() {
  // 创建 action server，并把收到的 goal 统一路由到 actionCallback。
  try {
    if (actionServer_) {
      closeActionServer();
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "[" << name_.toStdString() << "] Creating ActionServer..."
              << std::endl;

    std::string actionName = name_.toStdString() + "_actions";

    actionServer_ = std::make_unique<echo::ActionServer>(
        actionName, [this](uint32_t goalId, const nlohmann::json &goal,
                           std::function<void(const nlohmann::json &)> sendFb,
                           std::atomic<bool> &shouldCancel) {
          this->actionCallback(goalId, goal, std::move(sendFb), shouldCancel);
        });

    std::cout << "[" << name_.toStdString() << "] ActionServer created"
              << std::endl;
    return {{"success", true}, {"message", "ActionServer created"}};

  } catch (const std::exception &e) {
    std::cerr << "[" << name_.toStdString()
              << "] Error creating ActionServer: " << e.what() << std::endl;
    return {{"success", false}, {"message", e.what()}};
  }
}

void BaseSubnode::closeActionServer() {
  // 关闭 action server，阻止后续新的命令进入当前子节点。
  if (!actionServer_)
    return;
  std::cout << "[" << name_.toStdString() << "] Closing ActionServer..."
            << std::endl;
  actionServer_.reset();
  std::cout << "[" << name_.toStdString() << "] ActionServer closed"
            << std::endl;
}

void BaseSubnode::closeAllPublishers() {
  // 关闭全部数据发布器并清空对应限频器。
  if (publishers_.empty())
    return;
  std::cout << "[" << name_.toStdString() << "] Closing all publishers..."
            << std::endl;
  publishers_.clear();
  rateLimiters_.clear();
  std::cout << "[" << name_.toStdString() << "] All publishers closed"
            << std::endl;
}

// ============================================================================
// spin
// ============================================================================

static std::atomic<bool> g_spinFlag{false};

void spin(BaseSubnode &subnode) {
  // 提供一个最小运行循环：连接子节点、等待信号并在退出时做统一收尾。
  g_spinFlag = false;

  // 注册信号处理器
  auto handler = [](int signum) {
    std::cout << "Received signal " << signum << ", shutting down..."
              << std::endl;
    g_spinFlag = true;
  };
  std::signal(SIGTERM, handler);
  std::signal(SIGINT, handler);

  auto result = subnode.connect();
  if (!result.value("success", false)) {
    std::cerr << "Failed to connect: "
              << result.value("message", std::string("Unknown error"))
              << std::endl;
    return;
  }

  std::cout << "[" << subnode.name().toStdString() << "] SubNode ready"
            << std::endl;

  try {
    while (!g_spinFlag) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  } catch (...) {
    std::cout << "Interrupted" << std::endl;
  }

  std::cout << "Release SubNode..." << std::endl;
  subnode.disconnect();
  subnode.release();
}

} // namespace recordlab::subnodes
