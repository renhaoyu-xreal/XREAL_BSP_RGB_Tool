#pragma once

#include <QString>

/*
 * 兼容路径发现
 *
 * 这个结构现在同时承担两类信息：
 * 1. RecordLabC 自己的本地运行时资料路径；
 * 2. 旧版 RecordLab 的只读参考资料路径。
 *
 * 当前目标已经从“依赖旧工程启动”切换成“默认使用本地资料独立启动”。
 * 因此这里不再把旧工程视为硬前提，而是把它降级为可选参考输入。
 */
namespace recordlab::core {

struct LegacyPaths {
    // 新工程根目录，通常就是 RecordLabC 自身。
    QString appRoot;
    // 新工程自身的默认运行时资料路径。
    QString appConfigPath;
    QString appGuidePath;
    QString appWheelPath;
    QString appPyiPath;
    QString appEchoCppPath;

    // 旧工程根目录，默认推导为兄弟目录 ../RecordLab，也支持环境变量覆盖。
    // 注意：这已经不是启动硬依赖，而是可选参考根目录。
    QString legacyRoot;

    // 以下是后续迁移阶段可能仍会参考的旧版资料路径。
    QString legacyConfigPath;
    QString legacyGuidePath;
    QString legacyWheelPath;
    QString legacyPyiPath;
    QString legacyEchoCppPath;

    // 当前工程本地资料是否可用。
    bool appConfigExists = false;
    bool appGuideExists = false;
    bool appWheelExists = false;
    bool appPyiExists = false;
    bool appEchoCppExists = false;

    // 旧工程参考资料是否可用。
    bool legacyRootExists = false;
    bool legacyConfigExists = false;
    bool legacyGuideExists = false;
    bool legacyWheelExists = false;
    bool legacyPyiExists = false;
    bool legacyEchoCppExists = false;

    // 自动发现当前工程和旧工程的关键资料路径。
    static LegacyPaths discover(const QString& appRootPath);

    // 用于调试输出和启动阶段排障。
    QString summary() const;

    // 当前是否具备“至少能读本地配置”的条件。
    bool readyForConfig() const;
};

}  // namespace recordlab::core
