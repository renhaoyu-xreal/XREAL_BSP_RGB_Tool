#pragma once

#include <QVariantMap>
#include <QWidget>

#include "recordlab/core/app_context.h"

class QLabel;
class QPushButton;
class QTabWidget;

namespace recordlab::bsp {
class BspPage;
class BspRgbPage;
}

namespace recordlab::workflow {
class WorkflowController;
}

namespace recordlab::agent {
class AgentManagementPage;
}

namespace recordlab::script {
class ScriptExecutionPage;
}

namespace recordlab::backend {
class AgentManagerProcess;
class DataReceiverManager;
}

namespace recordlab::app {

/*
 * 工作区页面
 *
 * 这个页面对应旧版 Control Center 中“选完主 agent 后进入主工作区”的概念。
 * 当前阶段它主要负责：
 * 1. 展示当前激活的主 agent。
 * 2. 承载多个功能 tab。
 * 3. 将 BSP 相关能力优先放到真正可扩展的位置上。
 */
class WorkspacePage : public QWidget {
    Q_OBJECT

public:
    explicit WorkspacePage(const recordlab::core::AppContext& context, QWidget* parent = nullptr);
    ~WorkspacePage() override;

    // 主窗口选中某个主 agent 后，会调用此函数刷新工作区状态。
    void activateAgent(const QString& agentName);

signals:
    void backRequested();

private slots:
    void dispatchWorkflowAction(const QString& actionName, const QVariantMap& payload);

private:
    const recordlab::core::AppContext& context_;
    QString activeAgent_;
    QLabel* timerValueLabel_ = nullptr;
    QLabel* delayValueLabel_ = nullptr;
    QLabel* watchdogValueLabel_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    recordlab::workflow::WorkflowController* controller_ = nullptr;
    recordlab::backend::AgentManagerProcess* agentManagerProcess_ = nullptr;
    recordlab::backend::DataReceiverManager* dataReceiver_ = nullptr;
    recordlab::script::ScriptExecutionPage* batchScriptPage_ = nullptr;
    recordlab::bsp::BspPage* bspPage_ = nullptr;
    recordlab::bsp::BspRgbPage* bspRgbPage_ = nullptr;
    recordlab::agent::AgentManagementPage* agentManagementPage_ = nullptr;
    recordlab::script::ScriptExecutionPage* scriptDebugPage_ = nullptr;

    // 刷新顶部头信息。
    void updateHeader();
    void updateWatchdogHeader();
    void updateBspRgbTab();
    void forceVisualRefresh();
};

}  // namespace recordlab::app
