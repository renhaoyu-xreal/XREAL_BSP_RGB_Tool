#include "recordlab/bsp/xreal_runtime_locator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QVersionNumber>

#include "recordlab/bsp/xreal_sdk_compat.h"

namespace recordlab::bsp {

namespace {

QString cleanedOrEmpty(const QString& value)
{
    // 统一去除空白并规范化路径；空输入返回空字符串而不是当前目录。
    return value.trimmed().isEmpty() ? QString() : QDir::cleanPath(value.trimmed());
}

QString runtimeRootFromInputs(const QString& projectRoot)
{
    // 优先读环境变量，其次回退到项目内约定的 runtime 目录。
    const QString envRuntimeRoot = cleanedOrEmpty(qEnvironmentVariable("RECORDLABC_XREAL_RUNTIME_ROOT"));
    if (!envRuntimeRoot.isEmpty()) {
        return envRuntimeRoot;
    }

    if (!projectRoot.trimmed().isEmpty()) {
        return QDir(projectRoot).filePath(QStringLiteral("runtime/xreal_runtime"));
    }

    return {};
}

QString sitePackagesFromInputs(const QString& runtimeRoot)
{
    // site-packages 同样允许环境变量覆盖，便于单独调试 Python runtime。
    const QString envSitePackages =
        cleanedOrEmpty(qEnvironmentVariable("RECORDLABC_XREAL_SITE_PACKAGES"));
    if (!envSitePackages.isEmpty()) {
        return envSitePackages;
    }

    if (!runtimeRoot.isEmpty()) {
        return QDir(runtimeRoot).filePath(QStringLiteral("site-packages"));
    }

    return {};
}

QString readManifestQtVersion(const QString& manifestPath)
{
    // 兼容读取 manifest 中两种可能的 Qt 版本字段。
    QFile manifest(manifestPath);
    if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    const auto document = QJsonDocument::fromJson(manifest.readAll());
    if (!document.isObject()) {
        return {};
    }

    const auto object = document.object();
    const QString qtVersion = object.value(QStringLiteral("qt_version")).toString().trimmed();
    if (!qtVersion.isEmpty()) {
        return qtVersion;
    }

    return object.value(QStringLiteral("pyside6_version")).toString().trimmed();
}

bool versionAtLeast(const QString& version, const QVersionNumber& minimum)
{
    // 将字符串版本解析为语义版本后比较，解析失败统一视为不满足要求。
    const auto parsed = QVersionNumber::fromString(version);
    if (parsed.isNull()) {
        return false;
    }
    return parsed >= minimum;
}

bool hasShibokenRuntime(const QString& shibokenDir)
{
    // shiboken 运行库是加载 vendored PySide/XREAL 绑定的必要前提之一。
    if (shibokenDir.isEmpty()) {
        return false;
    }

    const QDir dir(shibokenDir);
    return !dir.entryList({QStringLiteral("libshiboken6.abi3.so*")}, QDir::Files).isEmpty();
}

}  // namespace

bool XrealRuntimeInfo::projectLocalRuntimeAvailable() const
{
    // 只有 runtime 关键目录和核心二进制都齐备时，才视为项目内 runtime 可用。
    return runtimeRootAvailable
        && sitePackagesAvailable
        && manifestAvailable
        && qtLibAvailable
        && qtPluginsAvailable
        && shibokenAvailable
        && xrealPackageAvailable
        && nativeLibAvailable
        && glassesServerAvailable;
}

bool XrealRuntimeInfo::effectiveQtCompatible() const
{
    // 有效 Qt 版本可能来自项目 runtime，也可能退回到当前进程；这里统一做兼容判断。
    return versionAtLeast(effectiveQtVersion, requiredVendoredXrealQtVersion());
}

QString XrealRuntimeInfo::summary() const
{
    // 生成适合 UI 和日志快速查看的 runtime 状态摘要。
    return QStringLiteral(
               "runtime-root:%1 | site-packages:%2 | qt-lib:%3 | xreal-package:%4 | glasses-server:%5 | effective-qt:%6 (%7)")
        .arg(runtimeRootAvailable ? QStringLiteral("ok") : QStringLiteral("missing"),
             sitePackagesAvailable ? QStringLiteral("ok") : QStringLiteral("missing"),
             qtLibAvailable ? QStringLiteral("ok") : QStringLiteral("missing"),
             xrealPackageAvailable ? QStringLiteral("ok") : QStringLiteral("missing"),
             glassesServerAvailable ? QStringLiteral("ok") : QStringLiteral("missing"),
             effectiveQtVersion.isEmpty() ? QStringLiteral("unknown") : effectiveQtVersion,
             effectiveQtSource.isEmpty() ? QStringLiteral("unknown") : effectiveQtSource);
}

XrealRuntimeInfo XrealRuntimeLocator::probe(const QString& projectRoot)
{
    // 全面探测项目内 XREAL runtime 的完整性以及对 vendored SDK 的兼容性。
    XrealRuntimeInfo info;
    info.projectRoot = QDir::cleanPath(projectRoot);
    info.currentProcessQtVersion = QLibraryInfo::version().toString();

    info.runtimeRoot = runtimeRootFromInputs(info.projectRoot);
    info.runtimeRootAvailable = !info.runtimeRoot.isEmpty() && QFileInfo::exists(info.runtimeRoot);

    info.sitePackagesPath = sitePackagesFromInputs(info.runtimeRoot);
    info.sitePackagesAvailable =
        !info.sitePackagesPath.isEmpty() && QFileInfo::exists(info.sitePackagesPath);

    if (!info.runtimeRoot.isEmpty()) {
        info.manifestPath = QDir(info.runtimeRoot).filePath(QStringLiteral("manifest.json"));
    }
    info.manifestAvailable = !info.manifestPath.isEmpty() && QFileInfo::exists(info.manifestPath);

    if (!info.sitePackagesPath.isEmpty()) {
        const QDir sitePackages(info.sitePackagesPath);
        info.qtLibDir = sitePackages.filePath(QStringLiteral("PySide6/Qt/lib"));
        info.qtPluginsDir = sitePackages.filePath(QStringLiteral("PySide6/Qt/plugins"));
        info.shibokenDir = sitePackages.filePath(QStringLiteral("shiboken6"));
        info.xrealPackageDir = sitePackages.filePath(QStringLiteral("xrglasses"));
        info.nativeLibDir = sitePackages.filePath(QStringLiteral("xrglasses/lib"));
        info.glassesServerPath = sitePackages.filePath(QStringLiteral("xrglasses/bin/glasses_server"));
    }

    info.qtLibAvailable =
        !info.qtLibDir.isEmpty()
        && QFileInfo::exists(QDir(info.qtLibDir).filePath(QStringLiteral("libQt6Core.so.6")));
    info.qtPluginsAvailable =
        !info.qtPluginsDir.isEmpty()
        && QFileInfo::exists(QDir(info.qtPluginsDir).filePath(QStringLiteral("platforms")));
    info.shibokenAvailable = hasShibokenRuntime(info.shibokenDir);
    info.xrealPackageAvailable =
        !info.xrealPackageDir.isEmpty()
        && QFileInfo::exists(QDir(info.xrealPackageDir).filePath(QStringLiteral("__init__.py")));
    info.nativeLibAvailable =
        !info.nativeLibDir.isEmpty()
        && QFileInfo::exists(QDir(info.nativeLibDir).filePath(QStringLiteral("libxreal_glasses.so")));
    info.glassesServerAvailable =
        !info.glassesServerPath.isEmpty() && QFileInfo::exists(info.glassesServerPath);

    info.requestedQtVersion = readManifestQtVersion(info.manifestPath);

    if (info.projectLocalRuntimeAvailable()) {
        info.effectiveQtVersion = info.requestedQtVersion;
        info.effectiveQtSource = QStringLiteral("project-local");
        if (info.effectiveQtVersion.isEmpty()) {
            info.blockers.push_back(
                QStringLiteral("项目内 XREAL runtime 缺少 Qt 版本清单，无法确认是否满足 %1。")
                    .arg(requiredVendoredXrealQtVersionString()));
        }
    } else {
        info.effectiveQtVersion = info.currentProcessQtVersion;
        info.effectiveQtSource = QStringLiteral("current-process");
    }

    if (!info.runtimeRootAvailable) {
        info.blockers.push_back(
            QStringLiteral("未发现项目内 XREAL runtime：%1。请运行 ./setup_xreal_runtime.sh。")
                .arg(info.runtimeRoot));
    } else {
        if (!info.sitePackagesAvailable) {
            info.blockers.push_back(
                QStringLiteral("项目内 XREAL runtime 缺少 site-packages：%1").arg(info.sitePackagesPath));
        }
        if (!info.manifestAvailable) {
            info.blockers.push_back(
                QStringLiteral("项目内 XREAL runtime 缺少 manifest.json：%1").arg(info.manifestPath));
        }
        if (!info.qtLibAvailable) {
            info.blockers.push_back(
                QStringLiteral("项目内 XREAL runtime 缺少 Qt6Core 运行库：%1").arg(info.qtLibDir));
        }
        if (!info.qtPluginsAvailable) {
            info.blockers.push_back(
                QStringLiteral("项目内 XREAL runtime 缺少 Qt 插件目录：%1").arg(info.qtPluginsDir));
        }
        if (!info.shibokenAvailable) {
            info.blockers.push_back(
                QStringLiteral("项目内 XREAL runtime 缺少 shiboken6 运行库：%1").arg(info.shibokenDir));
        }
        if (!info.xrealPackageAvailable) {
            info.blockers.push_back(
                QStringLiteral("项目内 XREAL runtime 缺少 xrglasses 包：%1").arg(info.xrealPackageDir));
        }
        if (!info.nativeLibAvailable) {
            info.blockers.push_back(
                QStringLiteral("项目内 XREAL runtime 缺少 libxreal_glasses.so：%1").arg(info.nativeLibDir));
        }
        if (!info.glassesServerAvailable) {
            info.blockers.push_back(
                QStringLiteral("项目内 XREAL runtime 缺少 glasses_server：%1").arg(info.glassesServerPath));
        }
    }

    if (!info.effectiveQtCompatible()) {
        info.blockers.push_back(
            QStringLiteral("XREAL runtime Qt 版本不足：需要 %1，当前有效版本为 %2（来源：%3）。")
                .arg(requiredVendoredXrealQtVersionString(),
                     info.effectiveQtVersion.isEmpty() ? QStringLiteral("unknown")
                                                       : info.effectiveQtVersion,
                     info.effectiveQtSource));
    }

    info.blockers.removeDuplicates();
    return info;
}

}  // namespace recordlab::bsp
