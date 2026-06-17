#include "recordlab/bsp/bsp_page.h"

#include <QComboBox>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

#include "recordlab/backend/data_receiver.h"
#include "recordlab/bsp/bsp_sdk_locator.h"
#include "recordlab/core/compatibility_contract.h"
#include "recordlab/widgets/data_monitor_widget.h"

/*
 * bsp_page.cpp
 *
 * 这一页需要同时承担“原版 Tab2 的操作感”和“当前 C++ 控制平面的稳定性”：
 * 1. 左侧继续保持实时数据与图像展示；
 * 2. 右侧收回到原版那种简单直接的命令面板；
 * 3. 所有按钮仍然通过 recordlab::workflow::WorkflowController 发起真实动作，避免 UI 直接拼命令。
 *
 * 这样做的目的不是做一个更漂亮的页面，而是尽量降低用户从原版迁移过来时的
 * 心智落差，同时让 Tab2 的行为能继续复用统一的工作流状态机。
 */
namespace recordlab::bsp {

namespace {

QString formatPayload(const QVariantMap &payload) {
  // 将动作 payload 渲染成紧凑文本，方便直接写入日志查看。
  QStringList items;
  for (auto it = payload.begin(); it != payload.end(); ++it) {
    QString valueText = it.value().toString();
    if (valueText.isEmpty() && (it.value().typeId() == QMetaType::QVariantMap ||
                                it.value().typeId() == QMetaType::QVariantList)) {
      valueText = QString::fromUtf8(
          QJsonDocument::fromVariant(it.value()).toJson(QJsonDocument::Compact));
    }
    items << QStringLiteral("%1=%2").arg(it.key(), valueText);
  }
  return items.join(QStringLiteral(", "));
}

QString commandParamsSummary(const QVariantMap &params) {
  // 把命令参数转换成紧凑 JSON，用于日志和操作回显。
  if (params.isEmpty()) {
    return QStringLiteral("{}");
  }
  const auto json = QJsonDocument::fromVariant(params);
  return QString::fromUtf8(json.toJson(QJsonDocument::Compact));
}

QString oneClickButtonText(const recordlab::workflow::WorkflowController *controller) {
  if (!controller) {
    return QStringLiteral("一键启动");
  }
  if (controller->isBspAgent()) {
    return QStringLiteral("一键启动 Glasses_bsp_node");
  }
  if (controller->isHelenAgent()) {
    return QStringLiteral("一键启动 helen_node");
  }
  return QStringLiteral("一键启动当前 Agent");
}

} // namespace

BspPage::BspPage(const recordlab::core::AppContext &context,
                 recordlab::workflow::WorkflowController *controller, QWidget *parent)
    : QWidget(parent), context_(context), controller_(controller) {
  // 构造 Tab2 页面：左侧数据监控，右侧命令面板，底部日志区。
  const auto sdkInfo = BspSdkLocator::probe(context_);

  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(6, 6, 6, 6);
  rootLayout->setSpacing(10);

  activeAgentValueLabel_ = makeSelectableLabel(
      controller_->activeAgent().isEmpty() ? QStringLiteral("未选择")
                                           : controller_->activeAgent(),
      this);
  glassesFsnValueLabel_ =
      makeSelectableLabel(displayOrDash(controller_->activeAgentGlassesFsn()), this);
  glassesProductValueLabel_ =
      makeSelectableLabel(displayOrDash(controller_->activeAgentGlassesProductLabel()),
                          this);

  auto *middleLayout = new QHBoxLayout();
  middleLayout->setSpacing(12);

  monitorWidget_ = new recordlab::widgets::DataMonitorWidget(this);
  syncMonitorDisplayMode();
  middleLayout->addWidget(monitorWidget_, 5);

  auto *rightColumn = new QWidget(this);
  rightColumn->setMinimumWidth(280);
  rightColumn->setMaximumWidth(340);
  auto *rightLayout = new QVBoxLayout(rightColumn);
  rightLayout->setContentsMargins(0, 0, 0, 0);
  rightLayout->setSpacing(10);

  auto *commandLabel =
      new QLabel(QStringLiteral("⚡ Agent 命令执行"), rightColumn);
  QFont commandFont = commandLabel->font();
  commandFont.setBold(true);
  commandLabel->setFont(commandFont);
  rightLayout->addWidget(commandLabel);

  auto *agentSelectLayout = new QHBoxLayout();
  agentSelectLayout->setSpacing(6);
  agentSelectLayout->addWidget(new QLabel(QStringLiteral("选择 Agent:"), rightColumn));
  agentSelector_ = new QComboBox(rightColumn);
  agentSelector_->setStyleSheet(
      QStringLiteral("QComboBox { background-color: #FFFFE0; padding: 5px; }"));
  populateAgentSelector();
  syncAgentSelector(controller_->activeAgent());
  agentSelectLayout->addWidget(agentSelector_, 1);
  rightLayout->addLayout(agentSelectLayout);

  auto *cmdNameLayout = new QHBoxLayout();
  cmdNameLayout->setSpacing(6);
  cmdNameLayout->addWidget(new QLabel(QStringLiteral("命令名称:"), rightColumn));
  commandNameEdit_ = new QLineEdit(rightColumn);
  commandNameEdit_->setPlaceholderText(
      QStringLiteral("例如: init_device, start_device, check"));
  commandNameEdit_->setStyleSheet(QStringLiteral(
      "QLineEdit { background-color: #FFFACD; border: 1px solid #888; padding: 5px; }"));
  cmdNameLayout->addWidget(commandNameEdit_, 1);
  rightLayout->addLayout(cmdNameLayout);

  rightLayout->addWidget(new QLabel(QStringLiteral("命令参数 (JSON):"), rightColumn));
  commandParamsEdit_ = new QPlainTextEdit(rightColumn);
  commandParamsEdit_->setPlaceholderText(QStringLiteral("{\"param\": \"value\"}"));
  commandParamsEdit_->setMaximumHeight(120);
  commandParamsEdit_->setStyleSheet(QStringLiteral(
      "QPlainTextEdit { background-color: #FFFFE0; border: 1px solid #888; font-family: "
      "Courier; padding: 4px; }"));
  rightLayout->addWidget(commandParamsEdit_);

  executeCommandButton_ = new QPushButton(QStringLiteral("⚡ 执行命令"), rightColumn);
  executeCommandButton_->setMinimumHeight(40);
  executeCommandButton_->setStyleSheet(QStringLiteral(
      "QPushButton { background-color: #90EE90; border: 2px solid #006400; }"));
  rightLayout->addWidget(executeCommandButton_);

  stopAllButton_ = new QPushButton(QStringLiteral("⏹ 停止所有 Agent"), rightColumn);
  stopAllButton_->setMinimumHeight(40);
  stopAllButton_->setStyleSheet(QStringLiteral(
      "QPushButton { background-color: #FFB6C1; border: 2px solid #8B0000; }"));
  rightLayout->addWidget(stopAllButton_);

  oneClickButton_ =
      new QPushButton(oneClickButtonText(controller_), rightColumn);
  oneClickButton_->setMinimumHeight(40);
  oneClickButton_->setStyleSheet(QStringLiteral(
      "QPushButton { background-color: #87CEEB; border: 2px solid #4682B4; }"
      "QPushButton:disabled { background-color: #D3D3D3; border-color: #888; color: #666; }"));
  rightLayout->addWidget(oneClickButton_);

  androidOneClickButton_ =
      new QPushButton(QStringLiteral("一键 Android IMU"), rightColumn);
  androidOneClickButton_->setMinimumHeight(40);
  androidOneClickButton_->setStyleSheet(QStringLiteral(
      "QPushButton { background-color: #FFD966; border: 2px solid #A66A00; }"
      "QPushButton:disabled { background-color: #D3D3D3; border-color: #888; color: #666; }"));
  rightLayout->addWidget(androidOneClickButton_);

  auto *statusGroup = new QGroupBox(QStringLiteral("当前状态"), rightColumn);
  auto *statusLayout = new QVBoxLayout(statusGroup);
  statusLayout->setContentsMargins(10, 10, 10, 10);
  statusLayout->setSpacing(8);
  const auto addStatusLine = [this, statusLayout](const QString &title,
                                                  QLabel *valueLabel) {
    auto *titleLabel = new QLabel(title, this);
    titleLabel->setStyleSheet(
        QStringLiteral("QLabel { color: #666; font-size: 11px; }"));
    statusLayout->addWidget(titleLabel);
    valueLabel->setStyleSheet(
        QStringLiteral("QLabel { background-color: #fbfbfb; border: 1px solid #d5d5d5; "
                       "padding: 6px 8px; }"));
    valueLabel->setWordWrap(true);
    statusLayout->addWidget(valueLabel);
  };
  addStatusLine(QStringLiteral("当前 Agent"), activeAgentValueLabel_);
  addStatusLine(QStringLiteral("当前眼镜 FSN"), glassesFsnValueLabel_);
  addStatusLine(QStringLiteral("当前眼镜 ID-名称"), glassesProductValueLabel_);
  rightLayout->addWidget(statusGroup);
  rightLayout->addStretch(1);

  middleLayout->addWidget(rightColumn, 1);
  rootLayout->addLayout(middleLayout, 3);

  rootLayout->addWidget(new QLabel(QStringLiteral("运行日志:"), this));
  logView_ = new QPlainTextEdit(this);
  logView_->setReadOnly(true);
  logView_->setMaximumBlockCount(2000);
  logView_->setPlaceholderText(
      QStringLiteral("BSP 工作流日志会持续回流到这里。"));
  logView_->setStyleSheet(
      QStringLiteral("QPlainTextEdit { background-color: #FFFFE0; border: 1px "
                     "solid #888; padding: 10px; }"));
  rootLayout->addWidget(logView_, 1);

  connect(agentSelector_, &QComboBox::textActivated, this,
          [this](const QString &agentName) {
            if (controller_ && !agentName.trimmed().isEmpty()) {
              controller_->setActiveAgent(agentName.trimmed());
            }
          });
  connect(executeCommandButton_, &QPushButton::clicked, this,
          [this]() { executeCustomCommand(); });
  connect(stopAllButton_, &QPushButton::clicked, this,
          [this]() { requestStopAllAgents(); });
  connect(oneClickButton_, &QPushButton::clicked, controller_,
          &recordlab::workflow::WorkflowController::requestOneClick);
  connect(androidOneClickButton_, &QPushButton::clicked, controller_,
          &recordlab::workflow::WorkflowController::requestAndroidOneClick);
  connect(controller_, &recordlab::workflow::WorkflowController::stateChanged, this,
          [this](recordlab::workflow::WorkflowController::State state) {
            Q_UNUSED(state);
            updateOneClickButtonState();
          });
  connect(
      controller_, &recordlab::workflow::WorkflowController::logAppended, this,
      [this](const QString &message) { logView_->appendPlainText(message); });
  connect(controller_, &recordlab::workflow::WorkflowController::activeAgentDeviceInfoChanged,
          this, [this](const QString &, const QString &) {
            updateCurrentStatusLabels();
          });
  connect(controller_, &recordlab::workflow::WorkflowController::activeAgentChanged, this,
          [this](const QString &agentName) {
            syncAgentSelector(agentName);
            updateCurrentStatusLabels();
            syncMonitorDisplayMode();
            updateOneClickButtonState();
          });
  connect(controller_, &recordlab::workflow::WorkflowController::oneClickSuccessChanged, this,
          [this](bool) { updateOneClickButtonState(); });
  connect(controller_, &recordlab::workflow::WorkflowController::actionRequested, this,
          [this](const QString &actionName, const QVariantMap &payload) {
            logView_->appendPlainText(
                QStringLiteral("已排队动作: %1 [%2]")
                    .arg(actionName, formatPayload(payload)));
          });

  for (const auto &warning : sdkInfo.warnings) {
    logView_->appendPlainText(warning);
  }

  updateOneClickButtonState();
  updateCurrentStatusLabels();
}

QLabel *BspPage::makeSelectableLabel(const QString &text, QWidget *parent) {
  // 创建可复制文本标签，方便用户在排障时复制状态与路径信息。
  auto *label = new QLabel(text, parent);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  label->setWordWrap(true);
  return label;
}

QString BspPage::displayOrDash(const QString &value) {
  const auto text = value.trimmed();
  return text.isEmpty() ? QStringLiteral("--") : text;
}

void BspPage::updateCurrentStatusLabels() {
  if (!controller_) {
    return;
  }
  if (activeAgentValueLabel_) {
    activeAgentValueLabel_->setText(
        controller_->activeAgent().isEmpty() ? QStringLiteral("未选择")
                                             : controller_->activeAgent());
  }
  if (glassesFsnValueLabel_) {
    glassesFsnValueLabel_->setText(
        displayOrDash(controller_->activeAgentGlassesFsn()));
  }
  if (glassesProductValueLabel_) {
    glassesProductValueLabel_->setText(
        displayOrDash(controller_->activeAgentGlassesProductLabel()));
  }
}

void BspPage::populateAgentSelector() {
  // 用配置里的 agent 列表填充下拉框，并为兜底场景保留默认 BSP agent。
  if (!agentSelector_) {
    return;
  }

  QStringList agents = context_.config().availableAgents();
  agents.removeDuplicates();

  if (agents.isEmpty() && controller_ && !controller_->activeAgent().isEmpty()) {
    agents.push_back(controller_->activeAgent());
  }
  if (agents.isEmpty()) {
    agents.push_back(QString::fromUtf8(recordlab::core::compat::kPrimaryBspAgent));
    agents.push_back(QString::fromUtf8(recordlab::core::compat::kPrimaryHelenAgent));
  }

  const QSignalBlocker blocker(agentSelector_);
  agentSelector_->clear();
  agentSelector_->addItems(agents);
}

void BspPage::syncAgentSelector(const QString &agentName) {
  // 将控制器中的当前主 agent 同步到下拉框显示，必要时动态补充缺失项。
  if (!agentSelector_) {
    return;
  }
  const QString trimmed = agentName.trimmed();
  if (trimmed.isEmpty()) {
    return;
  }

  const QSignalBlocker blocker(agentSelector_);
  if (agentSelector_->findText(trimmed) < 0) {
    agentSelector_->addItem(trimmed);
  }
  agentSelector_->setCurrentText(trimmed);
}

void BspPage::executeCustomCommand() {
  // 从右侧命令面板读取 agent、命令名和 JSON 参数，然后交给控制器分发。
  if (!controller_) {
    return;
  }

  const QString agentName =
      agentSelector_ ? agentSelector_->currentText().trimmed() : QString();
  const QString cmdName =
      commandNameEdit_ ? commandNameEdit_->text().trimmed() : QString();
  if (cmdName.isEmpty()) {
    logView_->appendPlainText(QStringLiteral("⚠ 请输入命令名称"));
    return;
  }

  QVariantMap params;
  const QString rawParams =
      commandParamsEdit_ ? commandParamsEdit_->toPlainText().trimmed() : QString();
  if (!rawParams.isEmpty()) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(rawParams.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      logView_->appendPlainText(
          QStringLiteral("❌ 命令参数格式错误，请输入 JSON 对象"));
      return;
    }
    params = document.object().toVariantMap();
  }

  logView_->appendPlainText(QStringLiteral("执行命令: %1 - %2 - %3")
                                .arg(agentName, cmdName,
                                     commandParamsSummary(params)));
  controller_->requestExecuteCommand(agentName, cmdName, params);
}

void BspPage::requestStopAllAgents() {
  // 将“停止所有 Agent”动作透传给控制器，页面不直接拼后台命令。
  if (controller_) {
    controller_->requestStopAllAgents();
  }
}

void BspPage::handleRealtimeData(const QString &dataName,
                                 const nlohmann::json &value, double timestamp,
                                 double frequency) {
  // 将实时数据继续转交给监控组件，由其决定如何更新曲线、文本和图像。
  if (monitorWidget_) {
    monitorWidget_->handleRealtimeData(dataName, value, timestamp, frequency);
  }
}

void BspPage::setCameraDisplayActive(bool active) {
  // 当前页是否可见时控制相机显示线程，避免后台无意义刷新。
  if (monitorWidget_) {
    monitorWidget_->setCameraDisplayActive(active);
  }
}

void BspPage::startCameraDisplay() {
  // 显式启动相机显示线程，供外部在页面激活时调用。
  if (monitorWidget_) {
    monitorWidget_->startCameraDisplayThread();
  }
}

void BspPage::stopCameraDisplay() {
  // 显式停止相机显示线程，供页面切走或收尾时调用。
  if (monitorWidget_) {
    monitorWidget_->stopCameraDisplayThread();
  }
}

void BspPage::syncLatestData(
    const recordlab::backend::DataReceiverManager *receiver) {
  // 页面切入前主动同步当前最新缓存，避免监控区出现短暂空白。
  if (monitorWidget_) {
    monitorWidget_->syncLatestData(receiver);
  }
}

void BspPage::updateOneClickButtonState() {
  // 根据当前状态机和选中 agent 判定一键按钮是否允许再次点击。
  if (!oneClickButton_ || !controller_) {
    return;
  }

  const auto state = controller_->state();
  const bool running =
      state == recordlab::workflow::WorkflowController::State::CheckingConnection ||
      state == recordlab::workflow::WorkflowController::State::AgentConnecting ||
      state == recordlab::workflow::WorkflowController::State::WaitingWatchdog ||
      state == recordlab::workflow::WorkflowController::State::DeviceInitializing ||
      state == recordlab::workflow::WorkflowController::State::WaitingStartDeviceResponse;
  const bool supported = controller_->isTargetAgentSelected();
  oneClickButton_->setText(oneClickButtonText(controller_));
  oneClickButton_->setEnabled(
      supported && !running && !controller_->oneClickSucceeded());
  if (androidOneClickButton_) {
    androidOneClickButton_->setVisible(false);
    androidOneClickButton_->setEnabled(false);
  }
}

void BspPage::syncMonitorDisplayMode() {
  if (!monitorWidget_ || !controller_) {
    return;
  }
  monitorWidget_->setCameraPreviewEnabled(
      !controller_->isHelenAgent());
}

} // namespace recordlab::bsp
