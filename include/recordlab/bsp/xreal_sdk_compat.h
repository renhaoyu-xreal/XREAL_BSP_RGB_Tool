/*
 * xreal_sdk_compat.h
 *
 * 这是一个非常轻量的“XREAL SDK 运行时兼容性判断层”。
 *
 * 目的不是动态加载 SDK，也不是替代真正的 NativeGlassesAdapter，
 * 而是先把当前最关键的事实统一下来：
 *
 * 1. vendored 的 xreal_glasses wheel 带了真实原生库；
 * 2. 这份库当前要求独立 XREAL runtime 使用 PySide6/Qt 6.8.3；
 * 3. 如果运行时 Qt 版本不满足，BSP 真机链路应当直接报出硬阻塞，
 *    不能继续假装初始化成功。
 *
 * 这层故意做成 header-only，方便 UI 页面、BSP 子节点和后续探测层共用。
 */
#pragma once

#include <QLibraryInfo>
#include <QString>
#include <QVersionNumber>

namespace recordlab::bsp {

struct XrealSdkCompatibilityInfo {
    QString runtimeQtVersion;
    QString requiredQtVersion;
    bool runtimeQtCompatible = false;

    QString summary() const
    {
        return QStringLiteral("runtime-qt:%1 | required-qt:%2 | compatible:%3")
            .arg(runtimeQtVersion, requiredQtVersion, runtimeQtCompatible ? QStringLiteral("yes") : QStringLiteral("no"));
    }

    QString blockerMessage(const QString& assetPath = QString()) const
    {
        if (runtimeQtCompatible) {
            return QString();
        }

        if (assetPath.isEmpty()) {
            return QStringLiteral(
                "vendored XREAL SDK 需要 Qt %1，但当前运行时 Qt 为 %2。")
                .arg(requiredQtVersion, runtimeQtVersion);
        }

        return QStringLiteral(
            "vendored XREAL SDK 与当前 Qt 运行时不兼容：需要 Qt %1，当前为 %2，资产路径：%3")
            .arg(requiredQtVersion, runtimeQtVersion, assetPath);
    }
};

inline QVersionNumber requiredVendoredXrealQtVersion()
{
    // 版本基线来自 vendored wheel 元数据：
    //   Requires-Dist: pyside6==6.8.3
    // 另一方面，对 libxreal_glasses.so 的 readelf 结果也确认了它至少要求 Qt_6.8。
    // 这里按 wheel 实际打包依赖收敛到 6.8.3，避免 bootstrap 与预检口径不一致。
    return QVersionNumber(6, 8, 3);
}

inline QString requiredVendoredXrealQtVersionString()
{
    return requiredVendoredXrealQtVersion().toString();
}

inline XrealSdkCompatibilityInfo probeVendoredXrealQtCompatibility()
{
    const QVersionNumber runtimeVersion = QLibraryInfo::version();
    const QVersionNumber requiredVersion = requiredVendoredXrealQtVersion();

    XrealSdkCompatibilityInfo info;
    info.runtimeQtVersion = runtimeVersion.toString();
    info.requiredQtVersion = requiredVersion.toString();
    info.runtimeQtCompatible = runtimeVersion >= requiredVersion;
    return info;
}

}  // namespace recordlab::bsp
