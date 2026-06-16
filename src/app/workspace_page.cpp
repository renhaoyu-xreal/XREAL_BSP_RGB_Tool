#include "recordlab/app/workspace_page.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QTabWidget>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>

#include <master.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "recordlab/backend/agent_manager_process.h"
#include "recordlab/backend/data_receiver.h"
#include "recordlab/bsp/bsp_page.h"
#include "recordlab/bsp/bsp_rgb_page.h"
#include "recordlab/workflow/workflow_controller.h"
#include "recordlab/common/commands.h"
#include "recordlab/common/topics.h"
#include "recordlab/core/compatibility_contract.h"

/*
 * workspace_page.cpp
 *
 * 这个文件现在开始承担真正的“页面协调层”职责：
 * 1. 继续维持旧版 Tab1/Tab2/Tab3/Tab4 结构；
 * 2. 将所有 BSP 页面统一挂到同一个 WorkflowController；
 * 3. 将控制器动作接到真实 AgentManagerProcess，而不是只停留在日志占位。
 *
 * 这样用户在 Tab3 / Tab2 里的操作终于可以影响实际 agent/subnode 生命周期，
 * 同时页面本身仍然不直接持有一堆零散业务逻辑。
 */
namespace recordlab::app {

namespace {

nlohmann::json variantMapToJson(const QVariantMap &payload) {
  // 这里统一通过 Qt JSON 做一次中转，避免手写大量 QVariant 分支转换。
  // 对当前工作流动作来说，payload 都是简单的 string/bool/number/object
  // 结构，足够稳定。
  const auto document = QJsonDocument::fromVariant(payload);
  const auto bytes = document.toJson(QJsonDocument::Compact);
  auto parsed = nlohmann::json::parse(bytes.constData(), nullptr, false);
  return parsed.is_discarded() ? nlohmann::json::object() : parsed;
}

bool isLocalMasterReachable(int timeoutMs = 200) {
  // 用短超时探测本地 echo master，可避免重复拉起服务发现进程。
  QTcpSocket socket;
  socket.connectToHost(QStringLiteral("127.0.0.1"), 5590);
  const bool connected = socket.waitForConnected(timeoutMs);
  if (connected) {
    socket.disconnectFromHost();
  }
  return connected;
}

class EchoMasterRuntime {
public:
  // 析构时清理内部启动的 master 和线程，避免 GUI 退出后遗留后台任务。
  ~EchoMasterRuntime() { stop(); }

  void ensureStarted() {
    // 优先复用外部 master；只有本地不可达时才启动内嵌实例。
    if (startedInternally_ || usingExternal_) {
      return;
    }

    if (isLocalMasterReachable()) {
      usingExternal_ = true;
      std::cout << "[EchoMasterRuntime] Reusing existing master on "
                   "tcp://127.0.0.1:5590"
                << std::endl;
      return;
    }

    master_ = std::make_unique<echo::Master>(5590);
    masterThread_ = std::thread([this]() {
      try {
        master_->start();
      } catch (const std::exception &e) {
        std::cerr << "[EchoMasterRuntime] Master thread failed: " << e.what()
                  << std::endl;
      }
    });

    for (int attempt = 0; attempt < 20; ++attempt) {
      if (isLocalMasterReachable()) {
        startedInternally_ = true;
        std::cout << "[EchoMasterRuntime] Started embedded master on "
                     "tcp://127.0.0.1:5590"
                  << std::endl;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cerr << "[EchoMasterRuntime] Embedded master did not become reachable "
                 "in time"
              << std::endl;
  }

private:
  void stop() {
    // 按 stop -> join -> reset 的顺序回收资源，避免线程访问悬空对象。
    if (master_) {
      master_->stop();
    }
    if (masterThread_.joinable()) {
      masterThread_.join();
    }
    master_.reset();
  }

  std::unique_ptr<echo::Master> master_;
  std::thread masterThread_;
  bool startedInternally_ = false;
  bool usingExternal_ = false;
};

EchoMasterRuntime &echoMasterRuntime() {
  // 通过静态单例共享 master 运行时，避免多个页面重复起服务。
  static EchoMasterRuntime runtime;
  return runtime;
}

bool isExclusiveGlassesAgent(const QString &agentName) {
  // 本地眼镜链路共享同一套 XREAL bridge / USB 设备资源，切换主链路时
  // 必须先释放旧 agent，避免两个 subnode 同时抢设备导致 check 超时。
  return agentName == QString::fromUtf8(recordlab::core::compat::kPrimaryBspAgent) ||
         agentName == QString::fromUtf8(recordlab::core::compat::kPrimaryNvizAgent) ||
         agentName == QString::fromUtf8(recordlab::core::compat::kPrimaryHelenAgent);
}

} // namespace

WorkspacePage::WorkspacePage(const recordlab::core::AppContext &context,
                             QWidget *parent)
    : QWidget(parent), context_(context) {
  setAutoFillBackground(true);
  QPalette pagePalette = palette();
  pagePalette.setColor(QPalette::Window, QColor(QStringLiteral("#f4f1ea")));
  setPalette(pagePalette);
  setStyleSheet(QStringLiteral("WorkspacePage { background: #f4f1ea; }"));

  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(8, 8, 8, 8);
  rootLayout->setSpacing(8);

  auto *toolbarFrame = new QFrame(this);
  toolbarFrame->setAutoFillBackground(true);
  toolbarFrame->setStyleSheet(
      QStringLiteral("QFrame { background: #f4f1ea; border: none; }"));
  auto *toolbarLayout = new QHBoxLayout(toolbarFrame);
  toolbarLayout->setContentsMargins(0, 0, 0, 0);
  toolbarLayout->setSpacing(10);

  timerValueLabel_ = new QLabel(QStringLiteral("录制时长: --"), toolbarFrame);
  delayValueLabel_ = new QLabel(QStringLiteral("时间延迟: --"), toolbarFrame);
  watchdogValueLabel_ =
      new QLabel(QStringLiteral("Watchdog: 无监控"), toolbarFrame);
  toolbarLayout->addStretch(1);
  toolbarLayout->addWidget(timerValueLabel_, 0);
  toolbarLayout->addWidget(delayValueLabel_, 0);
  toolbarLayout->addStretch(1);
  toolbarLayout->addWidget(watchdogValueLabel_, 0);
  rootLayout->addWidget(toolbarFrame);

  // echo_message_system 当前仍依赖 master 做服务发现。
  // 如果这里不先确保本地 master 存在，后面的 Subscriber / ActionClient
  // 会在 GUI 线程里直接卡死。
  echoMasterRuntime().ensureStarted();

  // 控制器负责 BSP 语义，AgentManagerProcess 负责真实 agent / subnode
  // 生命周期。
  // 这两个对象分开，后面继续扩完整运行时的时候不会把“状态机”和“执行器”绑死在一起。
  controller_ = new recordlab::workflow::WorkflowController(context_, this);
  agentManagerProcess_ = new recordlab::backend::AgentManagerProcess(
      context_.paths().appConfigPath, nullptr);
  dataReceiver_ =
      new recordlab::backend::DataReceiverManager("127.0.0.1", this);
  agentManagerProcess_->start();
  dataReceiver_->start();

  connect(controller_,
          &recordlab::workflow::WorkflowController::actionRequested, this,
          &WorkspacePage::dispatchWorkflowAction);
  connect(controller_,
          &recordlab::workflow::WorkflowController::activeAgentChanged, this,
          [this](const QString &agentName) {
            if (activeAgent_ == agentName) {
              return;
            }
            activeAgent_ = agentName;
            updateHeader();
          });
  connect(agentManagerProcess_,
          &recordlab::backend::AgentManagerProcess::commandResult, this,
          [this](const nlohmann::json &result) {
            if (controller_) {
              controller_->handleCommandResult(result);
            }
            if (bspRgbPage_) {
              bspRgbPage_->handleCommandResult(result);
            }
          });
  connect(agentManagerProcess_,
          &recordlab::backend::AgentManagerProcess::statusUpdate, this,
          [this](const nlohmann::json &status) {
            if (controller_) {
              controller_->handleStatusUpdate(status);
            }
          });
  connect(controller_,
          &recordlab::workflow::WorkflowController::watchdogSummaryChanged, this,
          [this](const QString &) { updateWatchdogHeader(); });
  connect(
      controller_,
      &recordlab::workflow::WorkflowController::activeAgentWatchdogStateChanged,
      this, [this](const QString &) { updateWatchdogHeader(); });

  tabs_ = new QTabWidget(this);
  tabs_->setAutoFillBackground(true);
  tabs_->setDocumentMode(false);

  bspPage_ = new recordlab::bsp::BspPage(context_, controller_, this);
  tabs_->addTab(bspPage_, QStringLiteral("数据 + 命令"));

  bspRgbPage_ = new recordlab::bsp::BspRgbPage(context_, controller_, this);

  rootLayout->addWidget(tabs_, 1);

  connect(
      dataReceiver_, &recordlab::backend::DataReceiverManager::dataUpdated,
      this, [this](const QString &dataName, const nlohmann::json &value,
                   double timestamp, double frequency) {
        if (!tabs_) {
          return;
        }

        QWidget *current = tabs_->currentWidget();
        if (current == bspPage_ && bspPage_) {
          bspPage_->handleRealtimeData(dataName, value, timestamp, frequency);
        } else if (current == bspRgbPage_ && bspRgbPage_) {
          bspRgbPage_->handleRealtimeData(dataName, value, timestamp,
                                          frequency);
        }
      });
  connect(dataReceiver_, &recordlab::backend::DataReceiverManager::dataUpdated,
          this,
          [this](const QString &dataName, const nlohmann::json &value, double,
                 double) {
            if (dataName ==
                QString::fromUtf8(recordlab::common::TOPIC_RECORD_TIMER)) {
              double seconds = 0.0;
              try {
                seconds = value.value("value", 0.0);
              } catch (...) {
              }
              timerValueLabel_->setText(
                  QStringLiteral("录制时长: %1 s").arg(seconds, 0, 'f', 1));
            } else if (dataName ==
                       QString::fromUtf8(recordlab::common::TOPIC_TIME_DELAY)) {
              double delayMs = 0.0;
              try {
                delayMs = value.value("value", 0.0);
              } catch (...) {
              }
              delayValueLabel_->setText(
                  QStringLiteral("时间延迟: %1 ms").arg(delayMs, 0, 'f', 1));
            }
          });
  connect(bspRgbPage_, &recordlab::bsp::BspRgbPage::scriptExecutionStateChanged,
          this, [this](bool running) {
            if (!agentManagerProcess_) {
              return;
            }
            if (running) {
              agentManagerProcess_->pauseWatchdogChecks("BSP RGB script running");
            } else {
              agentManagerProcess_->resumeWatchdogChecks("BSP RGB script finished");
            }
          });
  connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
    QWidget *current = tabs_->widget(index);
    if (bspPage_) {
      bspPage_->setCameraDisplayActive(current == bspPage_);
      if (current == bspPage_) {
        bspPage_->syncLatestData(dataReceiver_);
      }
    }
    if (bspRgbPage_) {
      bspRgbPage_->setCameraDisplayActive(current == bspRgbPage_);
      if (current == bspRgbPage_) {
        bspRgbPage_->syncLatestData(dataReceiver_);
      }
    }
  });

  updateBspRgbTab();
  updateHeader();
  updateWatchdogHeader();
  if (bspPage_) {
    bspPage_->setCameraDisplayActive(tabs_->currentWidget() == bspPage_);
    if (tabs_->currentWidget() == bspPage_) {
      bspPage_->syncLatestData(dataReceiver_);
    }
  }
  if (bspRgbPage_) {
    bspRgbPage_->setCameraDisplayActive(tabs_->currentWidget() == bspRgbPage_);
    if (tabs_->currentWidget() == bspRgbPage_) {
      bspRgbPage_->syncLatestData(dataReceiver_);
    }
  }
}

WorkspacePage::~WorkspacePage() {
  // 销毁工作区前先停掉后台执行器，确保后续 Qt 子对象销毁时没有并发访问。
  if (agentManagerProcess_) {
    agentManagerProcess_->stop();
    delete agentManagerProcess_;
    agentManagerProcess_ = nullptr;
  }
  if (dataReceiver_) {
    dataReceiver_->stop();
    dataReceiver_ = nullptr;
  }
}

void WorkspacePage::activateAgent(const QString &agentName) {
  // 激活主 agent 时同时更新表头、状态机和后台 manager 的目标对象。
  const QString previousAgent = activeAgent_;
  if (agentManagerProcess_ && previousAgent != agentName &&
      isExclusiveGlassesAgent(previousAgent) &&
      isExclusiveGlassesAgent(agentName)) {
    std::cout << "[WorkspacePage] Releasing previous exclusive agent "
              << previousAgent.toStdString() << " before switching to "
              << agentName.toStdString() << std::endl;
    agentManagerProcess_->sendCommand(
        {{"action", recordlab::common::ManagerAction::RELEASE_AGENT},
         {"agent_name", previousAgent.toStdString()}},
        true);
  }

  activeAgent_ = agentName;
  updateHeader();
  controller_->setActiveAgent(agentName);
  updateBspRgbTab();
  updateWatchdogHeader();

  if (!agentName.trimmed().isEmpty()) {
    std::cout << "[WorkspacePage] Active agent switched to "
              << agentName.toStdString()
              << "; watchdog auto-connect/start is enabled"
              << std::endl;
    controller_->requestConnect();
  }

  // 独立 RGB 工具默认直接切到 BSP RGB 页，数据+命令页作为辅助工作页保留。
  if (agentName ==
          QString::fromUtf8(recordlab::core::compat::kPrimaryBspAgent) ||
      agentName ==
          QString::fromUtf8(recordlab::core::compat::kPrimaryNvizAgent) ||
      agentName ==
          QString::fromUtf8(recordlab::core::compat::kPrimaryHelenAgent)) {
    if (bspRgbPage_ && tabs_->indexOf(bspRgbPage_) >= 0) {
      tabs_->setCurrentWidget(bspRgbPage_);
    } else {
      tabs_->setCurrentWidget(bspPage_);
    }
  }
  forceVisualRefresh();
}

void WorkspacePage::forceVisualRefresh() {
  if (!tabs_) {
    update();
    return;
  }
  updateGeometry();
  tabs_->updateGeometry();
  update();
  tabs_->update();
  if (auto *current = tabs_->currentWidget()) {
    current->updateGeometry();
    current->update();
  }
  QTimer::singleShot(0, this, [this]() {
    update();
    if (tabs_) {
      tabs_->update();
      if (auto *current = tabs_->currentWidget()) {
        current->update();
      }
    }
  });
}

void WorkspacePage::updateBspRgbTab() {
  if (!tabs_ || !bspRgbPage_) {
    return;
  }

  const bool shouldShow =
      activeAgent_ ==
      QString::fromUtf8(recordlab::core::compat::kPrimaryBspAgent);
  const int existingIndex = tabs_->indexOf(bspRgbPage_);
  if (shouldShow && existingIndex < 0) {
    const int insertIndex = bspPage_ ? tabs_->indexOf(bspPage_) + 1 : 2;
    tabs_->insertTab(qMax(0, insertIndex), bspRgbPage_,
                     QStringLiteral("BSP RGB"));
    return;
  }

  if (!shouldShow && existingIndex >= 0) {
    if (tabs_->currentWidget() == bspRgbPage_ && bspPage_) {
      tabs_->setCurrentWidget(bspPage_);
    }
    tabs_->removeTab(existingIndex);
    bspRgbPage_->setCameraDisplayActive(false);
  }
}

void WorkspacePage::dispatchWorkflowAction(const QString &actionName,
                                           const QVariantMap &payload) {
  // 控制器只产出语义动作；这里将其转换为后台进程可直接执行的 JSON 命令。
  if (!agentManagerProcess_) {
    return;
  }

  auto command = variantMapToJson(payload);
  command["action"] = actionName.toStdString();
  agentManagerProcess_->sendCommand(command);
}

void WorkspacePage::updateHeader() {
  // 顶部不再展示当前主 agent / 配置 / 指南；activeAgent_ 仍作为页面状态使用。
}

void WorkspacePage::updateWatchdogHeader() {
  // watchdog 文案由全局摘要和当前主 agent 的局部状态拼接而成。
  const auto summary = controller_ ? controller_->watchdogSummary()
                                      : QStringLiteral("无监控");
  const auto state =
      controller_ ? controller_->activeAgentWatchdogState() : QString();
  watchdogValueLabel_->setText(
      state.isEmpty()
          ? QStringLiteral("Watchdog: %1").arg(summary)
          : QStringLiteral("Watchdog: %1 | %2").arg(summary, state));
}

} // namespace recordlab::app
