#pragma once

#include <QWidget>

#include "recordlab/workflow/workflow_controller.h"
#include "recordlab/core/app_context.h"
#include "recordlab/core/qt_json_compat.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace recordlab::common {
class CameraSharedMemoryWriter;
}

namespace recordlab::backend {
class DataReceiverManager;
}

namespace recordlab::widgets {
class DataMonitorWidget;
}

namespace recordlab::bsp {

/*
 * BSP 页面
 *
 * 这一页直接对齐原版 Tab2 的职责：
 * 1. 左侧显示双目图像、IMU 列表和曲线；
 * 2. 右侧提供通用 Agent 命令输入面板；
 * 3. 底部持续展示 BSP 工作流日志。
 *
 * 页面本身不直接操作 AgentManagerProcess，而是统一经由
 * WorkflowController 发起动作。这样页面只负责交互形态，状态机和实际命令
 * 分发仍集中在控制器里，后续继续补齐原版行为时不会把业务逻辑重新散落回 UI。
 */
class BspPage : public QWidget {
  Q_OBJECT

public:
  explicit BspPage(const recordlab::core::AppContext &context,
                   recordlab::workflow::WorkflowController *controller,
                   QWidget *parent = nullptr);

public slots:
  void handleRealtimeData(const QString &dataName, const nlohmann::json &value,
                          double timestamp, double frequency);
  void setCameraDisplayActive(bool active);
  void syncLatestData(const recordlab::backend::DataReceiverManager *receiver);
  void startCameraDisplay();
  void stopCameraDisplay();

private:
  const recordlab::core::AppContext &context_;
  recordlab::workflow::WorkflowController *controller_ = nullptr;
  recordlab::widgets::DataMonitorWidget *monitorWidget_ = nullptr;
  QComboBox *agentSelector_ = nullptr;
  QLineEdit *commandNameEdit_ = nullptr;
  QPlainTextEdit *commandParamsEdit_ = nullptr;
  QPushButton *executeCommandButton_ = nullptr;
  QPushButton *stopAllButton_ = nullptr;
  QLabel *activeAgentValueLabel_ = nullptr;
  QLabel *glassesFsnValueLabel_ = nullptr;
  QLabel *glassesProductValueLabel_ = nullptr;
  QPlainTextEdit *logView_ = nullptr;
  QPushButton *oneClickButton_ = nullptr;
  QPushButton *androidOneClickButton_ = nullptr;

  // 统一创建可复制文本标签，便于路径排障。
  static QLabel *makeSelectableLabel(const QString &text,
                                     QWidget *parent = nullptr);
  static QString displayOrDash(const QString &value);
  void populateAgentSelector();
  void syncAgentSelector(const QString &agentName);
  void executeCustomCommand();
  void requestStopAllAgents();
  void updateOneClickButtonState();
  void updateCurrentStatusLabels();
  void syncMonitorDisplayMode();
};

} // namespace recordlab::bsp
