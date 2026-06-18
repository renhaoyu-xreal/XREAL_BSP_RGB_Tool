#pragma once

#include <QStringList>

/*
 * 兼容契约常量
 *
 * 这个头文件的作用不是实现逻辑，而是把“新工程默认要对齐旧工程的哪些命名”
 * 集中收敛到一处。后面无论是接 IPC、重建 AgentManager、还是做脚本桥接，
 * 都应优先从这里读取兼容面，而不是在各处散落硬编码字符串。
 */
namespace recordlab::core::compat {

// 当前独立工具只保留 BSP 主 agent。
inline constexpr const char* kPrimaryBspAgent = "glasses_bsp_node";


// 以下相对路径是当前工程内默认使用的本地资料路径。
inline constexpr const char* kAppConfigRelativePath = "config/agents_config.json";
inline constexpr const char* kAppGuideRelativePath = "docs/RecordLabC录数据使用指南.md";
inline constexpr const char* kAppWheelRelativePath = "third_party/xreal_glasses/xreal_glasses-0.4.3-py3-none-any.whl";
inline constexpr const char* kAppPyiRelativePath = "third_party/xreal_glasses/XrGlasses.pyi";
inline constexpr const char* kAppEchoCppRelativePath = "third_party/echo_message_system/cpp-refactor";

// 以下相对路径保留为“旧工程参考资料路径”。
inline constexpr const char* kLegacyConfigRelativePath = "config/agents_config.json";
inline constexpr const char* kLegacyGuideRelativePath = "docs/RecordLab录数据使用指南.md";
inline constexpr const char* kLegacyWheelRelativePath = "xreal_glasses-0.3.3-py3-none-any.whl";
inline constexpr const char* kLegacyPyiRelativePath = "XrGlasses.pyi";
inline constexpr const char* kLegacyEchoCppRelativePath = "third_party/echo_message_system/cpp-refactor";

// 当前先保留最核心的 ManagerAction。
// 这里列的是迁移第一阶段最常用的动作集合，不代表旧工程的全部 action。
inline QStringList managerActions()
{
    return {
        QStringLiteral("INIT_AGENT"),
        QStringLiteral("RELEASE_AGENT"),
        QStringLiteral("STOP_ALL_AGENTS"),
        QStringLiteral("SEND_AGENT_COMMAND"),
        QStringLiteral("EXECUTE_SCRIPT"),
        QStringLiteral("EMERGENCY_STOP")
    };
}

// 这些 topic 是后续 native 数据面重建时最先要对齐的“公共骨架”。
inline QStringList primaryTopics()
{
    return {
        QStringLiteral("camera_data"),
        QStringLiteral("imu_data"),
        QStringLiteral("motion_status"),
        QStringLiteral("record_timer"),
        QStringLiteral("time_delay")
    };
}

// BSP 主链路命令集合。
// 这些命令名后续会直接影响脚本桥接、UI 交互和 agent/subnode 协议层。
inline QStringList bspCommandNames()
{
    return {
        QStringLiteral("init_device"),
        QStringLiteral("start_device"),
        QStringLiteral("stop_device"),
        QStringLiteral("start_record"),
        QStringLiteral("stop_record"),
        QStringLiteral("release_device"),
        QStringLiteral("estop"),
        QStringLiteral("check")
    };
}

// 当前独立 RGB 工具只保留 RAW 抓取脚本入口。
inline QStringList bspScriptNames()
{
    return {
        QStringLiteral("scripts/record_bsp_rgb_raw.py")
    };
}

}  // namespace recordlab::core::compat
