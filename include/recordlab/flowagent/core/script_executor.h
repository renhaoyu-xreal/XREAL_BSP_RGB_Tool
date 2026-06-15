/*
 * ScriptExecutor - 脚本执行器
 *
 * 当前继续通过 QProcess 启动独立 Python 进程，
 * 但已经改为统一走 RecordLabC 本地脚本兼容运行时，
 * 而不是直接把业务脚本本身丢给 python3。
 *
 */
#pragma once

#include <QMutex>
#include <QObject>
#include <QProcess>
#include <QByteArray>
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "recordlab/script/python_script_bridge.h"

namespace recordlab::flowagent::core {

// ============================================================================
// ScriptContext — 脚本执行上下文
// ============================================================================

struct ScriptContext {
  std::string scriptPath;
  std::vector<std::string> output;
  std::string error;
  std::atomic<bool> isRunning{false};
  int currentLine = 0;
  int totalLines = 0;
};

// ============================================================================
// ScriptExecutor — 脚本执行器
// ============================================================================

class ScriptExecutor : public QObject {
  Q_OBJECT

public:
  explicit ScriptExecutor(QObject *parent = nullptr);
  ~ScriptExecutor() override;

  // ========== 加载 ==========

  struct LoadResult {
    bool success;
    std::string code;
    std::string error;
  };
  LoadResult loadScript(const std::string &scriptPath);

  // ========== 提取 ==========

  std::vector<std::string> extractRequiredAgents(const std::string &scriptCode);

  // ========== 执行 ==========

  std::shared_ptr<ScriptContext>
  executeInProcess(const std::string &scriptPath,
                   const std::vector<std::string> &agentNames = {});

  std::shared_ptr<ScriptContext>
  executeCommand(const recordlab::script::ScriptCommand &command,
                 const QString &scriptPath);

  void stopScript();
  bool isRunning() const;
  void setRuntimeDeviceInfo(const QString &agentName,
                            const QString &glassesFsn,
                            const QString &glassesProductLabel);

  // ========== 状态 ==========

  std::shared_ptr<ScriptContext> currentContext() { return context_; }

signals:
  void scriptStarted(const QString &scriptPath);
  void scriptProgress(int currentLine, int totalLines);
  void scriptLog(const QString &message);
  void workflowUpdated(const QString &title, const QString &message,
                       const QString &stepsJson, bool finished, bool success);
  void workflowCleared();
  void scriptCompleted(bool success, const QString &error);

private slots:
  void onProcessOutput();
  void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
  void startProcess(const recordlab::script::ScriptCommand &command,
                    const QString &scriptPath);
  void processOutputBytes(const QByteArray &data, QByteArray &buffer);
  void processOutputLine(const QString &line);
  bool handleRuntimeEvent(const QString &line);
  void handleDialogEvent(const nlohmann::json &event);
  void handleWorkflowEvent(const nlohmann::json &event);
  void sendRuntimeResponse(const nlohmann::json &response);

  std::unique_ptr<QProcess> process_;
  std::shared_ptr<ScriptContext> context_;
  QByteArray stdoutBuffer_;
  QByteArray stderrBuffer_;
  QString runtimeAgentName_;
  QString runtimeGlassesFsn_;
  QString runtimeGlassesProductLabel_;
  bool stopRequested_ = false;
};

} // namespace recordlab::flowagent::core
