/*
 * ControlCenterMainWindow 实现
 */
#include "recordlab/ui/control_center_main.h"
#include "recordlab/backend/agent_manager_process.h"
#include "recordlab/backend/data_receiver.h"

#include <QCloseEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <iostream>

namespace recordlab::ui {

ControlCenterMainWindow::ControlCenterMainWindow(QWidget *parent)
    : QMainWindow(parent) {
  // 构造旧版控制中心壳层，并在后台拉起 manager 与 data receiver。
  initUi();
  setupTimers();

  agentManagerProcess_ = std::make_unique<backend::AgentManagerProcess>();
  dataReceiver_ = std::make_unique<backend::DataReceiverManager>();

  agentManagerProcess_->start();
  dataReceiver_->start();

  std::cout << "[ControlCenter] Initialized" << std::endl;
}

ControlCenterMainWindow::~ControlCenterMainWindow() {
  // 析构时先停掉后台线程型组件，避免窗口销毁后仍有回调落回 UI。
  dataReceiver_->stop();
  agentManagerProcess_->stop();
}

void ControlCenterMainWindow::initUi() {
  // 初始化旧版风格的入口页和工作区占位 tab。
  setWindowTitle("RecordLab C++ - Control Center");
  resize(1400, 900);

  stackedWidget_ = new QStackedWidget(this);
  setCentralWidget(stackedWidget_);

  // Entry page
  entryPage_ = new QWidget();
  auto *entryLayout = new QVBoxLayout(entryPage_);
  auto *titleLabel = new QLabel("RecordLab C++ Control Center");
  titleLabel->setAlignment(Qt::AlignCenter);
  titleLabel->setStyleSheet(
      "font-size: 24px; font-weight: bold; padding: 20px;");
  entryLayout->addWidget(titleLabel);

  auto *agentList = new QListWidget();
  agentList->addItems(
      {"glasses_bsp_node", "imu_simulation", "localhost"});
  connect(agentList, &QListWidget::itemDoubleClicked,
          [this](QListWidgetItem *item) {
            onAgentSelected(item->text().toStdString());
          });
  entryLayout->addWidget(agentList);
  stackedWidget_->addWidget(entryPage_);

  // Workspace tabs (created on agent selection)
  tabWidget_ = new QTabWidget();
  stackedWidget_->addWidget(tabWidget_);
}

void ControlCenterMainWindow::setupTimers() {
  // 启动一个轮询定时器，用于后续承接 dialog 请求等异步事件。
  dialogCheckTimer_ = new QTimer(this);
  connect(dialogCheckTimer_, &QTimer::timeout, this,
          &ControlCenterMainWindow::checkDialogRequests);
  dialogCheckTimer_->start(100);
}

void ControlCenterMainWindow::onAgentSelected(const std::string &agentName) {
  // 选中入口页 agent 后切换到工作区，并在 BSP agent 场景下触发一键启动。
  currentAgent_ = agentName;
  createWorkspace(agentName);
  stackedWidget_->setCurrentWidget(tabWidget_);

  // Auto one-click for BSP
  if (agentName == "glasses_bsp_node") {
    startGlassesBspOneClick();
  }
}

void ControlCenterMainWindow::createWorkspace(const std::string &agentName) {
  // 根据当前 agent 重建四个基础 tab，占位承载旧版工作区结构。
  tabWidget_->clear();

  // Tab 1: Script Execution
  auto *tab1 = new QWidget();
  auto *tab1Layout = new QVBoxLayout(tab1);
  tab1Layout->addWidget(new QLabel(
      QString("Script Execution - %1").arg(QString::fromStdString(agentName))));
  auto *backBtn = new QPushButton("← Back to Entry");
  connect(backBtn, &QPushButton::clicked, this,
          &ControlCenterMainWindow::backToEntry);
  tab1Layout->addWidget(backBtn);
  tabWidget_->addTab(tab1, "脚本执行");

  // Tab 2: Single Operation
  auto *tab2 = new QWidget();
  auto *tab2Layout = new QVBoxLayout(tab2);
  tab2Layout->addWidget(new QLabel(
      QString("Single Operation - %1").arg(QString::fromStdString(agentName))));
  tabWidget_->addTab(tab2, "单个操作");

  // Tab 3: Agent Management
  auto *tab3 = new QWidget();
  auto *tab3Layout = new QVBoxLayout(tab3);
  tab3Layout->addWidget(new QLabel(
      QString("Agent Management - %1").arg(QString::fromStdString(agentName))));
  tabWidget_->addTab(tab3, "Agent管理");

  // Tab 4: Script Debug
  auto *tab4 = new QWidget();
  auto *tab4Layout = new QVBoxLayout(tab4);
  tab4Layout->addWidget(new QLabel(
      QString("Script Debug - %1").arg(QString::fromStdString(agentName))));
  tabWidget_->addTab(tab4, "脚本调试");

  std::cout << "[ControlCenter] Workspace created for: " << agentName
            << std::endl;
}

void ControlCenterMainWindow::backToEntry() {
  // 返回入口页时重置当前 agent 和一键流程状态。
  currentAgent_.clear();
  oneClickInProgress_ = false;
  oneClickSuccess_ = false;
  stackedWidget_->setCurrentWidget(entryPage_);
}

void ControlCenterMainWindow::startGlassesBspOneClick() {
  // 发起旧版壳层里的简化一键启动流程，目前先完成 init_agent 阶段。
  if (oneClickInProgress_)
    return;
  oneClickInProgress_ = true;
  oneClickPhase_ = "connect";

  // Send init agent command
  agentManagerProcess_->sendCommand(
      {{"action", "init_agent"}, {"agent_name", "glasses_bsp_node"}});

  oneClickTimer_ = new QTimer(this);
  connect(oneClickTimer_, &QTimer::timeout, this,
          &ControlCenterMainWindow::pollOneClickStatus);
  oneClickTimer_->start(500);

  std::cout << "[ControlCenter] One-click BSP startup initiated" << std::endl;
}

void ControlCenterMainWindow::pollOneClickStatus() {
  // 预留给一键状态轮询的阶段机入口，当前仍是简化占位实现。
  if (!oneClickInProgress_) {
    if (oneClickTimer_)
      oneClickTimer_->stop();
    return;
  }
  // Phase machine would poll for responses here
  // Simplified: just mark success after connect
}

void ControlCenterMainWindow::initDevice() {
  // 通过 AgentManagerProcess 给当前 agent 发起 init_device 命令。
  agentManagerProcess_->sendCommand({{"action", "send_agent_command"},
                                     {"agent_name", currentAgent_},
                                     {"args", {{"cmd_name", "init_device"}}}});

  initDeviceTimer_ = new QTimer(this);
  connect(initDeviceTimer_, &QTimer::timeout, this,
          &ControlCenterMainWindow::pollInitDeviceResult);
  initDeviceTimer_->start(500);
}

void ControlCenterMainWindow::pollInitDeviceResult() {
  // 预留给 init_device 回执轮询的钩子。
  // Poll for init_device response
}

void ControlCenterMainWindow::checkDialogRequests() {
  // 预留给 dialogRequest 轮询处理的钩子。
  // Check agent manager process for dialog requests
}

void ControlCenterMainWindow::closeEvent(QCloseEvent *event) {
  // 窗口关闭时同步停止后台组件，避免应用退出过程拖延。
  dataReceiver_->stop();
  agentManagerProcess_->stop();
  event->accept();
}

} // namespace recordlab::ui
