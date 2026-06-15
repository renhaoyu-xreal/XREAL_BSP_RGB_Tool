#pragma once

#include <QMap>
#include <QStringList>
#include <QWidget>

#include "recordlab/bsp/bsp_orchestration_bridge.h"
#include "recordlab/core/app_context.h"
#include "recordlab/core/qt_json_compat.h"

class QLabel;
class QGroupBox;
class QHBoxLayout;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace recordlab::workflow {
class WorkflowController;
}

namespace recordlab::backend {
class DataReceiverManager;
}

namespace recordlab::flowagent::core {
class ScriptExecutor;
}

namespace recordlab::widgets {
class DataMonitorWidget;
class DataOutputDirectoryWidget;
}

namespace recordlab::script {

class ScriptExecutionPage : public QWidget {
    Q_OBJECT

public:
    enum class Mode {
        Batch,
        Debug
    };

    explicit ScriptExecutionPage(
        const recordlab::core::AppContext& context,
        recordlab::workflow::WorkflowController* controller,
        Mode mode,
        QWidget* parent = nullptr);

    void handleRealtimeData(const QString& dataName,
                            const nlohmann::json& value,
                            double timestamp,
                            double frequency);
    void setCameraDisplayActive(bool active);
    void syncLatestData(const recordlab::backend::DataReceiverManager* receiver);

private:
    const recordlab::core::AppContext& context_;
    recordlab::workflow::WorkflowController* controller_ = nullptr;
    Mode mode_;
    recordlab::bsp::BspOrchestrationBridge orchestrationBridge_;
    recordlab::flowagent::core::ScriptExecutor* scriptExecutor_ = nullptr;
    QLabel* activeAgentValueLabel_ = nullptr;
    QLabel* modeValueLabel_ = nullptr;
    QLabel* scriptBridgeValueLabel_ = nullptr;
    QLabel* watchdogSummaryValueLabel_ = nullptr;
    QLabel* watchdogStateValueLabel_ = nullptr;
    QLabel* selectedScriptsValueLabel_ = nullptr;
    QLabel* currentScriptPathValueLabel_ = nullptr;
    QGroupBox* workflowGroup_ = nullptr;
    QLabel* workflowTitleLabel_ = nullptr;
    QLabel* workflowMessageLabel_ = nullptr;
    QHBoxLayout* workflowStepsLayout_ = nullptr;
    recordlab::widgets::DataMonitorWidget* monitorWidget_ = nullptr;
    recordlab::widgets::DataOutputDirectoryWidget* dataOutputWidget_ = nullptr;
    QListWidget* selectedScriptsListWidget_ = nullptr;
    QPlainTextEdit* scriptEditor_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QPushButton* selectScriptsButton_ = nullptr;
    QPushButton* clearScriptsButton_ = nullptr;


    QPushButton* runButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* clearLogButton_ = nullptr;
    QStringList selectedScriptPaths_;
    QString currentDebugScriptPath_;
    QStringList pendingScripts_;
    QMap<QString, recordlab::core::ScriptCatalogEntry> configuredScriptsByPath_;
    QMap<QString, QString> currentEnvironment_;
    bool isRunning_ = false;
    bool bootstrapPending_ = false;

    static QLabel* makeSelectableLabel(const QString& text, QWidget* parent = nullptr);
    static QString formatWatchdogState(const QString& state);
    QString displayScriptPath(const QString& scriptPath) const;
    QString defaultScriptDirectory() const;
    QMap<QString, QString> buildScriptEnvironment() const;
    bool currentScriptsRequireNoSystemBsp() const;
    bool needsBspBootstrap() const;
    bool canStartPendingScripts() const;
    void startPendingScriptsNow();
    void refreshExecutionButtons();
    void refreshSelectedScriptsView();
    void reloadConfiguredScripts();
    QStringList selectedBatchScriptPaths() const;
    bool validateScriptsForCurrentDevice(const QStringList& scriptPaths);
    void updateOneClickButtonState();
    void syncScriptExecutorDeviceInfo();
    void syncMonitorDisplayMode();
    void loadDebugScriptPreview(const QString& scriptPath);
    //void requestOneClickForCurrentAgent();
    void executeNextPendingScript();
    void appendIntroLog();
    QWidget* buildWorkflowPanel();
    void clearWorkflowPanel();
    void updateWorkflowPanel(const QString& title, const QString& message,
                             const QString& stepsJson, bool finished, bool success);

private slots:
    void selectScripts();
    void clearScripts();
    void loadScript();
    void clearLog();
    void runSelectedScripts();
    void stopSelectedScripts();
    void onScriptStarted(const QString& scriptPath);
    void onScriptLog(const QString& message);
    void onScriptCompleted(bool success, const QString& error);
};

}  // namespace recordlab::script
