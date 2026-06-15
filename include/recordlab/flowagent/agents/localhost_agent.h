/*
 * LocalhostAgent - 本机控制Agent
 *
 * 用于控制本地计算机操作：
 * - 播放视频/音频
 * - 执行本地脚本
 * - 系统控制命令
 *
 * LocalhostAgent : BaseAgent — 无需 SubNode，直接执行本地操作
 */
#pragma once

#include "recordlab/flowagent/agents/base_agent.h"

namespace recordlab::flowagent::agents {

class LocalhostAgent : public BaseAgent {
  Q_OBJECT

public:
  explicit LocalhostAgent(const QString &name = "localhost",
                          const QString &scriptsDir = "./scripts",
                          const QVariantMap &customParams = {},
                          QObject *parent = nullptr);

  // 覆盖: 本机Agent无需远程连接
  CmdResult connect() override;
  CmdResult disconnect() override;

  // 覆盖: 路由到内置命令或脚本
  CmdResult cmd(const QString &cmdName, const nlohmann::json &params = {},
                bool waitForResult = true, double timeout = 30.0) override;

private:
  QString scriptsDir_;

  CmdResult cmdCheck();
  CmdResult cmdEstop();
  CmdResult cmdPlayVideo(const nlohmann::json &params);
  CmdResult cmdPlayAudio(const nlohmann::json &params);
  CmdResult cmdRunScript(const nlohmann::json &params, bool wait,
                         double timeout);
  CmdResult cmdStopAll();
};

} // namespace recordlab::flowagent::agents
