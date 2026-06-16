#include <QApplication>
#include <QDir>
#include <QMetaType>

#include "recordlab/app/main_window.h"
#include "recordlab/core/app_context.h"
#include "recordlab/core/logger.h"
#include "recordlab/core/qt_json_compat.h"

/*
 * 新工程入口
 *
 * 当前 main 的职责尽量保持简单：
 * 1. 初始化 Qt 应用对象。
 * 2. 安装统一日志处理器。
 * 3. 构建启动上下文。
 * 4. 打开主窗口。
 *
 * 这样后续即使接入 IPC、设备层或更复杂的启动流程，main 也不会迅速膨胀。
 */
int main(int argc, char* argv[])
{
    // 初始化 Qt 应用、日志和运行时根目录，然后把上下文交给主窗口。
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("XREAL_BSP_RGB_Tool"));
    application.setApplicationVersion(QStringLiteral("v1.0.1"));
    application.setOrganizationName(QStringLiteral("XREAL"));

    // 跨线程 signal/slot 如果传递 nlohmann::json，需要先把 metatype 注册给 Qt。
    qRegisterMetaType<nlohmann::json>("nlohmann::json");

    // 运行时优先使用外部传入的 RECORDLABC_ROOT。
    // 这样把整个 RecordLabC 目录移动到新位置后，不需要重新编译也能继续运行。
    QString appRoot = qEnvironmentVariable("RECORDLABC_ROOT");
    if (appRoot.isEmpty()) {
        QDir appDir(QCoreApplication::applicationDirPath());
        if (appDir.dirName() == QStringLiteral("build")) {
            appDir.cdUp();
        }
        appRoot = appDir.absolutePath();
    }

    recordlab::core::installTerminalLogTee(QDir::cleanPath(appRoot));
    recordlab::core::installQtLogger();

    const auto context = recordlab::core::AppContext::create(QDir::cleanPath(appRoot));
    recordlab::app::MainWindow window(context);
    window.showMaximized();

    return QApplication::exec();
}
