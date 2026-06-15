#include "recordlab/bsp/bsp_guide_status.h"

#include "recordlab/bsp/bsp_asset_resolver.h"
#include "recordlab/bsp/bsp_sdk_locator.h"

/*
 * bsp_guide_status.cpp
 *
 * 当前迁移已经进入“必须能回答还差多少”的阶段。
 * 因此这里把《使用指南》的关键操作能力拆成若干条目，并给出两套视角：
 *
 * 1. 结构就绪度
 *    允许“页面骨架/状态机/资产收编”获得部分分数；
 *    适合衡量工程是否在朝正确结构前进。
 *
 * 2. 实际可操作度
 *    只把真正 ready 的能力计分；
 *    更接近用户问的“现在能不能按指南操作”。
 *
 * 这两者必须分开，否则很容易把“按钮已经画出来了”误判成“功能真的可用”。
 */
namespace recordlab::bsp {

namespace {

int structuralScore(GuideCapabilityState state)
{
    // 结构就绪度允许“骨架已到位”拿到部分分数，用来反映迁移进展。
    switch (state) {
    case GuideCapabilityState::Missing:
        return 0;
    case GuideCapabilityState::Scaffolded:
        return 45;
    case GuideCapabilityState::Ready:
        return 100;
    }

    return 0;
}

int operationalScore(GuideCapabilityState state)
{
    // 实际可操作度更严格，只有真正 Ready 的条目才记满分。
    return state == GuideCapabilityState::Ready ? 100 : 0;
}

int averagePercent(const QList<GuideCapabilityItem>& items, int (*scorer)(GuideCapabilityState))
{
    // 把评分函数参数化，便于结构视角和操作视角共用同一统计逻辑。
    if (items.isEmpty()) {
        return 0;
    }

    int totalScore = 0;
    for (const auto& item : items) {
        totalScore += scorer(item.state);
    }

    return totalScore / items.size();
}

}  // namespace

int GuideStatusReport::structuralPercent() const
{
    // 计算面向工程迁移进度的结构完成度。
    return averagePercent(items, structuralScore);
}

int GuideStatusReport::operationalPercent() const
{
    // 计算面向最终用户的实际可操作完成度。
    return averagePercent(items, operationalScore);
}

QString GuideStatusReport::summary() const
{
    // 生成适合展示在摘要卡片上的双指标说明文本。
    return QStringLiteral("结构就绪度 %1%，实际可操作度 %2%")
        .arg(structuralPercent())
        .arg(operationalPercent());
}

QStringList GuideStatusReport::blockingItems(int limit) const
{
    // 提取仍在阻塞主流程的条目，便于 UI 聚焦展示最关键问题。
    QStringList blockers;
    for (const auto& item : items) {
        if (!item.blocking || item.state == GuideCapabilityState::Ready) {
            continue;
        }

        blockers.push_back(QStringLiteral("%1：%2").arg(item.title, item.detail));
        if (limit > 0 && blockers.size() >= limit) {
            break;
        }
    }
    return blockers;
}

QString guideCapabilityStateToChinese(GuideCapabilityState state)
{
    // 把内部枚举转换为统一中文文案，避免各页面重复实现映射。
    switch (state) {
    case GuideCapabilityState::Missing:
        return QStringLiteral("未开始");
    case GuideCapabilityState::Scaffolded:
        return QStringLiteral("骨架已就位");
    case GuideCapabilityState::Ready:
        return QStringLiteral("已就绪");
    }

    return QStringLiteral("未知");
}

GuideStatusReport BspGuideStatus::evaluate(const recordlab::core::AppContext& context)
{
    // 结合资产状态与 runtime 兼容性，生成一份对照 BSP 使用指南的诊断结果。
    GuideStatusReport report;
    const auto assets = BspAssetResolver::resolve(context);
    const auto sdkInfo = BspSdkLocator::probe(context);

    /*
     * 下面这份条目列表直接对照《RecordLab录数据使用指南》的主操作路径。
     *
     * 这份状态表有两个要求：
     * 1. 要跟上当前代码真实进度，不能继续保留“脚本只会预览命令”之类的过期结论；
     * 2. 要把当前最致命的硬阻塞显式写出来，尤其是 XREAL vendored SDK 依赖 Qt 6.8。
     *
     * 这样 UI 页面上的“还差多少”才会真的有诊断价值，而不是只输出一串泛泛而谈的 TODO。
     */
    report.items = {
        {
            QStringLiteral("XREAL SDK 资产收编"),
            QStringLiteral("3.2 启动设备"),
            assets.vendoredWheelAvailable && assets.vendoredPyiAvailable
                ? QStringLiteral("wheel 与 pyi 已复制到新工程，可作为 native 适配线索。")
                : QStringLiteral("wheel 或 pyi 尚未完整复制，本地 SDK 线索不足。"),
            (assets.vendoredWheelAvailable && assets.vendoredPyiAvailable)
                ? GuideCapabilityState::Ready
                : GuideCapabilityState::Missing,
            true
        },
        {
            QStringLiteral("XREAL SDK 运行时兼容性"),
            QStringLiteral("3.2 启动设备"),
            sdkInfo.runtimeQtCompatible
                ? QStringLiteral("项目内 XREAL runtime 已满足 vendored SDK 的版本要求，可与主 GUI 的 Qt %1 并存。")
                      .arg(sdkInfo.currentProcessQtVersion)
                : QStringLiteral("当前 vendored XREAL SDK 需要独立 XREAL runtime Qt %1，当前有效版本为 %2（来源：%3）；请先运行 ./setup_xreal_runtime.sh。")
                      .arg(sdkInfo.requiredQtVersion, sdkInfo.runtimeQtVersion, sdkInfo.runtimeQtSource),
            sdkInfo.runtimeQtCompatible ? GuideCapabilityState::Ready : GuideCapabilityState::Missing,
            true
        },
                {
            QStringLiteral("热插拔恢复"),
            QStringLiteral("3.2.2 热插拔"),
            QStringLiteral("尚未接入断开重连、缓存清理和一键按钮复位逻辑。"),
            GuideCapabilityState::Missing,
            true
        },
    };

    return report;
}

}  // namespace recordlab::bsp
