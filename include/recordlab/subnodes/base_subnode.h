/*
 * BaseSubnode - 子节点基类
 *
 * 与 BaseAgent 配合使用，负责接收和处理命令。
 *
 * 核心功能：
 * - ActionServer：处理来自 Agent 的命令
 * - Publisher：发布数据到 Agent
 *
 * 所有方法名、字段名与 Python 版完全一致。
 */
#pragma once

#include <QMap>
#include <QMutex>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

#include "recordlab/common/data_registry_client.h"
#include "recordlab/common/rate_limiter.h"

// 前向声明 echo 类型
namespace echo {
class ActionServer;
class Publisher;
} // namespace echo

namespace recordlab::subnodes {

// 命令处理函数签名
using CmdHandler = std::function<nlohmann::json(
    uint32_t goalId, const std::string &cmdName, const nlohmann::json &params)>;

/*
 * SubNode 基类 - 处理来自 Agent 的命令
 *
 */
class BaseSubnode : public QObject {
  Q_OBJECT

public:
  explicit BaseSubnode(const QString &name,
                       const QString &subnodeHost = "127.0.0.1",
                       int goalPort = 5690, int feedbackPort = 5691,
                       const QString &rootPath = "./output",
                       QObject *parent = nullptr);
  ~BaseSubnode() override;

  // ========== 生命周期 ==========

  nlohmann::json connect();    // 创建 ActionServer
  nlohmann::json disconnect(); // 关闭所有通信组件
  nlohmann::json release();    // 释放资源

  // ========== 命令/状态 ==========

  nlohmann::json estop(); // 紧急停止
  nlohmann::json check(); // 检查状态

  // ========== 虚函数钩子（子类重载） ==========

  virtual nlohmann::json onRelease();
  virtual nlohmann::json onEstop();
  virtual nlohmann::json onCheck();

  // ========== 命令注册 ==========

  void registerCmd(const std::string &cmdName, CmdHandler handler);

  // ========== Publisher 管理 ==========

  bool createPublisher(int port, const std::string &topicName = "",
                       const std::string &encoding = "json",
                       double frequency = 0.0, bool publishToAgent = false);

  void publish(int port, const nlohmann::json &data);

  // ========== 自定义数据注册 ==========

  bool registerCustomData(const std::string &dataName,
                          const std::string &dataType, int port);

  // ========== Feedback ==========

  void sendFeedback(uint32_t goalId, const nlohmann::json &feedback);

  // ========== 属性 ==========

  const QString &name() const { return name_; }
  bool isConnected() const { return isConnected_; }
  const QString &rootPath() const { return rootPath_; }
  bool estopFlag() const { return estopFlag_; }

protected:
  QString name_;
  QString subnodeHost_;
  int goalPort_;
  int feedbackPort_;
  QString rootPath_;

  std::atomic<bool> estopFlag_{false};
  std::atomic<bool> isShuttingDown_{false};

private:
  bool isConnected_ = false;

  // ActionServer
  std::unique_ptr<echo::ActionServer> actionServer_;

  // Publishers: port -> Publisher
  std::unordered_map<int, std::unique_ptr<echo::Publisher>> publishers_;
  std::unordered_map<int, recordlab::common::RateLimiter> rateLimiters_;

  // 命令处理器
  std::unordered_map<std::string, CmdHandler> cmdHandlers_;
  QMutex commandExecutionLock_;

  // Agent 话题映射: port -> topicName
  std::unordered_map<int, std::string> agentTopics_;

  // 数据注册客户端
  std::unique_ptr<recordlab::common::DataRegistryClient> registryClient_;

  // ========== 内部方法 ==========

  void registerBuiltinCommands();
  nlohmann::json createActionServer();
  void closeActionServer();
  void closeAllPublishers();

  // ActionServer 回调
  void actionCallback(uint32_t goalId, const nlohmann::json &goalData,
                      std::function<void(const nlohmann::json &)> sendFeedback,
                      std::atomic<bool> &shouldCancel);

  void sendResult(uint32_t goalId, const nlohmann::json &result, bool success);
};

/*
 * spin — 标准主循环
 *
 * 连接子节点，注册信号处理，保持进程运行。
 */
void spin(BaseSubnode &subnode);

} // namespace recordlab::subnodes
