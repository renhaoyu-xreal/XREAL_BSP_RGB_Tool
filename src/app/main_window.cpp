#include "recordlab/app/main_window.h"

#include <algorithm>

#include <QDir>
#include <QFrame>
#include <QGuiApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QRect>
#include <QScrollArea>
#include <QScreen>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QWidget>

#include <utility>

#include "recordlab/app/entry_page.h"
#include "recordlab/app/workspace_page.h"
#include "recordlab/core/compatibility_contract.h"

/*
 * main_window.cpp
 *
 * 主窗口当前是“壳层”而不是“业务层”。
 * 它只负责管理页面切换和启动消息，不应该提前背上 agent 管理、
 * 设备控制或数据接收等重逻辑。
 *
 * 这是为了避免重走旧工程里“主窗口越来越重”的老路。
 */
namespace recordlab::app {

namespace {

QScrollArea* makeResponsivePageContainer(QWidget* page, QWidget* parent)
{
    auto* scrollArea = new QScrollArea(parent);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    scrollArea->setMinimumSize(0, 0);

    page->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    scrollArea->setWidget(page);
    return scrollArea;
}

QString loadConfiguredVersion(const recordlab::core::AppContext& context)
{
    const QString fallback = QStringLiteral("v1.0.1");
    const auto version = context.recordLabConfig().version.trimmed();
    return version.isEmpty() ? fallback : version;
}

QString defaultPrimaryAgent()
{
    return QString::fromUtf8(recordlab::core::compat::kPrimaryBspAgent);
}

}  // namespace

MainWindow::MainWindow(recordlab::core::AppContext context, QWidget* parent)
    : QMainWindow(parent)
    , context_(std::move(context))
{
    setObjectName(QStringLiteral("RecordLabMainWindow"));
    setWindowTitle(QStringLiteral("XREAL BSP RGB Tool"));
    setMinimumSize(360, 260);
    applyResponsiveWindowSize();
    setStyleSheet(QStringLiteral(R"(
        QMainWindow#RecordLabMainWindow {
            background: #f4f1ea;
        }
        QWidget {
            color: #2f2a22;
            font-size: 13px;
        }
        QLabel[role="heroTitle"] {
            font-size: 30px;
            font-weight: 700;
            color: #1f2d26;
        }
        QLabel[role="heroSubtitle"] {
            color: #6a6258;
            font-size: 14px;
        }
        QGroupBox {
            background: #fbfaf7;
            border: 1px solid #d8cfbf;
            border-radius: 8px;
            margin-top: 12px;
            padding-top: 10px;
            font-weight: 600;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 6px;
            color: #5e5446;
        }
        QPushButton {
            background: #efe7d8;
            border: 1px solid #b9ab8e;
            border-radius: 6px;
            padding: 8px 14px;
            min-height: 34px;
        }
        QPushButton:hover {
            background: #f5edde;
        }
        QPushButton:pressed {
            background: #e8dfce;
        }
        QTabWidget::pane {
            border: 1px solid #d8cfbf;
            background: #fbfaf7;
            border-radius: 8px;
            top: -1px;
        }
        QTabBar::tab {
            background: #ece3d3;
            border: 1px solid #d8cfbf;
            border-bottom: none;
            border-top-left-radius: 6px;
            border-top-right-radius: 6px;
            padding: 8px 18px;
            margin-right: 3px;
            font-size: 10.5pt;
        }
        QTabBar::tab:selected {
            background: #fbfaf7;
            border-bottom: 2px solid #2f6b53;
            color: #1f1a15;
        }
        QPlainTextEdit, QTextEdit, QListWidget, QTableWidget, QLineEdit, QComboBox {
            background: #fffdf8;
            border: 1px solid #d6ccb8;
            border-radius: 6px;
            selection-background-color: #d9e7ff;
        }
        QHeaderView::section {
            background: #ece3d3;
            border: none;
            border-right: 1px solid #dccfb7;
            border-bottom: 1px solid #dccfb7;
            padding: 6px;
            font-weight: 600;
        }
        QStatusBar {
            background: #e8e0d0;
            border-top: 1px solid #d4c7af;
        }
    )"));

    stack_ = new QStackedWidget(this);
    setCentralWidget(stack_);

    entryPage_ = new EntryPage(context_);
    workspacePage_ = new WorkspacePage(context_);
    entryPageContainer_ = makeResponsivePageContainer(entryPage_, this);
    workspacePageContainer_ = makeResponsivePageContainer(workspacePage_, this);
    stack_->addWidget(entryPageContainer_);
    stack_->addWidget(workspacePageContainer_);
    stack_->setCurrentWidget(workspacePageContainer_);

    connect(entryPage_, &EntryPage::agentSelected, this, &MainWindow::onAgentSelected);

    versionLabel_ = new QLabel(
        QStringLiteral("版本：%1").arg(loadConfiguredVersion(context_)), this);
    versionLabel_->setObjectName(QStringLiteral("RecordLabVersionLabel"));
    versionLabel_->setStyleSheet(QStringLiteral(
        "QLabel#RecordLabVersionLabel { color: #5f5649; padding: 0 8px; font-weight: 600; }"));
    statusBar()->addPermanentWidget(versionLabel_);
    statusBar()->showMessage(QStringLiteral("当前运行时目录：XREAL_BSP_RGB_Tool"));
    showStartupMessages();
    onAgentSelected(defaultPrimaryAgent());
}

void MainWindow::onAgentSelected(const QString& agentName)
{
    // 入口页选中主 agent 后，立即切换到工作区并同步状态栏提示。
    workspacePage_->activateAgent(agentName);
    stack_->setCurrentWidget(workspacePageContainer_);
    workspacePageContainer_->update();
    workspacePage_->update();
    QTimer::singleShot(0, this, [this]() {
        workspacePageContainer_->update();
        workspacePage_->update();
    });
    statusBar()->showMessage(QStringLiteral("已选择主 Agent：%1").arg(agentName));
}

void MainWindow::showStartupMessages()
{
    // 启动错误属于阻断条件，必须立刻弹窗说明，避免用户进入半可用界面。
    if (!context_.startupError().isEmpty()) {
        QMessageBox::critical(this, QStringLiteral("Startup Error"), context_.startupError());
        return;
    }

    // 普通预检告警只写入状态栏，减少每次启动时的交互打断。
    if (!context_.startupWarnings().isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("启动预检存在告警，可在 doctor 或工作区信息区查看；启动时不再弹窗提示。"));
    }

    const QString updateInfo = context_.recordLabConfig().updateInfo.trimmed();
    if (!updateInfo.isEmpty()) {
        const QString title = QStringLiteral("更新信息");
        const QString message =
            QStringLiteral("%1\n\n%2").arg(loadConfiguredVersion(context_), updateInfo);
        QTimer::singleShot(0, this, [this, title, message]() {
            QMessageBox::information(this, title, message);
        });
    }
}

void MainWindow::applyResponsiveWindowSize()
{
    const QSize preferredSize(1600, 980);
    const QSize fallbackSize(1280, 800);
    const QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) {
        resize(fallbackSize);
        return;
    }

    const QRect availableGeometry = screen->availableGeometry();
    const QSize availableSize = availableGeometry.size();
    const QSize targetSize(
        std::min(preferredSize.width(), qMax(360, availableSize.width() - 80)),
        std::min(preferredSize.height(), qMax(260, availableSize.height() - 80)));

    resize(targetSize);
    move(availableGeometry.center() - rect().center());
}

}  // namespace recordlab::app
