/*
 * UIMessageBridge 实现
 */
#include "recordlab/core/ui_message_bridge.h"

#include <QDateTime>
#include <chrono>
#include <iostream>
#include <thread>

namespace recordlab::core {

// ============================================================================
// UIMessage
// ============================================================================

nlohmann::json UIMessage::toJson() const {
  // 将 UI 消息序列化成统一 JSON 结构，便于日志记录和跨模块桥接。
  return {{"message_type", messageType.toStdString()},
          {"title", title.toStdString()},
          {"content", content.toStdString()},
          {"timeout", timeout},
          {"need_response", needResponse},
          {"auto_close", autoClose},
          {"priority", priority},
          {"timestamp", timestamp},
          {"message_id", messageId.toStdString()}};
}

// ============================================================================
// UIMessageBridge
// ============================================================================

UIMessageBridge::UIMessageBridge(QObject *parent) : QObject(parent) {}

// 析构时停止后台消息线程，避免对象释放后仍有循环访问成员。
UIMessageBridge::~UIMessageBridge() { stop(); }

void UIMessageBridge::start() {
  // 惰性启动处理线程；重复调用保持幂等，防止出现多条并行消息循环。
  if (running_)
    return;
  running_ = true;
  thread_ = std::make_unique<QThread>();
  QObject::connect(thread_.get(), &QThread::started,
                   [this]() { processLoop(); });
  thread_->start();
  std::cout << "[UIMessageBridge] started" << std::endl;
}

void UIMessageBridge::stop() {
  // 平滑停止消息线程，并在有限时间内等待线程退出。
  if (!running_)
    return;
  running_ = false;
  if (thread_ && thread_->isRunning()) {
    thread_->quit();
    thread_->wait(2000);
  }
  std::cout << "[UIMessageBridge] stopped" << std::endl;
}

void UIMessageBridge::processLoop() {
  // 后台循环负责派发排队消息，并定期清理超时待响应消息。
  while (running_) {
    {
      QMutexLocker locker(&queueLock_);
      while (!messageQueue_.isEmpty()) {
        auto msg = messageQueue_.dequeue();
        if (msg.needResponse) {
          pendingMessages_[msg.messageId.toStdString()] = msg;
        }
        if (uiMessageHandler_) {
          try {
            uiMessageHandler_(msg);
          } catch (...) {
          }
        }
        emit messageReceived(msg);
      }

      // Check timeouts
      double now = std::chrono::duration<double>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
      std::vector<std::string> timedOut;
      for (auto &[id, m] : pendingMessages_) {
        if (m.timeout > 0 && (now - m.timestamp) > m.timeout) {
          timedOut.push_back(id);
        }
      }
      for (auto &id : timedOut) {
        auto &m = pendingMessages_[id];
        if (m.autoClose) {
          UIResponse resp;
          resp.messageId = QString::fromStdString(id);
          resp.buttonClicked = "TIMEOUT";
          resp.timestamp = now;
          responseQueue_.enqueue(resp);
          emit responseReceived(resp);
        }
        pendingMessages_.erase(id);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

static double currentTimeSec() {
  // 采用单调时钟计时，避免系统时间回拨影响 timeout 判断。
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

QString UIMessageBridge::sendMessage(const UIMessage &message) {
  // 为消息补齐时间戳和 ID 后入队，让调用方不必关心队列细节。
  UIMessage msg = message;
  if (msg.timestamp == 0.0)
    msg.timestamp = currentTimeSec();
  if (msg.messageId.isEmpty()) {
    msg.messageId = QString("%1_%2")
                        .arg(msg.messageType)
                        .arg(static_cast<qint64>(msg.timestamp * 1000));
  }
  QMutexLocker locker(&queueLock_);
  messageQueue_.enqueue(msg);
  return msg.messageId;
}

QString UIMessageBridge::sendInfo(const QString &title, const QString &content,
                                  double timeout) {
  // 发送无需用户反馈的普通信息提示。
  return sendMessage({"info", title, content, timeout, {"OK"}, false, true});
}

QString UIMessageBridge::sendWarning(const QString &title,
                                     const QString &content, double timeout) {
  // 发送 warning 类型消息，展示样式由 UI 层自行决定。
  return sendMessage({"warning", title, content, timeout, {"OK"}, false, true});
}

QString UIMessageBridge::sendError(const QString &title, const QString &content,
                                   double timeout) {
  // 错误消息默认不自动关闭，确保用户明确看到异常。
  return sendMessage({"error", title, content, timeout, {"OK"}, false, false});
}

QString UIMessageBridge::sendConfirm(const QString &title,
                                     const QString &content,
                                     const QStringList &buttons,
                                     double timeout) {
  // 发送需要按钮反馈的确认消息，可按 timeout 决定是否允许自动关闭。
  return sendMessage(
      {"confirm", title, content, timeout, buttons, true, timeout > 0});
}

QString UIMessageBridge::sendInput(const QString &title, const QString &content,
                                   double timeout) {
  // 输入消息固定使用 OK/Cancel 响应，便于上层脚本统一处理。
  return sendMessage(
      {"input", title, content, timeout, {"OK", "Cancel"}, true, false});
}

QString UIMessageBridge::sendProgress(const QString &title,
                                      const QString &content) {
  // 进度消息用于长任务状态展示，不要求用户交互。
  return sendMessage({"progress", title, content, 0.0, {}, false, false});
}

void UIMessageBridge::submitResponse(const UIResponse &response) {
  // UI 端回应后，从 pending 集合移除该消息，并通知等待方有结果可取。
  QMutexLocker locker(&queueLock_);
  pendingMessages_.erase(response.messageId.toStdString());
  responseQueue_.enqueue(response);
  emit responseReceived(response);
}

std::optional<UIResponse>
UIMessageBridge::waitForResponse(const QString &messageId, double timeout) {
  // 同步等待指定消息的响应，适合串行流程或阻塞式脚本使用。
  double start = currentTimeSec();
  while (true) {
    {
      QMutexLocker locker(&queueLock_);
      for (int i = 0; i < responseQueue_.size(); ++i) {
        if (responseQueue_[i].messageId == messageId) {
          auto resp = responseQueue_.takeAt(i);
          return resp;
        }
      }
    }
    if (timeout > 0 && (currentTimeSec() - start) > timeout)
      return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void UIMessageBridge::setUIHandler(UIMessageHandler handler) {
  // 注册一个可选 UI 回调，用于把桥接消息转成实际弹窗或通知。
  uiMessageHandler_ = std::move(handler);
}

int UIMessageBridge::getPendingCount() const {
  // 返回当前仍在等待用户响应的消息数量，便于监控桥接负载。
  QMutexLocker locker(&queueLock_);
  return static_cast<int>(pendingMessages_.size());
}

void UIMessageBridge::clearPending() {
  // 清空全部待响应消息，多用于流程重置或强制退出场景。
  QMutexLocker locker(&queueLock_);
  pendingMessages_.clear();
}

UIMessageBridge &UIMessageBridge::instance() {
  // 通过单例暴露全局消息桥，供各模块共享同一套 UI 通知管道。
  static UIMessageBridge b;
  return b;
}

} // namespace recordlab::core
