#include "recordlab/core/app_context.h"

#include <QDir>
#include <QFileInfo>

#include "recordlab/bsp/native_glasses_adapter.h"

/*
 * app_context.cpp
 *
 * 这个文件对应新工程的启动准备阶段。
 *
 * 它要解决的问题是：
 * 1. 程序启动时优先去哪里找本地运行时资料；
 * 2. 本地配置是否可用；
 * 3. 哪些问题是 warning，哪些问题会阻断启动；
 * 4. 旧工程是否还能作为迁移参考存在。
 *
 * 当前把这些判断集中放到 AppContext::create 中，
 * 可以避免 UI 或业务层在启动时各自重复判断。
 */
namespace recordlab::core {

AppContext AppContext::create(const QString& appRootPath)
{
    // 统一完成启动阶段的路径发现、配置加载和环境预检，向上层输出一个稳定结果。
    AppContext context;

    // 第一步：发现当前工程和旧工程的关键资料路径。
    context.paths_ = LegacyPaths::discover(appRootPath);

    // 第二步：先验证新工程自己的配置是否存在。
    if (!context.paths_.appConfigExists) {
        context.startupError_ = QStringLiteral("Local config not found: %1").arg(context.paths_.appConfigPath);
        return context;
    }

    // 第三步：加载新工程本地配置。
    const auto configResult = LegacyConfigLoader::load(context.paths_.appConfigPath);
    if (!configResult.success) {
        context.startupError_ = configResult.error;
        return context;
    }

    const QString recordLabConfigPath =
        QDir(context.paths_.appRoot).filePath(QStringLiteral("config/recordLabConfig.json"));
    const auto recordLabConfigResult =
        LegacyConfigLoader::loadRecordLabConfig(recordLabConfigPath);
    if (!recordLabConfigResult.success) {
        context.startupError_ = recordLabConfigResult.error;
        return context;
    }

    context.config_ = configResult.config;
    context.recordLabConfig_ = recordLabConfigResult.config;
    context.ready_ = true;

    // 第四步：收集非阻塞性告警。
    // 这些问题不会阻止 UI 骨架启动，但会提示后续某些独立运行能力还不完整。
    if (!context.paths_.appGuideExists) {
        context.startupWarnings_.push_back(QStringLiteral("Local BSP guide missing: %1").arg(context.paths_.appGuidePath));
    }
    if (!context.paths_.appWheelExists) {
        context.startupWarnings_.push_back(QStringLiteral("Local XREAL wheel missing: %1").arg(context.paths_.appWheelPath));
    }
    if (!context.paths_.appPyiExists) {
        context.startupWarnings_.push_back(QStringLiteral("Local XrGlasses.pyi missing: %1").arg(context.paths_.appPyiPath));
    }
    if (!context.paths_.appEchoCppExists) {
        context.startupWarnings_.push_back(QStringLiteral("Local C++ echo_message_system missing: %1").arg(context.paths_.appEchoCppPath));
    }

    // 第五步：补充 BSP 真机链路的预检结果。
    // 这类问题不应该只藏在 BSP 页面里，因为用户一启动主程序就需要知道：
    // “当前环境是不是连真机 createGlasses 的前置条件都不满足”。
    const recordlab::bsp::NativeGlassesAdapter bspAdapter(context.paths_.appRoot);
    const auto preflight = bspAdapter.preflight();
    if (!preflight.blockers.isEmpty()) {
        context.startupWarnings_.push_back(
            QStringLiteral("BSP 预检阻塞：%1").arg(preflight.blockers.join(QStringLiteral(" | "))));
    }

    return context;
}

bool AppContext::isReady() const
{
    // 表示当前上下文是否已经满足进入主界面的最低运行条件。
    return ready_;
}

const LegacyPaths& AppContext::paths() const
{
    // 返回统一解析后的路径集合，供 UI 与业务层共享。
    return paths_;
}

const AppConfig& AppContext::config() const
{
    // 返回已经过兼容层转换的本地配置对象。
    return config_;
}

const RecordLabConfig& AppContext::recordLabConfig() const
{
    return recordLabConfig_;
}

const QStringList& AppContext::startupWarnings() const
{
    // 暴露所有非阻断性预检告警，供状态栏或 doctor 页面集中展示。
    return startupWarnings_;
}

const QString& AppContext::startupError() const
{
    // 暴露最终的阻断性启动错误文本，供主窗口直接显示。
    return startupError_;
}

}  // namespace recordlab::core
