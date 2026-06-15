/*
 * UIMessageBridge - UI 消息桥接器
 *
 * 作为系统与 UI 之间的消息桥梁
 *
 */
#pragma once

#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QThread>
#include <QVariant>

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_map>

namespace recordlab::core {

// ============================================================================
// 消息/按钮/响应 类型
// UIResponse
// ============================================================================

struct UIMessage {
  QString messageType; // "info", "warning", "error", "confirm", "input",
                       // "progress", "notification"
  QString title;
  QString content;
  double timeout = 0.0;
  QStringList buttons = {"OK"};
  bool needResponse = false;
  bool autoClose = false;
  int priority = 0;
  double timestamp = 0.0;
  QString messageId;

  nlohmann::json toJson() const;
};

struct UIResponse {
  QString messageId;
  QString buttonClicked;
  QVariant inputValue;
  double timestamp = 0.0;
};

// ============================================================================
// UIMessageBridge
// ============================================================================

class UIMessageBridge : public QObject {
  Q_OBJECT

public:
  using UIMessageHandler = std::function<void(const UIMessage &)>;

  explicit UIMessageBridge(QObject *parent = nullptr);
  ~UIMessageBridge() override;

  void start();
  void stop();

  // ========== 发送消息 ==========
  QString sendMessage(const UIMessage &message);
  QString sendInfo(const QString &title, const QString &content,
                   double timeout = 3.0);
  QString sendWarning(const QString &title, const QString &content,
                      double timeout = 5.0);
  QString sendError(const QString &title, const QString &content,
                    double timeout = 0.0);
  QString sendConfirm(const QString &title, const QString &content,
                      const QStringList &buttons = {"Yes", "No"},
                      double timeout = 0.0);
  QString sendInput(const QString &title, const QString &content,
                    double timeout = 0.0);
  QString sendProgress(const QString &title, const QString &content);

  // ========== 响应处理 ==========
  void submitResponse(const UIResponse &response);
  std::optional<UIResponse> waitForResponse(const QString &messageId,
                                            double timeout = 0.0);

  // ========== 回调 ==========
  void setUIHandler(UIMessageHandler handler);

  int getPendingCount() const;
  void clearPending();

  // ========== 单例 ==========
  static UIMessageBridge &instance();

signals:
  void messageReceived(const UIMessage &message);
  void responseReceived(const UIResponse &response);

private:
  void processLoop();

  mutable QMutex queueLock_;
  QQueue<UIMessage> messageQueue_;
  QQueue<UIResponse> responseQueue_;
  std::unordered_map<std::string, UIMessage> pendingMessages_;

  UIMessageHandler uiMessageHandler_;

  bool running_ = false;
  std::unique_ptr<QThread> thread_;
};

} // namespace recordlab::core
