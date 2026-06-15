#include "recordlab/bsp/bsp_sdk_locator.h"

#include <QFileInfo>

#include "recordlab/bsp/bsp_asset_resolver.h"
#include "recordlab/bsp/xreal_runtime_locator.h"

/*
 * bsp_sdk_locator.cpp
 *
 * 这里是 BSP 原生迁移的前置探测层。
 *
 * 由于当前手头是 wheel + pyi + so 的组合，而不是现成的 C++ SDK 项目，
 * 所以第一步必须先把“我们到底掌握了哪些原生资产”结构化出来。
 *
 * 后续 NativeGlassesAdapter 真正实现时，会以这里的探测结果为输入依据。
 */
namespace recordlab::bsp {

namespace {

bool legacyFallbackEnabled()
{
    // 只有显式允许时才回退到旧工程 SDK 资产，避免新工程产生隐式耦合。
    const auto value = qEnvironmentVariable("RECORDLABC_ENABLE_LEGACY_FALLBACK").trimmed().toLower();
    return value == QStringLiteral("1")
        || value == QStringLiteral("true")
        || value == QStringLiteral("yes")
        || value == QStringLiteral("on");
}

}  // namespace

QString BspSdkInfo::summary() const
{
    // 生成适合 UI 摘要卡片展示的一行状态说明。
    QStringList parts;
    parts << (vendoredWheelAvailable ? QStringLiteral("vendored-wheel:present") : QStringLiteral("vendored-wheel:missing"));
    parts << (vendoredPyiAvailable ? QStringLiteral("vendored-pyi:present") : QStringLiteral("vendored-pyi:missing"));
    parts << (wheelAvailable ? QStringLiteral("effective-wheel:present") : QStringLiteral("effective-wheel:missing"));
    parts << (pyiAvailable ? QStringLiteral("effective-pyi:present") : QStringLiteral("effective-pyi:missing"));
    parts << QStringLiteral("runtime-root:%1").arg(runtimeRoot.isEmpty() ? QStringLiteral("missing") : QStringLiteral("present"));
    parts << QStringLiteral("runtime-qt:%1").arg(runtimeQtVersion);
    parts << QStringLiteral("runtime-source:%1").arg(runtimeQtSource);
    parts << QStringLiteral("required-qt:%1").arg(requiredQtVersion);
    parts << (runtimeQtCompatible ? QStringLiteral("sdk-qt:compatible") : QStringLiteral("sdk-qt:incompatible"));
    parts << QStringLiteral("expected-assets:%1").arg(expectedAssets.size());
    return parts.join(QStringLiteral(" | "));
}

QString BspSdkInfo::compatibilitySummary() const
{
    // 详细说明当前进程 Qt、有效 runtime Qt 与 vendored 要求之间的关系。
    return QStringLiteral("当前进程 Qt=%1，XREAL 使用的有效 Qt=%2（%3），vendored SDK 需要 Qt=%4，兼容=%5")
        .arg(currentProcessQtVersion,
             runtimeQtVersion,
             runtimeQtSource,
             requiredQtVersion,
             runtimeQtCompatible ? QStringLiteral("是") : QStringLiteral("否"));
}

QString BspSdkInfo::preferredWheelPath() const
{
    // 优先返回当前工程内的 wheel，减少对旧工程路径的运行时依赖。
    if (vendoredWheelAvailable) {
        return vendoredWheelPath;
    }
    return legacyFallbackEnabled() ? legacyWheelPath : vendoredWheelPath;
}

QString BspSdkInfo::preferredPyiPath() const
{
    // 与 wheel 保持一致，优先使用当前工程内的 pyi 契约文件。
    if (vendoredPyiAvailable) {
        return vendoredPyiPath;
    }
    return legacyFallbackEnabled() ? legacyPyiPath : vendoredPyiPath;
}

BspSdkInfo BspSdkLocator::probe(const recordlab::core::AppContext& context)
{
    // 收集 wheel、pyi、runtime 与 Qt 兼容性信息，供 UI 和 native 适配层共享。
    BspSdkInfo info;
    const auto assets = BspAssetResolver::resolve(context);

    // 先把本地 vendored 和旧版回退路径都记录下来，后续 UI 与 native 适配层都可以直接使用。
    info.vendoredWheelPath = assets.vendoredWheelPath;
    info.vendoredPyiPath = assets.vendoredPyiPath;
    info.legacyWheelPath = context.paths().legacyWheelPath;
    info.legacyPyiPath = context.paths().legacyPyiPath;
    info.vendoredWheelAvailable = assets.vendoredWheelAvailable;
    info.vendoredPyiAvailable = assets.vendoredPyiAvailable;
    info.wheelAvailable = !assets.preferredWheelPath().isEmpty() && QFileInfo::exists(assets.preferredWheelPath());
    info.pyiAvailable = !assets.preferredPyiPath().isEmpty() && QFileInfo::exists(assets.preferredPyiPath());
    const auto runtimeInfo = XrealRuntimeLocator::probe(context.paths().appRoot);
    info.runtimeRoot = runtimeInfo.runtimeRoot;
    info.runtimeSitePackages = runtimeInfo.sitePackagesPath;
    info.runtimeGlassesServerPath = runtimeInfo.glassesServerPath;
    info.currentProcessQtVersion = runtimeInfo.currentProcessQtVersion;
    info.runtimeQtVersion = runtimeInfo.effectiveQtVersion;
    info.runtimeQtSource = runtimeInfo.effectiveQtSource;
    info.requiredQtVersion = requiredVendoredXrealQtVersionString();
    info.runtimeQtCompatible = runtimeInfo.effectiveQtCompatible();

    // 这里列出的资产并不是“当前已经在代码里动态加载了”，
    // 而是明确告诉后续 BSP native 适配层：这些是当前优先要验证的 wheel 内条目。
    info.expectedAssets = {
        {QStringLiteral("xrglasses/XrGlasses.so"), QStringLiteral("Python Qt binding shim")},
        {QStringLiteral("xrglasses/lib/libxreal_glasses.so"), QStringLiteral("Native glasses runtime")},
        {QStringLiteral("xrglasses/lib/libnr_sensor.so"), QStringLiteral("Native sensor backend")},
        {QStringLiteral("xrglasses/lib/libnr_libusb.so"), QStringLiteral("USB transport layer")},
        {QStringLiteral("xrglasses/bin/glasses_server"), QStringLiteral("Bundled helper executable")}
    };

    // 这些 warning 会直接提示“后续哪部分 native BSP 工作会被卡住”。
    if (!info.wheelAvailable) {
        info.warnings.push_back(QStringLiteral("未找到可用的 BSP wheel；后续 native 适配层会被阻塞。"));
    }
    if (!info.pyiAvailable) {
        info.warnings.push_back(QStringLiteral("未找到可用的 XrGlasses.pyi；Python 接口契约线索不可用。"));
    }
    if (info.wheelAvailable && !info.runtimeQtCompatible) {
        info.warnings.push_back(
            QStringLiteral("vendored XREAL 原生库要求 Qt %1，但当前有效 XREAL runtime Qt 为 %2（来源：%3）；请先运行 ./setup_xreal_runtime.sh。")
                .arg(info.requiredQtVersion, info.runtimeQtVersion, info.runtimeQtSource));
    }
    for (const auto& blocker : runtimeInfo.blockers) {
        info.warnings.push_back(blocker);
    }
    if (info.vendoredWheelAvailable) {
        info.warnings.push_back(QStringLiteral("当前优先使用本地 vendored BSP wheel：%1").arg(info.vendoredWheelPath));
    } else if (legacyFallbackEnabled() && assets.legacyWheelAvailable) {
        info.warnings.push_back(QStringLiteral("当前仍需回退到旧工程 BSP wheel：%1").arg(info.legacyWheelPath));
    } else {
        info.warnings.push_back(QStringLiteral("当前工程内未找到本地 BSP wheel。"));
    }
    if (runtimeInfo.projectLocalRuntimeAvailable()) {
        info.warnings.push_back(
            QStringLiteral("项目内 XREAL runtime 已就绪：Qt=%1，glasses_server=%2")
                .arg(runtimeInfo.effectiveQtVersion, runtimeInfo.glassesServerPath));
    }

    return info;
}

}  // namespace recordlab::bsp
