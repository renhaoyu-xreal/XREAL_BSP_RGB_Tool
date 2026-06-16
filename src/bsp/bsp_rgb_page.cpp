#include "recordlab/bsp/bsp_rgb_page.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QItemSelectionModel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSplitter>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include "recordlab/backend/data_receiver.h"
#include "recordlab/common/camera_shared_memory.h"
#include "recordlab/common/topics.h"
#include "recordlab/core/compatibility_contract.h"
#include "recordlab/flowagent/core/script_executor.h"
#include "recordlab/widgets/camera_display_thread.h"
#include "recordlab/widgets/image_display_widget.h"
#include "recordlab/workflow/workflow_controller.h"

namespace recordlab::bsp {

namespace {

QString bspAgentName() {
  return QString::fromUtf8(recordlab::core::compat::kPrimaryBspAgent);
}

QString dataRootPath(const recordlab::core::AppContext &context) {
  return QDir(context.paths().appRoot).filePath(QStringLiteral("data/slam_rgb_imu"));
}

QString jsonText(const nlohmann::json &value) {
  try {
    if (value.is_null()) {
      return QStringLiteral("--");
    }
    if (value.is_boolean()) {
      return value.get<bool>() ? QStringLiteral("是") : QStringLiteral("否");
    }
    if (value.is_string()) {
      const auto text = QString::fromStdString(value.get<std::string>());
      return text.isEmpty() ? QStringLiteral("--") : text;
    }
    if (value.is_number_float()) {
      return QStringLiteral("%1").arg(value.get<double>(), 0, 'f', 3);
    }
    if (value.is_number_integer()) {
      return QString::number(value.get<qint64>());
    }
    if (value.is_array()) {
      QStringList items;
      for (const auto &item : value) {
        items << jsonText(item);
      }
      return items.isEmpty() ? QStringLiteral("--")
                             : items.join(QStringLiteral(", "));
    }
    return QString::fromStdString(value.dump());
  } catch (...) {
    return QStringLiteral("--");
  }
}

QString temperatureText(const nlohmann::json &value) {
  try {
    if (value.is_null()) {
      return QStringLiteral("--");
    }
    const double numeric = value.is_number() ? value.get<double>() : 0.0;
    if (!value.is_number()) {
      return jsonText(value);
    }
    return QStringLiteral("%1 °C").arg(numeric, 0, 'f', 2);
  } catch (...) {
    return QStringLiteral("--");
  }
}

nlohmann::json commandPayload(const nlohmann::json &result) {
  try {
    if (result.contains("result") && result["result"].is_object()) {
      return result["result"];
    }
    if (result.contains("message") && result["message"].is_object()) {
      return result["message"];
    }
  } catch (...) {
  }
  return nlohmann::json::object();
}

QString commandMessage(const nlohmann::json &result,
                       const nlohmann::json &payload) {
  try {
    if (payload.contains("message") && payload["message"].is_string()) {
      return QString::fromStdString(payload["message"].get<std::string>());
    }
    if (result.contains("message") && result["message"].is_string()) {
      return QString::fromStdString(result["message"].get<std::string>());
    }
    if (result.contains("error") && result["error"].is_string()) {
      return QString::fromStdString(result["error"].get<std::string>());
    }
  } catch (...) {
  }
  return QStringLiteral("<no message>");
}

bool jsonSuccess(const nlohmann::json &result, const nlohmann::json &payload) {
  const bool resultSuccess = result.value("success", false);
  if (payload.is_object() && payload.contains("success")) {
    return resultSuccess && payload.value("success", false);
  }
  return resultSuccess;
}

struct WorkflowStatusMeta {
  QString text;
  QString background;
  QString foreground;
  QString border;
};

WorkflowStatusMeta workflowStatusMeta(const QString &status) {
  if (status == QStringLiteral("running")) {
    return {QStringLiteral("运行中"), QStringLiteral("#E3F2FD"),
            QStringLiteral("#0D47A1"), QStringLiteral("#2196F3")};
  }
  if (status == QStringLiteral("success")) {
    return {QStringLiteral("成功"), QStringLiteral("#E8F5E9"),
            QStringLiteral("#1B5E20"), QStringLiteral("#4CAF50")};
  }
  if (status == QStringLiteral("failed")) {
    return {QStringLiteral("失败"), QStringLiteral("#FFEBEE"),
            QStringLiteral("#B71C1C"), QStringLiteral("#F44336")};
  }
  if (status == QStringLiteral("stopping")) {
    return {QStringLiteral("停止中"), QStringLiteral("#FFF3E0"),
            QStringLiteral("#E65100"), QStringLiteral("#FB8C00")};
  }
  if (status == QStringLiteral("stopped")) {
    return {QStringLiteral("已停止"), QStringLiteral("#ECEFF1"),
            QStringLiteral("#37474F"), QStringLiteral("#78909C")};
  }
  return {QStringLiteral("等待"), QStringLiteral("#F5F5F5"),
          QStringLiteral("#555555"), QStringLiteral("#BDBDBD")};
}

QString workflowStatusText(bool finished, bool success) {
  if (!finished) {
    return QStringLiteral("运行中");
  }
  return success ? QStringLiteral("已完成") : QStringLiteral("已失败");
}

QString uniqueExportPath(const QString &path) {
  if (!QFileInfo::exists(path)) {
    return path;
  }

  const QFileInfo info(path);
  const QDir parent = info.dir();
  const QString stem = info.completeBaseName();
  const QString suffix = info.suffix().isEmpty()
                             ? QString()
                             : QStringLiteral(".") + info.suffix();
  QString candidate = parent.filePath(stem + QStringLiteral("_copy") + suffix);
  int counter = 2;
  while (QFileInfo::exists(candidate)) {
    candidate = parent.filePath(
        QStringLiteral("%1_copy%2%3").arg(stem).arg(counter).arg(suffix));
    ++counter;
  }
  return candidate;
}

bool copyPathRecursively(const QString &sourcePath, const QString &targetPath,
                         QString *errorMessage) {
  const QFileInfo sourceInfo(sourcePath);
  if (!sourceInfo.exists()) {
    if (errorMessage) {
      *errorMessage = QStringLiteral("源路径不存在: %1").arg(sourcePath);
    }
    return false;
  }

  if (sourceInfo.isDir()) {
    QDir targetDir(targetPath);
    if (!targetDir.exists() && !QDir().mkpath(targetPath)) {
      if (errorMessage) {
        *errorMessage = QStringLiteral("无法创建目录: %1").arg(targetPath);
      }
      return false;
    }
    const QDir sourceDir(sourcePath);
    const auto entries =
        sourceDir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const auto &entry : entries) {
      const QString childTarget = targetDir.filePath(entry.fileName());
      if (!copyPathRecursively(entry.absoluteFilePath(), childTarget,
                               errorMessage)) {
        return false;
      }
    }
    return true;
  }

  QDir().mkpath(QFileInfo(targetPath).dir().absolutePath());
  if (!QFile::copy(sourcePath, targetPath)) {
    if (errorMessage) {
      *errorMessage =
          QStringLiteral("复制失败: %1 -> %2").arg(sourcePath, targetPath);
    }
    return false;
  }
  return true;
}

} // namespace

BspRgbPage::BspRgbPage(const recordlab::core::AppContext &context,
                       recordlab::workflow::WorkflowController *controller,
                       QWidget *parent)
    : QWidget(parent), context_(context), controller_(controller),
      orchestrationBridge_(context),
      scriptExecutor_(new recordlab::flowagent::core::ScriptExecutor(this)) {
  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(6, 6, 6, 6);
  rootLayout->setSpacing(8);

  auto *middleLayout = new QHBoxLayout();
  middleLayout->setSpacing(12);
  middleLayout->addWidget(buildRuntimePanel(), 1);
  middleLayout->addWidget(buildPreviewPanel(), 3);
  middleLayout->addWidget(buildActionPanel(), 2);
  rootLayout->addLayout(middleLayout, 3);

  auto *bottomSplitter = new QSplitter(Qt::Horizontal, this);
  auto *logGroup = new QGroupBox(QStringLiteral("执行日志"), bottomSplitter);
  auto *logLayout = new QVBoxLayout(logGroup);
  logView_ = new QPlainTextEdit(logGroup);
  logView_->setReadOnly(true);
  logView_->setMaximumBlockCount(2000);
  logView_->setStyleSheet(QStringLiteral(
      "QPlainTextEdit { background-color: #FFFFE0; border: 1px solid #888; "
      "padding: 6px; font-family: monospace; }"));
  logLayout->addWidget(logView_);
  bottomSplitter->addWidget(logGroup);
  bottomSplitter->addWidget(buildWorkflowPanel());
  bottomSplitter->setStretchFactor(0, 1);
  bottomSplitter->setStretchFactor(1, 1);
  bottomSplitter->setSizes({650, 550});
  rootLayout->addWidget(bottomSplitter, 1);

  connect(runScriptButton_, &QPushButton::clicked, this,
          &BspRgbPage::runSelectedScript);
  connect(stopScriptButton_, &QPushButton::clicked, this,
          &BspRgbPage::stopScript);
  connect(scriptsList_, &QListWidget::itemSelectionChanged, this, [this]() {
    refreshSelectedScriptsLabel();
    refreshScriptButtons();
  });
  connect(scriptExecutor_,
          &recordlab::flowagent::core::ScriptExecutor::scriptStarted, this,
          [this](const QString &scriptPath) {
            scriptRunning_ = true;
            emit scriptExecutionStateChanged(true);
            clearWorkflowPanel();
            refreshScriptButtons();
            appendLog(QStringLiteral("脚本已启动: %1").arg(scriptPath));
          });
  connect(scriptExecutor_,
          &recordlab::flowagent::core::ScriptExecutor::scriptLog, this,
          [this](const QString &message) { appendLog(message); });
  connect(scriptExecutor_,
          &recordlab::flowagent::core::ScriptExecutor::scriptCompleted, this,
          [this](bool success, const QString &error) {
            scriptRunning_ = false;
            emit scriptExecutionStateChanged(false);
            refreshScriptButtons();
            appendLog(success ? QStringLiteral("脚本执行完成")
                              : QStringLiteral("脚本结束: %1").arg(error));
            requestRuntimeState(true);
            if (!pendingScripts_.isEmpty()) {
              const QString nextScript = pendingScripts_.takeFirst();
              appendLog(QStringLiteral("启动 RGB 脚本: %1").arg(nextScript));
              const auto command =
                  orchestrationBridge_.buildScriptCommandForPath(nextScript);
              scriptExecutor_->executeCommand(command, nextScript);
            }
          });
  connect(scriptExecutor_,
          &recordlab::flowagent::core::ScriptExecutor::workflowUpdated, this,
          &BspRgbPage::updateWorkflowPanel);
  connect(scriptExecutor_,
          &recordlab::flowagent::core::ScriptExecutor::workflowCleared, this,
          &BspRgbPage::clearWorkflowPanel);
  connect(controller_,
          &recordlab::workflow::WorkflowController::activeAgentChanged, this,
          [this](const QString &) { syncScriptExecutorDeviceInfo(); });
  connect(controller_,
          &recordlab::workflow::WorkflowController::activeAgentDeviceInfoChanged,
          this, [this](const QString &, const QString &) {
            syncScriptExecutorDeviceInfo();
          });
  connect(controller_,
          &recordlab::workflow::WorkflowController::activeAgentWatchdogStateChanged,
          this, [this](const QString &state) {
            if (state == QStringLiteral("healthy") && pageActive_) {
              requestRuntimeState(true);
            } else if (state != QStringLiteral("healthy")) {
              runtimeRequestPending_ = false;
            }
          });

  loadScriptList();
  refreshSelectedScriptsLabel();
  refreshScriptButtons();
  syncScriptExecutorDeviceInfo();
  refreshDataTree();
  appendLog(QStringLiteral("BSP RGB 页已就绪。RAW/RGB 脚本参数会在脚本运行时弹窗填写。"));
}

BspRgbPage::~BspRgbPage() { stopCameraDisplayThread(); }

QWidget *BspRgbPage::buildRuntimePanel() {
  auto *content = new QWidget(this);
  auto *layout = new QVBoxLayout(content);
  layout->setSpacing(8);

  auto *title = new QLabel(QStringLiteral("BSP RGB 运行态"), content);
  QFont titleFont = title->font();
  titleFont.setBold(true);
  titleFont.setPointSize(titleFont.pointSize() + 1);
  title->setFont(titleFont);
  layout->addWidget(title);

  const QList<QPair<QString, QList<QPair<QString, QString>>>> sections = {
      {QStringLiteral("设备状态"),
       {{QStringLiteral("device_connected"), QStringLiteral("连接状态")},
        {QStringLiteral("device_opened"), QStringLiteral("isOpened")},
        {QStringLiteral("device_fsn"), QStringLiteral("FSN")},
        {QStringLiteral("device_fw"), QStringLiteral("固件版本")},
        {QStringLiteral("device_has_rgb"), QStringLiteral("是否有 RGB")},
        {QStringLiteral("device_active_sensors"), QStringLiteral("活跃传感器")},
        {QStringLiteral("device_camera_mode"), QStringLiteral("当前模式")}}},
      {QStringLiteral("温度"),
       {{QStringLiteral("temp_rgb"), QStringLiteral("RGB temperature")},
        {QStringLiteral("temp_imu0"), QStringLiteral("IMU0 temperature")},
        {QStringLiteral("temp_imu1"), QStringLiteral("IMU1 temperature")}}},
      {QStringLiteral("RGB 相机状态"),
       {{QStringLiteral("rgb_sn"), QStringLiteral("rgbCamSn")},
        {QStringLiteral("rgb_fps"), QStringLiteral("当前帧率")},
        {QStringLiteral("rgb_auto_exposure"), QStringLiteral("自动曝光")},
        {QStringLiteral("rgb_exposure"), QStringLiteral("曝光值")},
        {QStringLiteral("rgb_gain"), QStringLiteral("增益")}}},
      {QStringLiteral("最新帧"),
       {{QStringLiteral("frame_timestamp"), QStringLiteral("时间戳")},
        {QStringLiteral("frame_resolution"), QStringLiteral("分辨率")},
        {QStringLiteral("frame_exposure_duration"), QStringLiteral("曝光时长")},
        {QStringLiteral("frame_rolling_shutter"), QStringLiteral("卷帘时间")},
        {QStringLiteral("frame_stride"), QStringLiteral("stride")},
        {QStringLiteral("frame_gain"), QStringLiteral("gain")}}},
      {QStringLiteral("录制状态"),
       {{QStringLiteral("record_active"), QStringLiteral("是否录制中")},
        {QStringLiteral("record_dataset"), QStringLiteral("当前数据集")},
        {QStringLiteral("record_frame_count"), QStringLiteral("累计帧数")},
        {QStringLiteral("record_last_frame"), QStringLiteral("最近一帧")}}},
  };

  for (const auto &section : sections) {
    auto *group = new QGroupBox(section.first, content);
    auto *form = new QFormLayout(group);
    form->setLabelAlignment(Qt::AlignLeft);
    for (const auto &field : section.second) {
      auto *value = new QLabel(QStringLiteral("--"), group);
      value->setWordWrap(true);
      value->setTextInteractionFlags(Qt::TextSelectableByMouse);
      infoLabels_.insert(field.first, value);
      form->addRow(field.second + QStringLiteral(":"), value);
    }
    layout->addWidget(group);
  }

  layout->addStretch(1);
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setWidget(content);
  scroll->setMaximumWidth(340);
  return scroll;
}

QWidget *BspRgbPage::buildPreviewPanel() {
  auto *container = new QWidget(this);
  auto *layout = new QVBoxLayout(container);
  layout->setSpacing(8);

  previewWidget_ =
      new recordlab::widgets::ImageDisplayWidget(QStringLiteral("RGB 图像"),
                                                 container);
  previewWidget_->setZoomImageProvider([this]() { return latestSourceImage(); });
  layout->addWidget(previewWidget_, 1);

  previewHintLabel_ =
      new QLabel(QStringLiteral("当前模式不是 RGB 时，此处不会参与相机渲染。"),
                 container);
  previewHintLabel_->setAlignment(Qt::AlignCenter);
  previewHintLabel_->setStyleSheet(QStringLiteral("color: #666; padding: 4px;"));
  layout->addWidget(previewHintLabel_);
  return container;
}

QWidget *BspRgbPage::buildActionPanel() {
  auto *container = new QWidget(this);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(10);

  auto *rightSplitter = new QSplitter(Qt::Vertical, container);

  auto *scriptsGroup = new QGroupBox(QStringLiteral("RGB 脚本列表"), container);
  auto *scriptsLayout = new QVBoxLayout(scriptsGroup);
  scriptsList_ = new QListWidget(scriptsGroup);
  scriptsList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  scriptsList_->setStyleSheet(QStringLiteral(
      "QListWidget { background-color: #FFFFE0; border: 1px solid #888; "
      "padding: 5px; }"));
  selectedScriptsLabel_ = new QLabel(QStringLiteral("已选: 0 个脚本"), scriptsGroup);
  selectedScriptsLabel_->setWordWrap(true);
  selectedScriptsLabel_->setStyleSheet(QStringLiteral(
      "QLabel { background-color: #FFFFE0; border: 1px solid #888; padding: 8px; }"));
  auto *scriptButtons = new QHBoxLayout();
  runScriptButton_ = new QPushButton(QStringLiteral("▶ 开始执行"), scriptsGroup);
  stopScriptButton_ = new QPushButton(QStringLiteral("⏹ 停止执行"), scriptsGroup);
  runScriptButton_->setMinimumHeight(40);
  stopScriptButton_->setMinimumHeight(40);
  scriptButtons->addWidget(runScriptButton_);
  scriptButtons->addWidget(stopScriptButton_);
  scriptsLayout->addWidget(scriptsList_, 1);
  scriptsLayout->addWidget(selectedScriptsLabel_);
  scriptsLayout->addLayout(scriptButtons);
  auto *autoStartHint = new QLabel(
      QStringLiteral("RGB 连接、启动和断线恢复由 watchdog 自动执行。"),
      scriptsGroup);
  autoStartHint->setWordWrap(true);
  autoStartHint->setStyleSheet(QStringLiteral(
      "QLabel { background-color: #FFF8DC; border: 1px solid #D6C28A; "
      "padding: 8px; color: #6B5A22; }"));
  scriptsLayout->addWidget(autoStartHint);

  rightSplitter->addWidget(scriptsGroup);
  rightSplitter->addWidget(buildDataPanel());
  rightSplitter->setStretchFactor(0, 1);
  rightSplitter->setStretchFactor(1, 1);
  rightSplitter->setSizes({360, 320});

  layout->addWidget(rightSplitter);
  return container;
}

QWidget *BspRgbPage::buildDataPanel() {
  auto *group = new QGroupBox(QStringLiteral("data 输出目录"), this);
  auto *layout = new QVBoxLayout(group);
  const QString root = dataRootPath(context_);
  QDir().mkpath(root);

  auto *pathLabel = new QLabel(root, group);
  pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  pathLabel->setStyleSheet(QStringLiteral("color: #555;"));
  layout->addWidget(pathLabel);

  dataFileModel_ = new QFileSystemModel(group);
  dataFileModel_->setRootPath(root);
  dataFileModel_->setReadOnly(true);

  dataTree_ = new QTreeView(group);
  dataTree_->setModel(dataFileModel_);
  dataTree_->setRootIndex(dataFileModel_->index(root));
  dataTree_->setAlternatingRowColors(true);
  dataTree_->setSortingEnabled(true);
  dataTree_->sortByColumn(3, Qt::DescendingOrder);
  dataTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  dataTree_->header()->setStretchLastSection(false);
  dataTree_->setColumnWidth(0, 260);
  layout->addWidget(dataTree_, 1);

  auto *dataButtons = new QHBoxLayout();

  auto *refreshButton = new QPushButton(QStringLiteral("刷新目录"), group);
  refreshButton->setMinimumHeight(34);
  connect(refreshButton, &QPushButton::clicked, this,
          &BspRgbPage::refreshDataTree);
  dataButtons->addWidget(refreshButton);

  auto *openButton = new QPushButton(QStringLiteral("打开所在目录"), group);
  openButton->setMinimumHeight(34);
  connect(openButton, &QPushButton::clicked, this,
          &BspRgbPage::openSelectedDataDirectory);
  dataButtons->addWidget(openButton);

  auto *exportButton = new QPushButton(QStringLiteral("导出选中项"), group);
  exportButton->setMinimumHeight(34);
  connect(exportButton, &QPushButton::clicked, this,
          &BspRgbPage::exportSelectedDataItems);
  dataButtons->addWidget(exportButton);

  layout->addLayout(dataButtons);
  return group;
}

QWidget *BspRgbPage::buildWorkflowPanel() {
  workflowGroup_ = new QGroupBox(QStringLiteral("流程状态"), this);
  auto *layout = new QVBoxLayout(workflowGroup_);
  layout->setSpacing(8);

  workflowTitleLabel_ = new QLabel(QString(), workflowGroup_);
  workflowTitleLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  workflowTitleLabel_->setStyleSheet(QStringLiteral("font-weight: 600;"));
  layout->addWidget(workflowTitleLabel_);

  auto *stepsScroll = new QScrollArea(workflowGroup_);
  stepsScroll->setWidgetResizable(true);
  stepsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  stepsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  stepsScroll->setMinimumHeight(100);
  auto *stepsContainer = new QWidget(stepsScroll);
  workflowStepsLayout_ = new QHBoxLayout(stepsContainer);
  workflowStepsLayout_->setContentsMargins(4, 4, 4, 4);
  workflowStepsLayout_->setSpacing(6);
  stepsScroll->setWidget(stepsContainer);
  layout->addWidget(stepsScroll);

  workflowMessageLabel_ = new QLabel(QString(), workflowGroup_);
  workflowMessageLabel_->setWordWrap(true);
  workflowMessageLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  workflowMessageLabel_->setStyleSheet(QStringLiteral(
      "QLabel { background-color: #FFFDF2; border: 1px solid #C8B36A; padding: 6px; }"));
  layout->addWidget(workflowMessageLabel_);

  workflowGroup_->hide();
  return workflowGroup_;
}

void BspRgbPage::clearWorkflowPanel() {
  if (workflowGroup_) {
    workflowGroup_->hide();
  }
  if (workflowTitleLabel_) {
    workflowTitleLabel_->clear();
  }
  if (workflowMessageLabel_) {
    workflowMessageLabel_->clear();
  }
  if (workflowStepsLayout_) {
    while (workflowStepsLayout_->count() > 0) {
      auto *item = workflowStepsLayout_->takeAt(0);
      if (auto *widget = item->widget()) {
        widget->deleteLater();
      }
      delete item;
    }
  }
}

void BspRgbPage::updateWorkflowPanel(const QString &title,
                                     const QString &message,
                                     const QString &stepsJson,
                                     bool finished, bool success) {
  if (workflowGroup_) {
    workflowGroup_->show();
  }
  if (workflowTitleLabel_) {
    workflowTitleLabel_->setText(
        QStringLiteral("%1 [%2]").arg(title, workflowStatusText(finished, success)));
  }
  if (!workflowStepsLayout_) {
    return;
  }
  while (workflowStepsLayout_->count() > 0) {
    auto *item = workflowStepsLayout_->takeAt(0);
    if (auto *widget = item->widget()) {
      widget->deleteLater();
    }
    delete item;
  }

  QString focusLabel;
  QString focusMessage = message;
  try {
    const auto steps = nlohmann::json::parse(stepsJson.toStdString());
    if (!steps.is_array()) {
      return;
    }
    const auto focusStatuses = {std::string("failed"), std::string("stopping"),
                                std::string("running"), std::string("stopped")};
    nlohmann::json focusStep;
    for (const auto &target : focusStatuses) {
      for (const auto &step : steps) {
        if (step.value("status", std::string()) == target) {
          focusStep = step;
          break;
        }
      }
      if (!focusStep.is_null()) {
        break;
      }
    }
    if (focusStep.is_null()) {
      for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        if (it->value("status", std::string()) == "success") {
          focusStep = *it;
          break;
        }
      }
    }
    if (focusStep.is_null() && !steps.empty()) {
      focusStep = steps.front();
    }

    for (qsizetype index = 0; index < static_cast<qsizetype>(steps.size()); ++index) {
      const auto &step = steps.at(static_cast<size_t>(index));
      const auto label = QString::fromStdString(step.value("label", std::string()));
      const auto status = QString::fromStdString(step.value("status", std::string("pending")));
      const auto meta = workflowStatusMeta(status);
      auto *stepLabel = new QLabel(
          QStringLiteral("%1\n[%2]").arg(label.isEmpty() ? QStringLiteral("步骤") : label,
                                         meta.text),
          workflowGroup_);
      stepLabel->setAlignment(Qt::AlignCenter);
      stepLabel->setMinimumWidth(110);
      stepLabel->setStyleSheet(QStringLiteral(
          "QLabel { background-color: %1; color: %2; border: 2px solid %3; "
          "border-radius: 6px; padding: 8px 10px; }")
                                    .arg(meta.background, meta.foreground, meta.border));
      workflowStepsLayout_->addWidget(stepLabel);
      if (index < static_cast<qsizetype>(steps.size()) - 1) {
        auto *arrow = new QLabel(QStringLiteral("->"), workflowGroup_);
        arrow->setAlignment(Qt::AlignCenter);
        arrow->setStyleSheet(QStringLiteral("color: #888; font-weight: bold;"));
        workflowStepsLayout_->addWidget(arrow);
      }
    }
    workflowStepsLayout_->addStretch();

    if (!focusStep.is_null()) {
      focusLabel = QString::fromStdString(focusStep.value("label", std::string("步骤")));
      const auto stepMessage =
          QString::fromStdString(focusStep.value("message", std::string()));
      if (!stepMessage.isEmpty()) {
        focusMessage = stepMessage;
      }
    }
  } catch (...) {
    auto *errorLabel = new QLabel(QStringLiteral("流程事件解析失败"), workflowGroup_);
    errorLabel->setStyleSheet(QStringLiteral(
        "QLabel { background-color: #FFEBEE; color: #B71C1C; border: 2px solid #F44336; "
        "border-radius: 6px; padding: 8px 10px; }"));
    workflowStepsLayout_->addWidget(errorLabel);
    workflowStepsLayout_->addStretch();
    focusMessage = QStringLiteral("流程事件解析失败");
  }

  if (workflowMessageLabel_) {
    if (!focusLabel.isEmpty()) {
      workflowMessageLabel_->setText(
          QStringLiteral("说明: %1 - %2").arg(
              focusLabel, focusMessage.isEmpty() ? QStringLiteral("--") : focusMessage));
    } else {
      workflowMessageLabel_->setText(
          QStringLiteral("说明: %1").arg(focusMessage.isEmpty() ? QStringLiteral("--")
                                                               : focusMessage));
    }
  }
}

void BspRgbPage::loadScriptList() {
  scriptsList_->clear();
  const QDir scriptsDir(QDir(context_.paths().appRoot).filePath(QStringLiteral("scripts")));
  const auto entries = scriptsDir.entryInfoList(
      {QStringLiteral("record_bsp_rgb*.py")}, QDir::Files, QDir::Name);
  for (const auto &entry : entries) {
    if (entry.fileName() == QStringLiteral("record_bsp_rgb.py")) {
      continue;
    }
    auto *item = new QListWidgetItem(QStringLiteral("📄 %1").arg(entry.fileName()),
                                     scriptsList_);
    item->setData(Qt::UserRole, entry.absoluteFilePath());
  }
  if (scriptsList_->count() > 0) {
    scriptsList_->item(0)->setSelected(true);
  } else {
    auto *item = new QListWidgetItem(QStringLiteral("未找到 record_bsp_rgb*.py"),
                                     scriptsList_);
    item->setFlags(Qt::NoItemFlags);
  }
}

void BspRgbPage::refreshSelectedScriptsLabel() {
  QStringList names;
  if (scriptsList_) {
    for (const auto *item : scriptsList_->selectedItems()) {
      const QString scriptPath = item ? item->data(Qt::UserRole).toString() : QString();
      if (!scriptPath.isEmpty()) {
        names << QFileInfo(scriptPath).fileName();
      }
    }
  }
  if (names.isEmpty()) {
    selectedScriptsLabel_->setText(QStringLiteral("已选: 0 个脚本"));
    return;
  }
  QString preview = names.mid(0, 3).join(QStringLiteral("\n"));
  if (names.size() > 3) {
    preview += QStringLiteral("\n...");
  }
  selectedScriptsLabel_->setText(
      QStringLiteral("已选: %1 个脚本\n%2").arg(names.size()).arg(preview));
}

void BspRgbPage::refreshScriptButtons() {
  bool hasScript = false;
  if (scriptsList_) {
    for (const auto *item : scriptsList_->selectedItems()) {
      if (item && !item->data(Qt::UserRole).toString().isEmpty()) {
        hasScript = true;
        break;
      }
    }
  }
  if (runScriptButton_) {
    runScriptButton_->setEnabled(hasScript && !scriptRunning_);
  }
  if (stopScriptButton_) {
    stopScriptButton_->setEnabled(scriptRunning_);
  }
}

void BspRgbPage::syncScriptExecutorDeviceInfo() {
  if (!scriptExecutor_ || !controller_) {
    return;
  }
  scriptExecutor_->setRuntimeDeviceInfo(
      controller_->activeAgent(),
      controller_->activeAgentGlassesFsn(),
      controller_->activeAgentGlassesProductLabel());
}

void BspRgbPage::requestRuntimeState(bool force) {
  if (!controller_) {
    return;
  }
  if (controller_->activeAgent() != bspAgentName()) {
    runtimeRequestPending_ = false;
    return;
  }
  if (controller_->activeAgentWatchdogState() != QStringLiteral("healthy")) {
    runtimeRequestPending_ = false;
    return;
  }
  if (!pageActive_ && !force) {
    return;
  }
  if (runtimeRequestPending_ && !force) {
    return;
  }
  runtimeRequestPending_ = true;
  controller_->requestExecuteCommand(bspAgentName(),
                                     QStringLiteral("get_bsp_runtime_state"));
}

void BspRgbPage::handleCommandResult(const nlohmann::json &result) {
  const QString cmdName =
      QString::fromStdString(result.value("cmd_name", std::string()));
  if (cmdName.isEmpty()) {
    return;
  }
  const auto payload = commandPayload(result);

  if (cmdName == QStringLiteral("get_bsp_runtime_state")) {
    runtimeRequestPending_ = false;
    if (controller_ && controller_->activeAgent() != bspAgentName()) {
      return;
    }
    if (jsonSuccess(result, payload)) {
      applyRuntimeState(payload);
    }
    return;
  }

  if (cmdName == QStringLiteral("capture_raw_frame")) {
    if (jsonSuccess(result, payload)) {
      appendLog(QStringLiteral("RAW 抓取成功: %1")
                    .arg(jsonText(payload.value("raw_file", nlohmann::json()))));
      refreshDataTree();
    } else {
      appendLog(QStringLiteral("RAW 抓取失败: %1")
                    .arg(commandMessage(result, payload)));
    }
    requestRuntimeState(true);
    return;
  }

  if (cmdName == QStringLiteral("start_record") ||
      cmdName == QStringLiteral("stop_record") ||
      cmdName == QStringLiteral("start_device") ||
      cmdName == QStringLiteral("stop_device")) {
    appendLog(QStringLiteral("%1 %2: %3")
                  .arg(cmdName,
                       jsonSuccess(result, payload) ? QStringLiteral("成功")
                                                    : QStringLiteral("失败"),
                       commandMessage(result, payload)));
    requestRuntimeState(true);
  }
}

void BspRgbPage::applyRuntimeState(const nlohmann::json &state) {
  cameraMode_ =
      QString::fromStdString(state.value("camera_mode", std::string("slam")));
  setInfo(QStringLiteral("device_camera_mode"), cameraMode_);

  const auto device = state.value("device", nlohmann::json::object());
  setInfoFromJson(QStringLiteral("device_connected"), device.value("connected", nlohmann::json()));
  setInfoFromJson(QStringLiteral("device_opened"), device.value("is_opened", nlohmann::json()));
  setInfoFromJson(QStringLiteral("device_fsn"), device.value("fsn", nlohmann::json()));
  setInfoFromJson(QStringLiteral("device_fw"), device.value("mcu_firmware_version", nlohmann::json()));
  setInfoFromJson(QStringLiteral("device_has_rgb"), device.value("has_rgb_sensor", nlohmann::json()));
  setInfoFromJson(QStringLiteral("device_active_sensors"), device.value("active_sensors", nlohmann::json()));

  const auto temps = state.value("temperatures", nlohmann::json::object());
  setInfo(QStringLiteral("temp_rgb"), temperatureText(temps.value("rgb_temperature", nlohmann::json())));
  setInfo(QStringLiteral("temp_imu0"), temperatureText(temps.value("imu0_temperature", nlohmann::json())));
  setInfo(QStringLiteral("temp_imu1"), temperatureText(temps.value("imu1_temperature", nlohmann::json())));

  const auto rgbConfig = state.value("rgb_config", nlohmann::json::object());
  setInfoFromJson(QStringLiteral("rgb_sn"), rgbConfig.value("rgb_cam_sn", nlohmann::json()));
  setInfoFromJson(QStringLiteral("rgb_fps"), rgbConfig.value("fps", nlohmann::json()));
  setInfoFromJson(QStringLiteral("rgb_auto_exposure"), rgbConfig.value("auto_exposure", nlohmann::json()));
  setInfoFromJson(QStringLiteral("rgb_exposure"), rgbConfig.value("exposure", nlohmann::json()));
  setInfoFromJson(QStringLiteral("rgb_gain"), rgbConfig.value("gain", nlohmann::json()));

  applyLatestFrameState(state.value("latest_frame", nlohmann::json::object()));
  applyRecordState(state.value("record_state", nlohmann::json::object()));
  if (previewHintLabel_) {
    previewHintLabel_->setText(cameraMode_ == QStringLiteral("rgb")
                                   ? QStringLiteral("RGB 预览已启用")
                                   : QStringLiteral("当前模式不是 RGB，预览线程已关闭"));
  }
  syncCameraDisplayThread();
}

void BspRgbPage::applyLatestFrameState(const nlohmann::json &latestFrame) {
  setInfoFromJson(QStringLiteral("frame_timestamp"),
                  latestFrame.value("timestamp_ns", nlohmann::json()));
  const int width = latestFrame.value("width", latestFrame.value("raw_width", 0));
  const int height = latestFrame.value("height", latestFrame.value("raw_height", 0));
  setInfo(QStringLiteral("frame_resolution"),
          width > 0 && height > 0
              ? QStringLiteral("%1 x %2").arg(width).arg(height)
              : QStringLiteral("--"));
  setInfoFromJson(QStringLiteral("frame_exposure_duration"),
                  latestFrame.value("exposure_duration", nlohmann::json()));
  setInfoFromJson(QStringLiteral("frame_rolling_shutter"),
                  latestFrame.value("rolling_shutter_time", nlohmann::json()));
  setInfoFromJson(QStringLiteral("frame_stride"),
                  latestFrame.value("stride", nlohmann::json()));
  setInfoFromJson(QStringLiteral("frame_gain"),
                  latestFrame.value("gain", nlohmann::json()));
  if (latestFrame.contains("temperature")) {
    setInfo(QStringLiteral("temp_rgb"),
            temperatureText(latestFrame.value("temperature", nlohmann::json())));
  }
}

void BspRgbPage::applyRecordState(const nlohmann::json &recordState) {
  setInfoFromJson(QStringLiteral("record_active"),
                  recordState.value("is_recording", nlohmann::json()));
  setInfoFromJson(QStringLiteral("record_dataset"),
                  recordState.value("dataset_name", nlohmann::json()));
  setInfoFromJson(QStringLiteral("record_frame_count"),
                  recordState.value("frame_count", nlohmann::json()));
  setInfoFromJson(QStringLiteral("record_last_frame"),
                  recordState.value("last_frame_file", nlohmann::json()));
}

void BspRgbPage::handleRealtimeData(const QString &dataName,
                                    const nlohmann::json &value, double,
                                    double frequency) {
  if (!pageActive_) {
    return;
  }

  if (dataName == QString::fromUtf8(recordlab::common::TOPIC_CAMERA)) {
    latestCameraFrequency_ = frequency;
    try {
      if (value.contains("cam_meta") && value["cam_meta"].is_object()) {
        if (value["cam_meta"].contains("0")) {
          latestCameraMeta_ = value["cam_meta"]["0"];
          applyLatestFrameState(latestCameraMeta_);
        }
      }
    } catch (...) {
    }
  } else if (dataName == QStringLiteral("IMU0-temperature") && value.is_object()) {
    setInfo(QStringLiteral("temp_imu0"),
            temperatureText(value.value("value", nlohmann::json())));
  } else if (dataName == QStringLiteral("IMU1-temperature") && value.is_object()) {
    setInfo(QStringLiteral("temp_imu1"),
            temperatureText(value.value("value", nlohmann::json())));
  }
}

void BspRgbPage::setCameraDisplayActive(bool active) {
  pageActive_ = active;
  if (active) {
    runtimeRequestPending_ = false;
    if (controller_ &&
        controller_->activeAgentWatchdogState() == QStringLiteral("healthy")) {
      requestRuntimeState(true);
    }
  } else {
    runtimeRequestPending_ = false;
    cameraShmMissingLogged_ = false;
  }
  syncCameraDisplayThread();
}

void BspRgbPage::syncLatestData(
    const recordlab::backend::DataReceiverManager *receiver) {
  if (!receiver || !pageActive_) {
    return;
  }
  for (const auto &name :
       {QString::fromUtf8(recordlab::common::TOPIC_CAMERA),
        QStringLiteral("IMU0-temperature"),
        QStringLiteral("IMU1-temperature")}) {
    const auto latest = receiver->getLatestData(name.toStdString());
    if (latest.is_object() && latest.contains("value")) {
      handleRealtimeData(name, latest["value"], latest.value("timestamp", 0.0),
                         receiver->getFrequency(name.toStdString()));
    }
  }
}

void BspRgbPage::syncCameraDisplayThread() {
  const bool shouldRun =
      pageActive_ && cameraMode_ == QStringLiteral("rgb");
  if (shouldRun && !cameraDisplayThread_) {
    startCameraDisplayThread();
  } else if (!shouldRun && cameraDisplayThread_) {
    stopCameraDisplayThread();
    if (previewWidget_) {
      previewWidget_->clearImage(QStringLiteral("切换到 RGB 模式后显示"));
    }
  }
}

void BspRgbPage::startCameraDisplayThread() {
  if (cameraDisplayThread_) {
    return;
  }
  if (!cameraShmReader_) {
    cameraShmReader_ =
        std::make_unique<recordlab::common::CameraSharedMemoryReader>();
  }
  const bool attachedNow =
      cameraShmReader_->isAttached() || cameraShmReader_->attach();
  if (!attachedNow) {
    if (!cameraShmMissingLogged_) {
      appendLog(QStringLiteral("相机共享内存尚未就绪，等待第一帧 RGB 数据。"));
      cameraShmMissingLogged_ = true;
    }
  } else if (cameraShmMissingLogged_) {
    appendLog(QStringLiteral("相机共享内存已就绪，RGB 预览线程启动。"));
    cameraShmMissingLogged_ = false;
  }

  cameraDisplayThread_ = new recordlab::widgets::CameraDisplayThread(
      cameraShmReader_.get(), previewTargetSize(), 24.0, this);
  connect(cameraDisplayThread_,
          &recordlab::widgets::CameraDisplayThread::leftFrameReady, this,
          [this](const QImage &previewImage, double meanValue,
                 double stdValue) {
            if (previewWidget_) {
              previewWidget_->showCameraFrame(
                  previewImage, cameraOverlayLines(meanValue, stdValue));
            }
            if (cameraShmMissingLogged_) {
              appendLog(QStringLiteral("相机共享内存已就绪，RGB 预览线程启动。"));
              cameraShmMissingLogged_ = false;
            }
            if (cameraDisplayThread_) {
              cameraDisplayThread_->markFrameConsumed(0);
            }
          });
  connect(cameraDisplayThread_,
          &recordlab::widgets::CameraDisplayThread::rightFrameReady, this,
          [this](const QImage &, double, double) {
            if (cameraDisplayThread_) {
              cameraDisplayThread_->markFrameConsumed(1);
            }
          });
  cameraDisplayThread_->start();
}

void BspRgbPage::stopCameraDisplayThread() {
  if (!cameraDisplayThread_) {
    return;
  }
  cameraDisplayThread_->stopThread();
  cameraDisplayThread_->deleteLater();
  cameraDisplayThread_ = nullptr;
  if (cameraShmReader_) {
    cameraShmReader_->detach();
    cameraShmReader_.reset();
  }
  cameraShmMissingLogged_ = false;
}

QSize BspRgbPage::previewTargetSize() const {
  if (previewWidget_) {
    return previewWidget_->displayTargetSize().boundedTo(QSize(672, 504));
  }
  return QSize(640, 480);
}

QStringList BspRgbPage::cameraOverlayLines(double meanValue,
                                           double stdValue) const {
  QStringList lines;
  QStringList header;
  const int width = latestCameraMeta_.value("width", 0);
  const int height = latestCameraMeta_.value("height", 0);
  if (width > 0 && height > 0) {
    header << QStringLiteral("%1x%2").arg(width).arg(height);
  }
  if (latestCameraFrequency_ > 0.1) {
    header << QStringLiteral("%1Hz").arg(latestCameraFrequency_, 0, 'f', 1);
  }
  if (!header.isEmpty()) {
    lines << header.join(QStringLiteral("  "));
  }

  QStringList exposure;
  if (latestCameraMeta_.contains("exposure_duration")) {
    exposure << QStringLiteral("Exposure: %1 ms")
                    .arg(latestCameraMeta_.value("exposure_duration", 0.0) /
                             1'000'000.0,
                         0, 'f', 2);
  }
  if (latestCameraMeta_.contains("gain")) {
    exposure << QStringLiteral("Gain: %1")
                    .arg(jsonText(latestCameraMeta_.value("gain", nlohmann::json())));
  }
  if (!exposure.isEmpty()) {
    lines << exposure.join(QStringLiteral("  "));
  }
  lines << QStringLiteral("mean: %1  std: %2")
               .arg(meanValue, 0, 'f', 3)
               .arg(stdValue, 0, 'f', 3);
  return lines;
}

QImage BspRgbPage::latestSourceImage() const {
  return cameraDisplayThread_ ? cameraDisplayThread_->latestSourceImage(0)
                              : QImage{};
}

void BspRgbPage::runSelectedScript() {
  if (!scriptsList_ || scriptRunning_) {
    return;
  }
  pendingScripts_.clear();
  for (const auto *item : scriptsList_->selectedItems()) {
    const QString scriptPath = item ? item->data(Qt::UserRole).toString() : QString();
    if (!scriptPath.isEmpty()) {
      pendingScripts_ << scriptPath;
    }
  }
  if (pendingScripts_.isEmpty()) {
    return;
  }
  const QString scriptPath = pendingScripts_.takeFirst();
  appendLog(QStringLiteral("启动 RGB 脚本: %1").arg(scriptPath));
  const auto command = orchestrationBridge_.buildScriptCommandForPath(scriptPath);
  scriptExecutor_->executeCommand(command, scriptPath);
}

void BspRgbPage::stopScript() {
  if (!scriptRunning_) {
    return;
  }
  appendLog(QStringLiteral("正在停止 RGB 脚本..."));
  pendingScripts_.clear();
  scriptExecutor_->stopScript();
}

void BspRgbPage::refreshDataTree() {
  const QString root = dataRootPath(context_);
  QDir().mkpath(root);
  if (dataFileModel_ && dataTree_) {
    dataFileModel_->setRootPath(root);
    dataTree_->setRootIndex(dataFileModel_->index(root));
  }
}

QStringList BspRgbPage::selectedDataPaths() const {
  QStringList paths;
  if (!dataTree_ || !dataFileModel_ || !dataTree_->selectionModel()) {
    return paths;
  }

  const QDir rootDir(dataRootPath(context_));
  const auto selectedRows = dataTree_->selectionModel()->selectedRows(0);
  for (const auto &index : selectedRows) {
    const QString path = dataFileModel_->filePath(index);
    if (path.isEmpty()) {
      continue;
    }
    const QString relative = rootDir.relativeFilePath(path);
    if (relative.startsWith(QStringLiteral("..")) || relative == QStringLiteral(".")) {
      continue;
    }
    if (!paths.contains(path) && QFileInfo::exists(path)) {
      paths << path;
    }
  }
  return paths;
}

void BspRgbPage::openSelectedDataDirectory() {
  const QStringList paths = selectedDataPaths();
  QString directory = dataRootPath(context_);
  if (!paths.isEmpty()) {
    const QFileInfo info(paths.first());
    directory = info.isDir() ? info.absoluteFilePath() : info.dir().absolutePath();
  }
  QDir().mkpath(directory);
  const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
  if (opened) {
    appendLog(QStringLiteral("已打开目录: %1").arg(directory));
  } else {
    appendLog(QStringLiteral("无法打开目录: %1").arg(directory));
    QMessageBox::warning(this, QStringLiteral("警告"),
                         QStringLiteral("无法打开目录: %1").arg(directory));
  }
}

void BspRgbPage::exportSelectedDataItems() {
  const QStringList paths = selectedDataPaths();
  if (paths.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("警告"),
                         QStringLiteral("请先在 data 输出目录中选择要导出的文件或文件夹"));
    return;
  }

  const QString targetRoot =
      QFileDialog::getExistingDirectory(this, QStringLiteral("选择导出目标目录"));
  if (targetRoot.isEmpty()) {
    return;
  }

  QString errorMessage;
  int exportedCount = 0;
  for (const auto &sourcePath : paths) {
    const QFileInfo sourceInfo(sourcePath);
    const QString targetPath =
        uniqueExportPath(QDir(targetRoot).filePath(sourceInfo.fileName()));
    if (!copyPathRecursively(sourcePath, targetPath, &errorMessage)) {
      appendLog(QStringLiteral("导出 data 文件失败: %1").arg(errorMessage));
      QMessageBox::critical(this, QStringLiteral("导出失败"),
                            QStringLiteral("导出 data 文件失败: %1").arg(errorMessage));
      return;
    }
    ++exportedCount;
  }

  appendLog(QStringLiteral("已导出 %1 个 data 项到: %2")
                .arg(exportedCount)
                .arg(targetRoot));
  QMessageBox::information(this, QStringLiteral("导出完成"),
                           QStringLiteral("已导出 %1 个文件/文件夹").arg(exportedCount));
}

void BspRgbPage::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  if (cameraDisplayThread_) {
    cameraDisplayThread_->setTargetSize(previewTargetSize());
  }
}

void BspRgbPage::appendLog(const QString &message) {
  if (!logView_) {
    return;
  }
  logView_->appendPlainText(
      QStringLiteral("[%1] %2")
          .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
               message));
}

void BspRgbPage::setInfo(const QString &key, const QString &value) {
  auto *label = infoLabels_.value(key, nullptr);
  if (!label) {
    return;
  }
  label->setText(value.isEmpty() ? QStringLiteral("--") : value);
}

void BspRgbPage::setInfoFromJson(const QString &key,
                                 const nlohmann::json &value) {
  setInfo(key, jsonText(value));
}

} // namespace recordlab::bsp
