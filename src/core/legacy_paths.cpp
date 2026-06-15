#include "recordlab/core/legacy_paths.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include "recordlab/core/compatibility_contract.h"

/*
 * legacy_paths.cpp
 *
 * 这个实现文件对应“兼容路径发现层”。
 *
 * 当前阶段的设计目标：
 * 1. 默认只依赖 RecordLabC 自己的本地资料启动。
 * 2. 旧版 RecordLab 只作为可选参考目录被定位出来。
 * 3. 把本地配置、指南、wheel、C++ IPC 目录等关键资料统一定位出来。
 * 4. 让上层模块只面对一个结构化结果，而不是自己到处拼路径。
 *
 * 这层越稳定，后续上层模块越不容易因为目录结构变化而散架。
 */
namespace recordlab::core {

namespace {

// 简单文件存在性检查，避免在主体逻辑里重复写 QFileInfo。
bool fileExists(const QString& path)
{
    // 对普通文件存在性检查做一个薄封装，减少主流程里的样板代码。
    return QFileInfo::exists(path);
}

// 旧工程根目录的发现策略：
// 1. 如果用户显式设置了 RECORDLAB_LEGACY_ROOT，则优先使用；
// 2. 否则默认推导为新工程的兄弟目录 ../RecordLab；
// 3. 即使找不到旧工程，也不再影响新工程独立启动。
QString resolveLegacyRoot(const QString& appRootPath)
{
    // 允许通过环境变量显式指定旧工程位置，方便多环境调试和迁移。
    const auto envRoot = qEnvironmentVariable("RECORDLAB_LEGACY_ROOT");
    if (!envRoot.isEmpty()) {
        return QDir::cleanPath(envRoot);
    }

    QDir siblingDir(appRootPath);
    siblingDir.cdUp();
    return QDir::cleanPath(siblingDir.filePath(QStringLiteral("RecordLab")));
}

}  // namespace

LegacyPaths LegacyPaths::discover(const QString& appRootPath)
{
    // 一次性收敛新旧工程的关键路径和可用性状态，避免上层重复拼路径。
    LegacyPaths paths;

    // 先确定新工程根目录和旧工程根目录。
    paths.appRoot = QDir::cleanPath(appRootPath);
    paths.appConfigPath = QDir(paths.appRoot).filePath(QString::fromUtf8(compat::kAppConfigRelativePath));
    paths.appGuidePath = QDir(paths.appRoot).filePath(QString::fromUtf8(compat::kAppGuideRelativePath));
    paths.appWheelPath = QDir(paths.appRoot).filePath(QString::fromUtf8(compat::kAppWheelRelativePath));
    paths.appPyiPath = QDir(paths.appRoot).filePath(QString::fromUtf8(compat::kAppPyiRelativePath));
    paths.appEchoCppPath = QDir(paths.appRoot).filePath(QString::fromUtf8(compat::kAppEchoCppRelativePath));
    paths.legacyRoot = resolveLegacyRoot(paths.appRoot);

    // 再统一推导后续可能仍会参考的旧工程资料路径。
    paths.legacyConfigPath = QDir(paths.legacyRoot).filePath(QString::fromUtf8(compat::kLegacyConfigRelativePath));
    paths.legacyGuidePath = QDir(paths.legacyRoot).filePath(QString::fromUtf8(compat::kLegacyGuideRelativePath));
    paths.legacyWheelPath = QDir(paths.legacyRoot).filePath(QString::fromUtf8(compat::kLegacyWheelRelativePath));
    paths.legacyPyiPath = QDir(paths.legacyRoot).filePath(QString::fromUtf8(compat::kLegacyPyiRelativePath));
    paths.legacyEchoCppPath = QDir(paths.legacyRoot).filePath(QString::fromUtf8(compat::kLegacyEchoCppRelativePath));

    // 最后检查这些路径当前是否真实可用。
    paths.appConfigExists = fileExists(paths.appConfigPath);
    paths.appGuideExists = fileExists(paths.appGuidePath);
    paths.appWheelExists = fileExists(paths.appWheelPath);
    paths.appPyiExists = fileExists(paths.appPyiPath);
    paths.appEchoCppExists = QFileInfo(paths.appEchoCppPath).isDir();

    paths.legacyRootExists = QFileInfo(paths.legacyRoot).isDir();
    paths.legacyConfigExists = fileExists(paths.legacyConfigPath);
    paths.legacyGuideExists = fileExists(paths.legacyGuidePath);
    paths.legacyWheelExists = fileExists(paths.legacyWheelPath);
    paths.legacyPyiExists = fileExists(paths.legacyPyiPath);
    paths.legacyEchoCppExists = QFileInfo(paths.legacyEchoCppPath).isDir();

    return paths;
}

QString LegacyPaths::summary() const
{
    // 生成适合直接写日志或发给同事排障的多行路径摘要。
    QStringList lines;
    lines << QStringLiteral("appRoot=%1").arg(appRoot);
    lines << QStringLiteral("appConfig=%1").arg(appConfigPath);
    lines << QStringLiteral("appGuide=%1").arg(appGuidePath);
    lines << QStringLiteral("appWheel=%1").arg(appWheelPath);
    lines << QStringLiteral("appPyi=%1").arg(appPyiPath);
    lines << QStringLiteral("appEchoCpp=%1").arg(appEchoCppPath);
    lines << QStringLiteral("legacyRoot=%1").arg(legacyRoot);
    lines << QStringLiteral("legacyConfig=%1").arg(legacyConfigPath);
    lines << QStringLiteral("legacyGuide=%1").arg(legacyGuidePath);
    lines << QStringLiteral("legacyWheel=%1").arg(legacyWheelPath);
    lines << QStringLiteral("legacyPyi=%1").arg(legacyPyiPath);
    lines << QStringLiteral("legacyEchoCpp=%1").arg(legacyEchoCppPath);
    return lines.join(QLatin1Char('\n'));
}

bool LegacyPaths::readyForConfig() const
{
    // 当前启动判定刻意保持最小化：只要本地配置存在，就允许程序先进入 UI。
    return appConfigExists;
}

}  // namespace recordlab::core
