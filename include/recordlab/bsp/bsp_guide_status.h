#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "recordlab/core/app_context.h"

/*
 * BSP 使用指南覆盖状态
 *
 * 这层不是运行时核心，而是迁移过程中的“项目温度计”：
 * 1. 明确当前离《RecordLab录数据使用指南》还有多远；
 * 2. 区分“页面骨架已就位”和“真的能按指南操作”；
 * 3. 让 UI 与文档都能共用同一份差距判断口径。
 *
 * 当前阶段非常需要这层，因为工程已经脱离了“纯空壳”，
 * 但距离“真正可按指南完成 BSP 录制”仍然有显著差距。
 */
namespace recordlab::bsp {

enum class GuideCapabilityState {
    Missing,
    Scaffolded,
    Ready
};

struct GuideCapabilityItem {
    QString title;
    QString guideSection;
    QString detail;
    GuideCapabilityState state = GuideCapabilityState::Missing;
    bool blocking = true;
};

struct GuideStatusReport {
    QList<GuideCapabilityItem> items;

    // 结构就绪度允许“骨架已落地”获得部分分数。
    int structuralPercent() const;
    // 实际可操作度只统计真正 ready 的能力，不把骨架当成功能完成。
    int operationalPercent() const;
    QString summary() const;
    QStringList blockingItems(int limit = 6) const;
};

QString guideCapabilityStateToChinese(GuideCapabilityState state);

class BspGuideStatus {
public:
    static GuideStatusReport evaluate(const recordlab::core::AppContext& context);
};

}  // namespace recordlab::bsp
