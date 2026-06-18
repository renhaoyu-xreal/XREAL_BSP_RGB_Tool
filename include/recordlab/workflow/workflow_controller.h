#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <nlohmann/json.hpp>

#include "recordlab/core/app_context.h"

namespace recordlab::workflow {

/*
 * RGB BSP 独立工作流控制器
 *
 * 负责 glasses_bsp_node 的 Connect → Watchdog → InitDevice → StartDevice
 * → DeviceReady → Record 状态流转。
 */
class WorkflowController : public QObject {
    Q_OBJECT

public:
    enum class State {
        Idle,
        CheckingConnection,
        AgentConnecting,
        WaitingWatchdog,
        DeviceInitializing,
        WaitingStartDeviceResponse,
        DeviceReady,
        RecordingRequested,
        Failed,
        EmergencyStop
    };
    Q_ENUM(State)

    explicit WorkflowController(const recordlab::core::AppContext& context, QObject* parent = nullptr);

    // 当前激活的 agent 一般由 WorkspacePage 传入。
    void setActiveAgent(const QString& agentName);
    QString activeAgent() const;
    State state() const;
    QStringList logHistory() const;

    /// 当前主 agent 是否属于受支持的 agent 集合。
    bool isTargetAgentSelected() const;

    /// 便利：当前主 agent 是否为 BSP agent。
    bool isBspAgent() const;

    QString watchdogSummary() const;
    QString activeAgentWatchdogState() const;
    QString activeAgentGlassesFsn() const;
    QString activeAgentGlassesProductLabel() const;
    int activeAgentGlassesProductId() const;
    bool oneClickSucceeded() const;

public slots:
    // 这些入口对应页面上的主要用户动作，BSP / Nviz 共用。
    void requestResetWorkflow();
    void requestOneClick();

    void requestOneClickWithInitDeviceParams(const QVariantMap& params);

    void requestOneClickWithStartDeviceParams(const QVariantMap& params);
    void requestOneClickWithInitAndStartDeviceParams(const QVariantMap& initParams,
                                                     const QVariantMap& startParams);
    void requestConnect();
    void requestCheck();
    void requestInitDevice();
    void requestStartDevice();
    void requestStartRecord();
    void requestStopRecord();
    void requestStopAllAgents();
    void requestExecuteCommand(const QString& agentName, const QString& cmdName,
                               const QVariantMap& params = {});
    void requestEmergencyStop();
    void handleCommandResult(const nlohmann::json& result);
    void handleStatusUpdate(const nlohmann::json& status);

signals:
    void activeAgentChanged(const QString& agentName);
    void stateChanged(recordlab::workflow::WorkflowController::State state);
    void logAppended(const QString& message);
    void watchdogSummaryChanged(const QString& summary);
    void activeAgentWatchdogStateChanged(const QString& state);
    void activeAgentDeviceInfoChanged(const QString& fsn, const QString& productLabel);
    void oneClickSuccessChanged(bool success);
    void actionRequested(const QString& actionName, const QVariantMap& payload);

private:
    const recordlab::core::AppContext& context_;
    QString activeAgent_;
    QString watchdogSummary_ = QStringLiteral("无监控");
    QString activeAgentWatchdogState_;
    QString activeAgentGlassesFsn_;
    QString activeAgentGlassesProductLabel_;
    int activeAgentGlassesProductId_ = -1;
    QHash<QString, QString> watchdogStatesByAgent_;
    QHash<QString, QString> glassesFsnsByAgent_;
    QHash<QString, QString> glassesProductLabelsByAgent_;
    QHash<QString, int> glassesProductIdsByAgent_;
    State state_ = State::Idle;
    QStringList logHistory_;
    bool oneClickFlowActive_ = false;
    bool oneClickSucceeded_ = false;
    QTimer* watchdogWaitTimer_ = nullptr;
    bool oneClickStartDeviceDispatched_ = false;
    QVariantMap oneClickStartDeviceParamsOverride_;

    void appendLog(const QString& message);
    void transitionTo(State nextState, const QString& reason);
    /// 校验当前主 agent 是否在受支持的集合里。
    bool ensureValidAgentSelected();
    void dispatchAgentCommand(const QString& cmdName, const QVariantMap& params = {});
    void startOneClick(const QVariantMap& initDeviceParamsOverride,
                       const QVariantMap& startDeviceParamsOverride);
    void dispatchOneClickStartDevice(const QString& reason);

    void armWatchdogWaitTimeout();
    void cancelWatchdogWaitTimeout();

    QString oneClickDeviceCommand() const;

    void setOneClickSucceeded(bool success);

    /// 为当前 BSP agent 生成 start_device 的默认参数。
    QVariantMap defaultStartDeviceParams() const;
    QVariantMap oneClickStartDeviceParams() const;
    /// 返回人可读的 agent 标签，用于日志。
    QString agentLabel() const;
};

}  // namespace recordlab::workflow
