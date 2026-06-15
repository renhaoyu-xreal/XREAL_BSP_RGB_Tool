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
 * 统一工作流控制器
 *
 * 从 BspWorkflowController 演化而来，同时服务 glasses_bsp_node 和 glasses_nviz_node。
 * 核心状态机（Connect → Watchdog → InitDevice → StartDevice → DeviceReady → Record）
 * 在两种 agent 下完全一致，唯一区别是：
 *   - nviz 的 start_device 默认补上 data_type=3dof
 *   - 合法 agent 集合包含 bsp 和 nviz 两个
 *
 * UI 页面（BspPage / AgentManagementPage / ScriptExecutionPage）
 * 统一依赖此控制器，避免两套状态机导致的代码倍增。
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

    /// 当前主 agent 是否属于受支持的 agent 集合（bsp 或 nviz）。
    bool isTargetAgentSelected() const;

    /// 便利：当前主 agent 是否为 BSP agent。
    bool isBspAgent() const;
    /// 便利：当前主 agent 是否为 Nviz agent。
    bool isNvizAgent() const;

    /// 便利：当前主 agent 是否为 Helen 无 Linux 系统眼镜 agent。
    bool isHelenAgent() const;

    /// 便利：当前主 agent 是否为 Android agent。
    bool isAndroidAgent() const;


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

    void requestAndroidOneClick();

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
    bool androidOneClickFlowActive_ = false;
    bool oneClickSucceeded_ = false;
    QTimer* watchdogWaitTimer_ = nullptr;
    bool oneClickStartDeviceDispatched_ = false;
    QVariantMap oneClickStartDeviceParamsOverride_;

    void appendLog(const QString& message);
    void transitionTo(State nextState, const QString& reason);
    /// 校验当前主 agent 是否在受支持的集合里（bsp / nviz）。
    bool ensureValidAgentSelected();
    void dispatchAgentCommand(const QString& cmdName, const QVariantMap& params = {});
    void startOneClick(const QVariantMap& initDeviceParamsOverride,
                       const QVariantMap& startDeviceParamsOverride);
    void dispatchOneClickStartDevice(const QString& reason);

    void armWatchdogWaitTimeout();
    void cancelWatchdogWaitTimeout();

    QString oneClickDeviceCommand() const;

    void setOneClickSucceeded(bool success);

    /// 为当前 agent 类型生成 start_device 的默认参数（nviz 补 data_type）。
    QVariantMap defaultStartDeviceParams() const;
    QVariantMap oneClickStartDeviceParams() const;
    QVariantMap androidOneClickParams() const;
    /// 返回人可读的 agent 标签，用于日志。
    QString agentLabel() const;
};

}  // namespace recordlab::workflow
