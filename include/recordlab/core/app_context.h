#pragma once

#include <QString>
#include <QStringList>

#include "recordlab/core/legacy_config_loader.h"
#include "recordlab/core/legacy_paths.h"

/*
 * 启动上下文
 *
 * AppContext 是当前新工程启动时的“只读全局上下文”：
 * 1. 它负责知道旧工程在哪里。
 * 2. 它负责知道旧配置是否成功加载。
 * 3. 它负责在启动阶段整理 warning / error。
 *
 * 当前先做成轻量值对象，后面再视需要扩展为更完整的 runtime context。
 */
namespace recordlab::core {

class AppContext {
public:
    // 从新工程根目录出发，构造完整启动上下文。
    static AppContext create(const QString& appRootPath);

    bool isReady() const;
    const LegacyPaths& paths() const;
    const AppConfig& config() const;
    const RecordLabConfig& recordLabConfig() const;
    const QStringList& startupWarnings() const;
    const QString& startupError() const;

private:
    // 旧工程路径与关键资料路径。
    LegacyPaths paths_;
    // 解析后的旧版配置。
    AppConfig config_;
    RecordLabConfig recordLabConfig_;
    // 非阻塞性问题，通常允许程序继续启动。
    QStringList startupWarnings_;
    // 阻塞性问题，通常会影响启动。
    QString startupError_;
    bool ready_ = false;
};

}  // namespace recordlab::core
