#include "recordlab/agent/agent_management_page.h"

#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "recordlab/workflow/workflow_controller.h"
#include "recordlab/core/compatibility_contract.h"

/*
 * agent_management_page.cpp
 *
 * 旧版指南要求用户先在 Tab3 完成连接，再去 Tab2 执行 start_device。
 * 当前这里已经不只是结构页，而是开始承接真实的 Connect / init_device / 急停动作。
 */
namespace recordlab::agent {

namespace {

QString stateToString(recordlab::workflow::WorkflowController::State state)
{
    // 将工作流状态枚举转换为页面上直接展示的中文文案。
    using State = recordlab::workflow::WorkflowController::State;
    switch (state) {
    case State::Idle:
        return QStringLiteral("空闲");
    case State::CheckingConnection:
        return QStringLiteral("检查连接前提");
    case State::AgentConnecting:
        return QStringLiteral("连接 Agent 中");
    case State::WaitingWatchdog:
        return QStringLiteral("等待 Watchdog");
    case State::DeviceInitializing:
        return QStringLiteral("初始化设备中");
    case State::WaitingStartDeviceResponse:
        return QStringLiteral("等待 Start Device 响应");
    case State::DeviceReady:
        return QStringLiteral("设备就绪");
    case State::RecordingRequested:
        return QStringLiteral("录制请求已发出");
    case State::Failed:
        return QStringLiteral("失败");
    case State::EmergencyStop:
        return QStringLiteral("紧急停止");
    }

    return QStringLiteral("未知");
}

}  // namespace

AgentManagementPage::AgentManagementPage(
    const recordlab::core::AppContext& context,
    recordlab::workflow::WorkflowController* controller,
    QWidget* parent)
    : QWidget(parent)
    , context_(context)
    , controller_(controller)
{
    // 构造 Tab3 页面骨架：状态卡片、Agent 表格、操作按钮和日志区。
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(10);

    const auto definition = context_.config().agent(QString::fromUtf8(recordlab::core::compat::kPrimaryBspAgent));

    auto* titleLabel = new QLabel(QStringLiteral("Agent 状态检查"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(12);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    rootLayout->addWidget(titleLabel);

    auto* infoFrame = new QFrame(this);
    infoFrame->setStyleSheet(QStringLiteral(
        "QFrame { background-color: #fbfaf7; border: 1px solid #d8cfbf; border-radius: 6px; }"));
    auto* summaryRow = new QHBoxLayout(infoFrame);
    summaryRow->setContentsMargins(8, 6, 8, 6);
    summaryRow->setSpacing(10);
    activeAgentValueLabel_ = makeSelectableLabel(QStringLiteral("<none>"), this);
    targetAgentValueLabel_ = makeSelectableLabel(QString::fromUtf8(recordlab::core::compat::kPrimaryBspAgent), this);
    stateValueLabel_ = makeSelectableLabel(stateToString(controller_->state()), this);
    const auto addInfoCard = [this, summaryRow](const QString& title, QLabel* valueLabel) {
        auto* card = new QFrame(this);
        card->setStyleSheet(QStringLiteral(
            "QFrame { background-color: #fffdf7; border: 1px solid #d8cfbf; border-radius: 4px; }"));
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(8, 6, 8, 6);
        cardLayout->setSpacing(2);
        auto* titleValue = new QLabel(title, card);
        titleValue->setStyleSheet(QStringLiteral("QLabel { color: #6a655c; font-size: 11px; }"));
        valueLabel->setParent(card);
        valueLabel->setWordWrap(false);
        valueLabel->setStyleSheet(QStringLiteral(
            "QLabel { background: transparent; border: none; color: #222; font-weight: 600; }"));
        cardLayout->addWidget(titleValue);
        cardLayout->addWidget(valueLabel);
        summaryRow->addWidget(card, 1);
    };
    addInfoCard(QStringLiteral("当前主 Agent"), activeAgentValueLabel_);
    addInfoCard(QStringLiteral("目标 Agent"), targetAgentValueLabel_);
    addInfoCard(QStringLiteral("流程状态"), stateValueLabel_);
    rootLayout->addWidget(infoFrame);

    auto* hintLabel = new QLabel(
        QStringLiteral("子节点: %1 | Host: %2 | 端口: goal=%3, feedback=%4")
            .arg(QFileInfo(definition.subnodePath).fileName(),
                 definition.subnodeHost)
            .arg(definition.goalPort)
            .arg(definition.feedbackPort),
        this);
    hintLabel->setStyleSheet(QStringLiteral("QLabel { color: #6a655c; padding-left: 2px; }"));
    hintLabel->setToolTip(definition.subnodePath);
    rootLayout->addWidget(hintLabel);

    agentTable_ = new QTableWidget(this);
    agentTable_->setColumnCount(4);
    agentTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Agent"), QStringLiteral("状态"), QStringLiteral("Connect"), QStringLiteral("Check")});
    agentTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    agentTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    agentTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    agentTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    agentTable_->verticalHeader()->setVisible(false);
    agentTable_->verticalHeader()->setDefaultSectionSize(42);
    agentTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    agentTable_->setSelectionMode(QAbstractItemView::NoSelection);
    agentTable_->setStyleSheet(QStringLiteral(R"(
        QTableWidget {
            background-color: #FFFFE0;
            border: 1px solid #888;
        }
        QTableWidget::item {
            padding: 5px;
        }
    )"));
    refreshAgentTable(QString());
    rootLayout->addWidget(agentTable_, 2);

    auto* actionsLayout = new QHBoxLayout();
    actionsLayout->setSpacing(8);

    const auto addButton = [this, actionsLayout](const QString& text, const char* slot, const QString& style) {
        auto* button = new QPushButton(text, this);
        button->setMinimumSize(138, 40);
        button->setStyleSheet(style);
        connect(button, SIGNAL(clicked()), controller_, slot);
        actionsLayout->addWidget(button, 0);
    };

    addButton(QStringLiteral("检查连接状态"), SLOT(requestConnect()),
              QStringLiteral("background-color: #87CEEB; border: 2px solid #4682B4; padding: 8px; font-weight: 600;"));
    addButton(QStringLiteral("检查主 Agent"), SLOT(requestCheck()),
              QStringLiteral("background-color: #DDA0DD; border: 2px solid #8B008B; padding: 8px; font-weight: 600;"));
    addButton(QStringLiteral("Init Device"), SLOT(requestInitDevice()),
              QStringLiteral("background-color: #90EE90; border: 2px solid #006400; padding: 8px; font-weight: 600;"));
    addButton(QStringLiteral("重置流程"), SLOT(requestResetWorkflow()),
              QStringLiteral("background-color: #D3D3D3; border: 1px solid #888; padding: 8px; font-weight: 600;"));
    addButton(QStringLiteral("Emergency Stop"), SLOT(requestEmergencyStop()),
              QStringLiteral("background-color: #FFB6C1; border: 2px solid #8B0000; padding: 8px; font-weight: 600;"));
    actionsLayout->addStretch(1);
    rootLayout->addLayout(actionsLayout);

    rootLayout->addWidget(new QLabel(QStringLiteral("操作日志:"), this));
    logView_ = new QPlainTextEdit(this);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(2000);
    logView_->setStyleSheet(QStringLiteral(R"(
        QPlainTextEdit {
            background-color: #FFFFE0;
            border: 1px solid #888;
            padding: 10px;
        }
    )"));
    rootLayout->addWidget(logView_, 1);

    connect(controller_, &recordlab::workflow::WorkflowController::activeAgentChanged, this,
        [this](const QString& agentName) {
            activeAgentValueLabel_->setText(agentName.isEmpty() ? QStringLiteral("<none>") : agentName);
            targetAgentValueLabel_->setText(agentName.isEmpty() ? QStringLiteral("<none>") : agentName);
            refreshAgentTable(agentName);
        });
    connect(controller_, &recordlab::workflow::WorkflowController::stateChanged, this,
        [this](recordlab::workflow::WorkflowController::State state) {
            stateValueLabel_->setText(stateToString(state));
        });
    connect(controller_, &recordlab::workflow::WorkflowController::logAppended, this,
        [this](const QString& message) {
            logView_->appendPlainText(message);
        });

    appendIntroLog();
}

QLabel* AgentManagementPage::makeSelectableLabel(const QString& text, QWidget* parent)
{
    // 创建一个支持鼠标复制的标签，方便用户复制 agent 名称和状态信息。
    auto* label = new QLabel(text, parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

void AgentManagementPage::appendIntroLog()
{
    // 输出页面首次进入时的简要说明，让用户知道当前页负责哪一段流程。
    logView_->appendPlainText(QStringLiteral("先在这里完成连接，再到数据+命令页启动设备。"));
    logView_->appendPlainText(QStringLiteral("当前已接入真实的 Connect / init_device / 急停 动作出口。"));
    logView_->appendPlainText(QStringLiteral("表格会高亮当前主 Agent，对应操作也优先作用在当前主 Agent 上。"));
}

void AgentManagementPage::refreshAgentTable(const QString& activeAgent)
{
    // 按当前主 agent 重建表格状态和按钮可用性，避免误操作到非主 agent。
    if (!agentTable_) {
        return;
    }

    const auto names = context_.config().availableAgents();
    agentTable_->setRowCount(names.size());
    int row = 0;
    for (const auto& agentName : names) {
        const auto definition = context_.config().agent(agentName);
        const bool isActive = agentName == activeAgent;
        QString status = QStringLiteral("未选中");
        if (isActive) {
            status = stateToString(controller_->state());
        } else if (context_.config().primaryAgents.contains(agentName)) {
            status = QStringLiteral("可切换主 Agent");
        } else {
            status = definition.subnodePath.isEmpty() ? QStringLiteral("远程 Agent") : QStringLiteral("本地 Agent");
        }

        auto* nameItem = new QTableWidgetItem(agentName);
        if (isActive) {
            nameItem->setText(QStringLiteral("%1  [当前主]").arg(agentName));
        }
        agentTable_->setItem(row, 0, nameItem);
        agentTable_->setItem(row, 1, new QTableWidgetItem(status));

        auto* connectButton = new QPushButton(QStringLiteral("Connect"), agentTable_);
        connectButton->setMinimumSize(88, 30);
        connectButton->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #90EE90; border: 1px solid #5f9b62; }"
            "QPushButton:disabled { background-color: #dddddd; color: #666; border-color: #aaa; }"));
        connectButton->setEnabled(isActive);
        connect(connectButton, &QPushButton::clicked, this, [this, isActive, agentName]() {
            if (!isActive) {
                logView_->appendPlainText(QStringLiteral("%1 不是当前主 Agent，请先从入口页切换。").arg(agentName));
                return;
            }
            controller_->requestConnect();
        });
        agentTable_->setCellWidget(row, 2, connectButton);

        auto* checkButton = new QPushButton(QStringLiteral("Check"), agentTable_);
        checkButton->setMinimumSize(88, 30);
        checkButton->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #d8e7fb; border: 1px solid #7ea2d6; }"
            "QPushButton:disabled { background-color: #dddddd; color: #666; border-color: #aaa; }"));
        checkButton->setEnabled(isActive);
        connect(checkButton, &QPushButton::clicked, this, [this, isActive, agentName]() {
            if (!isActive) {
                logView_->appendPlainText(QStringLiteral("%1 不是当前主 Agent，请先从入口页切换。").arg(agentName));
                return;
            }
            controller_->requestCheck();
        });
        agentTable_->setCellWidget(row, 3, checkButton);

        const QColor highlight = isActive ? QColor(QStringLiteral("#e5f1d0"))
                                                          : QColor(QStringLiteral("#fffbe9"));
        for (int column = 0; column < agentTable_->columnCount(); ++column) {
            if (auto* item = agentTable_->item(row, column)) {
                item->setBackground(highlight);
            }
        }
        ++row;
    }
}

}  // namespace recordlab::agent
