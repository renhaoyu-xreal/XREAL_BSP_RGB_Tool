#include "recordlab/core/legacy_config_loader.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

/*
 * legacy_config_loader.cpp
 *
 * 这里负责把旧版 JSON 配置读到 C++ 中。
 *
 * 当前阶段并不追求“重新设计配置模型”，而是优先保证：
 * 1. 旧版配置能稳定读入；
 * 2. 常用字段有明确映射；
 * 3. 暂时不理解的字段也尽量通过 raw 保留下来。
 *
 * 这种做法的好处是迁移初期不容易因为配置解释差异而阻塞后续功能开发。
 */
namespace recordlab::core {

namespace {

int parseHexOrInt(const QJsonValue& value, int fallback = -1)
{
    if (value.isDouble()) {
        return value.toInt(fallback);
    }

    const QString text = value.toString().trimmed();
    if (text.isEmpty()) {
        return fallback;
    }

    bool ok = false;
    const int base = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 10;
    const int parsed = text.toInt(&ok, base);
    return ok ? parsed : fallback;
}

QStringList namesFromUsbCatalogEntry(const QJsonObject& raw)
{
    QStringList names;
    const QJsonArray rawNames = raw.value(QStringLiteral("names")).toArray();
    for (const auto& value : rawNames) {
        const QString name = value.toString().trimmed();
        if (!name.isEmpty()) {
            names.push_back(name);
        }
    }

    const QString singleName = raw.value(QStringLiteral("name")).toString().trimmed();
    if (!singleName.isEmpty() && !names.contains(singleName)) {
        names.push_back(singleName);
    }
    return names;
}

}  // namespace

bool AgentDefinition::isLocal() const
{
    // 当前沿用旧工程“subnode_host == localhost 即本地 agent”的语义，
    // 这样旧配置可以尽量零改动迁移到新工程。
    return subnodeHost.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0;
}

bool AppConfig::containsAgent(const QString& agentName) const
{
    // 判断配置中是否声明了目标 agent，供 UI 和流程控制做快速校验。
    return agents.contains(agentName);
}

QStringList AppConfig::availableAgents() const
{
    // 返回全部 agent 名称，用于入口页和管理表格生成。
    return agents.keys();
}

AgentDefinition AppConfig::agent(const QString& agentName) const
{
    // 按名称提取 agent 定义；若不存在则返回默认构造结果。
    return agents.value(agentName);
}

QList<ScriptCatalogEntry> RecordLabConfig::scriptsForAgent(const QString& agentName) const
{
    return scriptsByAgent.value(agentName);
}

ConfigLoadResult LegacyConfigLoader::load(const QString& filePath)
{
    // 统一完成 JSON 文件读取和字段映射，策略是兼容优先、避免过早失败。
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ConfigLoadResult result;
        result.error = QStringLiteral("Failed to open config: %1").arg(filePath);
        result.success = false;
        return result;
    }

    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        ConfigLoadResult result;
        result.error = QStringLiteral("Legacy config is not a JSON object: %1").arg(filePath);
        result.success = false;
        return result;
    }

    const QJsonObject root = document.object();
    AppConfig config;

    // 读取 watchdog 配置。
    const QJsonObject watchdog = root.value(QStringLiteral("watchdog")).toObject();
    config.watchdog.checkInterval = watchdog.value(QStringLiteral("check_interval")).toDouble(1.0);
    config.watchdog.startupDelay = watchdog.value(QStringLiteral("startup_delay")).toDouble(1.0);
    config.watchdog.checkTimeout = watchdog.value(QStringLiteral("check_timeout")).toDouble(10.0);

    // 读取 agent 定义，并尽可能保留原始字段。
    const QJsonObject agents = root.value(QStringLiteral("agents")).toObject();
    for (auto it = agents.begin(); it != agents.end(); ++it) {
        const QString key = it.key();
        const QJsonObject raw = it.value().toObject();

        AgentDefinition definition;
        definition.name = raw.value(QStringLiteral("name")).toString(key);
        definition.subnodePath = raw.value(QStringLiteral("subnode_path")).toString();
        definition.subnodeHost = raw.value(QStringLiteral("subnode_host")).toString();
        definition.goalPort = raw.value(QStringLiteral("goal_port")).toInt();
        definition.feedbackPort = raw.value(QStringLiteral("feedback_port")).toInt();
        definition.rootPath = raw.value(QStringLiteral("root_path")).toString();
        definition.initDeviceParams = raw.value(QStringLiteral("init_device_params")).toObject();
        definition.customParams = raw.value(QStringLiteral("custom_params")).toObject();
        definition.raw = raw;
        config.agents.insert(definition.name, definition);
    }

    // 读取入口页需要使用的 primary_agents 列表。
    const QJsonArray primaryAgents = root.value(QStringLiteral("primary_agents")).toArray();
    for (const auto& value : primaryAgents) {
        config.primaryAgents.push_back(value.toString());
    }

    ConfigLoadResult result;
    result.config = config;
    result.success = true;
    return result;
}

RecordLabConfigLoadResult LegacyConfigLoader::loadRecordLabConfig(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        RecordLabConfigLoadResult result;
        result.error = QStringLiteral("Failed to open recordLabConfig: %1").arg(filePath);
        result.success = false;
        return result;
    }

    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        RecordLabConfigLoadResult result;
        result.error = QStringLiteral("recordLabConfig is not a JSON object: %1").arg(filePath);
        result.success = false;
        return result;
    }

    const QJsonObject root = document.object();
    RecordLabConfig config;
    config.version = root.value(QStringLiteral("version")).toString().trimmed();
    config.updateInfo = root.value(QStringLiteral("update_info")).toString().trimmed();

    const QJsonArray usbCatalog = root.value(QStringLiteral("usb_product_catalog")).toArray();
    for (const auto& value : usbCatalog) {
        const QJsonObject raw = value.toObject();
        UsbProductCatalogEntry entry;
        entry.raw = raw;
        entry.vid = parseHexOrInt(raw.value(QStringLiteral("vid")));
        entry.pid = parseHexOrInt(raw.value(QStringLiteral("pid")));
        entry.productNames = namesFromUsbCatalogEntry(raw);
        entry.displayName = raw.value(QStringLiteral("display_name")).toString().trimmed();
        if (entry.displayName.isEmpty()) {
            entry.displayName = entry.productNames.join(QStringLiteral("/"));
        }
        entry.agentName = raw.value(QStringLiteral("agent_name")).toString().trimmed();
        entry.sshPreferred = raw.value(QStringLiteral("ssh_preferred")).toBool(false);
        if (entry.vid >= 0 && entry.pid >= 0 && !entry.displayName.isEmpty()) {
            config.usbProductCatalog.push_back(entry);
        }
    }

    const QJsonObject scriptCatalog = root.value(QStringLiteral("script_catalog")).toObject();
    const QJsonObject agents = scriptCatalog.value(QStringLiteral("agents")).toObject();
    for (auto agentIt = agents.begin(); agentIt != agents.end(); ++agentIt) {
        QList<ScriptCatalogEntry> entries;
        const QJsonArray scripts = agentIt.value().toArray();
        for (const auto& scriptValue : scripts) {
            const QJsonObject raw = scriptValue.toObject();
            ScriptCatalogEntry entry;
            entry.relativePath = raw.value(QStringLiteral("relative_path")).toString().trimmed();
            entry.enabled = raw.value(QStringLiteral("enabled")).toBool(true);
            entry.raw = raw;

            const QJsonArray supportedIds = raw.value(QStringLiteral("supported_model_ids")).toArray();
            for (const auto& value : supportedIds) {
                const QString id = value.toString().trimmed();
                if (!id.isEmpty()) {
                    entry.supportedModelIds.push_back(id);
                }
            }

            const QJsonObject supportedModels = raw.value(QStringLiteral("supported_models")).toObject();
            for (auto modelIt = supportedModels.begin(); modelIt != supportedModels.end(); ++modelIt) {
                entry.supportedModels.insert(modelIt.key(), modelIt.value().toString());
            }

            const QJsonArray requiredAgents = raw.value(QStringLiteral("required_agents")).toArray();
            for (const auto& value : requiredAgents) {
                const QString agentName = value.toString().trimmed();
                if (!agentName.isEmpty()) {
                    entry.requiredAgents.push_back(agentName);
                }
            }

            if (!entry.relativePath.isEmpty()) {
                entries.push_back(entry);
            }
        }
        config.scriptsByAgent.insert(agentIt.key(), entries);
    }

    RecordLabConfigLoadResult result;
    result.config = config;
    result.success = true;
    return result;
}

}  // namespace recordlab::core
