#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

/*
 * 旧版配置加载器
 *
 * 这一层的目标不是“重新设计配置”，而是先把旧版
 * config/agents_config.json 的关键内容安全读进 C++ 世界。
 * 只有把旧配置稳定映射出来，后面 native agent/subnode/脚本桥接
 * 才能围绕同一份契约继续做。
 */
namespace recordlab::core {

struct WatchdogConfig {
    // 这些默认值直接对齐旧工程当前常见配置。
    double checkInterval = 1.0;
    double startupDelay = 1.0;
    double checkTimeout = 10.0;
};

struct AgentDefinition {
    // agent 的逻辑名称。
    QString name;
    // 本地子节点脚本路径。后续完全原生化后，这里会逐步转成新的 native 节点描述。
    QString subnodePath;
    // subnode 所在主机。当前保留旧结构，便于后面兼容旧语义。
    QString subnodeHost;
    int goalPort = 0;
    int feedbackPort = 0;
    QString rootPath;
    QJsonObject initDeviceParams;
    QJsonObject customParams;
    // 保存原始 JSON，避免迁移初期丢字段。
    QJsonObject raw;

    // 判断该 agent 是否按旧配置定义为本地运行。
    bool isLocal() const;
};

struct AppConfig {
    WatchdogConfig watchdog;
    // 以 agent name 为 key 的配置映射，便于后续按名字快速访问。
    QMap<QString, AgentDefinition> agents;
    // 入口页和主工作流优先展示的主 agent 列表。
    QStringList primaryAgents;

    bool containsAgent(const QString& agentName) const;
    QStringList availableAgents() const;
    AgentDefinition agent(const QString& agentName) const;
};

struct ScriptCatalogEntry {
    QString relativePath;
    QStringList supportedModelIds;
    QMap<QString, QString> supportedModels;
    QStringList requiredAgents;
    bool enabled = true;
    QJsonObject raw;
};

struct UsbProductCatalogEntry {
    QString displayName;
    QStringList productNames;
    int vid = -1;
    int pid = -1;
    QString agentName;
    bool sshPreferred = false;
    QJsonObject raw;
};

struct RecordLabConfig {
    QString version;
    QString updateInfo;
    QMap<QString, QList<ScriptCatalogEntry>> scriptsByAgent;
    QList<UsbProductCatalogEntry> usbProductCatalog;

    QList<ScriptCatalogEntry> scriptsForAgent(const QString& agentName) const;
};

struct ConfigLoadResult {
    // 成功时的解析结果。
    AppConfig config;
    // 失败时的错误说明。
    QString error;
    bool success = false;
};

struct RecordLabConfigLoadResult {
    RecordLabConfig config;
    QString error;
    bool success = false;
};

class LegacyConfigLoader {
public:
    // 从旧版 JSON 文件加载配置。
    static ConfigLoadResult load(const QString& filePath);
    static RecordLabConfigLoadResult loadRecordLabConfig(const QString& filePath);
};

}  // namespace recordlab::core
