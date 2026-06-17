#pragma once

#include <QHash>
#include <QWidget>

#include "recordlab/core/qt_json_compat.h"
#include "recordlab/widgets/simple_curve_plot_widget.h"

#include <memory>

class QLabel;
class QGroupBox;
class QResizeEvent;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QTabWidget;
class QTimer;

namespace recordlab::common {
class CameraSharedMemoryWriter;
class CameraSharedMemoryReader;
} // namespace recordlab::common

namespace recordlab::backend {
class DataReceiverManager;
}

namespace recordlab::widgets {

class CameraDisplayThread;
class ImageDisplayWidget;

class DataMonitorWidget : public QWidget {
public:
  explicit DataMonitorWidget(QWidget *parent = nullptr);

  void handleRealtimeData(const QString &dataName, const nlohmann::json &value,
                          double timestamp, double frequency);
  void setCameraPreviewEnabled(bool enabled);
  void setCameraDisplayActive(bool active);
  void syncLatestData(const recordlab::backend::DataReceiverManager *receiver);
  QString selectedDataName() const;

  /// 启动相机显示后台线程
  void startCameraDisplayThread();
  /// 停止相机显示后台线程
  void stopCameraDisplayThread();

protected:
  void resizeEvent(QResizeEvent *event) override;

private:
  ImageDisplayWidget *leftCameraWidget_ = nullptr;
  ImageDisplayWidget *rightCameraWidget_ = nullptr;
  QGroupBox *cameraGroup_ = nullptr;
  QGroupBox *curveGroup_ = nullptr;
  QLabel *selectedDataValueLabel_ = nullptr;
  QLabel *motionStatusValueLabel_ = nullptr;
  QListWidget *imuListWidget_ = nullptr;
  QListWidget *customListWidget_ = nullptr;
  QPlainTextEdit *imuStatusView_ = nullptr;
  QTabWidget *leftTabWidget_ = nullptr;
  SimpleCurvePlotWidget *curvePlotWidget_ = nullptr;
  QHash<QString, QListWidgetItem *> imuItems_;
  QHash<QString, QListWidgetItem *> customItems_;
  QHash<QString, QString> latestValueTextByName_;
  QHash<QString, nlohmann::json> latestPayloadByName_;
  QHash<QString, double> latestTimestampByName_;
  QHash<QString, double> latestFrequencyByName_;
  QHash<QString, double> latestReceiveMonotonicSecByName_;
  nlohmann::json latestCameraValue_;
  QHash<int, nlohmann::json> latestCameraMetaByIndex_;
  double latestCameraFrequency_ = 0.0;
  QString currentSelectedDataName_;
  recordlab::backend::DataReceiverManager *dataReceiver_ = nullptr;
  bool cameraPreviewEnabled_ = true;
  bool cameraDisplayActive_ = false;
  bool curveDirty_ = false;
  bool imuSnapshotDirty_ = false;
  QString lastMotionStatus_;
  QTimer *uiRefreshTimer_ = nullptr;
  QTimer *cameraAttachRetryTimer_ = nullptr;
  double lastFrequencyRefreshSec_ = 0.0;
  double lastImuSnapshotRefreshSec_ = 0.0;

  // CameraDisplayThread 集成
  CameraDisplayThread *cameraDisplayThread_ = nullptr;
  std::unique_ptr<recordlab::common::CameraSharedMemoryReader> cameraShmReader_;

  void updateImuSnapshot();
  void updateSelectableDataLabel(const QString &dataName);
  void updateCurveForSelection();
  void appendCurveSample(const CurveSample &sample, bool scalarMode);
  void drainSelectedCurveBuffer();
  void refreshFrequencyIndicators();
  void updateCurveSubscription(const QString &newDataName);
  bool curveSupportsCurrentSelection() const;
  void showRealtimeCurvePanel();
  void updateCameraPreview(const nlohmann::json &value, double frequency);
  void updateMotionStatusLabel(const QString &status);
  QStringList cameraOverlayLines(int camIdx, double meanValue,
                                 double stdValue) const;
  QSize currentCameraTargetSize() const;
  void scheduleCameraAttachRetry();
  void stopCameraAttachRetry();
};

} // namespace recordlab::widgets
