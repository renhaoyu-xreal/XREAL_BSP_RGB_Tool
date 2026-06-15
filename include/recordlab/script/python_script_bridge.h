#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

#include "recordlab/core/app_context.h"

namespace recordlab::script {

struct ScriptCommand {
    // 预期执行程序，一般为 python3 或用户指定解释器。
    QString program;
    // 传给解释器的参数。
    QStringList arguments;
    // 运行时工作目录。整个工程搬到新路径后，这里也要跟着走本地根目录。
    QString workingDirectory;
    // 为脚本额外注入的环境变量。
    QMap<QString, QString> environment;
};

/*
 * Python 脚本桥
 *
 * 设计原则已经明确：脚本保留为编排层。
 * 因此新工程不是要“消灭 Python 脚本”，而是要让 Python 只做编排，
 * 不再承担核心高频运行时职责。
 *
 * 当前这个类先提供最基础的命令拼装能力，后面再逐步接真正的进程执行、
 * 环境注入、输出捕获和脚本状态回传。
 */
class PythonScriptBridge : public QObject {
    Q_OBJECT

public:
    explicit PythonScriptBridge(const recordlab::core::AppContext& context, QObject* parent = nullptr);

    // 当前是否具备桥接旧脚本的最低前提。
    bool isAvailable() const;
    // 当前用于执行脚本的 Python 解释器。
    QString pythonExecutable() const;
    // 生成脚本执行时建议附带的 PYTHONPATH 条目。
    QStringList pythonPathEntries() const;
    // 生成一条脚本执行命令。这里不再直接执行目标脚本，而是统一走本地兼容运行时。
    ScriptCommand buildCommand(
        const QString& relativeScriptPath,
        const QStringList& extraArgs = {},
        const QMap<QString, QString>& extraEnvironment = {}) const;
    ScriptCommand buildCommandForScriptPath(
        const QString& scriptPath,
        const QStringList& extraArgs = {},
        const QMap<QString, QString>& extraEnvironment = {}) const;

private:
    const recordlab::core::AppContext& context_;
    QString pythonExecutable_;
};

}  // namespace recordlab::script
