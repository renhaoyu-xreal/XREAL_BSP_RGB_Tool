/*
 * BaseAgent - Agent 基类
 *
 * 负责与 SubNode 通信：
 * - 启动/停止子节点进程
 * - 通过 ActionClient 发送命令
 * - 通过 Subscriber 订阅状态
 *
 * 所有方法名、参数名与 Python 版完全一致。
 */
#pragma once

#include <QMap>
#include <QMutex>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QVariantMap>

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// 前向声明 echo 类型
namespace echo {
class ActionClient;
class Subscriber;
} // namespace echo

namespace recordlab::flowagent::agents {

class LegacyRemoteActionClient;

// ============================================================================
// CmdStorage - 命令结果存储（线程安全）
//
// ============================================================================

class CmdStorage {
public:
  void initGoal(uint32_t goalId);
  void addFeedback(uint32_t goalId, const nlohmann::json &feedback);
  void setResult(uint32_t goalId, const nlohmann::json &result, bool success);

  struct ResultEntry {
    nlohmann::json result;
    bool success = false;
  };

  std::optional<ResultEntry> getResult(uint32_t goalId) const;
  std::vector<nlohmann::json> getFeedbacks(uint32_t goalId) const;
  bool hasGoal(uint32_t goalId) const;
  void clearGoal(uint32_t goalId);
  void reset();

private:
  mutable QMutex lock_;
  std::unordered_map<uint32_t, std::vector<nlohmann::json>> feedbacks_;
  std::unordered_map<uint32_t, std::optional<ResultEntry>> results_;
};

// ============================================================================
// BaseAgent - Agent 基类
//
// 注意: Python 的 async cmd() 在 C++ 中改为同步+超时，
// 外部调用者使用 QtConcurrent / QThread 实现异步。
// ============================================================================

struct CmdResult {
  bool success = false;
  QString message;
  nlohmann::json result;   // 原始 result payload
  nlohmann::json feedback; // 可选 feedback
  int statusValue = 0;

  CmdResult() = default;
  CmdResult(bool successValue, const QString &messageValue,
            const nlohmann::json &resultValue = {},
            const nlohmann::json &feedbackValue = {}, int statusCode = 0)
      : success(successValue), message(messageValue), result(resultValue),
        feedback(feedbackValue), statusValue(statusCode) {}
};

struct CmdStatus {
  bool done = false;
  QString status; // "SUCCEEDED", "FAILED", "CANCELED" or empty
  nlohmann::json result;

  CmdStatus() = default;
  CmdStatus(bool doneValue, const QString &statusValue,
            const nlohmann::json &resultValue = {})
      : done(doneValue), status(statusValue), result(resultValue) {}
};

class BaseAgent : public QObject {
  Q_OBJECT

public:
  explicit BaseAgent(const QString &name, const QString &subnodePath,
                     const QString &subnodeHost = "127.0.0.1",
                     int goalPort = 5690, int feedbackPort = 5691,
                     const QString &rootPath = "./output",
                     const QVariantMap &customParams = {},
                     QObject *parent = nullptr);
  ~BaseAgent() override;

  // ========== 对外接口 ==========

  /*
   * 连接 SubNode（启动进程并创建客户端）
   */
  virtual CmdResult connect();

  /*
   * 断开与 SubNode 的连接（关闭客户端并终止进程）
   */
  virtual CmdResult disconnect();

  /*
   * 发送命令到 SubNode（同步等待结果）
   *
   * @param cmdName       命令名称
   * @param params        命令参数
   * @param waitForResult 是否阻塞等待结果
   * @param timeout       超时时间（秒）
   */
  virtual CmdResult cmd(const QString &cmdName,
                        const nlohmann::json &params = {},
                        bool waitForResult = true, double timeout = 30.0);

  /*
   * 获取命令执行状态
   */
  CmdStatus getCmdStatus(uint32_t goalId);

  /*
   * 重置状态变量（不影响连接）
   */
  void reset();

  /*
   * 从SubNode同步获取root_path配置
   */
  CmdResult getRootPathSync();

  /*
   * 从远程机器复制文件夹到本地
   */
  CmdResult copyFolderFromRemote(const QString &remotePath,
                                 const QString &localPath,
                                 const QString &method = "scp",
                                 const QString &remoteHost = {},
                                 const QString &remoteUser = {},
                                 const QString &remotePassword = {},
                                 const QString &adbDevice = {});

  /*
   * 订阅 Topic
   */
  void createSubscriber(int port, const QString &topic = {},
                        const QString &encoding = "json");

  /*
   * 获取 Topic 最新数据
   */
  nlohmann::json getTopicData(int port);
  nlohmann::json getTopicDataByName(const QString &topicName);

  /*
   * 获取 Topic 接收数据的计数
   */
  int getTopicReceiveCount(int port);

  // ========== 属性 ==========
  const QString &name() const { return name_; }
  bool isConnected() const { return isConnected_; }
  const QString &subnodeHost() const { return subnodeHost_; }
  int goalPort() const { return goalPort_; }
  int feedbackPort() const { return feedbackPort_; }
  const QString &rootPath() const { return rootPath_; }

private:
  mutable QMutex commandLock_;

  // ========== 内部方法 ==========

  // 启动子节点进程
  CmdResult launchSubnode();

  // 终止子节点进程
  CmdResult terminateSubnode();

  // 清理残留进程
  void cleanupStaleSubnodes();

  // 创建 ActionClient
  CmdResult createActionClient();

  // 关闭 ActionClient
  void closeActionClient();

  // 关闭所有 Subscriber
  void closeAllSubscribers();

  // 在断开前优雅释放设备
  void gracefulReleaseBeforeDisconnect();

  // 同步发送命令（disconnect 流程使用）
  // _send_command_sync_direct()
  CmdResult sendCommandSyncDirect(const QString &cmdName,
                                  const nlohmann::json &params = {},
                                  double timeout = 5.0);

  // Topic 数据回调
  void onTopicData(int port, const std::string &rawData);

  // 检查命令是否完成
  bool isCmdDone(uint32_t goalId);

  // 判断是否远程模式
  bool isRemoteMode() const;

  // ========== 数据成员 ==========
  QString name_;
  QString subnodePath_;
  QString subnodeHost_;
  int goalPort_;
  int feedbackPort_;
  QString rootPath_;
  QVariantMap customParams_;

  bool isConnected_ = false;

protected:
  void setConnectedState(bool connected) { isConnected_ = connected; }

  // 子节点进程
  std::unique_ptr<QProcess> subnodeProcess_;

  // IPC 客户端
  std::unique_ptr<echo::ActionClient> actionClient_;
  std::unique_ptr<LegacyRemoteActionClient> legacyRemoteActionClient_;

  // 订阅者
  std::unordered_map<int, std::unique_ptr<echo::Subscriber>> subscribers_;

  // Topic 数据缓存
  mutable QMutex topicDataLock_;
  std::unordered_map<int, nlohmann::json> topicData_;
  std::unordered_map<std::string, int> topicToPort_;
  std::unordered_map<int, int> topicReceiveCount_;

  // 命令结果存储
  CmdStorage cmdStorage_;
};

} // namespace recordlab::flowagent::agents
