#pragma once

#include <QWidget>

#include "recordlab/core/app_context.h"

class QGridLayout;
class QLabel;

namespace recordlab::app {

/*
 * 入口页
 *
 * 当前职责很明确：
 * 1. 展示旧配置是否成功加载。
 * 2. 按旧版 primary_agents 生成入口按钮。
 * 3. 把“选择哪个主 agent”这件事交给主窗口继续处理。
 */
class EntryPage : public QWidget {
    Q_OBJECT

public:
    explicit EntryPage(const recordlab::core::AppContext& context, QWidget* parent = nullptr);

signals:
    // 当用户选择一个主 agent 时，通知主窗口切换到工作区。
    void agentSelected(const QString& agentName);

private:
    const recordlab::core::AppContext& context_;
    QLabel* summaryLabel_ = nullptr;
    QGridLayout* buttonGrid_ = nullptr;

    // 根据当前上下文重建入口按钮区域。
    void rebuildButtons();
};

}  // namespace recordlab::app
