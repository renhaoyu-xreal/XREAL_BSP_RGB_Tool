#include "recordlab/script/python_script_bridge.h"

#include <QDir>

#include "recordlab/bsp/bsp_asset_resolver.h"

/*
 * python_script_bridge.cpp
 *
 * 这个文件体现的是本次迁移的一个关键判断：
 * Python 脚本保留，但降级为编排层。
 *
 * 也就是说，脚本以后仍然可以存在，但它们不再承担高频核心控制和数据职责。
 * 当前先把“脚本命令如何被稳定构造出来”这一层固定下来，
 * 后面再继续补进程执行、输出回传和状态监控。
 */
namespace recordlab::script {

namespace {

bool legacyFallbackEnabled()
{
    // 只有在显式开启开关时才允许脚本路径回退到旧工程。
    const auto value = qEnvironmentVariable("RECORDLABC_ENABLE_LEGACY_FALLBACK").trimmed().toLower();
    return value == QStringLiteral("1")
        || value == QStringLiteral("true")
        || value == QStringLiteral("yes")
        || value == QStringLiteral("on");
}

}  // namespace

PythonScriptBridge::PythonScriptBridge(const recordlab::core::AppContext& context, QObject* parent)
    : QObject(parent)
    , context_(context)
{
    // 允许通过环境变量覆盖 Python 解释器，方便不同机器复用各自环境。
    // 允许用户通过环境变量显式覆盖 Python 解释器。
    // 这样后面即使存在多个 Python 环境，也不需要改代码。
    const auto fromEnv = qEnvironmentVariable("RECORDLAB_PYTHON_EXECUTABLE");
    pythonExecutable_ = fromEnv.isEmpty() ? QStringLiteral("python3") : fromEnv;
}

bool PythonScriptBridge::isAvailable() const
{
    // 判断脚本桥的最低可用条件：本地脚本存在，或显式允许的旧工程脚本存在。
    // 默认要求当前工程自己的脚本目录可用。
    // 只有显式开启 legacy fallback 时，才允许退回旧工程脚本目录。
    const auto assets = recordlab::bsp::BspAssetResolver::resolve(context_);
    return assets.vendoredScriptsAvailable || (legacyFallbackEnabled() && assets.legacyScriptsAvailable);
}

QString PythonScriptBridge::pythonExecutable() const
{
    // 返回当前桥接层选定的 Python 解释器名称或绝对路径。
    return pythonExecutable_;
}

QStringList PythonScriptBridge::pythonPathEntries() const
{
    // 统一通过资产解析器组装 PYTHONPATH，避免调用方手写路径拼接。
    // 当前统一由资产解析器决定 PythonPath：
    // 默认只用本地副本，必要时才显式开启旧工程回退。
    return recordlab::bsp::BspAssetResolver::resolve(context_).pythonPathEntries();
}

ScriptCommand PythonScriptBridge::buildCommand(
    const QString& relativeScriptPath,
    const QStringList& extraArgs,
    const QMap<QString, QString>& extraEnvironment) const
{
    // 先将脚本相对路径解析成实际路径，再交给统一命令构造入口。
    const auto assets = recordlab::bsp::BspAssetResolver::resolve(context_);
    return buildCommandForScriptPath(
        assets.resolveScriptPath(relativeScriptPath),
        extraArgs,
        extraEnvironment);
}

ScriptCommand PythonScriptBridge::buildCommandForScriptPath(
    const QString& scriptPath,
    const QStringList& extraArgs,
    const QMap<QString, QString>& extraEnvironment) const
{
    // 构造脚本兼容运行时命令，统一补齐项目根目录、配置路径和环境变量。
    ScriptCommand command;
    command.program = pythonExecutable_;

    const auto appRoot = context_.paths().appRoot;
    const auto runtimeScript =
        QDir(appRoot).filePath(QStringLiteral("scripts/runtime/run_recordlab_script.py"));

    // 当前统一通过本地兼容运行时执行脚本，而不是直接把业务脚本喂给 python3。
    // 这样脚本执行时需要的全局对象、topic 状态和 agent 代理都能在一个地方补齐。
    command.arguments << runtimeScript
                      << QStringLiteral("--project-root")
                      << appRoot
                      << QStringLiteral("--config")
                      << context_.paths().appConfigPath
                      << QStringLiteral("--script")
                      << scriptPath;
    command.arguments << extraArgs;
    command.workingDirectory = appRoot;

    // 默认把整个新工程自身作为根目录注入进去，保证脚本和 helper 在新路径下仍能找到资料。
    command.environment.insert(QStringLiteral("RECORDLABC_ROOT"), appRoot);
    command.environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));

    const auto pythonPath = pythonPathEntries();
    if (!pythonPath.isEmpty()) {
        command.environment.insert(QStringLiteral("PYTHONPATH"), pythonPath.join(QDir::listSeparator()));
    }

    for (auto it = extraEnvironment.constBegin(); it != extraEnvironment.constEnd(); ++it) {
        command.environment.insert(it.key(), it.value());
    }
    return command;
}

}  // namespace recordlab::script
