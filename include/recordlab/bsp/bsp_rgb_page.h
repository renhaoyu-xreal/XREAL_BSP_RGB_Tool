#pragma once

#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <QWidget>

#include <memory>

#include "recordlab/bsp/bsp_orchestration_bridge.h"
#include "recordlab/core/app_context.h"
#include "recordlab/core/qt_json_compat.h"

class QFileSystemModel;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class QTreeView;

namespace recordlab::backend {
class DataReceiverManager;
}

namespace recordlab::common {
class CameraSharedMemoryReader;
}

namespace recordlab::flowagent::core {
class ScriptExecutor;
}

namespace recordlab::widgets {
class CameraDisplayThread;
class ImageDisplayWidget;
}

namespace recordlab::workflow {
class WorkflowController;
}

namespace recordlab::bsp {

class BspRgbPage : public QWidget {
  Q_OBJECT

public:
  explicit BspRgbPage(const recordlab::core::AppContext &context,
                      recordlab::workflow::WorkflowController *controller,
                      QWidget *parent = nullptr);
  ~BspRgbPage() override;

public slots:
  void handleRealtimeData(const QString &dataName, const nlohmann::json &value,
                          double timestamp, double frequency);
  void handleCommandResult(const nlohmann::json &result);
  void setCameraDisplayActive(bool active);
  void syncLatestData(const recordlab::backend::DataReceiverManager *receiver);

protected:
  void resizeEvent(QResizeEvent *event) override;

private:
  const recordlab::core::AppContext &context_;
  recordlab::workflow::WorkflowController *controller_ = nullptr;
  recordlab::bsp::BspOrchestrationBridge orchestrationBridge_;
  recordlab::flowagent::core::ScriptExecutor *scriptExecutor_ = nullptr;

  QHash<QString, QLabel *> infoLabels_;
  recordlab::widgets::ImageDisplayWidget *previewWidget_ = nullptr;
  QLabel *previewHintLabel_ = nullptr;
  QPlainTextEdit *logView_ = nullptr;
  QTimer *runtimeRefreshTimer_ = nullptr;
  QListWidget *scriptsList_ = nullptr;
  QLabel *selectedScriptsLabel_ = nullptr;
  QPushButton *runScriptButton_ = nullptr;
  QPushButton *stopScriptButton_ = nullptr;
  QGroupBox *workflowGroup_ = nullptr;
  QLabel *workflowTitleLabel_ = nullptr;
  QLabel *workflowMessageLabel_ = nullptr;
  QHBoxLayout *workflowStepsLayout_ = nullptr;
  QFileSystemModel *dataFileModel_ = nullptr;
  QTreeView *dataTree_ = nullptr;

  bool pageActive_ = false;
  bool runtimeRequestPending_ = false;
  bool scriptRunning_ = false;
  bool cameraShmMissingLogged_ = false;
  QString cameraMode_ = QStringLiteral("slam");
  nlohmann::json latestCameraMeta_;
  double latestCameraFrequency_ = 0.0;
  QStringList pendingScripts_;

  std::unique_ptr<recordlab::common::CameraSharedMemoryReader> cameraShmReader_;
  recordlab::widgets::CameraDisplayThread *cameraDisplayThread_ = nullptr;

  QWidget *buildRuntimePanel();
  QWidget *buildPreviewPanel();
  QWidget *buildActionPanel();
  QWidget *buildDataPanel();
  void loadScriptList();
  void refreshSelectedScriptsLabel();
  void refreshScriptButtons();
  void syncScriptExecutorDeviceInfo();
  void requestRuntimeState(bool force = false);
  void applyRuntimeState(const nlohmann::json &state);
  void applyLatestFrameState(const nlohmann::json &latestFrame);
  void applyRecordState(const nlohmann::json &recordState);
  void syncCameraDisplayThread();
  void startCameraDisplayThread();
  void stopCameraDisplayThread();
  QSize previewTargetSize() const;
  QStringList cameraOverlayLines(double meanValue, double stdValue) const;
  QImage latestSourceImage() const;
  QWidget *buildWorkflowPanel();
  void clearWorkflowPanel();
  void updateWorkflowPanel(const QString &title, const QString &message,
                           const QString &stepsJson, bool finished, bool success);
  QStringList selectedDataPaths() const;
  void appendLog(const QString &message);
  void setInfo(const QString &key, const QString &value);
  void setInfoFromJson(const QString &key, const nlohmann::json &value);

private slots:
  void runSelectedScript();
  void stopScript();
  void refreshDataTree();
  void openSelectedDataDirectory();
  void exportSelectedDataItems();
};

} // namespace recordlab::bsp
