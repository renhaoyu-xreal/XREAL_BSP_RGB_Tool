#pragma once

#include <QString>
#include <QStringList>

#include "recordlab/core/app_context.h"

/*
 * BSP 资产解析器
 *
 * 当前 RecordLabC 已经开始把可复用资产复制到本地目录：
 * - third_party/echo_message_system
 * - third_party/xreal_glasses
 * - scripts/common
 * - scripts/record_bsp_*.py
 *
 * 因此后续所有 BSP 相关模块都不应继续各自手写“优先本地、否则回退旧工程”的逻辑，
 * 而应该统一通过本解析器拿到资产位置。
 */
namespace recordlab::bsp {

struct BspAssetLayout {
    // ===== 本地 vendored 资产 =====
    QString vendoredProjectRoot;
    QString vendoredScriptsRoot;
    QString vendoredCommonScriptsRoot;
    QString vendoredPythonMessageSystemRoot;
    QString vendoredEchoCppRoot;
    QString vendoredWheelPath;
    QString vendoredPyiPath;

    // ===== 旧工程回退资产 =====
    QString legacyProjectRoot;
    QString legacyScriptsRoot;
    QString legacyCommonScriptsRoot;
    QString legacyPythonMessageSystemRoot;
    QString legacyEchoCppRoot;
    QString legacyWheelPath;
    QString legacyPyiPath;

    // ===== 可用性标记 =====
    bool vendoredScriptsAvailable = false;
    bool vendoredPythonMessageSystemAvailable = false;
    bool vendoredEchoCppAvailable = false;
    bool vendoredWheelAvailable = false;
    bool vendoredPyiAvailable = false;

    bool legacyScriptsAvailable = false;
    bool legacyPythonMessageSystemAvailable = false;
    bool legacyEchoCppAvailable = false;
    bool legacyWheelAvailable = false;
    bool legacyPyiAvailable = false;

    // 生成给 Python 解释器使用的 PYTHONPATH 条目列表。
    QStringList pythonPathEntries() const;

    // 解析某个脚本文件的优先路径：先本地，再旧工程。
    QString resolveScriptPath(const QString& relativeScriptPath) const;

    // BSP SDK 线索优先使用本地复制版本。
    QString preferredWheelPath() const;
    QString preferredPyiPath() const;
};

class BspAssetResolver {
public:
    static BspAssetLayout resolve(const recordlab::core::AppContext& context);
};

}  // namespace recordlab::bsp
