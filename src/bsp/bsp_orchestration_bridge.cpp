#include "recordlab/bsp/bsp_orchestration_bridge.h"

#include "recordlab/bsp/bsp_asset_resolver.h"
#include "recordlab/core/compatibility_contract.h"

/*
 * bsp_orchestration_bridge.cpp
 *
 * 这层本质上是在说：
 * “虽然 Python 脚本不是核心运行时，但 BSP 工作流依然需要一条稳定的脚本编排入口。”
 *
 * 当前通过这个桥接层，把：
 * - 支持哪些 BSP 脚本
 * - 它们在本地解析到哪里
 * - 执行时需要哪些 PythonPath
 *
 * 统一封装起来，便于后续真正接入执行器。
 */
namespace recordlab::bsp {

BspOrchestrationBridge::BspOrchestrationBridge(const recordlab::core::AppContext& context)
    : context_(context)
    , pythonBridge_(context)
{
}

bool BspOrchestrationBridge::isAvailable() const
{
    // 底层 Python 桥接可用时，工作流才能安全地发起 BSP 脚本执行。
    return pythonBridge_.isAvailable();
}

QStringList BspOrchestrationBridge::supportedScripts() const
{
    // 返回兼容契约里声明支持的 BSP 脚本名称集合。
    return recordlab::core::compat::bspScriptNames();
}

QString BspOrchestrationBridge::resolvedScriptPath(const QString& relativeScriptPath) const
{
    // 按统一资产规则解析脚本真实路径，避免调用方各自拼接目录。
    const auto assets = BspAssetResolver::resolve(context_);
    return assets.resolveScriptPath(relativeScriptPath);
}

QStringList BspOrchestrationBridge::pythonPathEntries() const
{
    // 暴露脚本执行所需的 PythonPath，便于调试和执行器复用。
    return pythonBridge_.pythonPathEntries();
}

recordlab::script::ScriptCommand BspOrchestrationBridge::buildScriptCommand(
    const QString& relativeScriptPath,
    const QStringList& extraArgs,
    const QMap<QString, QString>& extraEnvironment) const
{
    // 根据脚本名、额外参数和环境变量生成标准化执行命令。
    return pythonBridge_.buildCommand(relativeScriptPath, extraArgs, extraEnvironment);
}

recordlab::script::ScriptCommand BspOrchestrationBridge::buildScriptCommandForPath(
    const QString& scriptPath,
    const QStringList& extraArgs,
    const QMap<QString, QString>& extraEnvironment) const
{
    // 当脚本绝对路径已知时，直接构造执行命令而不再做路径解析。
    return pythonBridge_.buildCommandForScriptPath(scriptPath, extraArgs, extraEnvironment);
}

}  // namespace recordlab::bsp
