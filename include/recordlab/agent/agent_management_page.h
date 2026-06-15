#pragma once

#include <QWidget>

#include "recordlab/core/app_context.h"

class QLabel;
class QPlainTextEdit;
class QTableWidget;

namespace recordlab::workflow {
class WorkflowController;
}

namespace recordlab::agent {

/*
 * Agent 管理页
 *
 * 这个页面直接对照旧版指南里的 Tab3：
 * - 负责承接“先连接 glasses_bsp_node/glasses_bsp_subnode”这一步；
 * - 负责把目标主 agent、子节点脚本、端口、初始化参数展示清楚；
 * - 后续原生 agent manager 接入后，这里会成为真实连接状态页面。
 *
 * 当前它仍是骨架，但已经不再是纯占位文本。
 */
class AgentManagementPage : public QWidget {
    Q_OBJECT

public:
    explicit AgentManagementPage(
        const recordlab::core::AppContext& context,
        recordlab::workflow::WorkflowController* controller,
        QWidget* parent = nullptr);

private:
    const recordlab::core::AppContext& context_;
    recordlab::workflow::WorkflowController* controller_ = nullptr;
    QLabel* activeAgentValueLabel_ = nullptr;
    QLabel* targetAgentValueLabel_ = nullptr;
    QLabel* stateValueLabel_ = nullptr;
    QTableWidget* agentTable_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;

    static QLabel* makeSelectableLabel(const QString& text, QWidget* parent = nullptr);
    void appendIntroLog();
    void refreshAgentTable(const QString& activeAgent);
};

}  // namespace recordlab::agent
