#include "recordlab/workflow/workflow_controller.h"

#include "recordlab/common/commands.h"
#include "recordlab/core/compatibility_contract.h"

#include <QJsonDocument>
#include <QTimer>

/*
 * workflow_controller.cpp
 *
 * 从 bsp_workflow_controller.cpp 演化而来。
 * 唯一的结构性差异：
 *   - ensureBspAgentSelected() → ensureValidAgentSelected()：同时接受 bsp 和 nviz
 *   - requestOneClick() 和 dispatchOneClickStartDevice() 通过 defaultStartDeviceParams()
 *     为 nviz 自动补上 data_type=3dof
 *   - 日志里使用 agentLabel() 而不是硬编码 "glasses_bsp_node"
 */
namespace recordlab::workflow {

namespace {

QString jsonMessage(const nlohmann::json& result)
{
    try {
        if (result.contains("message") && result["message"].is_string()) {
            return QString::fromStdString(result["message"].get<std::string>());
        }
        if (result.contains("error") && result["error"].is_string()) {
            return QString::fromStdString(result["error"].get<std::string>());
        }
    } catch (...) {
    }
    return QString();
}

QString jsonStringValue(const nlohmann::json& result, const char* key)
{
    try {
        if (result.contains(key) && result[key].is_string()) {
            return QString::fromStdString(result[key].get<std::string>());
        }
    } catch (...) {
    }
    return QString();
}

QString paramsSummary(const QVariantMap& params)
{
    if (params.isEmpty()) {
        return QStringLiteral("{}");
    }
    const auto document = QJsonDocument::fromVariant(params);
    return QString::fromUtf8(document.toJson(QJsonDocument::Compact));
}

QString agentWatchdogState(const nlohmann::json& status, const QString& agentName)
{
    try {
        if (agentName.isEmpty() || !status.contains("agents")) {
            return QString();
        }

        const auto agentNameStd = agentName.toStdString();
        const auto& agents = status.at("agents");
        if (!agents.is_object() || !agents.contains(agentNameStd)) {
            return QString();
        }

        const auto& agent = agents.at(agentNameStd);
        if (agent.contains("state") && agent["state"].is_string()) {
            return QString::fromStdString(agent["state"].get<std::string>());
        }
    } catch (...) {
    }

    return QString();
}

QString jsonStringField(const nlohmann::json& object, const char* key)
{
    try {
        if (object.contains(key)) {
            if (object[key].is_string()) {
                return QString::fromStdString(object[key].get<std::string>());
            }
            if (object[key].is_number_integer()) {
                return QString::number(object[key].get<int>());
            }
        }
    } catch (...) {
    }
    return QString();
}

QString agentProductLabel(const nlohmann::json& agent)
{
    auto productId = jsonStringField(agent, "product_display_id");
    if (productId.isEmpty()) {
        productId = jsonStringField(agent, "product_id");
    }
    const auto productName = jsonStringField(agent, "product_name");
    if (!productId.isEmpty() && productId != QStringLiteral("-1") && !productName.isEmpty()) {
        return QStringLiteral("%1-%2").arg(productId, productName);
    }
    if (!productId.isEmpty() && productId != QStringLiteral("-1")) {
        return productId;
    }
    if (!productName.isEmpty()) {
        return productName;
    }
    return QString();
}

}  // namespace

WorkflowController::WorkflowController(const recordlab::core::AppContext& context, QObject* parent)
    : QObject(parent)
    , context_(context)
{
    watchdogWaitTimer_ = new QTimer(this);
    watchdogWaitTimer_->setSingleShot(true);
    watchdogWaitTimer_->setInterval(300000);
    connect(watchdogWaitTimer_, &QTimer::timeout, this, [this]() {
        if (!oneClickFlowActive_
            || (state_ != State::WaitingWatchdog && state_ != State::DeviceInitializing)) {
            return;
        }
        oneClickFlowActive_ = false;
        oneClickStartDeviceDispatched_ = false;
        transitionTo(State::Failed, QStringLiteral("等待 watchdog 自动初始化成功超时"));
    });
}

void WorkflowController::setActiveAgent(const QString& agentName)
{
    if (activeAgent_ == agentName) {
        return;
    }

    oneClickFlowActive_ = false;
    androidOneClickFlowActive_ = false;
    oneClickStartDeviceDispatched_ = false;
    oneClickStartDeviceParamsOverride_.clear();
    cancelWatchdogWaitTimeout();
    setOneClickSucceeded(false);

    activeAgent_ = agentName;
    appendLog(QStringLiteral("当前主 agent 已切换为 %1").arg(activeAgent_));
    if (state_ != State::Idle) {
        transitionTo(State::Idle, QStringLiteral("主 agent 已切换，工作流状态已重置"));
    }
    const auto nextWatchdogState = watchdogStatesByAgent_.value(activeAgent_);
    const auto nextFsn = glassesFsnsByAgent_.value(activeAgent_);
    const auto nextProductLabel = glassesProductLabelsByAgent_.value(activeAgent_);
    const auto nextProductId = glassesProductIdsByAgent_.value(activeAgent_, -1);
    if (activeAgentWatchdogState_ != nextWatchdogState) {
        activeAgentWatchdogState_ = nextWatchdogState;
        emit activeAgentWatchdogStateChanged(activeAgentWatchdogState_);
    }
    if (activeAgentGlassesFsn_ != nextFsn
        || activeAgentGlassesProductLabel_ != nextProductLabel
        || activeAgentGlassesProductId_ != nextProductId) {
        activeAgentGlassesFsn_ = nextFsn;
        activeAgentGlassesProductLabel_ = nextProductLabel;
        activeAgentGlassesProductId_ = nextProductId;
        emit activeAgentDeviceInfoChanged(activeAgentGlassesFsn_,
                                          activeAgentGlassesProductLabel_);
    }
    emit activeAgentChanged(activeAgent_);
}

QString WorkflowController::activeAgent() const
{
    return activeAgent_;
}

WorkflowController::State WorkflowController::state() const
{
    return state_;
}

QStringList WorkflowController::logHistory() const
{
    return logHistory_;
}

QString WorkflowController::watchdogSummary() const
{
    return watchdogSummary_;
}

QString WorkflowController::activeAgentWatchdogState() const
{
    return activeAgentWatchdogState_;
}

QString WorkflowController::activeAgentGlassesFsn() const
{
    return activeAgentGlassesFsn_;
}

QString WorkflowController::activeAgentGlassesProductLabel() const
{
    return activeAgentGlassesProductLabel_;
}

int WorkflowController::activeAgentGlassesProductId() const
{
    return activeAgentGlassesProductId_;
}

bool WorkflowController::oneClickSucceeded() const
{
    return oneClickSucceeded_;
}


bool WorkflowController::isTargetAgentSelected() const
{
    return activeAgent_ == QString::fromUtf8(recordlab::core::compat::kPrimaryBspAgent)
        || activeAgent_ == QString::fromUtf8(recordlab::core::compat::kPrimaryNvizAgent)
        || activeAgent_ == QString::fromUtf8(recordlab::core::compat::kPrimaryHelenAgent)
        || activeAgent_ == QString::fromUtf8(recordlab::core::compat::kPrimaryAndroidAgent);
}

bool WorkflowController::isBspAgent() const
{
    return activeAgent_ == QString::fromUtf8(recordlab::core::compat::kPrimaryBspAgent);
}

bool WorkflowController::isNvizAgent() const
{
    return activeAgent_ == QString::fromUtf8(recordlab::core::compat::kPrimaryNvizAgent);
}

bool WorkflowController::isHelenAgent() const
{
    return activeAgent_ == QString::fromUtf8(recordlab::core::compat::kPrimaryHelenAgent);
}
bool WorkflowController::isAndroidAgent() const
{
    return activeAgent_ == QString::fromUtf8(recordlab::core::compat::kPrimaryAndroidAgent);
}

void WorkflowController::requestResetWorkflow()
{
    oneClickFlowActive_ = false;
    androidOneClickFlowActive_ = false;
    oneClickStartDeviceDispatched_ = false;
    oneClickStartDeviceParamsOverride_.clear();
    setOneClickSucceeded(false);
    cancelWatchdogWaitTimeout();
    appendLog(QStringLiteral("手动重置工作流状态。"));
    transitionTo(State::Idle, QStringLiteral("工作流已回到空闲态"));
}

void WorkflowController::requestOneClick()
{
    startOneClick({}, {});
}

void WorkflowController::requestOneClickWithInitDeviceParams(const QVariantMap& params)
{
    startOneClick(params, {});
}

void WorkflowController::requestAndroidOneClick()
{
    if (!isNvizAgent()) {
        appendLog(QStringLiteral("Android IMU 一键启动只挂在 glasses_nviz_node 主链路下。"));
        return;
    }
    if (androidOneClickFlowActive_) {
        appendLog(QStringLiteral("Android IMU 一键启动仍在执行中，请等待当前流程完成。"));
        return;
    }

    androidOneClickFlowActive_ = true;
    appendLog(QStringLiteral("开始一键启动 Android IMU。"));
    transitionTo(State::AgentConnecting, QStringLiteral("已发出 Android Agent Connect 请求"));
    emit actionRequested(QString::fromUtf8(recordlab::common::ManagerAction::INIT_AGENT),
                         {{"agent_name", QStringLiteral("android")}});
}

void WorkflowController::requestOneClickWithStartDeviceParams(const QVariantMap& params)
{
    startOneClick({}, params);
}

void WorkflowController::requestOneClickWithInitAndStartDeviceParams(
    const QVariantMap& initParams,
    const QVariantMap& startParams)
{
    startOneClick(initParams, startParams);
}

void WorkflowController::startOneClick(const QVariantMap& initDeviceParamsOverride,
                                       const QVariantMap& startDeviceParamsOverride)
{
    if (!ensureValidAgentSelected()) {
        return;
    }

    if (oneClickFlowActive_ && state_ != State::Failed && state_ != State::EmergencyStop
        && state_ != State::DeviceReady) {
        appendLog(QStringLiteral("一键启动仍在执行中，请等待当前流程完成。"));
        return;
    }

    if (oneClickSucceeded_) {
        appendLog(QStringLiteral("一键启动已完成，无需重复执行。"));
        return;
    }

    oneClickFlowActive_ = true;
    oneClickStartDeviceDispatched_ = false;
    oneClickStartDeviceParamsOverride_ = startDeviceParamsOverride;
    cancelWatchdogWaitTimeout();
    if (!initDeviceParamsOverride.isEmpty()) {
        appendLog(QStringLiteral("本次 init_device 参数交由 watchdog 使用: %1")
                      .arg(paramsSummary(initDeviceParamsOverride)));
        emit actionRequested(
            QString::fromUtf8(recordlab::common::ManagerAction::SET_WATCHDOG_INIT_PARAMS),
            {{"agent_name", activeAgent_}, {"params", initDeviceParamsOverride}});
    }
    appendLog(QStringLiteral("开始一键启动 %1。").arg(agentLabel()));
    transitionTo(State::CheckingConnection, QStringLiteral("检查主 agent 是否已选中"));
    transitionTo(State::AgentConnecting, QStringLiteral("已发出 Connect 请求"));
    emit actionRequested(QString::fromUtf8(recordlab::common::ManagerAction::INIT_AGENT),
                         {{"agent_name", activeAgent_}});
}

void WorkflowController::requestConnect()
{
    if (!ensureValidAgentSelected()) {
        return;
    }

    oneClickFlowActive_ = false;
    androidOneClickFlowActive_ = false;
    oneClickStartDeviceDispatched_ = false;
    cancelWatchdogWaitTimeout();
    transitionTo(State::AgentConnecting, QStringLiteral("已排队 Connect 动作"));
    emit actionRequested(QString::fromUtf8(recordlab::common::ManagerAction::INIT_AGENT),
                         {{"agent_name", activeAgent_}});
}

void WorkflowController::requestCheck()
{
    if (!ensureValidAgentSelected()) {
        return;
    }

    oneClickFlowActive_ = false;
    androidOneClickFlowActive_ = false;
    oneClickStartDeviceDispatched_ = false;
    cancelWatchdogWaitTimeout();
    appendLog(QStringLiteral("已排队 check 请求"));
    dispatchAgentCommand(QStringLiteral("check"));
}

void WorkflowController::requestInitDevice()
{
    if (!ensureValidAgentSelected()) {
        return;
    }

    oneClickFlowActive_ = false;
    androidOneClickFlowActive_ = false;
    oneClickStartDeviceDispatched_ = false;
    cancelWatchdogWaitTimeout();
    transitionTo(State::DeviceInitializing, QStringLiteral("已排队 init_device 请求"));
    dispatchAgentCommand(QStringLiteral("init_device"));
}

void WorkflowController::requestStartDevice()
{
    if (!ensureValidAgentSelected()) {
        return;
    }

    oneClickFlowActive_ = false;
    androidOneClickFlowActive_ = false;
    oneClickStartDeviceDispatched_ = false;
    cancelWatchdogWaitTimeout();
    transitionTo(State::WaitingStartDeviceResponse, QStringLiteral("已排队 start_device 请求"));
    dispatchAgentCommand(QStringLiteral("start_device"), defaultStartDeviceParams());
}

void WorkflowController::requestStartRecord()
{
    if (!ensureValidAgentSelected()) {
        return;
    }

    oneClickFlowActive_ = false;
    androidOneClickFlowActive_ = false;
    oneClickStartDeviceDispatched_ = false;
    cancelWatchdogWaitTimeout();
    transitionTo(State::RecordingRequested, QStringLiteral("已排队 start_record 请求"));
    dispatchAgentCommand(QStringLiteral("start_record"));
}

void WorkflowController::requestStopRecord()
{
    if (!ensureValidAgentSelected()) {
        return;
    }

    oneClickFlowActive_ = false;
    androidOneClickFlowActive_ = false;
    oneClickStartDeviceDispatched_ = false;
    cancelWatchdogWaitTimeout();
    transitionTo(State::DeviceReady, QStringLiteral("已排队 stop_record 请求"));
    dispatchAgentCommand(QStringLiteral("stop_record"));
}

void WorkflowController::requestStopAllAgents()
{
    oneClickFlowActive_ = false;
    androidOneClickFlowActive_ = false;
    oneClickStartDeviceDispatched_ = false;
    setOneClickSucceeded(false);
    cancelWatchdogWaitTimeout();
    appendLog(QStringLiteral("已排队停止所有 Agent 请求。"));
    emit actionRequested(QString::fromUtf8(recordlab::common::ManagerAction::STOP_ALL_AGENTS),
                         {});
}

void WorkflowController::requestExecuteCommand(const QString& agentName,
                                                const QString& cmdName,
                                                const QVariantMap& params)
{
    const QString trimmedAgent = agentName.trimmed();
    const QString trimmedCommand = cmdName.trimmed();
    if (trimmedAgent.isEmpty()) {
        appendLog(QStringLiteral("未选择 Agent，忽略命令请求。"));
        return;
    }
    if (trimmedCommand.isEmpty()) {
        appendLog(QStringLiteral("命令名称为空，忽略执行。"));
        return;
    }

    if (activeAgent_ != trimmedAgent) {
        setActiveAgent(trimmedAgent);
    }

    oneClickFlowActive_ = false;
    androidOneClickFlowActive_ = false;
    oneClickStartDeviceDispatched_ = false;
    cancelWatchdogWaitTimeout();

    if (isTargetAgentSelected()) {
        if (trimmedCommand == QStringLiteral("init_device")) {
            transitionTo(State::DeviceInitializing, QStringLiteral("已排队 init_device 请求"));
        } else if (trimmedCommand == QStringLiteral("start_device")) {
            transitionTo(State::WaitingStartDeviceResponse,
                         QStringLiteral("已排队 start_device 请求"));
        } else if (trimmedCommand == QStringLiteral("start_record")) {
            transitionTo(State::RecordingRequested, QStringLiteral("已排队 start_record 请求"));
        } else if (trimmedCommand == QStringLiteral("stop_record")) {
            transitionTo(State::DeviceReady, QStringLiteral("已排队 stop_record 请求"));
        }
    }

    appendLog(QStringLiteral("执行命令: %1 - %2")
                  .arg(trimmedAgent, trimmedCommand));
    QVariantMap payload;
    payload.insert(QStringLiteral("agent_name"), trimmedAgent);
    payload.insert(QStringLiteral("cmd_name"), trimmedCommand);
    if (!params.isEmpty()) {
        payload.insert(QStringLiteral("params"), params);
    }
    emit actionRequested(QString::fromUtf8(recordlab::common::ManagerAction::SEND_AGENT_COMMAND),
                         payload);
}

void WorkflowController::requestEmergencyStop()
{
    if (activeAgent_.isEmpty()) {
        appendLog(QStringLiteral("未选择 agent，忽略 Emergency Stop。"));
        return;
    }

    oneClickFlowActive_ = false;
    androidOneClickFlowActive_ = false;
    oneClickStartDeviceDispatched_ = false;
    setOneClickSucceeded(false);
    cancelWatchdogWaitTimeout();
    transitionTo(State::EmergencyStop, QStringLiteral("已排队 Emergency Stop"));
    emit actionRequested(QString::fromUtf8(recordlab::common::ManagerAction::EMERGENCY_STOP),
                         {{"agent_name", activeAgent_},
                          {"reason", QStringLiteral("用户请求紧急停止")}});
}

void WorkflowController::handleCommandResult(const nlohmann::json& result)
{
    const auto action = jsonStringValue(result, "action");
    const auto cmdName = jsonStringValue(result, "cmd_name");
    const bool success = result.value("success", false);
    const auto message = jsonMessage(result);
    const bool isBackgroundSyncCall = result.contains("request_id");

    if (action == QString::fromUtf8(recordlab::common::ManagerAction::SEND_AGENT_COMMAND)
        && cmdName == QStringLiteral("check") && isBackgroundSyncCall) {
        return;
    }

    if (action == QString::fromUtf8(recordlab::common::ManagerAction::INIT_AGENT)) {
        if (androidOneClickFlowActive_ &&
            jsonStringValue(result, "agent_name") == QStringLiteral("android")) {
            appendLog(QStringLiteral("Connect Android %1: %2")
                          .arg(success ? QStringLiteral("成功") : QStringLiteral("失败"),
                               message.isEmpty() ? QStringLiteral("<no message>") : message));

            if (!success) {
                androidOneClickFlowActive_ = false;
                transitionTo(State::Failed, QStringLiteral("Android Agent Connect 失败"));
                return;
            }

            transitionTo(State::WaitingStartDeviceResponse,
                         QStringLiteral("Android Agent 已连接，开始执行 restart_device"));
            QVariantMap payload;
            payload.insert(QStringLiteral("agent_name"), QStringLiteral("android"));
            payload.insert(QStringLiteral("cmd_name"), QStringLiteral("restart_device"));
            payload.insert(QStringLiteral("params"), androidOneClickParams());
            emit actionRequested(QString::fromUtf8(recordlab::common::ManagerAction::SEND_AGENT_COMMAND),
                                 payload);
            return;
        }

        if (androidOneClickFlowActive_) {
            return;
        }

        appendLog(QStringLiteral("Connect %1: %2")
                      .arg(success ? QStringLiteral("成功") : QStringLiteral("失败"),
                           message.isEmpty() ? QStringLiteral("<no message>") : message));

        if (!success) {
            oneClickFlowActive_ = false;
            oneClickStartDeviceDispatched_ = false;
            cancelWatchdogWaitTimeout();
            transitionTo(State::Failed, QStringLiteral("Connect 失败"));
            return;
        }

        if (oneClickFlowActive_) {
            if (activeAgentWatchdogState_ == QStringLiteral("healthy")) {
                dispatchOneClickStartDevice(
                    QStringLiteral("Connect 成功，watchdog 已健康，开始执行 start_device"));
            } else if (activeAgentWatchdogState_ == QStringLiteral("initializing")) {
                armWatchdogWaitTimeout();
                transitionTo(State::DeviceInitializing,
                             QStringLiteral("Connect 成功，watchdog 正在执行设备初始化"));
            } else {
                armWatchdogWaitTimeout();
                transitionTo(State::WaitingWatchdog,
                             QStringLiteral("Connect 成功，等待 watchdog 完成设备初始化"));
            }
        } else if (activeAgentWatchdogState_ == QStringLiteral("healthy")) {
            cancelWatchdogWaitTimeout();
            transitionTo(State::DeviceReady, QStringLiteral("Connect 成功，设备已健康"));
        }
        return;
    }

    if (action == QString::fromUtf8(recordlab::common::ManagerAction::SEND_AGENT_COMMAND)) {
        if (androidOneClickFlowActive_ &&
            jsonStringValue(result, "agent_name") != QStringLiteral("android")) {
            return;
        }

        appendLog(QStringLiteral("%1 %2: %3")
                      .arg(cmdName,
                           success ? QStringLiteral("成功") : QStringLiteral("失败"),
                           message.isEmpty() ? QStringLiteral("<no message>") : message));

        if (cmdName == QStringLiteral("start_device")
            && message.contains(QStringLiteral("Already started"), Qt::CaseInsensitive)) {
            if (oneClickFlowActive_) {
                setOneClickSucceeded(true);
            }
            oneClickFlowActive_ = false;
            androidOneClickFlowActive_ = false;
            oneClickStartDeviceDispatched_ = false;
            cancelWatchdogWaitTimeout();
            transitionTo(State::DeviceReady, QStringLiteral("设备已经处于启动状态"));
            return;
        }

        if (!success) {
            if (isBackgroundSyncCall
                && cmdName == QStringLiteral("init_device")
                && oneClickFlowActive_
                && (state_ == State::WaitingWatchdog || state_ == State::DeviceInitializing)) {
                armWatchdogWaitTimeout();
                transitionTo(State::WaitingWatchdog,
                             QStringLiteral("watchdog init_device 尚未返回成功，继续等待自动恢复"));
                return;
            }
            if (cmdName == QStringLiteral("init_device")
                && message.contains(QStringLiteral("reentry detected"), Qt::CaseInsensitive)) {
                cancelWatchdogWaitTimeout();
                transitionTo(State::DeviceInitializing,
                             QStringLiteral("init_device 已在设备链路中执行，继续等待结果"));
                return;
            }
            if (androidOneClickFlowActive_ && cmdName == QStringLiteral("restart_device")) {
                androidOneClickFlowActive_ = false;
            }
            oneClickFlowActive_ = false;
            oneClickStartDeviceDispatched_ = false;
            cancelWatchdogWaitTimeout();
            transitionTo(State::Failed, QStringLiteral("%1 执行失败").arg(cmdName));
            return;
        }

        if (cmdName == QStringLiteral("init_device")) {
            if (isBackgroundSyncCall && oneClickFlowActive_) {
                armWatchdogWaitTimeout();
                transitionTo(State::DeviceInitializing,
                             QStringLiteral("watchdog init_device 已返回，等待健康状态确认"));
                return;
            }
            if (oneClickFlowActive_) {
                armWatchdogWaitTimeout();
                transitionTo(State::DeviceInitializing,
                             QStringLiteral("init_device 已返回，等待 watchdog 状态确认"));
                return;
            }
            return;
        }

        if (cmdName == QStringLiteral("start_device")) {
            if (oneClickFlowActive_) {
                setOneClickSucceeded(true);
            }
            oneClickFlowActive_ = false;
            oneClickStartDeviceDispatched_ = false;
            cancelWatchdogWaitTimeout();
            transitionTo(State::DeviceReady, QStringLiteral("设备已就绪，可以进入录制阶段"));
            return;
        }

        if (cmdName == QStringLiteral("restart_device")) {
            androidOneClickFlowActive_ = false;
            oneClickFlowActive_ = false;
            oneClickStartDeviceDispatched_ = false;
            cancelWatchdogWaitTimeout();
            transitionTo(State::DeviceReady, QStringLiteral("Android IMU 已就绪，可以进入录制阶段"));
            return;
        }

        if ((cmdName == QStringLiteral("stop_device")
             || cmdName == QStringLiteral("release_device")) && success) {
            setOneClickSucceeded(false);
        }

        if (cmdName == QStringLiteral("start_record")) {
            transitionTo(State::RecordingRequested, QStringLiteral("录制已启动"));
            return;
        }

        if (cmdName == QStringLiteral("stop_record")) {
            transitionTo(State::DeviceReady, QStringLiteral("录制已停止，设备仍保持就绪"));
            return;
        }
    }

    if (action == QString::fromUtf8(recordlab::common::ManagerAction::STOP_ALL_AGENTS)) {
        QStringList stoppedAgents;
        QStringList failedAgents;
        try {
            if (result.contains("stopped") && result["stopped"].is_array()) {
                for (const auto& item : result["stopped"]) {
                    if (item.is_string()) {
                        stoppedAgents.push_back(QString::fromStdString(item.get<std::string>()));
                    }
                }
            }
            if (result.contains("failed") && result["failed"].is_array()) {
                for (const auto& item : result["failed"]) {
                    if (item.is_string()) {
                        failedAgents.push_back(QString::fromStdString(item.get<std::string>()));
                    }
                }
            }
        } catch (...) {
        }

        if (!stoppedAgents.isEmpty()) {
            appendLog(QStringLiteral("已停止: %1").arg(stoppedAgents.join(QStringLiteral(", "))));
        }
        if (!failedAgents.isEmpty()) {
            appendLog(QStringLiteral("停止失败: %1").arg(failedAgents.join(QStringLiteral(", "))));
        }
        if (stoppedAgents.isEmpty() && failedAgents.isEmpty()) {
            appendLog(QStringLiteral("没有需要停止的 Agent。"));
        }

        if (success) {
            transitionTo(State::EmergencyStop, QStringLiteral("所有 Agent 停止流程已完成"));
        } else {
            transitionTo(State::Failed, QStringLiteral("停止所有 Agent 失败"));
        }
        return;
    }

    if (action == QString::fromUtf8(recordlab::common::ManagerAction::EMERGENCY_STOP)) {
        oneClickFlowActive_ = false;
        oneClickStartDeviceDispatched_ = false;
        cancelWatchdogWaitTimeout();
        appendLog(QStringLiteral("Emergency Stop 已执行。"));
        transitionTo(State::EmergencyStop, QStringLiteral("系统处于急停态"));
    }
}

void WorkflowController::handleStatusUpdate(const nlohmann::json& status)
{
    const auto summary = jsonStringValue(status, "summary");
    if (!summary.isEmpty() && summary != watchdogSummary_) {
        watchdogSummary_ = summary;
        appendLog(QStringLiteral("watchdog 状态更新: %1").arg(summary));
        emit watchdogSummaryChanged(watchdogSummary_);
    }

    if (status.contains("agents") && status["agents"].is_object()) {
        watchdogStatesByAgent_.clear();
        glassesFsnsByAgent_.clear();
        glassesProductLabelsByAgent_.clear();
        glassesProductIdsByAgent_.clear();
        try {
            for (auto it = status["agents"].begin(); it != status["agents"].end(); ++it) {
                const auto agentName = QString::fromStdString(it.key());
                if (it.value().contains("state") && it.value()["state"].is_string()) {
                    watchdogStatesByAgent_.insert(
                        agentName,
                        QString::fromStdString(it.value()["state"].get<std::string>()));
                }
                const auto fsn = jsonStringField(it.value(), "fsn");
                if (!fsn.isEmpty()) {
                    glassesFsnsByAgent_.insert(agentName, fsn);
                }
                const auto productLabel = agentProductLabel(it.value());
                if (!productLabel.isEmpty()) {
                    glassesProductLabelsByAgent_.insert(agentName, productLabel);
                }
                const auto productId = jsonStringField(it.value(), "product_id").toInt();
                if (productId > 0) {
                    glassesProductIdsByAgent_.insert(agentName, productId);
                }
            }
        } catch (...) {
        }
    }

    const auto agentState = agentWatchdogState(status, activeAgent_);
    const auto agentFsn = glassesFsnsByAgent_.value(activeAgent_);
    const auto agentProductLabel = glassesProductLabelsByAgent_.value(activeAgent_);
    const auto agentProductId = glassesProductIdsByAgent_.value(activeAgent_, -1);
    if (activeAgentGlassesFsn_ != agentFsn
        || activeAgentGlassesProductLabel_ != agentProductLabel
        || activeAgentGlassesProductId_ != agentProductId) {
        activeAgentGlassesFsn_ = agentFsn;
        activeAgentGlassesProductLabel_ = agentProductLabel;
        activeAgentGlassesProductId_ = agentProductId;
        emit activeAgentDeviceInfoChanged(activeAgentGlassesFsn_,
                                          activeAgentGlassesProductLabel_);
    }
    if (activeAgentWatchdogState_ != agentState) {
        activeAgentWatchdogState_ = agentState;
        if (!agentState.isEmpty()) {
            appendLog(QStringLiteral("agent %1 watchdog 状态: %2").arg(activeAgent_, agentState));
        }
        emit activeAgentWatchdogStateChanged(activeAgentWatchdogState_);
    }

    if (agentState.isEmpty()) {
        return;
    }

    if (androidOneClickFlowActive_) {
        // Android IMU 是 glasses_nviz_node 页面下的辅助链路；执行期间不要让
        // 眼镜 watchdog 的 disconnected/initializing 状态抢走 Android 流程状态。
        return;
    }

    if (agentState == QStringLiteral("healthy")) {
        cancelWatchdogWaitTimeout();
        if (oneClickFlowActive_
            && (state_ == State::WaitingWatchdog || state_ == State::DeviceInitializing)) {
            dispatchOneClickStartDevice(QStringLiteral("watchdog 初始化完成，开始执行 start_device"));
            return;
        }

        if (state_ == State::WaitingWatchdog) {
            transitionTo(State::DeviceReady, QStringLiteral("watchdog 报告设备已健康"));
        }
    } else if (agentState == QStringLiteral("initializing")) {
        if (oneClickSucceeded_) {
            setOneClickSucceeded(false);
        }
        if (oneClickFlowActive_) {
            armWatchdogWaitTimeout();
        }
        if (state_ == State::WaitingWatchdog || state_ == State::AgentConnecting) {
            transitionTo(State::DeviceInitializing, QStringLiteral("watchdog 正在执行 init_device"));
        }
    } else if (agentState == QStringLiteral("disconnected")) {
        if (oneClickSucceeded_) {
            setOneClickSucceeded(false);
        }
        if (oneClickFlowActive_
            && (state_ == State::AgentConnecting || state_ == State::WaitingWatchdog
                || state_ == State::DeviceInitializing)) {
            appendLog(QStringLiteral("watchdog 当前仍报告 disconnected，继续等待自动初始化。"));
            armWatchdogWaitTimeout();
            if (state_ == State::DeviceInitializing) {
                transitionTo(State::WaitingWatchdog,
                             QStringLiteral("watchdog 初始化尚未成功，继续等待自动恢复"));
            }
            return;
        }
        if (state_ != State::Idle && state_ != State::EmergencyStop) {
            transitionTo(State::Failed, QStringLiteral("watchdog 报告主 agent 已断开"));
        }
    }
}

void WorkflowController::appendLog(const QString& message)
{
    logHistory_.push_back(message);
    emit logAppended(message);
}

void WorkflowController::transitionTo(State nextState, const QString& reason)
{
    state_ = nextState;
    appendLog(QStringLiteral("[状态 %1] %2").arg(static_cast<int>(nextState)).arg(reason));
    emit stateChanged(state_);
}

bool WorkflowController::ensureValidAgentSelected()
{
    if (activeAgent_.isEmpty()) {
        appendLog(QStringLiteral("当前没有选中主 agent。"));
        return false;
    }

    if (!isTargetAgentSelected()) {
        appendLog(QStringLiteral("当前选中的 agent 是 %1，不在受支持的 agent 列表中（glasses_bsp_node / glasses_nviz_node / helen_node）。")
                      .arg(activeAgent_));
        return false;
    }

    return true;
}

void WorkflowController::dispatchAgentCommand(const QString& cmdName, const QVariantMap& params)
{
    QVariantMap payload;
    payload.insert(QStringLiteral("agent_name"), activeAgent_);
    payload.insert(QStringLiteral("cmd_name"), cmdName);
    if (!params.isEmpty()) {
        payload.insert(QStringLiteral("params"), params);
    }
    if (cmdName == QStringLiteral("init_device") || cmdName == QStringLiteral("start_device")) {
        appendLog(QStringLiteral("发送 %1 参数: %2").arg(cmdName, paramsSummary(params)));
    }
    emit actionRequested(QString::fromUtf8(recordlab::common::ManagerAction::SEND_AGENT_COMMAND), payload);
}

void WorkflowController::dispatchOneClickStartDevice(const QString& reason)
{
    if (oneClickStartDeviceDispatched_) {
        appendLog(QStringLiteral("%1 已发出，继续等待设备响应。").arg(oneClickDeviceCommand()));
        return;
    }

    oneClickStartDeviceDispatched_ = true;
    transitionTo(State::WaitingStartDeviceResponse, reason);
    dispatchAgentCommand(oneClickDeviceCommand(), oneClickStartDeviceParams());
}

QString WorkflowController::oneClickDeviceCommand() const
{
    return QStringLiteral("start_device");
}

void WorkflowController::armWatchdogWaitTimeout()
{
    if (watchdogWaitTimer_ && !watchdogWaitTimer_->isActive()) {
        watchdogWaitTimer_->start();
    }
}

void WorkflowController::setOneClickSucceeded(bool success)
{
    if (oneClickSucceeded_ == success) {
        return;
    }
    oneClickSucceeded_ = success;
    emit oneClickSuccessChanged(oneClickSucceeded_);
}

void WorkflowController::cancelWatchdogWaitTimeout()
{
    if (watchdogWaitTimer_) {
        watchdogWaitTimer_->stop();
    }
}

QVariantMap WorkflowController::defaultStartDeviceParams() const
{
    // Nviz 的 start_device 需要指定 data_type，对齐 Python 版的
    // _normalize_glasses_one_click_params() 中 params.setdefault("data_type", "3dof")
    if (isNvizAgent()) {
        return {{QStringLiteral("data_type"), QStringLiteral("3dof")}};
    }
    if (isHelenAgent()) {
        return {
            {QStringLiteral("camera_mode"), QStringLiteral("none")},
            {QStringLiteral("enable_display"), false},
        };
    }
    return {};
}

QVariantMap WorkflowController::androidOneClickParams() const
{
    return {{QStringLiteral("tcp_port"), 8100}};
}

QVariantMap WorkflowController::oneClickStartDeviceParams() const
{
    QVariantMap params = defaultStartDeviceParams();
    for (auto it = oneClickStartDeviceParamsOverride_.constBegin();
         it != oneClickStartDeviceParamsOverride_.constEnd(); ++it) {
        params.insert(it.key(), it.value());
    }
    return params;
}

QString WorkflowController::agentLabel() const
{
    if (isBspAgent()) {
        return QStringLiteral("Glasses_bsp_node");
    }
    if (isNvizAgent()) {
        return QStringLiteral("Glasses_nviz_node");
    }
    if (isHelenAgent()) {
        return QStringLiteral("helen_node");
    }
    if (isAndroidAgent()) {
        return QStringLiteral("Android");
    }
    return activeAgent_;
}

}  // namespace recordlab::workflow
