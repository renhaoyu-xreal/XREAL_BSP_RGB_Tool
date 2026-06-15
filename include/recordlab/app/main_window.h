#pragma once

#include <QMainWindow>

#include "recordlab/core/app_context.h"

class QStackedWidget;
class QWidget;
class QLabel;

namespace recordlab::app {

class EntryPage;
class WorkspacePage;

/*
 * 主窗口
 *
 * 当前主窗口是一个非常明确的“壳”：
 * 1. 持有启动上下文。
 * 2. 管理入口页和工作区页的切换。
 * 3. 在启动时统一显示错误和警告。
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(recordlab::core::AppContext context, QWidget* parent = nullptr);

private slots:
    // 从入口页收到主 agent 选择结果后切换到工作区。
    void onAgentSelected(const QString& agentName);

private:
    recordlab::core::AppContext context_;
    QStackedWidget* stack_ = nullptr;
    EntryPage* entryPage_ = nullptr;
    WorkspacePage* workspacePage_ = nullptr;
    QWidget* entryPageContainer_ = nullptr;
    QWidget* workspacePageContainer_ = nullptr;
    QLabel* versionLabel_ = nullptr;

    // 统一处理启动阶段的错误提示；普通 warning 只留状态栏提示，不再弹窗。
    void showStartupMessages();
    void applyResponsiveWindowSize();
};

}  // namespace recordlab::app
