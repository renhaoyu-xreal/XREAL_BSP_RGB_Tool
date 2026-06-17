#include "recordlab/widgets/data_monitor_widget.h"

#include "recordlab/backend/data_receiver.h"
#include "recordlab/common/camera_shared_memory.h"
#include "recordlab/common/topics.h"
#include "recordlab/widgets/camera_display_thread.h"
#include "recordlab/widgets/image_display_widget.h"

#include <QCoreApplication>
#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace recordlab::widgets {

namespace {

QString baseDataName(const QString &displayText) {
  // 从列表项显示文本中剥离频率后缀，恢复真实数据名。
  const int suffixPos = displayText.indexOf(QStringLiteral(" ["));
  return suffixPos >= 0 ? displayText.left(suffixPos) : displayText.trimmed();
}

QString formatFrequencyItem(const QString &name, double frequency, bool active) {
  // 根据频率是否大于阈值给列表项打勾/打叉，便于快速判断数据是否在线。
  const QString mark = active ? QStringLiteral("✓") : QStringLiteral("✗");
  return QStringLiteral("%1 [%2 %3Hz]")
      .arg(name, mark)
      .arg(frequency, 0, 'f', 1);
}

QString scalarText(double value) {
  // 统一标量值的展示精度，避免不同控件格式不一致。
  return QStringLiteral("%1").arg(value, 0, 'f', 3);
}

double monotonicNowSec() {
  // 使用单调时钟驱动 UI 刷新节奏，避免系统时间变化造成抖动。
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

QString vectorText(const nlohmann::json &value) {
  // 把三轴向量格式化成紧凑单行文本。
  return QStringLiteral("x:%1 y:%2 z:%3")
      .arg(scalarText(value.value("x", 0.0)), scalarText(value.value("y", 0.0)),
           scalarText(value.value("z", 0.0)));
}

QString snapshotTextForValue(const QString &dataName,
                             const nlohmann::json &value) {
  // 为快照区域构造统一显示文本，兼容标量与向量两种数据形态。
  if (!value.is_object() || value.empty()) {
    if (dataName.contains(QStringLiteral("temperature")) ||
        dataName == QStringLiteral("time_delay") ||
        dataName == QStringLiteral("record_timer")) {
      return QStringLiteral("%1\nval: --").arg(dataName);
    }
    return QStringLiteral("%1\nx:-- y:-- z:--").arg(dataName);
  }

  if (value.contains("value")) {
    return QStringLiteral("%1\nval:%2")
        .arg(dataName, scalarText(value.value("value", 0.0)));
  }
  if (value.contains("x") && value.contains("y") && value.contains("z")) {
    return QStringLiteral("%1\n%2").arg(dataName, vectorText(value));
  }
  return dataName;
}

QStringList imuNames() {
  // 定义监控面板固定支持的 IMU 数据名称顺序。
  return {QStringLiteral("IMU0-gyro"),       QStringLiteral("IMU0-acc"),
          QStringLiteral("IMU0-mag"),        QStringLiteral("IMU0-temperature"),
          QStringLiteral("IMU1-gyro"),       QStringLiteral("IMU1-acc"),
          QStringLiteral("IMU1-temperature"),
          QStringLiteral("Android-gyro"),    QStringLiteral("Android-acc"),
          QStringLiteral("Android-mag"),
          QStringLiteral("Android-temperature")};
}

QString curvePlaceholderForData(const QString &dataName) {
  // 针对不能绘制曲线的数据类型给出更具体的占位说明。
  if (dataName == QStringLiteral("camera_data")) {
    return QStringLiteral("图像数据在上方预览区显示，不绘制曲线。");
  }
  if (dataName == QStringLiteral("motion_status")) {
    return QStringLiteral("运动状态是离散状态，当前不绘制曲线。");
  }
  return QStringLiteral("当前数据暂无可绘制曲线。");
}

} // namespace

DataMonitorWidget::DataMonitorWidget(QWidget *parent) : QWidget(parent) {
  // 构造监控页面：左侧数据选择与快照，右侧预览和曲线区域。
  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  rootLayout->setSpacing(8);

  auto *splitter = new QSplitter(Qt::Horizontal, this);
  splitter->setChildrenCollapsible(false);

  auto *monitorListPane = new QWidget(splitter);
  monitorListPane->setMinimumWidth(320);
  monitorListPane->setMaximumWidth(460);
  auto *leftOuterLayout = new QVBoxLayout(monitorListPane);
  leftOuterLayout->setContentsMargins(0, 0, 0, 0);
  leftOuterLayout->setSpacing(0);

  leftTabWidget_ = new QTabWidget(monitorListPane);
  leftTabWidget_->setDocumentMode(true);
  leftOuterLayout->addWidget(leftTabWidget_);

  auto *realtimePage = new QWidget(leftTabWidget_);
  auto *monitorListLayout = new QVBoxLayout(realtimePage);
  monitorListLayout->setContentsMargins(0, 0, 0, 0);
  monitorListLayout->setSpacing(8);

  auto *imuGroup = new QGroupBox(QStringLiteral("IMU 数据选择"), monitorListPane);
  auto *imuLayout = new QVBoxLayout(imuGroup);
  imuListWidget_ = new QListWidget(imuGroup);
  imuListWidget_->setStyleSheet(QStringLiteral(
      "QListWidget { background-color: #fff7d8; border: 1px solid #a59470; }"
      "QListWidget::item:selected { background-color: #d7e7ff; }"));
  for (const auto &name : imuNames()) {
    auto *item =
        new QListWidgetItem(formatFrequencyItem(name, 0.0, false), imuListWidget_);
    imuItems_.insert(name, item);
  }
  imuListWidget_->setMinimumHeight(250);
  imuListWidget_->setMaximumHeight(250);
  imuLayout->addWidget(imuListWidget_);
  monitorListLayout->addWidget(imuGroup, 0);

  auto *customGroup = new QGroupBox(QStringLiteral("自定义数据选择"), monitorListPane);
  auto *customLayout = new QVBoxLayout(customGroup);
  customListWidget_ = new QListWidget(customGroup);
  customListWidget_->setStyleSheet(QStringLiteral(
      "QListWidget { background-color: #eef9ea; border: 1px solid #90a78c; }"
      "QListWidget::item:selected { background-color: #d7e7ff; }"));
  for (const QString &name : {QStringLiteral("motion_status"), QStringLiteral("camera_data"),
                              QStringLiteral("time_delay"), QStringLiteral("record_timer")}) {
    auto *item =
        new QListWidgetItem(formatFrequencyItem(name, 0.0, false), customListWidget_);
    customItems_.insert(name, item);
  }
  customListWidget_->setMinimumHeight(118);
  customListWidget_->setMaximumHeight(118);
  customLayout->addWidget(customListWidget_);
  monitorListLayout->addWidget(customGroup, 0);

  auto *imuDisplayGroup = new QGroupBox(QStringLiteral("所有 IMU 数据"), monitorListPane);
  auto *imuDisplayLayout = new QVBoxLayout(imuDisplayGroup);
  imuStatusView_ = new QPlainTextEdit(imuDisplayGroup);
  imuStatusView_->setReadOnly(true);
  imuStatusView_->setStyleSheet(
      QStringLiteral("QPlainTextEdit { background-color: #F5F5F5; border: 1px "
                     "solid #888; padding: 8px; font-family: monospace; }"));
  imuStatusView_->setMinimumHeight(250);
  imuDisplayLayout->addWidget(imuStatusView_);
  monitorListLayout->addWidget(imuDisplayGroup, 1);

  motionStatusValueLabel_ = new QLabel(QStringLiteral("运动状态: 数据不足"), monitorListPane);
  motionStatusValueLabel_->setAlignment(Qt::AlignCenter);
  motionStatusValueLabel_->setMinimumHeight(36);
  monitorListLayout->addWidget(motionStatusValueLabel_, 0);

  leftTabWidget_->addTab(realtimePage, QStringLiteral("实时监控"));

  // 右侧主显示区
  auto *monitorDisplayPane = new QWidget(splitter);
  auto *monitorDisplayLayout = new QVBoxLayout(monitorDisplayPane);
  monitorDisplayLayout->setContentsMargins(0, 0, 0, 0);
  monitorDisplayLayout->setSpacing(8);

  // 创建但隐藏相机控件（保留数据接收/SHM逻辑），不将其添加到布局
  leftCameraWidget_ = new ImageDisplayWidget(QStringLiteral("图像 1"), monitorDisplayPane);
  rightCameraWidget_ = new ImageDisplayWidget(QStringLiteral("图像 2"), monitorDisplayPane);
  leftCameraWidget_->setZoomImageProvider([this]() -> QImage {
    return cameraDisplayThread_ ? cameraDisplayThread_->latestSourceImage(0) : QImage{};
  });
  rightCameraWidget_->setZoomImageProvider([this]() -> QImage {
    return cameraDisplayThread_ ? cameraDisplayThread_->latestSourceImage(1) : QImage{};
  });

  cameraGroup_ = new QGroupBox(QStringLiteral("图像预览"), monitorDisplayPane);
  auto *cameraLayout = new QHBoxLayout(cameraGroup_);
  cameraLayout->setContentsMargins(8, 8, 8, 8);
  cameraLayout->setSpacing(8);
  cameraLayout->addWidget(leftCameraWidget_, 1);
  cameraLayout->addWidget(rightCameraWidget_, 1);
  monitorDisplayLayout->addWidget(cameraGroup_, 3);

  selectedDataValueLabel_ = new QLabel(QStringLiteral("当前选择: 未选择数据"), monitorDisplayPane);
  selectedDataValueLabel_->setAlignment(Qt::AlignCenter);
  selectedDataValueLabel_->setStyleSheet(
      QStringLiteral("QLabel { background-color: #F0F0F0; border: 1px solid #888; padding: 5px; font-weight: bold; }"));
  monitorDisplayLayout->addWidget(selectedDataValueLabel_);

  curvePlotWidget_ = new SimpleCurvePlotWidget(monitorDisplayPane);
  curveGroup_ = new QGroupBox(QStringLiteral("普通曲线"), monitorDisplayPane);
  auto *curveLayout = new QVBoxLayout(curveGroup_);
  curveLayout->addWidget(curvePlotWidget_);
  monitorDisplayLayout->addWidget(curveGroup_, 6);

  splitter->addWidget(monitorListPane);
  splitter->addWidget(monitorDisplayPane);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({360, 940});
  rootLayout->addWidget(splitter);
  showRealtimeCurvePanel();

  // 连接 UI 信号
  connect(imuListWidget_, &QListWidget::itemClicked, this,
          [this](QListWidgetItem *item) {
            customListWidget_->clearSelection();
            updateSelectableDataLabel(item ? baseDataName(item->text()) : QString());
          });
  connect(customListWidget_, &QListWidget::itemClicked, this,
          [this](QListWidgetItem *item) {
            imuListWidget_->clearSelection();
            updateSelectableDataLabel(item ? baseDataName(item->text()) : QString());
          });

  uiRefreshTimer_ = new QTimer(this);
  uiRefreshTimer_->setInterval(33);
  lastFrequencyRefreshSec_ = monotonicNowSec();
  lastImuSnapshotRefreshSec_ = lastFrequencyRefreshSec_;
  connect(uiRefreshTimer_, &QTimer::timeout, this, [this]() {
    if (!isVisible()) return;
    const double now = monotonicNowSec();
    if (now - lastFrequencyRefreshSec_ >= 0.5) {
      refreshFrequencyIndicators();
      lastFrequencyRefreshSec_ = now;
    }
    if (imuSnapshotDirty_ && now - lastImuSnapshotRefreshSec_ >= 0.1) {
      updateImuSnapshot();
      imuSnapshotDirty_ = false;
      lastImuSnapshotRefreshSec_ = now;
    }
    drainSelectedCurveBuffer();
  });
  uiRefreshTimer_->start();

  updateMotionStatusLabel(QStringLiteral("none"));
  updateImuSnapshot();
}

void DataMonitorWidget::handleRealtimeData(const QString &dataName,
                                           const nlohmann::json &value,
                                           double timestamp,
                                           double frequency) {
  // 缓存最新数据并分发到 IMU、状态、相机等各自显示路径。
  if (!cameraPreviewEnabled_ && dataName == QStringLiteral("camera_data")) {
    return;
  }

  latestPayloadByName_.insert(dataName, value);
  latestTimestampByName_.insert(dataName, timestamp);
  latestFrequencyByName_.insert(dataName, frequency);
  latestReceiveMonotonicSecByName_.insert(dataName, monotonicNowSec());

  if (imuItems_.contains(dataName)) {
    latestValueTextByName_.insert(dataName,
                                  snapshotTextForValue(dataName, value));
    imuSnapshotDirty_ = true;
    return;
  }

  if (dataName == QStringLiteral("motion_status")) {
    QString status;
    try {
      if (value.contains("value") && value["value"].is_string()) {
        status = QString::fromStdString(value["value"].get<std::string>());
      } else if (value.is_string()) {
        status = QString::fromStdString(value.get<std::string>());
      }
    } catch (...) {
    }
    updateMotionStatusLabel(status);
    latestValueTextByName_.insert(
        dataName, QStringLiteral("motion_status\n%1").arg(status));
    return;
  }

  if (dataName == QStringLiteral("camera_data")) {
    latestValueTextByName_.insert(
        dataName,
        QStringLiteral("camera_data\n%1Hz").arg(frequency, 0, 'f', 1));
    updateCameraPreview(value, frequency);
    return;
  }

  latestValueTextByName_.insert(dataName, snapshotTextForValue(dataName, value));
}

void DataMonitorWidget::setCameraPreviewEnabled(bool enabled) {
  if (cameraPreviewEnabled_ == enabled) {
    return;
  }

  cameraPreviewEnabled_ = enabled;
  if (cameraGroup_) {
    cameraGroup_->setVisible(cameraPreviewEnabled_);
  }
  if (auto *cameraItem = customItems_.value(QStringLiteral("camera_data"), nullptr)) {
    cameraItem->setHidden(!cameraPreviewEnabled_);
  }

  if (!cameraPreviewEnabled_) {
    stopCameraDisplayThread();
    latestCameraValue_ = nlohmann::json::object();
    latestCameraMetaByIndex_.clear();
    latestCameraFrequency_ = 0.0;
    if (currentSelectedDataName_ == QStringLiteral("camera_data")) {
      updateSelectableDataLabel(QString());
    }
    showRealtimeCurvePanel();
    return;
  }

  showRealtimeCurvePanel();
  if (cameraDisplayActive_) {
    startCameraDisplayThread();
  }
}

void DataMonitorWidget::setCameraDisplayActive(bool active) {
  // 页面可见时启动相机显示线程；不可见时停止线程并清空占位图。
  cameraDisplayActive_ = active;
  if (!cameraDisplayActive_) {
    stopCameraDisplayThread();
    if (leftCameraWidget_) {
      leftCameraWidget_->clearImage(QStringLiteral("切换到当前页后显示"));
    }
    if (rightCameraWidget_) {
      rightCameraWidget_->clearImage(QStringLiteral("切换到当前页后显示"));
    }
    return;
  }

  if (cameraPreviewEnabled_ && leftCameraWidget_) {
    leftCameraWidget_->clearImage(QStringLiteral("等待图像流"));
  }
  if (cameraPreviewEnabled_ && rightCameraWidget_) {
    rightCameraWidget_->clearImage(QStringLiteral("等待图像流"));
  }
  lastFrequencyRefreshSec_ = 0.0;
  lastImuSnapshotRefreshSec_ = 0.0;
  refreshFrequencyIndicators();
  updateCurveSubscription(currentSelectedDataName_);
  if (cameraPreviewEnabled_) {
    startCameraDisplayThread();
  }
}

void DataMonitorWidget::syncLatestData(
    const recordlab::backend::DataReceiverManager *receiver) {
  // 从 DataReceiverManager 拉取最新缓存，快速补齐页面切入时的初始显示。
  if (!receiver) {
    return;
  }

  dataReceiver_ =
      const_cast<recordlab::backend::DataReceiverManager *>(receiver);

  updateCurveSubscription(currentSelectedDataName_);

  QStringList names;
  for (const auto &name : receiver->getDataNameList()) {
    names.push_back(QString::fromStdString(name));
  }
  names << QStringLiteral("motion_status") << QStringLiteral("time_delay")
        << QStringLiteral("record_timer");
  if (cameraPreviewEnabled_) {
    names << QStringLiteral("camera_data");
  }

  for (const auto &name : names) {
    const auto latest = receiver->getLatestData(name.toStdString());
    if (!latest.is_object() || !latest.contains("value")) {
      continue;
    }
    handleRealtimeData(name, latest["value"], latest.value("timestamp", 0.0),
                       receiver->getFrequency(name.toStdString()));
  }
  refreshFrequencyIndicators();
}

QString DataMonitorWidget::selectedDataName() const {
  // 返回当前曲线面板正在关注的数据名。
  return currentSelectedDataName_;
}

void DataMonitorWidget::resizeEvent(QResizeEvent *event) {
  // 窗口大小变化时把新的目标尺寸同步给相机显示线程。
  QWidget::resizeEvent(event);
  if (cameraDisplayThread_) {
    cameraDisplayThread_->setTargetSize(currentCameraTargetSize());
  }
}

void DataMonitorWidget::updateImuSnapshot() {
  // 将所有 IMU 最新值拼成快照文本，一次性刷新左下角状态区域。
  QStringList lines;
  for (const QString &name : imuNames()) {
    const auto payload = latestPayloadByName_.value(name, nlohmann::json::object());
    const double timestamp = latestTimestampByName_.value(name, 0.0);
    if (payload.contains("x") && payload.contains("y") && payload.contains("z")) {
      lines << name
            << QStringLiteral("x:%1 y:%2 z:%3")
                   .arg(scalarText(payload.value("x", 0.0)),
                        scalarText(payload.value("y", 0.0)),
                        scalarText(payload.value("z", 0.0)))
            << QStringLiteral("t:%1s").arg(timestamp, 0, 'f', 2);
    } else if (payload.contains("value")) {
      lines << name
            << QStringLiteral("val:%1 t:%2s")
                   .arg(scalarText(payload.value("value", 0.0)))
                   .arg(timestamp, 0, 'f', 2);
    } else {
      lines << name << QStringLiteral("val:-- t:--");
    }
  }
  imuStatusView_->setPlainText(lines.join(QStringLiteral("\n")));
}

void DataMonitorWidget::updateSelectableDataLabel(const QString &dataName) {
  // 切换当前选中数据，并刷新标题和曲线订阅关系。
  showRealtimeCurvePanel();
  updateCurveSubscription(dataName);
  currentSelectedDataName_ = dataName;
  selectedDataValueLabel_->setText(
      dataName.isEmpty() ? QStringLiteral("当前选择: 未选择数据")
                         : QStringLiteral("当前选择: %1").arg(dataName));
  updateCurveForSelection();
}

void DataMonitorWidget::updateCurveForSelection() {
  // 根据当前选择的数据类型决定清空曲线还是进入等待实时数据状态。
  curvePlotWidget_->setSelectedDataName(currentSelectedDataName_);
  if (currentSelectedDataName_.isEmpty()) {
    curvePlotWidget_->clearData();
    return;
  }

  if (!curveSupportsCurrentSelection()) {
    curvePlotWidget_->setPlaceholderText(
        curvePlaceholderForData(currentSelectedDataName_));
    curvePlotWidget_->clearData();
    return;
  }
  curvePlotWidget_->setPlaceholderText(
      QStringLiteral("等待实时数据进入当前 5 秒窗口。"));
  curvePlotWidget_->clearData();
}

void DataMonitorWidget::appendCurveSample(const CurveSample &sample,
                                          bool scalarMode) {
  // 将单个曲线样本转交给绘图组件。
  if (!curvePlotWidget_) {
    return;
  }
  curvePlotWidget_->appendSample(sample, scalarMode);
}

void DataMonitorWidget::drainSelectedCurveBuffer() {
  // 从 DataReceiverManager 消费当前选中数据的曲线缓冲，并批量喂给绘图组件。
  if (!dataReceiver_ || !curvePlotWidget_ || !curveSupportsCurrentSelection()) {
    return;
  }

  const auto buffered =
      dataReceiver_->getDisplayBuffer(currentSelectedDataName_.toStdString());
  if (buffered.empty()) {
    return;
  }

  // 30Hz 刷新下保留约 5 帧 backlog，既追实时也避免短时突发被截得太狠。
  const int backlogTrimThreshold = 160;
  const int startIndex =
      buffered.size() > static_cast<std::size_t>(backlogTrimThreshold)
          ? static_cast<int>(buffered.size()) - backlogTrimThreshold
          : 0;

  QVector<CurveSample> samples;
  samples.reserve(static_cast<int>(buffered.size()) - startIndex);
  bool scalarMode = false;
  bool modeDetected = false;

  for (int i = startIndex; i < static_cast<int>(buffered.size()); ++i) {
    const auto &item = buffered[static_cast<std::size_t>(i)];
    if (!item.is_object()) {
      continue;
    }
    const auto value = item.value("value", nlohmann::json::object());
    const double timestamp = item.value("timestamp", 0.0);
    if (value.is_object() && value.contains("value")) {
      scalarMode = true;
      modeDetected = true;
      samples.push_back(
          CurveSample{timestamp, value.value("value", 0.0), 0.0, 0.0});
    } else if (value.is_object() && value.contains("x") && value.contains("y") &&
               value.contains("z")) {
      if (!modeDetected) {
        scalarMode = false;
        modeDetected = true;
      }
      samples.push_back(CurveSample{timestamp, value.value("x", 0.0),
                                    value.value("y", 0.0),
                                    value.value("z", 0.0)});
    }
  }

  if (samples.isEmpty()) {
    return;
  }

  curvePlotWidget_->beginBatchUpdate();
  curvePlotWidget_->appendSamples(samples, scalarMode);
  curvePlotWidget_->flushUpdates();
}

void DataMonitorWidget::refreshFrequencyIndicators() {
  // 用最近统计出的频率刷新左右列表项的在线状态标记。
  const double now = monotonicNowSec();
  const auto isRecentlyUpdated = [this, now](const QString &name) {
    const double lastReceive =
        latestReceiveMonotonicSecByName_.value(name, -1.0);
    return lastReceive > 0.0 && (now - lastReceive) <= 2.0;
  };
  for (auto it = imuItems_.cbegin(); it != imuItems_.cend(); ++it) {
    if (auto *item = it.value()) {
      const double frequency = latestFrequencyByName_.value(it.key(), 0.0);
      item->setText(
          formatFrequencyItem(it.key(), frequency,
                              frequency > 0.1 || isRecentlyUpdated(it.key())));
    }
  }

  for (auto it = customItems_.cbegin(); it != customItems_.cend(); ++it) {
    if (auto *item = it.value()) {
      const double frequency = latestFrequencyByName_.value(it.key(), 0.0);
      item->setText(
          formatFrequencyItem(it.key(), frequency,
                              frequency > 0.1 || isRecentlyUpdated(it.key())));
    }
  }
}

void DataMonitorWidget::updateCurveSubscription(const QString &newDataName) {
  // 随着选中数据变化动态订阅/退订曲线缓冲，减少无关数据积累。
  if (!dataReceiver_) {
    return;
  }

  if (currentSelectedDataName_ == newDataName) {
    return;
  }

  if (!currentSelectedDataName_.isEmpty() &&
      currentSelectedDataName_ != newDataName) {
    dataReceiver_->unsubscribeData(currentSelectedDataName_.toStdString());
  }

  if (!newDataName.isEmpty() && newDataName != QStringLiteral("camera_data") &&
      newDataName != QStringLiteral("motion_status")) {
    dataReceiver_->subscribeData(newDataName.toStdString());
  }
}

bool DataMonitorWidget::curveSupportsCurrentSelection() const {
  // 相机图像和离散状态不画曲线，其余连续数据允许进入曲线面板。
  return !currentSelectedDataName_.isEmpty() &&
         currentSelectedDataName_ != QStringLiteral("camera_data") &&
         currentSelectedDataName_ != QStringLiteral("motion_status");
}

void DataMonitorWidget::showRealtimeCurvePanel() {
  if (curveGroup_) {
    curveGroup_->setVisible(true);
  }
}

void DataMonitorWidget::updateCameraPreview(const nlohmann::json &value,
                                            double frequency) {
  // 记录相机 metadata 与频率，供显示线程回调时叠加到预览图上。
  latestCameraValue_ = value;
  latestCameraFrequency_ = frequency;

  const nlohmann::json *metaSource = nullptr;
  if (value.is_object() && value.contains("cam_meta") &&
      value["cam_meta"].is_object()) {
    metaSource = &value["cam_meta"];
  } else if (value.is_object() && value.contains("cam_data") &&
             value["cam_data"].is_object()) {
    metaSource = &value["cam_data"];
  }

  if (!metaSource) {
    return;
  }

  try {
    for (auto it = metaSource->begin(); it != metaSource->end(); ++it) {
      bool ok = false;
      const int camIdx = QString::fromStdString(it.key()).toInt(&ok);
      if (!ok || !it.value().is_object()) {
        continue;
      }
      latestCameraMetaByIndex_.insert(camIdx, it.value());
    }
  } catch (...) {
  }
}

void DataMonitorWidget::updateMotionStatusLabel(const QString &status) {
  // 根据运动状态更新颜色和文案，直观区分静止、运动和活跃。
  const QString normalized =
      status.trimmed().isEmpty() ? QStringLiteral("none") : status.trimmed();
  if (normalized == lastMotionStatus_) {
    return;
  }
  lastMotionStatus_ = normalized;

  QString color = QStringLiteral("#E0E0E0");
  QString text = QStringLiteral("运动状态: %1").arg(normalized);
  if (normalized == QStringLiteral("static")) {
    color = QStringLiteral("#90EE90");
    text = QStringLiteral("运动状态: 静止");
  } else if (normalized == QStringLiteral("moving")) {
    color = QStringLiteral("#FFD700");
    text = QStringLiteral("运动状态: 运动");
  } else if (normalized == QStringLiteral("active")) {
    color = QStringLiteral("#FF6B6B");
    text = QStringLiteral("运动状态: 活跃");
  } else if (normalized == QStringLiteral("none")) {
    color = QStringLiteral("#D3D3D3");
    text = QStringLiteral("运动状态: 数据不足");
  }

  motionStatusValueLabel_->setText(text);
  motionStatusValueLabel_->setStyleSheet(QStringLiteral(
      "QLabel { background-color: %1; border: 2px solid #666; padding: 8px; "
      "font-size: 12px; font-weight: bold; }")
                                             .arg(color));
}

QStringList DataMonitorWidget::cameraOverlayLines(int camIdx, double meanValue,
                                                  double stdValue) const {
  // 组装相机叠字信息，包括分辨率、曝光、频率和灰度统计值。
  QStringList lines;
  const auto meta = latestCameraMetaByIndex_.value(camIdx);
  if (meta.is_object()) {
    const int width = meta.value("width", 0);
    const int height = meta.value("height", 0);
    QStringList header;
    if (width > 0 && height > 0) {
      header << QStringLiteral("%1x%2").arg(width).arg(height);
    }
    if (latestCameraFrequency_ > 0.1) {
      header << QStringLiteral("%1Hz").arg(latestCameraFrequency_, 0, 'f', 1);
    }
    if (!header.isEmpty()) {
      lines << header.join(QStringLiteral("  "));
    }

    QStringList metaLine;
    try {
      const double gain = meta.value("gain", 0.0);
      if (std::abs(gain) > 1e-6) {
        metaLine << QStringLiteral("Gain: %1").arg(gain, 0, 'f', gain == std::floor(gain) ? 0 : 2);
      }
    } catch (...) {
    }
    try {
      const long long exposureNs = meta.value("exposure_duration", 0LL);
      if (exposureNs > 0) {
        metaLine << QStringLiteral("Exposure: %1 ms")
                        .arg(static_cast<double>(exposureNs) / 1'000'000.0, 0,
                             'f', 2);
      }
    } catch (...) {
    }
    if (!metaLine.isEmpty()) {
      lines << metaLine.join(QStringLiteral("  "));
    }
  }

  QStringList statLine;
  if (std::isfinite(meanValue)) {
    statLine << QStringLiteral("mean: %1").arg(meanValue, 0, 'f', 3);
  }
  if (std::isfinite(stdValue)) {
    statLine << QStringLiteral("std: %1").arg(stdValue, 0, 'f', 3);
  }
  if (!statLine.isEmpty()) {
    lines << statLine.join(QStringLiteral("  "));
  }
  return lines;
}

QSize DataMonitorWidget::currentCameraTargetSize() const {
  // 以左侧相机控件当前尺寸作为双目预览共同的缩放目标。
  if (leftCameraWidget_) {
    return leftCameraWidget_->displayTargetSize();
  }
  return QSize(640, 400);
}

void DataMonitorWidget::startCameraDisplayThread() {
  // 准备共享内存 reader 并启动后台显示线程，把最新预览帧送到左右图像卡片。
  if (!cameraDisplayActive_) {
    return;
  }

  if (!cameraShmReader_) {
    cameraShmReader_ =
        std::make_unique<recordlab::common::CameraSharedMemoryReader>();
    if (!cameraShmReader_->attach()) {
      std::cerr << "[DataMonitorWidget] Camera SHM not ready yet" << std::endl;
      cameraShmReader_.reset();
      scheduleCameraAttachRetry();
      return;
    }
  }

  stopCameraAttachRetry();
  if (cameraDisplayThread_) {
    cameraDisplayThread_->setTargetSize(currentCameraTargetSize());
    return;
  }

  cameraDisplayThread_ = new CameraDisplayThread(
      cameraShmReader_.get(), currentCameraTargetSize(), 24.0, this);

  connect(cameraDisplayThread_, &CameraDisplayThread::leftFrameReady, this,
          [this](const QImage &previewImage, double meanValue,
                 double stdValue) {
            if (leftCameraWidget_) {
              leftCameraWidget_->showCameraFrame(
                  previewImage, cameraOverlayLines(0, meanValue, stdValue));
            }
            if (cameraDisplayThread_) {
              cameraDisplayThread_->markFrameConsumed(0);
            }
          });

  connect(cameraDisplayThread_, &CameraDisplayThread::rightFrameReady, this,
          [this](const QImage &previewImage, double meanValue,
                 double stdValue) {
            if (rightCameraWidget_) {
              rightCameraWidget_->showCameraFrame(
                  previewImage, cameraOverlayLines(1, meanValue, stdValue));
            }
            if (cameraDisplayThread_) {
              cameraDisplayThread_->markFrameConsumed(1);
            }
          });

  cameraDisplayThread_->start();
}

void DataMonitorWidget::stopCameraDisplayThread() {
  // 停止显示线程并释放共享内存 reader，彻底关闭相机预览链路。
  stopCameraAttachRetry();
  if (!cameraDisplayThread_) {
    if (cameraShmReader_) {
      cameraShmReader_->detach();
      cameraShmReader_.reset();
    }
    return;
  }
  cameraDisplayThread_->stopThread();
  cameraDisplayThread_->deleteLater();
  cameraDisplayThread_ = nullptr;
  if (cameraShmReader_) {
    cameraShmReader_->detach();
    cameraShmReader_.reset();
  }
}

void DataMonitorWidget::scheduleCameraAttachRetry() {
  // 页面已经可见但 bridge 还没创建共享内存时，持续轻量重试；
  // 这样首次进入“数据 + 命令”页无需靠切换 tab 触发第二次 attach。
  if (!cameraDisplayActive_ || !cameraPreviewEnabled_) {
    return;
  }
  if (!cameraAttachRetryTimer_) {
    cameraAttachRetryTimer_ = new QTimer(this);
    cameraAttachRetryTimer_->setInterval(500);
    connect(cameraAttachRetryTimer_, &QTimer::timeout, this, [this]() {
      if (!cameraDisplayActive_ || !cameraPreviewEnabled_) {
        stopCameraAttachRetry();
        return;
      }
      if (!cameraDisplayThread_) {
        startCameraDisplayThread();
      } else {
        stopCameraAttachRetry();
      }
    });
  }
  if (!cameraAttachRetryTimer_->isActive()) {
    cameraAttachRetryTimer_->start();
  }
}

void DataMonitorWidget::stopCameraAttachRetry() {
  if (cameraAttachRetryTimer_) {
    cameraAttachRetryTimer_->stop();
  }
}

} // namespace recordlab::widgets
