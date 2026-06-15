/*
 * ControlCenterMainWindow - 控制中心主窗口
 *
 * 管理整个应用工作区：
 * - 入口页 Agent 选择
 * - 工作区创建（Tab1-4）
 * - 一键启动流程（glasses_bsp_node）
 * - 数据接收与分发
 *
 */
#pragma once

#include <QMainWindow>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace recordlab::backend {
class AgentManagerProcess;
class DataReceiverManager;
class AgentWatchdog;
} // namespace recordlab::backend

namespace recordlab::ui {

class ControlCenterMainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit ControlCenterMainWindow(QWidget *parent = nullptr);
  ~ControlCenterMainWindow() override;

  // ========== 工作区管理 ==========
  void onAgentSelected(const std::string &agentName);
  void createWorkspace(const std::string &agentName);
  void backToEntry();

  // ========== 一键启动（glasses_bsp_node）==========
  void startGlassesBspOneClick();

  // ========== 设备初始化 ==========
  void initDevice();

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  void initUi();
  void setupTimers();
  void checkDialogRequests();
  void pollOneClickStatus();
  void pollInitDeviceResult();

  // UI
  QStackedWidget *stackedWidget_ = nullptr;
  QWidget *entryPage_ = nullptr;
  QTabWidget *tabWidget_ = nullptr;

  // Backend
  std::unique_ptr<backend::AgentManagerProcess> agentManagerProcess_;
  std::unique_ptr<backend::DataReceiverManager> dataReceiver_;

  // Timers
  QTimer *dialogCheckTimer_ = nullptr;
  QTimer *oneClickTimer_ = nullptr;
  QTimer *initDeviceTimer_ = nullptr;

  // State
  std::string currentAgent_;
  bool oneClickInProgress_ = false;
  bool oneClickSuccess_ = false;
  std::string oneClickPhase_;
};

} // namespace recordlab::ui
