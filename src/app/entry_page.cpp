#include "recordlab/app/entry_page.h"

#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

/*
 * entry_page.cpp
 *
 * 入口页是当前新工程里最轻的一层 UI。
 * 它不负责业务执行，只负责把“本地配置已加载成功，可以从哪些主 agent 进入”
 * 这件事明确展示给用户。
 *
 * 后续即使主界面变复杂，这一页的职责也应尽量保持简单。
 */
namespace recordlab::app {

EntryPage::EntryPage(const recordlab::core::AppContext& context, QWidget* parent)
    : QWidget(parent)
    , context_(context)
{
    // 构造入口页骨架：标题、说明文字和主 agent 按钮区都在这里一次性创建。
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(24, 24, 24, 24);
    rootLayout->setSpacing(14);
    rootLayout->setAlignment(Qt::AlignCenter);
    rootLayout->addStretch(1);

    auto* titleLabel = new QLabel(QStringLiteral("RecordLabC 控制中心"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(28);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setProperty("role", QStringLiteral("heroTitle"));
    titleLabel->setAlignment(Qt::AlignCenter);
    rootLayout->addWidget(titleLabel);

    auto* subtitleLabel = new QLabel(QStringLiteral("选择主 Agent"), this);
    subtitleLabel->setProperty("role", QStringLiteral("heroSubtitle"));
    subtitleLabel->setAlignment(Qt::AlignCenter);
    rootLayout->addWidget(subtitleLabel);

    // 入口页不再显示配置摘要，这里仅保留一个可复用占位标签给异常场景使用。
    summaryLabel_ = new QLabel(this);
    summaryLabel_->hide();

    buttonGrid_ = new QGridLayout();
    buttonGrid_->setHorizontalSpacing(40);
    buttonGrid_->setVerticalSpacing(40);
    rootLayout->addLayout(buttonGrid_);
    rootLayout->addStretch(1);

    rebuildButtons();
}

void EntryPage::rebuildButtons()
{
    if (!context_.isReady()) {
        // 如果上下文不可用，显示最小错误提示，避免界面完全空白。
        auto* errorLabel = new QLabel(QStringLiteral("当前无法读取本地配置。"), this);
        errorLabel->setWordWrap(true);
        buttonGrid_->addWidget(errorLabel, 0, 0);
        return;
    }

    // 将每个主 agent 生成为一个入口按钮，供用户进入对应工作区。
    int index = 0;
    for (const QString& agentName : context_.config().primaryAgents) {
        const int row = index / 3;
        const int column = index % 3;

        auto* button = new QPushButton(
            QStringLiteral("%1\n(%2)").arg(agentName.toUpper(), agentName),
            this);
        button->setMinimumSize(250, 120);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        QFont buttonFont = button->font();
        buttonFont.setPointSize(16);
        button->setFont(buttonFont);
        button->setStyleSheet(QStringLiteral(R"(
            QPushButton {
                background: #90EE90;
                border: 3px solid #006400;
                border-radius: 15px;
                color: #1b2514;
                text-align: center;
                padding: 14px;
            }
            QPushButton:hover {
                background: #7CFC00;
            }
            QPushButton:pressed {
                background: #82d46f;
            }
        )"));
        connect(button, &QPushButton::clicked, this, [this, agentName]() {
            emit agentSelected(agentName);
        });
        buttonGrid_->addWidget(button, row, column);
        ++index;
    }
}

}  // namespace recordlab::app
