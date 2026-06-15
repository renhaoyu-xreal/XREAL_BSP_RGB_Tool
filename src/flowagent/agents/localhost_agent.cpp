/*
 * LocalhostAgent - 本机控制Agent 实现
 *
 */

#include "recordlab/flowagent/agents/localhost_agent.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>

#include <iostream>

namespace recordlab::flowagent::agents {

LocalhostAgent::LocalhostAgent(const QString &name, const QString &scriptsDir,
                               const QVariantMap &customParams, QObject *parent)
    : BaseAgent(name,
                QString(),   // subnode_path = "" (无需SubNode)
                "localhost", // subnode_host
                0,           // goal_port (不使用)
                0,           // feedback_port (不使用)
                "./output", customParams, parent),
      scriptsDir_(scriptsDir) {
  QDir().mkpath(scriptsDir_);
  std::cout << "[" << name.toStdString()
            << "] LocalhostAgent initialized, scripts_dir: "
            << scriptsDir_.toStdString() << std::endl;
}

CmdResult LocalhostAgent::connect() {
  // 本机 agent 不需要远程握手，但仍需显式标记 connected 以兼容上层状态机。
  std::cout << "[" << name().toStdString()
            << "] LocalhostAgent ready (no remote connection needed)"
            << std::endl;
  // 本机 agent 不依赖 SubNode / ActionClient，但仍要标记为已连接，
  // 否则上层 stop/check 逻辑会把它当成未初始化对象。
  setConnectedState(true);
  return {true, "Localhost agent ready"};
}

CmdResult LocalhostAgent::disconnect() {
  // 清除连接标志，表示本机控制 agent 已退出可用状态。
  std::cout << "[" << name().toStdString() << "] LocalhostAgent disconnected"
            << std::endl;
  setConnectedState(false);
  return {true, "Disconnected"};
}

CmdResult LocalhostAgent::cmd(const QString &cmdName,
                              const nlohmann::json &params, bool waitForResult,
                              double timeout) {
  // 将通用命令名分发到本地播放、脚本执行和急停等具体实现。
  if (cmdName == "check")
    return cmdCheck();
  if (cmdName == "estop")
    return cmdEstop();
  if (cmdName == "play_video")
    return cmdPlayVideo(params);
  if (cmdName == "play_audio")
    return cmdPlayAudio(params);
  if (cmdName == "run_script")
    return cmdRunScript(params, waitForResult, timeout);
  if (cmdName == "stop_all")
    return cmdStopAll();

  // 尝试作为脚本名执行
  nlohmann::json scriptParams = params;
  scriptParams["script"] = cmdName.toStdString();
  return cmdRunScript(scriptParams, waitForResult, timeout);
}

CmdResult LocalhostAgent::cmdCheck() { return {true, "Localhost is ready"}; }

CmdResult LocalhostAgent::cmdEstop() {
  // 急停通过 pkill 清理常见媒体进程，保证本机播放类任务立即终止。
  std::cout << "[" << name().toStdString()
            << "] Emergency stop: stopping all processes" << std::endl;

  QStringList targets = {"vlc", "mpv", "mplayer", "ffplay"};
  QStringList stopped, failed;

  for (const auto &proc : targets) {
    QProcess pkill;
    pkill.start("pkill", {"-9", proc});
    if (pkill.waitForFinished(2000) && pkill.exitCode() == 0) {
      stopped << proc;
    }
  }

  QString msg = stopped.isEmpty()
                    ? "Estop completed. No running processes found"
                    : "Estop completed. Stopped: " + stopped.join(", ");

  return {true, msg};
}

CmdResult LocalhostAgent::cmdPlayVideo(const nlohmann::json &params) {
  // 使用指定播放器异步打开视频文件，默认回退到系统 xdg-open。
  std::string file = params.value("file", std::string());
  if (file.empty())
    return {false, "Missing 'file' parameter"};
  if (!QFileInfo::exists(QString::fromStdString(file)))
    return {
        false,
        QString("Video file not found: %1").arg(QString::fromStdString(file))};

  std::string player = params.value("player", std::string("xdg-open"));
  QStringList args;
  if (player == "vlc")
    args << QString::fromStdString(file);
  else if (player == "mpv")
    args << QString::fromStdString(file);
  else
    args << QString::fromStdString(file);

  QProcess::startDetached(QString::fromStdString(player), args);
  return {true, QString("Video playing: %1").arg(QString::fromStdString(file))};
}

CmdResult LocalhostAgent::cmdPlayAudio(const nlohmann::json &params) {
  // 使用指定播放器异步播放音频文件，并按播放器类型补齐参数。
  std::string file = params.value("file", std::string());
  if (file.empty())
    return {false, "Missing 'file' parameter"};
  if (!QFileInfo::exists(QString::fromStdString(file)))
    return {
        false,
        QString("Audio file not found: %1").arg(QString::fromStdString(file))};

  std::string player = params.value("player", std::string("xdg-open"));
  QStringList args;
  if (player == "mpv")
    args << "--no-video" << QString::fromStdString(file);
  else if (player == "vlc")
    args << QString::fromStdString(file);
  else if (player == "aplay")
    args << QString::fromStdString(file);
  else
    args << QString::fromStdString(file);

  QProcess::startDetached(QString::fromStdString(player), args);
  return {true, QString("Audio playing: %1").arg(QString::fromStdString(file))};
}

CmdResult LocalhostAgent::cmdRunScript(const nlohmann::json &params, bool wait,
                                       double timeout) {
  // 在 scriptsDir 或显式路径中定位脚本，并支持同步/异步两种执行方式。
  std::string scriptName = params.value("script", std::string());
  if (scriptName.empty())
    return {false, "Missing 'script' parameter"};

  QString scriptPath =
      QDir(scriptsDir_).filePath(QString::fromStdString(scriptName));
  if (!QFileInfo::exists(scriptPath)) {
    scriptPath = QString::fromStdString(scriptName);
  }
  if (!QFileInfo::exists(scriptPath)) {
    return {false, QString("Script not found: %1")
                       .arg(QString::fromStdString(scriptName))};
  }

  QStringList scriptArgs;
  if (params.contains("args") && params["args"].is_array()) {
    for (const auto &arg : params["args"]) {
      scriptArgs << QString::fromStdString(arg.get<std::string>());
    }
  }

  QString program;
  QStringList allArgs;
  if (scriptPath.endsWith(".sh")) {
    program = "bash";
    allArgs << scriptPath << scriptArgs;
  } else if (scriptPath.endsWith(".py")) {
    program = "python3";
    allArgs << scriptPath << scriptArgs;
  } else {
    program = scriptPath;
    allArgs = scriptArgs;
  }

  if (wait) {
    QProcess proc;
    proc.start(program, allArgs);
    int timeoutMs = static_cast<int>(timeout * 1000);
    if (!proc.waitForFinished(timeoutMs)) {
      proc.kill();
      return {false, QString("Script timeout after %1s").arg(timeout)};
    }
    if (proc.exitCode() == 0) {
      return {true, "Script executed successfully"};
    }
    return {
        false, "Script failed",
        nlohmann::json{{"error", proc.readAllStandardError().toStdString()},
                       {"output", proc.readAllStandardOutput().toStdString()}}};
  }

  QProcess::startDetached(program, allArgs);
  return {true, QString("Script started in background: %1")
                    .arg(QString::fromStdString(scriptName))};
}

CmdResult LocalhostAgent::cmdStopAll() {
  // 统一停止常见媒体播放器，作为本机 agent 的收尾命令。
  std::cout << "[" << name().toStdString() << "] Stopping all media players"
            << std::endl;
  for (const auto &proc : {"vlc", "mpv", "mplayer"}) {
    QProcess::execute("pkill", {proc});
  }
  return {true, "Stop commands sent"};
}

} // namespace recordlab::flowagent::agents
