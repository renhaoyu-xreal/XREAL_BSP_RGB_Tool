#include "recordlab/script/script_execution_page.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QAbstractItemView>
#include <QColor>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTextDocument>
#include <QVBoxLayout>

#include "recordlab/bsp/bsp_asset_resolver.h"
#include "recordlab/workflow/workflow_controller.h"
#include "recordlab/backend/data_receiver.h"
#include "recordlab/core/compatibility_contract.h"
#include "recordlab/flowagent/core/script_executor.h"
#include "recordlab/widgets/data_output_directory_widget.h"
#include "recordlab/widgets/data_monitor_widget.h"

namespace recordlab::script {

namespace {

bool isNoSystemBspScriptPath(const QString& scriptPath)
{
    const QString fileName = QFileInfo(scriptPath).fileName();
    return fileName == QStringLiteral("record_bsp_id1088_ur_gt_3dof_batch.py")
        || fileName == QStringLiteral("record_ur_gt_3dof_batch_bsp.py");
}

struct WorkflowStatusMeta {
    QString text;
    QString background;
    QString foreground;
    QString border;
};

WorkflowStatusMeta workflowStatusMeta(const QString& status)
{
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

QString workflowStatusText(bool finished, bool success)
{
    if (!finished) {
        return QStringLiteral("运行中");
    }
    return success ? QStringLiteral("已完成") : QStringLiteral("已失败");
}

QString productIdText(int productId)
{
    return productId > 0 ? QString::number(productId) : QString();
}

QString scriptSupportedModelsText(const recordlab::core::ScriptCatalogEntry& entry)
{
    QStringList parts;
    for (const auto& id : entry.supportedModelIds) {
        const auto name = entry.supportedModels.value(id).trimmed();
        parts << (name.isEmpty() ? id : QStringLiteral("%1/%2").arg(id, name));
    }
    return parts.isEmpty() ? QStringLiteral("未配置") : parts.join(QStringLiteral(", "));
}

} // namespace

ScriptExecutionPage::ScriptExecutionPage(
    const recordlab::core::AppContext& context,
    recordlab::workflow::WorkflowController* controller,
    Mode mode,
    QWidget* parent)
    : QWidget(parent)
    , context_(context)
    , controller_(controller)
    , mode_(mode)
    , orchestrationBridge_(context)
    , scriptExecutor_(new recordlab::flowagent::core::ScriptExecutor(this))
{
    // 根据 Batch/Debug 两种模式构造不同布局，但共用同一套脚本执行与日志回流机制。
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(8);

    activeAgentValueLabel_ = makeSelectableLabel(
        controller_->activeAgent().isEmpty() ? QStringLiteral("<none>") : controller_->activeAgent(),
        this);
    watchdogSummaryValueLabel_ = makeSelectableLabel(controller_->watchdogSummary(), this);
    watchdogStateValueLabel_ =
        makeSelectableLabel(formatWatchdogState(controller_->activeAgentWatchdogState()), this);
    if (mode_ != Mode::Batch) {
        auto* infoRow = new QHBoxLayout();
        infoRow->setSpacing(10);
        infoRow->addWidget(new QLabel(QStringLiteral("当前主 Agent:"), this));
        infoRow->addWidget(activeAgentValueLabel_, 1);
        infoRow->addWidget(new QLabel(QStringLiteral("Watchdog:"), this));
        infoRow->addWidget(watchdogSummaryValueLabel_, 2);
        infoRow->addWidget(new QLabel(QStringLiteral("主 Agent 状态:"), this));
        infoRow->addWidget(watchdogStateValueLabel_, 1);
        infoRow->addStretch(1);
        rootLayout->addLayout(infoRow);
    } else {
        activeAgentValueLabel_->hide();
        watchdogSummaryValueLabel_->hide();
        watchdogStateValueLabel_->hide();
    }

    if (mode_ == Mode::Batch) {
        auto* middleLayout = new QHBoxLayout();
        middleLayout->setSpacing(12);
        monitorWidget_ = new recordlab::widgets::DataMonitorWidget(this);
        syncMonitorDisplayMode();
        middleLayout->addWidget(monitorWidget_, 5);

        auto* rightPane = new QWidget(this);
        auto* rightLayout = new QVBoxLayout(rightPane);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(8);

        auto* scriptGroup = new QGroupBox(QStringLiteral("脚本列表"), rightPane);
        auto* scriptLayout = new QVBoxLayout(scriptGroup);
        selectedScriptsListWidget_ = new QListWidget(scriptGroup);
        selectedScriptsListWidget_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        selectedScriptsListWidget_->setStyleSheet(QStringLiteral(R"(
            QListWidget {
                background-color: #FFFFE0;
                border: 1px solid #888;
                padding: 5px;
            }
        )"));
        selectedScriptsListWidget_->setMinimumHeight(220);
        connect(selectedScriptsListWidget_, &QListWidget::itemSelectionChanged, this, [this]() {
            selectedScriptPaths_ = selectedBatchScriptPaths();
            refreshSelectedScriptsView();
            refreshExecutionButtons();
        });
        scriptLayout->addWidget(selectedScriptsListWidget_);
        rightLayout->addWidget(scriptGroup, 2);

        auto* buttonRow = new QHBoxLayout();
        buttonRow->setSpacing(8);
        selectScriptsButton_ = new QPushButton(QStringLiteral("刷新脚本"), rightPane);
        selectScriptsButton_->setMinimumHeight(40);
        selectScriptsButton_->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #90EE90; border: 2px solid #006400; border-radius: 5px; }"));
        connect(selectScriptsButton_, &QPushButton::clicked, this, &ScriptExecutionPage::selectScripts);
        buttonRow->addWidget(selectScriptsButton_, 1);

        clearScriptsButton_ = new QPushButton(QStringLiteral("取消选择"), rightPane);
        clearScriptsButton_->setMinimumHeight(40);
        clearScriptsButton_->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #FFB6C1; border: 2px solid #8B0000; border-radius: 5px; }"));
        connect(clearScriptsButton_, &QPushButton::clicked, this, &ScriptExecutionPage::clearScripts);
        buttonRow->addWidget(clearScriptsButton_, 1);
        rightLayout->addLayout(buttonRow);

        selectedScriptsValueLabel_ = makeSelectableLabel(QStringLiteral("已选: 0 个脚本"), rightPane);
        selectedScriptsValueLabel_->setStyleSheet(QStringLiteral(
            "QLabel { background-color: #FFFFE0; border: 1px solid #888; padding: 8px; }"));
        rightLayout->addWidget(selectedScriptsValueLabel_);

        auto* executionButtonRow = new QHBoxLayout();
        executionButtonRow->setSpacing(8);
        runButton_ = new QPushButton(QStringLiteral("开始执行"), rightPane);
        runButton_->setMinimumHeight(40);
        runButton_->setStyleSheet(QStringLiteral(R"(
            QPushButton {
                background-color: #90EE90;
                border: 2px solid #006400;
                border-radius: 5px;
                font-weight: 600;
            }
            QPushButton:disabled {
                background-color: #D3D3D3;
                border-color: #888;
            }
        )"));
        connect(runButton_, &QPushButton::clicked, this, &ScriptExecutionPage::runSelectedScripts);
        executionButtonRow->addWidget(runButton_, 1);

        stopButton_ = new QPushButton(QStringLiteral("停止执行"), rightPane);
        stopButton_->setMinimumHeight(40);
        stopButton_->setStyleSheet(QStringLiteral(R"(
            QPushButton {
                background-color: #FFB6C1;
                border: 2px solid #8B0000;
                border-radius: 5px;
                font-weight: 600;
            }
            QPushButton:disabled {
                background-color: #D3D3D3;
                border-color: #888;
            }
        )"));
        connect(stopButton_, &QPushButton::clicked, this, &ScriptExecutionPage::stopSelectedScripts);
        executionButtonRow->addWidget(stopButton_, 1);
        rightLayout->addLayout(executionButtonRow);

        auto* dataGroup = new QGroupBox(QStringLiteral("data 输出目录"), rightPane);
        auto* dataLayout = new QVBoxLayout(dataGroup);
        const QString dataRoot = QDir(context_.paths().appRoot).filePath(QStringLiteral("data"));
        dataOutputWidget_ = new recordlab::widgets::DataOutputDirectoryWidget(dataRoot, dataGroup);
        connect(dataOutputWidget_, &recordlab::widgets::DataOutputDirectoryWidget::messageReady,
                this, [this](const QString& message) {
                    if (logView_) {
                        logView_->appendPlainText(message);
                    }
                });
        dataLayout->addWidget(dataOutputWidget_);
        rightLayout->addWidget(dataGroup, 2);

        // oneClickButton_ = new QPushButton(oneClickButtonText(controller_), rightPane);
        // oneClickButton_->setMinimumHeight(40);
        // oneClickButton_->setStyleSheet(QStringLiteral(R"(
        //     QPushButton {
        //         background-color: #87CEEB;
        //         border: 2px solid #4682B4;
        //         border-radius: 5px;
        //         font-weight: 600;
        //     }
        //     QPushButton:disabled {
        //         background-color: #D3D3D3;
        //         border-color: #888;
        //         color: #666;
        //     }
        // )"));
        // connect(oneClickButton_, &QPushButton::clicked, controller_,
        //         &recordlab::workflow::WorkflowController::requestOneClick);
        // rightLayout->addWidget(oneClickButton_);

        // androidOneClickButton_ = new QPushButton(QStringLiteral("一键 Android IMU"), rightPane);
        // androidOneClickButton_->setMinimumHeight(40);
        // androidOneClickButton_->setStyleSheet(QStringLiteral(R"(
        //     QPushButton {
        //         background-color: #FFD966;
        //         border: 2px solid #A66A00;
        //         border-radius: 5px;
        //         font-weight: 600;
        //     }
        //     QPushButton:disabled {
        //         background-color: #D3D3D3;
        //         border-color: #888;
        //         color: #666;
        //     }
        // )"));
        // connect(androidOneClickButton_, &QPushButton::clicked, controller_,
        //         &recordlab::workflow::WorkflowController::requestAndroidOneClick);
        // rightLayout->addWidget(androidOneClickButton_);
        rightLayout->addStretch(1);

        middleLayout->addWidget(rightPane, 1);
        rootLayout->addLayout(middleLayout, 3);
    } else {
        auto* topRow = new QHBoxLayout();
        topRow->setSpacing(8);

        selectScriptsButton_ = new QPushButton(QStringLiteral("加载脚本"), this);
        selectScriptsButton_->setMinimumHeight(36);
        selectScriptsButton_->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #90EE90; border: 1px solid #006400; }"));
        connect(selectScriptsButton_, &QPushButton::clicked, this, &ScriptExecutionPage::loadScript);
        topRow->addWidget(selectScriptsButton_, 0);

        currentScriptPathValueLabel_ = makeSelectableLabel(QStringLiteral("未加载脚本"), this);
        currentScriptPathValueLabel_->setStyleSheet(QStringLiteral(
            "QLabel { background-color: #FFFFE0; border: 1px solid #888; padding: 5px; }"));
        topRow->addWidget(currentScriptPathValueLabel_, 1);
        rootLayout->addLayout(topRow);

        auto* middleLayout = new QHBoxLayout();
        middleLayout->setSpacing(12);

        scriptEditor_ = new QPlainTextEdit(this);
        scriptEditor_->setReadOnly(true);
        scriptEditor_->setFont(QFont(QStringLiteral("Monospace")));
        scriptEditor_->setPlaceholderText(QStringLiteral("脚本内容将显示在这里..."));
        scriptEditor_->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { background-color: #FFFFE0; border: 1px solid #888; padding: 6px; }"));
        middleLayout->addWidget(scriptEditor_, 2);

        auto* controlPane = new QWidget(this);
        auto* controlLayout = new QVBoxLayout(controlPane);
        controlLayout->setContentsMargins(0, 0, 0, 0);
        controlLayout->setSpacing(10);

        auto* debugExecutionButtonRow = new QHBoxLayout();
        debugExecutionButtonRow->setSpacing(8);
        runButton_ = new QPushButton(QStringLiteral("运行脚本"), controlPane);
        runButton_->setMinimumHeight(42);
        runButton_->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #90EE90; border: 1px solid #006400; font-weight: 600; }"));
        connect(runButton_, &QPushButton::clicked, this, &ScriptExecutionPage::runSelectedScripts);
        debugExecutionButtonRow->addWidget(runButton_, 1);

        stopButton_ = new QPushButton(QStringLiteral("停止脚本"), controlPane);
        stopButton_->setMinimumHeight(42);
        stopButton_->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #FFB6C1; border: 1px solid #8B0000; font-weight: 600; }"));
        connect(stopButton_, &QPushButton::clicked, this, &ScriptExecutionPage::stopSelectedScripts);
        debugExecutionButtonRow->addWidget(stopButton_, 1);
        controlLayout->addLayout(debugExecutionButtonRow);

        auto* dataGroup = new QGroupBox(QStringLiteral("data 输出目录"), controlPane);
        auto* dataLayout = new QVBoxLayout(dataGroup);
        const QString dataRoot = QDir(context_.paths().appRoot).filePath(QStringLiteral("data"));
        dataOutputWidget_ = new recordlab::widgets::DataOutputDirectoryWidget(dataRoot, dataGroup);
        connect(dataOutputWidget_, &recordlab::widgets::DataOutputDirectoryWidget::messageReady,
                this, [this](const QString& message) {
                    if (logView_) {
                        logView_->appendPlainText(message);
                    }
                });
        dataLayout->addWidget(dataOutputWidget_);
        controlLayout->addWidget(dataGroup, 1);

        clearLogButton_ = new QPushButton(QStringLiteral("清空日志"), controlPane);
        clearLogButton_->setMinimumHeight(42);
        clearLogButton_->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #D3D3D3; border: 1px solid #888; font-weight: 600; }"));
        connect(clearLogButton_, &QPushButton::clicked, this, &ScriptExecutionPage::clearLog);
        controlLayout->addWidget(clearLogButton_);

        controlLayout->addStretch(1);
        middleLayout->addWidget(controlPane, 1);
        rootLayout->addLayout(middleLayout, 1);
    }

    auto* bottomSplitter = new QSplitter(Qt::Horizontal, this);
    auto* logGroup = new QGroupBox(QStringLiteral("执行日志"), bottomSplitter);
    auto* logLayout = new QVBoxLayout(logGroup);
    logView_ = new QPlainTextEdit(logGroup);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(2000);
    logView_->setPlaceholderText(QStringLiteral("脚本启动、停止、运行日志会持续输出到这里。"));
    logView_->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background-color: #FFFFE0; border: 1px solid #888; padding: 5px; }"));
    logView_->document()->setUndoRedoEnabled(false);
    logLayout->addWidget(logView_);
    bottomSplitter->addWidget(logGroup);
    bottomSplitter->addWidget(buildWorkflowPanel());
    bottomSplitter->setStretchFactor(0, 1);
    bottomSplitter->setStretchFactor(1, 1);
    bottomSplitter->setSizes({650, 550});
    rootLayout->addWidget(bottomSplitter, 1);

    connect(controller_, &recordlab::workflow::WorkflowController::activeAgentChanged, this,
            [this](const QString& agentName) {
                activeAgentValueLabel_->setText(
                    agentName.isEmpty() ? QStringLiteral("<none>") : agentName);
                syncScriptExecutorDeviceInfo();
                syncMonitorDisplayMode();
                reloadConfiguredScripts();
                updateOneClickButtonState();
            });
    connect(controller_, &recordlab::workflow::WorkflowController::watchdogSummaryChanged, this,
            [this](const QString& summary) { watchdogSummaryValueLabel_->setText(summary); });
    connect(controller_, &recordlab::workflow::WorkflowController::activeAgentWatchdogStateChanged, this,
            [this](const QString& state) { watchdogStateValueLabel_->setText(formatWatchdogState(state)); });
    connect(controller_, &recordlab::workflow::WorkflowController::activeAgentDeviceInfoChanged,
            this, [this](const QString&, const QString&) {
                syncScriptExecutorDeviceInfo();
                refreshSelectedScriptsView();
                refreshExecutionButtons();
            });
    connect(controller_, &recordlab::workflow::WorkflowController::stateChanged, this,
            [this](recordlab::workflow::WorkflowController::State state) {
                updateOneClickButtonState();
                if (bootstrapPending_
                    && state == recordlab::workflow::WorkflowController::State::Failed) {
                    bootstrapPending_ = false;
                    pendingScripts_.clear();
                    refreshExecutionButtons();
                    logView_->appendPlainText(QStringLiteral("一键启动失败，脚本队列已取消。"));
                    return;
                }
                if (bootstrapPending_ && canStartPendingScripts()) {
                    startPendingScriptsNow();
                }
            });
    connect(controller_, &recordlab::workflow::WorkflowController::oneClickSuccessChanged, this,
            [this](bool) {
                updateOneClickButtonState();
                if (bootstrapPending_ && canStartPendingScripts()) {
                    startPendingScriptsNow();
                }
            });
    connect(controller_, &recordlab::workflow::WorkflowController::logAppended, this,
            [this](const QString& message) { logView_->appendPlainText(message); });
    connect(scriptExecutor_, &recordlab::flowagent::core::ScriptExecutor::scriptStarted,
            this, &ScriptExecutionPage::onScriptStarted);
    connect(scriptExecutor_, &recordlab::flowagent::core::ScriptExecutor::scriptLog,
            this, &ScriptExecutionPage::onScriptLog);
    connect(scriptExecutor_, &recordlab::flowagent::core::ScriptExecutor::scriptCompleted,
            this, &ScriptExecutionPage::onScriptCompleted);
    connect(scriptExecutor_, &recordlab::flowagent::core::ScriptExecutor::workflowUpdated,
            this, &ScriptExecutionPage::updateWorkflowPanel);
    connect(scriptExecutor_, &recordlab::flowagent::core::ScriptExecutor::workflowCleared,
            this, &ScriptExecutionPage::clearWorkflowPanel);

    reloadConfiguredScripts();
    refreshExecutionButtons();
    appendIntroLog();
    updateOneClickButtonState();
    syncScriptExecutorDeviceInfo();
}

QLabel* ScriptExecutionPage::makeSelectableLabel(const QString& text, QWidget* parent)
{
    // 创建支持鼠标复制的标签，方便用户直接复制路径和状态文本。
    auto* label = new QLabel(text, parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

QString ScriptExecutionPage::formatWatchdogState(const QString& state)
{
    // 将空 watchdog 状态标准化成“未注册”，避免界面出现空白。
    return state.isEmpty() ? QStringLiteral("未注册") : state;
}

QString ScriptExecutionPage::displayScriptPath(const QString& scriptPath) const
{
    // 若脚本位于默认脚本目录下，则尽量显示相对路径，减少列表噪音。
    const QDir scriptsRoot(defaultScriptDirectory());
    if (!scriptsRoot.path().isEmpty()) {
        const auto relativePath = scriptsRoot.relativeFilePath(scriptPath);
        if (!relativePath.startsWith(QStringLiteral(".."))) {
            return relativePath;
        }
    }
    return scriptPath;
}

QString ScriptExecutionPage::defaultScriptDirectory() const
{
    // 默认优先使用当前工程 vendored 脚本目录，必要时才回退到旧工程目录。
    const auto assets = recordlab::bsp::BspAssetResolver::resolve(context_);
    if (assets.vendoredScriptsAvailable) {
        return assets.vendoredScriptsRoot;
    }
    if (assets.legacyScriptsAvailable) {
        return assets.legacyScriptsRoot;
    }
    return context_.paths().appRoot;
}

QMap<QString, QString> ScriptExecutionPage::buildScriptEnvironment() const
{
    // 只注入运行态提示；业务参数统一由脚本运行时弹窗获取。
    QMap<QString, QString> environment;
    if (controller_
        && (controller_->oneClickSucceeded()
            || controller_->state()
                   == recordlab::workflow::WorkflowController::State::DeviceReady)) {
        environment.insert(QStringLiteral("RECORDLAB_SCRIPT_ASSUME_DEVICE_READY"),
                           QStringLiteral("1"));
    }
    return environment;
}

QWidget* ScriptExecutionPage::buildWorkflowPanel()
{
    workflowGroup_ = new QGroupBox(QStringLiteral("流程状态"), this);
    auto* layout = new QVBoxLayout(workflowGroup_);
    layout->setSpacing(8);

    workflowTitleLabel_ = makeSelectableLabel(QString(), workflowGroup_);
    workflowTitleLabel_->setStyleSheet(QStringLiteral("font-weight: 600;"));
    layout->addWidget(workflowTitleLabel_);

    auto* stepsScroll = new QScrollArea(workflowGroup_);
    stepsScroll->setWidgetResizable(true);
    stepsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    stepsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    stepsScroll->setMinimumHeight(100);
    auto* stepsContainer = new QWidget(stepsScroll);
    workflowStepsLayout_ = new QHBoxLayout(stepsContainer);
    workflowStepsLayout_->setContentsMargins(4, 4, 4, 4);
    workflowStepsLayout_->setSpacing(6);
    stepsScroll->setWidget(stepsContainer);
    layout->addWidget(stepsScroll);

    workflowMessageLabel_ = makeSelectableLabel(QString(), workflowGroup_);
    workflowMessageLabel_->setStyleSheet(QStringLiteral(
        "QLabel { background-color: #FFFDF2; border: 1px solid #C8B36A; padding: 6px; }"));
    layout->addWidget(workflowMessageLabel_);

    workflowGroup_->hide();
    return workflowGroup_;
}

void ScriptExecutionPage::clearWorkflowPanel()
{
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
            auto* item = workflowStepsLayout_->takeAt(0);
            if (auto* widget = item->widget()) {
                widget->deleteLater();
            }
            delete item;
        }
    }
}

void ScriptExecutionPage::updateWorkflowPanel(const QString& title,
                                              const QString& message,
                                              const QString& stepsJson,
                                              bool finished,
                                              bool success)
{
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
        auto* item = workflowStepsLayout_->takeAt(0);
        if (auto* widget = item->widget()) {
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
        for (const auto& target : focusStatuses) {
            for (const auto& step : steps) {
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
            const auto& step = steps.at(static_cast<size_t>(index));
            const auto label = QString::fromStdString(step.value("label", std::string()));
            const auto status = QString::fromStdString(step.value("status", std::string("pending")));
            const auto meta = workflowStatusMeta(status);
            auto* stepLabel = new QLabel(
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
                auto* arrow = new QLabel(QStringLiteral("->"), workflowGroup_);
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
        auto* errorLabel = new QLabel(QStringLiteral("流程事件解析失败"), workflowGroup_);
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

bool ScriptExecutionPage::needsBspBootstrap() const
{
    // 当目标是 BSP 主 agent 且设备尚未 ready 时，脚本执行前需要自动一键启动。
    if (!controller_) {
        return false;
    }
    if (!controller_->isTargetAgentSelected()) {
        return false;
    }
    return !controller_->oneClickSucceeded()
        && controller_->state() != recordlab::workflow::WorkflowController::State::DeviceReady;
}

bool ScriptExecutionPage::currentScriptsRequireNoSystemBsp() const
{
    const QStringList scripts =
        !pendingScripts_.isEmpty()
            ? pendingScripts_
            : (mode_ == Mode::Batch ? selectedScriptPaths_
                                    : QStringList{currentDebugScriptPath_});
    for (const auto& scriptPath : scripts) {
        if (isNoSystemBspScriptPath(scriptPath)) {
            return true;
        }
    }
    return false;
}

bool ScriptExecutionPage::canStartPendingScripts() const
{
    // 只有一键流程成功或设备已经 ready 时，才允许脚本队列真正开跑。
    if (!controller_) {
        return false;
    }
    return controller_->oneClickSucceeded()
        || controller_->state() == recordlab::workflow::WorkflowController::State::DeviceReady;
}

void ScriptExecutionPage::startPendingScriptsNow()
{
    // bootstrap 完成后正式启动排队脚本；若队列为空则直接结束等待状态。
    if (!bootstrapPending_ || pendingScripts_.isEmpty()) {
        bootstrapPending_ = false;
        refreshExecutionButtons();
        return;
    }

    bootstrapPending_ = false;
    currentEnvironment_ = buildScriptEnvironment();
    isRunning_ = true;
    refreshExecutionButtons();
    logView_->appendPlainText(QStringLiteral("设备已就绪，开始执行脚本队列。"));
    executeNextPendingScript();
}

void ScriptExecutionPage::refreshExecutionButtons()
{
    // 根据是否忙碌、是否已选脚本以及模式类型统一刷新按钮可用性。
    const bool busy = isRunning_ || bootstrapPending_;
    const bool hasBatchScripts = !selectedScriptPaths_.isEmpty();
    const bool hasDebugScript = !currentDebugScriptPath_.isEmpty();

    if (selectScriptsButton_) {
        selectScriptsButton_->setEnabled(!busy);
    }
    if (clearScriptsButton_) {
        clearScriptsButton_->setEnabled(!busy && hasBatchScripts);
    }
    if (runButton_) {
        const bool hasWork = mode_ == Mode::Batch ? hasBatchScripts : hasDebugScript;
        runButton_->setEnabled(!busy && hasWork);
    }
    if (stopButton_) {
        stopButton_->setEnabled(busy);
    }
    updateOneClickButtonState();
}

void ScriptExecutionPage::refreshSelectedScriptsView()
{
    if (!selectedScriptsValueLabel_) {
        return;
    }

    if (selectedScriptsListWidget_) {
        const int productId = controller_ ? controller_->activeAgentGlassesProductId() : -1;
        const QString productIdValue = productIdText(productId);
        const QString fsn = controller_ ? controller_->activeAgentGlassesFsn() : QString();
        const QString productLabel = controller_ ? controller_->activeAgentGlassesProductLabel() : QString();

        for (int index = 0; index < selectedScriptsListWidget_->count(); ++index) {
            auto* item = selectedScriptsListWidget_->item(index);
            const QString scriptPath = item->data(Qt::UserRole).toString();
            const auto entry = configuredScriptsByPath_.value(scriptPath);
            const QString supportedText = scriptSupportedModelsText(entry);
            QString statusText;
            QColor foreground;
            if (productIdValue.isEmpty()) {
                statusText = QStringLiteral("脚本支持: %1").arg(supportedText);
                foreground = QColor(QStringLiteral("#37474F"));
            } else if (entry.supportedModelIds.contains(productIdValue)) {
                statusText = QStringLiteral("适配当前型号 %1，FSN: %2")
                                 .arg(productLabel.isEmpty() ? productIdValue : productLabel,
                                      fsn.isEmpty() ? QStringLiteral("--") : fsn);
                foreground = QColor(QStringLiteral("#1B5E20"));
            } else {
                statusText = QStringLiteral("不适配当前型号 %1，支持: %2")
                                 .arg(productLabel.isEmpty() ? productIdValue : productLabel,
                                      supportedText);
                foreground = QColor(QStringLiteral("#B71C1C"));
            }

            item->setText(QStringLiteral("%1\n%2").arg(displayScriptPath(scriptPath), statusText));
            item->setToolTip(QStringLiteral("%1\n%2").arg(scriptPath, statusText));
            item->setForeground(foreground);
        }
    }
    selectedScriptsValueLabel_->setText(QStringLiteral("已选: %1 个脚本").arg(selectedScriptPaths_.size()));
}

void ScriptExecutionPage::reloadConfiguredScripts()
{
    if (mode_ != Mode::Batch || !selectedScriptsListWidget_) {
        return;
    }

    configuredScriptsByPath_.clear();
    selectedScriptPaths_.clear();
    selectedScriptsListWidget_->clear();

    const QString agentName = controller_ ? controller_->activeAgent() : QString();
    const auto entries = context_.recordLabConfig().scriptsForAgent(agentName);
    const QDir appRoot(context_.paths().appRoot);
    for (const auto& entry : entries) {
        if (!entry.enabled) {
            continue;
        }
        const QString scriptPath = QDir::cleanPath(appRoot.filePath(entry.relativePath));
        configuredScriptsByPath_.insert(scriptPath, entry);
        auto* item = new QListWidgetItem(selectedScriptsListWidget_);
        item->setData(Qt::UserRole, scriptPath);
        item->setFlags(item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    }

    if (logView_) {
        if (entries.isEmpty()) {
            logView_->appendPlainText(
                QStringLiteral("当前主 Agent 未配置可执行脚本: %1")
                    .arg(agentName.isEmpty() ? QStringLiteral("<none>") : agentName));
        } else {
            logView_->appendPlainText(
                QStringLiteral("已从 recordLabConfig 加载 %1 个脚本。").arg(entries.size()));
        }
    }
    refreshSelectedScriptsView();
}

QStringList ScriptExecutionPage::selectedBatchScriptPaths() const
{
    QStringList paths;
    if (!selectedScriptsListWidget_) {
        return paths;
    }
    const auto items = selectedScriptsListWidget_->selectedItems();
    for (auto* item : items) {
        const QString path = item->data(Qt::UserRole).toString();
        if (!path.isEmpty()) {
            paths.push_back(path);
        }
    }
    return paths;
}

bool ScriptExecutionPage::validateScriptsForCurrentDevice(const QStringList& scriptPaths)
{
    if (mode_ != Mode::Batch) {
        return true;
    }

    const int productId = controller_ ? controller_->activeAgentGlassesProductId() : -1;
    const QString productIdValue = productIdText(productId);
    const QString fsn = controller_ ? controller_->activeAgentGlassesFsn() : QString();
    const QString productLabel = controller_ ? controller_->activeAgentGlassesProductLabel() : QString();
    if (productIdValue.isEmpty()) {
        const QString message = QStringLiteral("未识别到当前眼镜型号，不能执行脚本。请先确认设备已连接并完成 check。");
        logView_->appendPlainText(message);
        QMessageBox::warning(this, QStringLiteral("脚本型号校验"), message);
        refreshSelectedScriptsView();
        return false;
    }

    for (const auto& scriptPath : scriptPaths) {
        const auto entry = configuredScriptsByPath_.value(scriptPath);
        if (entry.relativePath.isEmpty()) {
            const QString message =
                QStringLiteral("脚本未在 recordLabConfig 中声明，已阻止执行: %1").arg(scriptPath);
            logView_->appendPlainText(message);
            QMessageBox::warning(this, QStringLiteral("脚本型号校验"), message);
            return false;
        }
        if (!entry.supportedModelIds.contains(productIdValue)) {
            const QString message =
                QStringLiteral("该脚本不适配该型号眼镜。\n脚本: %1\n当前型号: %2\nFSN: %3\n支持型号: %4")
                    .arg(displayScriptPath(scriptPath),
                         productLabel.isEmpty() ? productIdValue : productLabel,
                         fsn.isEmpty() ? QStringLiteral("--") : fsn,
                         scriptSupportedModelsText(entry));
            logView_->appendPlainText(message);
            QMessageBox::critical(this, QStringLiteral("脚本型号不匹配"), message);
            refreshSelectedScriptsView();
            return false;
        }
    }

    logView_->appendPlainText(
        QStringLiteral("型号校验通过: %1 | FSN: %2")
            .arg(productLabel.isEmpty() ? productIdValue : productLabel,
                 fsn.isEmpty() ? QStringLiteral("--") : fsn));
    return true;
}

void ScriptExecutionPage::handleRealtimeData(const QString& dataName,
                                             const nlohmann::json& value,
                                             double timestamp,
                                             double frequency)
{
    // Batch 模式下把实时数据继续转交给监控组件，Debug 模式无需展示。
    if (mode_ == Mode::Batch && monitorWidget_) {
        monitorWidget_->handleRealtimeData(dataName, value, timestamp, frequency);
    }
}

void ScriptExecutionPage::setCameraDisplayActive(bool active)
{
    // 仅在 Batch 模式中切换相机显示线程，避免页面切走时继续刷新。
    if (mode_ == Mode::Batch && monitorWidget_) {
        monitorWidget_->setCameraDisplayActive(active);
    }
}

void ScriptExecutionPage::syncLatestData(const recordlab::backend::DataReceiverManager* receiver)
{
    // 页面切换回来时主动同步最新数据，减少监控区的空白时间。
    if (mode_ == Mode::Batch && monitorWidget_) {
        monitorWidget_->syncLatestData(receiver);
    }
}

void ScriptExecutionPage::updateOneClickButtonState()
{
    // 脚本执行页不再承载一键启动按钮；一键启动只保留在数据+页面。
}

void ScriptExecutionPage::syncScriptExecutorDeviceInfo()
{
    if (!scriptExecutor_ || !controller_) {
        return;
    }
    scriptExecutor_->setRuntimeDeviceInfo(
        controller_->activeAgent(),
        controller_->activeAgentGlassesFsn(),
        controller_->activeAgentGlassesProductLabel());
}

void ScriptExecutionPage::syncMonitorDisplayMode()
{
    if (mode_ != Mode::Batch || !monitorWidget_ || !controller_) {
        return;
    }
    monitorWidget_->setNvizDisplayMode(controller_->isNvizAgent());
    monitorWidget_->setCameraPreviewEnabled(
        !(controller_->isNvizAgent() || controller_->isHelenAgent()));
}

void ScriptExecutionPage::loadDebugScriptPreview(const QString& scriptPath)
{
    // 在调试模式中加载脚本内容并展示到只读编辑器，供人工检查。
    currentDebugScriptPath_ = scriptPath;

    if (currentScriptPathValueLabel_) {
        currentScriptPathValueLabel_->setText(
            scriptPath.isEmpty() ? QStringLiteral("未加载脚本") : scriptPath);
    }

    if (!scriptEditor_) {
        refreshExecutionButtons();
        return;
    }

    if (scriptPath.isEmpty()) {
        scriptEditor_->clear();
        refreshExecutionButtons();
        return;
    }

    QFile file(scriptPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        logView_->appendPlainText(QStringLiteral("✗ 加载脚本失败: %1").arg(scriptPath));
        currentDebugScriptPath_.clear();
        if (currentScriptPathValueLabel_) {
            currentScriptPathValueLabel_->setText(QStringLiteral("未加载脚本"));
        }
        scriptEditor_->clear();
        refreshExecutionButtons();
        return;
    }

    scriptEditor_->setPlainText(QString::fromUtf8(file.readAll()));
    logView_->appendPlainText(QStringLiteral("✓ 加载脚本: %1").arg(scriptPath));
    refreshExecutionButtons();
}

// void ScriptExecutionPage::requestOneClickForCurrentAgent()
// {
//     if (!controller_) {
//         return;
//     }

//     if ((controller_->isBspAgent() || controller_->isHelenAgent()) && currentScriptsRequireNoSystemBsp()) {
//         if (logView_) {
//             logView_->appendPlainText(
//                 QStringLiteral("使用无系统 BSP 参数启动；init_device 参数交给 watchdog 执行。"));
//         }
//         controller_->requestOneClickWithInitAndStartDeviceParams(
//             noSystemBspInitParams(), noSystemBspStartParams());
//         return;
//     }

//     controller_->requestOneClick();
// }

void ScriptExecutionPage::appendIntroLog()
{
    // 页面首次创建时输出模式说明，帮助用户快速理解当前用途。
    if (mode_ == Mode::Batch) {
        logView_->appendPlainText(QStringLiteral("这里用于批量执行脚本。"));
        logView_->appendPlainText(QStringLiteral("选择多个脚本后，会按顺序串行执行。"));
    } else {
        logView_->appendPlainText(QStringLiteral("这里用于单脚本调试。"));
        logView_->appendPlainText(QStringLiteral("先加载脚本文件，再观察脚本内容和运行日志。"));
    }
}

void ScriptExecutionPage::selectScripts()
{
    if (mode_ == Mode::Batch) {
        reloadConfiguredScripts();
        refreshExecutionButtons();
        return;
    }

    // 批量模式下选择多个脚本文件，并刷新列表显示。
    const auto files = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("选择脚本文件"),
        defaultScriptDirectory(),
        QStringLiteral("Python Scripts (*.py);;All Files (*)"),
        nullptr,
        QFileDialog::DontUseNativeDialog);
    if (files.isEmpty()) {
        return;
    }

    selectedScriptPaths_ = files;
    refreshSelectedScriptsView();
    refreshExecutionButtons();
    logView_->appendPlainText(QStringLiteral("已选择 %1 个脚本。").arg(selectedScriptPaths_.size()));
}

void ScriptExecutionPage::clearScripts()
{
    if (mode_ == Mode::Batch && selectedScriptsListWidget_) {
        selectedScriptsListWidget_->clearSelection();
        selectedScriptPaths_.clear();
        refreshSelectedScriptsView();
        refreshExecutionButtons();
        logView_->appendPlainText(QStringLiteral("已取消脚本选择。"));
        return;
    }

    selectedScriptPaths_.clear();
    refreshSelectedScriptsView();
    refreshExecutionButtons();
    logView_->appendPlainText(QStringLiteral("已清空脚本列表。"));
}

void ScriptExecutionPage::loadScript()
{
    // 调试模式下从文件系统选择单个脚本并载入预览。
    const auto filePath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("加载脚本"),
        defaultScriptDirectory(),
        QStringLiteral("Python 脚本 (*.py);;All Files (*)"),
        nullptr,
        QFileDialog::DontUseNativeDialog);
    if (filePath.isEmpty()) {
        return;
    }

    loadDebugScriptPreview(filePath);
}

void ScriptExecutionPage::clearLog()
{
    // 清空当前页面日志区，并保留一条“日志已清空”提示。
    if (!logView_) {
        return;
    }
    logView_->clear();
    logView_->appendPlainText(QStringLiteral("日志已清空。"));
}

void ScriptExecutionPage::runSelectedScripts()
{
    // 校验当前模式下的待执行脚本集合；必要时先触发一键启动再执行脚本。
    if (isRunning_) {
        logView_->appendPlainText(QStringLiteral("已有脚本在执行中，请先停止或等待当前执行完成。"));
        return;
    }

    QStringList selectedScripts;
    if (mode_ == Mode::Batch) {
        selectedScriptPaths_ = selectedBatchScriptPaths();
        selectedScripts = selectedScriptPaths_;
    } else if (!currentDebugScriptPath_.isEmpty()) {
        selectedScripts << currentDebugScriptPath_;
    }

    if (selectedScripts.isEmpty()) {
        logView_->appendPlainText(
            mode_ == Mode::Batch ? QStringLiteral("请先选择脚本。") : QStringLiteral("请先加载脚本。"));
        return;
    }

    if (!validateScriptsForCurrentDevice(selectedScripts)) {
        return;
    }

    if (!controller_->isTargetAgentSelected()) {
        logView_->appendPlainText(
            QStringLiteral("提示: 当前主 Agent 不是 glasses_bsp_node、glasses_nviz_node 或 helen_node，脚本中的设备动作可能无法执行。"));
    }

    currentEnvironment_ = buildScriptEnvironment();
    pendingScripts_ = selectedScripts;
    logView_->appendPlainText(QStringLiteral("准备执行 %1 个脚本。").arg(pendingScripts_.size()));
    if (needsBspBootstrap()) {
        logView_->appendPlainText(
            QStringLiteral("当前脚本需要设备先完成一键启动。请先在数据页面执行一键启动，设备就绪后再运行脚本。"));
        return;
    }

    isRunning_ = true;
    refreshExecutionButtons();
    executeNextPendingScript();
}

void ScriptExecutionPage::stopSelectedScripts()
{
    // 停止正在执行的脚本，或取消等待中的 bootstrap 队列。
    if (!isRunning_ && !bootstrapPending_) {
        logView_->appendPlainText(QStringLiteral("当前没有正在执行的脚本。"));
        return;
    }

    if (bootstrapPending_) {
        bootstrapPending_ = false;
        pendingScripts_.clear();
        refreshExecutionButtons();
        logView_->appendPlainText(
            QStringLiteral("已取消脚本队列；若一键启动已发出，它会继续自行收尾。"));
        return;
    }

    logView_->appendPlainText(QStringLiteral("正在停止当前脚本..."));
    pendingScripts_.clear();
    scriptExecutor_->stopScript();
}

void ScriptExecutionPage::executeNextPendingScript()
{
    // 取出队列头部脚本并交给 ScriptExecutor 执行，直到队列耗尽。
    if (pendingScripts_.isEmpty()) {
        isRunning_ = false;
        refreshExecutionButtons();
        logView_->appendPlainText(QStringLiteral("脚本执行队列已完成。"));
        return;
    }

    const auto scriptPath = pendingScripts_.front();
    const auto command =
        orchestrationBridge_.buildScriptCommandForPath(scriptPath, {}, currentEnvironment_);

    if (mode_ == Mode::Debug) {
        loadDebugScriptPreview(scriptPath);
    }

    logView_->appendPlainText(QStringLiteral("启动脚本: %1").arg(scriptPath));
    scriptExecutor_->executeCommand(command, scriptPath);
}

void ScriptExecutionPage::onScriptStarted(const QString& scriptPath)
{
    // 将脚本启动事件写入日志，便于用户确认当前执行项。
    clearWorkflowPanel();
    logView_->appendPlainText(QStringLiteral("[script] started: %1").arg(scriptPath));
}

void ScriptExecutionPage::onScriptLog(const QString& message)
{
    // 逐行追加脚本输出到日志视图。
    logView_->appendPlainText(message);
}

void ScriptExecutionPage::onScriptCompleted(bool success, const QString& error)
{
    // 消化脚本完成事件，推进队列或在失败/停止后恢复按钮状态。
    const QString finishedScript =
        pendingScripts_.isEmpty() ? QStringLiteral("<unknown>") : pendingScripts_.front();
    if (!pendingScripts_.isEmpty()) {
        pendingScripts_.pop_front();
    }

    if (success) {
        logView_->appendPlainText(QStringLiteral("[script] 完成: %1").arg(displayScriptPath(finishedScript)));
    } else {
        logView_->appendPlainText(
            QStringLiteral("[script] 结束: %1 | %2")
                .arg(displayScriptPath(finishedScript),
                     error.isEmpty() ? QStringLiteral("unknown error") : error));
    }

    if (!success && error == QStringLiteral("Script stopped by user")) {
        isRunning_ = false;
        refreshExecutionButtons();
        return;
    }

    if (pendingScripts_.isEmpty()) {
        isRunning_ = false;
        refreshExecutionButtons();
        logView_->appendPlainText(QStringLiteral("脚本执行队列已完成。"));
        return;
    }

    executeNextPendingScript();
}

}  // namespace recordlab::script
