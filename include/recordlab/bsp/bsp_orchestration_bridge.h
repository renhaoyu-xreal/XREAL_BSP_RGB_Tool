#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

#include "recordlab/core/app_context.h"
#include "recordlab/script/python_script_bridge.h"

/*
 * BSP 编排桥
 *
 * 这个类的定位介于“纯脚本桥”和“未来真实脚本执行器”之间。
 *
 * 当前职责：
 * 1. 收敛 BSP 支持的编排脚本集合；
 * 2. 统一解析这些脚本在新工程中的实际路径；
 * 3. 给未来 UI / 执行器提供统一的脚本命令和 PythonPath 信息。
 *
 * 这样后面即使要把脚本运行从按钮、调试页、批处理页等多个地方接起来，
 * 也不需要在各处重复写脚本解析逻辑。
 */
namespace recordlab::bsp {

class BspOrchestrationBridge {
public:
    explicit BspOrchestrationBridge(const recordlab::core::AppContext& context);

    bool isAvailable() const;
    QStringList supportedScripts() const;
    QString resolvedScriptPath(const QString& relativeScriptPath) const;
    QStringList pythonPathEntries() const;
    recordlab::script::ScriptCommand buildScriptCommand(
        const QString& relativeScriptPath,
        const QStringList& extraArgs = {},
        const QMap<QString, QString>& extraEnvironment = {}) const;
    recordlab::script::ScriptCommand buildScriptCommandForPath(
        const QString& scriptPath,
        const QStringList& extraArgs = {},
        const QMap<QString, QString>& extraEnvironment = {}) const;

private:
    const recordlab::core::AppContext& context_;
    recordlab::script::PythonScriptBridge pythonBridge_;
};

}  // namespace recordlab::bsp
