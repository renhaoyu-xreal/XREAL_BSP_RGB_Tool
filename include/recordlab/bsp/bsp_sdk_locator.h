#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "recordlab/core/app_context.h"
#include "recordlab/bsp/xreal_sdk_compat.h"

/*
 * BSP SDK 线索探测
 *
 * 由于当前手头并不是一套标准的、文档完整的 C++ SDK 工程，
 * 而是一个 Python wheel + pyi + 多个原生 so 组合出来的发布物，
 * 因此这里先单独建一个探测层，把“有哪些遗留 SDK 线索”清晰表达出来。
 *
 * 这层目前不做真正的动态加载，只做路径和预期资产描述，
 * 方便后续 NativeGlassesAdapter 接手。
 */
namespace recordlab::bsp {

struct BspSdkAssetExpectation {
    // wheel 内预期存在的条目路径。
    QString archiveEntry;
    // 该资产的大致用途。
    QString purpose;
};

struct BspSdkInfo {
    // 本地 vendored 与旧版回退路径。
    QString vendoredWheelPath;
    QString vendoredPyiPath;
    QString legacyWheelPath;
    QString legacyPyiPath;

    // 当前认为应重点关注的 wheel 内资产。
    QList<BspSdkAssetExpectation> expectedAssets;

    // 探测阶段发现的问题。
    QStringList warnings;
    bool vendoredWheelAvailable = false;
    bool vendoredPyiAvailable = false;
    bool wheelAvailable = false;
    bool pyiAvailable = false;
    QString runtimeRoot;
    QString runtimeSitePackages;
    QString runtimeGlassesServerPath;
    QString currentProcessQtVersion;
    QString runtimeQtVersion;
    QString runtimeQtSource;
    QString requiredQtVersion;
    bool runtimeQtCompatible = false;

    // 生成简短状态摘要，便于 UI 展示。
    QString summary() const;
    QString compatibilitySummary() const;

    // 当前优先使用的资产路径。
    QString preferredWheelPath() const;
    QString preferredPyiPath() const;
};

class BspSdkLocator {
public:
    // 基于启动上下文对 BSP SDK 线索进行一次快速探测。
    static BspSdkInfo probe(const recordlab::core::AppContext& context);
};

}  // namespace recordlab::bsp
