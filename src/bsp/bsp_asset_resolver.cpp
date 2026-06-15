#include "recordlab/bsp/bsp_asset_resolver.h"

#include <QDir>
#include <QFileInfo>

/*
 * bsp_asset_resolver.cpp
 *
 * 这个文件的目标很明确：
 * 把“本地 vendored 资产”和“旧工程只读资产”的选择逻辑统一收口。
 *
 * 这样后续：
 * - PythonScriptBridge
 * - BspSdkLocator
 * - NativeGlassesAdapter
 * - 未来脚本执行器
 *
 * 都可以复用同一套解析结果，而不是反复各自拼路径。
 */
namespace recordlab::bsp {

namespace {

bool legacyFallbackEnabled()
{
    // 只有在显式打开环境变量时才允许回退到旧工程资产，默认保持本地自洽。
    const auto value = qEnvironmentVariable("RECORDLABC_ENABLE_LEGACY_FALLBACK").trimmed().toLower();
    return value == QStringLiteral("1")
        || value == QStringLiteral("true")
        || value == QStringLiteral("yes")
        || value == QStringLiteral("on");
}

bool fileExists(const QString& path)
{
    // 判断脚本、wheel、pyi 等目标路径是否为真实文件。
    return QFileInfo::exists(path) && QFileInfo(path).isFile();
}

bool dirExists(const QString& path)
{
    // 判断脚本目录或 vendored 仓库目录是否存在。
    return QFileInfo(path).exists() && QFileInfo(path).isDir();
}

QString vendoredRootJoin(const QString& appRoot, const QString& relativePath)
{
    // 基于当前工程根目录拼接 vendored 资产路径。
    return QDir(appRoot).filePath(relativePath);
}

QString legacyRootJoin(const QString& legacyRoot, const QString& relativePath)
{
    // 基于旧工程根目录拼接回退资产路径。
    return QDir(legacyRoot).filePath(relativePath);
}

}  // namespace

QStringList BspAssetLayout::pythonPathEntries() const
{
    // 组装脚本执行所需的 PythonPath，默认优先完全依赖当前工程资产。
    QStringList entries;

    // 先放本地 vendored 工程根目录。
    // 这样 Python 才能正确解析诸如 from scripts.common import ... 这类导入。
    if (!vendoredProjectRoot.isEmpty()) {
        entries << vendoredProjectRoot;
    }
    if (vendoredPythonMessageSystemAvailable) {
        entries << vendoredPythonMessageSystemRoot;
    }

    // 默认不再依赖旧工程。
    // 只有显式打开 RECORDLABC_ENABLE_LEGACY_FALLBACK 时，才把旧工程路径加回来。
    if (legacyFallbackEnabled()) {
        if (!legacyProjectRoot.isEmpty()) {
            entries << legacyProjectRoot;
        }
        if (legacyPythonMessageSystemAvailable) {
            entries << legacyPythonMessageSystemRoot;
        }
    }

    entries.removeDuplicates();
    return entries;
}

QString BspAssetLayout::resolveScriptPath(const QString& relativeScriptPath) const
{
    // 先查本地脚本，再在允许时回退到旧工程，统一脚本路径解析语义。
    QString normalizedRelativePath = relativeScriptPath;
    if (normalizedRelativePath.startsWith(QStringLiteral("scripts/"))) {
        normalizedRelativePath.remove(0, QStringLiteral("scripts/").size());
    }

    const QString localCandidate = QDir(vendoredScriptsRoot).filePath(normalizedRelativePath);
    if (fileExists(localCandidate)) {
        return localCandidate;
    }

    if (legacyFallbackEnabled()) {
        const QString legacyCandidate = QDir(legacyScriptsRoot).filePath(normalizedRelativePath);
        if (fileExists(legacyCandidate)) {
            return legacyCandidate;
        }
    }

    return localCandidate;
}

QString BspAssetLayout::preferredWheelPath() const
{
    // 优先返回当前工程 vendored wheel，仅在明确允许回退时才使用旧副本。
    if (vendoredWheelAvailable) {
        return vendoredWheelPath;
    }
    return legacyFallbackEnabled() ? legacyWheelPath : vendoredWheelPath;
}

QString BspAssetLayout::preferredPyiPath() const
{
    // 与 wheel 相同，优先选择当前工程内的 pyi 契约文件。
    if (vendoredPyiAvailable) {
        return vendoredPyiPath;
    }
    return legacyFallbackEnabled() ? legacyPyiPath : vendoredPyiPath;
}

BspAssetLayout BspAssetResolver::resolve(const recordlab::core::AppContext& context)
{
    // 一次性解析 BSP 资产目录布局及其可用性，供多个模块共享同一结果。
    BspAssetLayout layout;

    // ===== 本地 vendored 目录 =====
    layout.vendoredProjectRoot = context.paths().appRoot;
    layout.vendoredScriptsRoot = vendoredRootJoin(context.paths().appRoot, QStringLiteral("scripts"));
    layout.vendoredCommonScriptsRoot = vendoredRootJoin(context.paths().appRoot, QStringLiteral("scripts/common"));
    layout.vendoredPythonMessageSystemRoot =
        vendoredRootJoin(context.paths().appRoot, QStringLiteral("third_party/echo_message_system/python"));
    layout.vendoredEchoCppRoot =
        vendoredRootJoin(context.paths().appRoot, QStringLiteral("third_party/echo_message_system/cpp-refactor"));
    layout.vendoredWheelPath =
        vendoredRootJoin(context.paths().appRoot, QStringLiteral("third_party/xreal_glasses/xreal_glasses-0.4.3-py3-none-any.whl"));
    layout.vendoredPyiPath =
        vendoredRootJoin(context.paths().appRoot, QStringLiteral("third_party/xreal_glasses/XrGlasses.pyi"));

    // ===== 旧工程回退目录 =====
    layout.legacyProjectRoot = context.paths().legacyRoot;
    layout.legacyScriptsRoot = legacyRootJoin(context.paths().legacyRoot, QStringLiteral("scripts"));
    layout.legacyCommonScriptsRoot = legacyRootJoin(context.paths().legacyRoot, QStringLiteral("scripts/common"));
    layout.legacyPythonMessageSystemRoot =
        legacyRootJoin(context.paths().legacyRoot, QStringLiteral("third_party/echo_message_system/python"));
    layout.legacyEchoCppRoot =
        legacyRootJoin(context.paths().legacyRoot, QStringLiteral("third_party/echo_message_system/cpp-refactor"));
    layout.legacyWheelPath = context.paths().legacyWheelPath;
    layout.legacyPyiPath = context.paths().legacyPyiPath;

    // ===== 可用性判断 =====
    layout.vendoredScriptsAvailable = dirExists(layout.vendoredScriptsRoot);
    layout.vendoredPythonMessageSystemAvailable = dirExists(layout.vendoredPythonMessageSystemRoot);
    layout.vendoredEchoCppAvailable = dirExists(layout.vendoredEchoCppRoot);
    layout.vendoredWheelAvailable = fileExists(layout.vendoredWheelPath);
    layout.vendoredPyiAvailable = fileExists(layout.vendoredPyiPath);

    layout.legacyScriptsAvailable = dirExists(layout.legacyScriptsRoot);
    layout.legacyPythonMessageSystemAvailable = dirExists(layout.legacyPythonMessageSystemRoot);
    layout.legacyEchoCppAvailable = dirExists(layout.legacyEchoCppRoot);
    layout.legacyWheelAvailable = fileExists(layout.legacyWheelPath);
    layout.legacyPyiAvailable = fileExists(layout.legacyPyiPath);

    return layout;
}

}  // namespace recordlab::bsp
