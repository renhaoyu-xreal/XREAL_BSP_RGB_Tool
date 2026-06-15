/*
 * ScriptExecutor 实现
 *
 * 这里的核心职责已经从“直接执行脚本文件”
 * 变成“启动本地兼容运行时，并把日志/退出状态稳定回传给 UI”。
 */
#include "recordlab/flowagent/core/script_executor.h"

#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <fstream>
#include <iostream>
#include <sstream>

namespace recordlab::flowagent::core {

namespace {

constexpr const char *kRuntimeEventPrefix = "__RECORDLAB_EVENT__ ";

QString jsonString(const nlohmann::json &value, const char *key,
                   const QString &fallback = {}) {
  if (!value.is_object() || !value.contains(key) || !value[key].is_string()) {
    return fallback;
  }
  return QString::fromStdString(value[key].get<std::string>());
}

bool jsonBool(const nlohmann::json &value, const char *key, bool fallback = false) {
  if (!value.is_object() || !value.contains(key) || !value[key].is_boolean()) {
    return fallback;
  }
  return value[key].get<bool>();
}

QStringList jsonStringList(const nlohmann::json &value, const char *key) {
  QStringList result;
  if (!value.is_object() || !value.contains(key) || !value[key].is_array()) {
    return result;
  }
  for (const auto &item : value[key]) {
    if (item.is_string()) {
      result << QString::fromStdString(item.get<std::string>());
    } else if (item.is_number_integer()) {
      result << QString::number(item.get<int64_t>());
    } else if (item.is_number_float()) {
      result << QString::number(item.get<double>());
    } else if (item.is_boolean()) {
      result << (item.get<bool>() ? QStringLiteral("true")
                                  : QStringLiteral("false"));
    }
  }
  return result;
}

void showTextMessage(const QString &kind, const QString &title,
                     const QString &message) {
  if (kind == QStringLiteral("error")) {
    QMessageBox::critical(nullptr, title, message);
  } else if (kind == QStringLiteral("warning")) {
    QMessageBox::warning(nullptr, title, message);
  } else {
    QMessageBox::information(nullptr, title, message);
  }
}

QString displayOrDash(const QString &value) {
  const auto text = value.trimmed();
  return text.isEmpty() ? QStringLiteral("--") : text;
}

QStringList productIdAndName(const QString &productLabel) {
  const QString label = productLabel.trimmed();
  if (label.isEmpty()) {
    return {QStringLiteral("--"), QStringLiteral("--")};
  }

  const int sep = label.indexOf(QLatin1Char('-'));
  if (sep > 0) {
    const QString productId = label.left(sep).trimmed();
    const QString productName = label.mid(sep + 1).trimmed();
    return {displayOrDash(productId), displayOrDash(productName)};
  }

  const QRegularExpression digitsOnly(QStringLiteral("^\\d+$"));
  if (digitsOnly.match(label).hasMatch()) {
    return {label, QStringLiteral("--")};
  }
  return {QStringLiteral("--"), label};
}

} // namespace

ScriptExecutor::ScriptExecutor(QObject *parent) : QObject(parent) {}

// 析构时停止当前脚本进程，避免对象释放后还有输出回调落到已销毁实例。
ScriptExecutor::~ScriptExecutor() { stopScript(); }

void ScriptExecutor::setRuntimeDeviceInfo(const QString &agentName,
                                          const QString &glassesFsn,
                                          const QString &glassesProductLabel) {
  runtimeAgentName_ = agentName;
  runtimeGlassesFsn_ = glassesFsn;
  runtimeGlassesProductLabel_ = glassesProductLabel;
}

ScriptExecutor::LoadResult
ScriptExecutor::loadScript(const std::string &scriptPath) {
  // 读取脚本源码用于基础校验和行数统计，不直接参与执行路径选择。
  QFileInfo fi(QString::fromStdString(scriptPath));
  if (!fi.exists()) {
    return {false, {}, "文件不存在: " + scriptPath};
  }
  if (fi.suffix() != "py") {
    return {false, {}, "不是 Python 文件: " + scriptPath};
  }

  std::ifstream file(scriptPath);
  if (!file.is_open()) {
    return {false, {}, "无法打开文件: " + scriptPath};
  }

  std::stringstream ss;
  ss << file.rdbuf();
  std::string code = ss.str();

  std::cout << "[ScriptExecutor] Loaded: " << scriptPath << " (" << code.size()
            << " bytes)" << std::endl;
  return {true, std::move(code), {}};
}

std::vector<std::string>
ScriptExecutor::extractRequiredAgents(const std::string &scriptCode) {
  // 从脚本源码提取 all_agent_names 列表，供批量执行前的 agent 预热使用。
  // Search for: all_agent_names = ["xxx", "yyy"]
  std::vector<std::string> agents;
  QRegularExpression re(R"(all_agent_names\s*=\s*\[([^\]]*)\])");
  auto match = re.match(QString::fromStdString(scriptCode));
  if (match.hasMatch()) {
    QString content = match.captured(1);
    QRegularExpression nameRe(R"("[^"]*"|'[^']*')");
    auto it = nameRe.globalMatch(content);
    while (it.hasNext()) {
      auto m = it.next();
      QString name = m.captured(0);
      name = name.mid(1, name.length() - 2); // strip quotes
      agents.push_back(name.toStdString());
    }
  }
  return agents;
}

std::shared_ptr<ScriptContext> ScriptExecutor::executeInProcess(
    const std::string &scriptPath,
    const std::vector<std::string> & /*agentNames*/) {
  // 兼容旧入口：直接用 python3 执行指定脚本，不额外包一层运行时。
  recordlab::script::ScriptCommand command;
  command.program = QStringLiteral("python3");
  command.arguments << QString::fromStdString(scriptPath);
  command.environment.insert(QStringLiteral("PYTHONUNBUFFERED"),
                             QStringLiteral("1"));
  return executeCommand(command, QString::fromStdString(scriptPath));
}

std::shared_ptr<ScriptContext>
ScriptExecutor::executeCommand(const recordlab::script::ScriptCommand &command,
                               const QString &scriptPath) {
  // 如有旧脚本先停掉，再创建新的执行上下文并启动外部进程。
  if (isRunning()) {
    stopScript();
  }

  context_ = std::make_shared<ScriptContext>();
  context_->scriptPath = scriptPath.toStdString();
  context_->isRunning = true;
  stopRequested_ = false;

  const auto result = loadScript(context_->scriptPath);
  if (result.success) {
    int lines = 0;
    for (char c : result.code) {
      if (c == '\n') {
        lines++;
      }
    }
    context_->totalLines = lines + 1;
  }

  startProcess(command, scriptPath);
  return context_;
}

void ScriptExecutor::startProcess(
    const recordlab::script::ScriptCommand &command,
    const QString &scriptPath) {
  // 配置 QProcess 的工作目录、环境变量和回调后，正式拉起脚本进程。
  process_ = std::make_unique<QProcess>();
  process_->setProcessChannelMode(QProcess::SeparateChannels);
  connect(process_.get(), &QProcess::readyReadStandardOutput, this,
          &ScriptExecutor::onProcessOutput);
  connect(process_.get(), &QProcess::readyReadStandardError, this,
          &ScriptExecutor::onProcessOutput);
  connect(process_.get(),
          qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          &ScriptExecutor::onProcessFinished);

  if (!command.workingDirectory.isEmpty()) {
    process_->setWorkingDirectory(command.workingDirectory);
  }

  auto environment = QProcessEnvironment::systemEnvironment();
  for (auto it = command.environment.constBegin();
       it != command.environment.constEnd(); ++it) {
    if (it.key() == QStringLiteral("PYTHONPATH")) {
      const auto existing = environment.value(QStringLiteral("PYTHONPATH"));
      if (!existing.isEmpty()) {
        environment.insert(it.key(), it.value() + QDir::listSeparator() + existing);
      } else {
        environment.insert(it.key(), it.value());
      }
      continue;
    }
    environment.insert(it.key(), it.value());
  }
  process_->setProcessEnvironment(environment);

  process_->start(command.program, command.arguments);
  if (!process_->waitForStarted(5000)) {
    context_->isRunning = false;
    context_->error = QStringLiteral("Failed to start process for %1")
                          .arg(scriptPath)
                          .toStdString();
    emit scriptCompleted(false, QString::fromStdString(context_->error));
    return;
  }

  emit scriptStarted(scriptPath);
  std::cout << "[ScriptExecutor] Started: " << scriptPath.toStdString()
            << std::endl;
}

void ScriptExecutor::stopScript() {
  // 优先请求脚本优雅退出；若长期无响应再升级为强制 kill。
  if (process_ && process_->state() != QProcess::NotRunning) {
    if (!stopRequested_) {
      stopRequested_ = true;
      process_->terminate();

      QPointer<QProcess> processGuard(process_.get());
      QTimer::singleShot(25000, this, [this, processGuard]() {
        if (!stopRequested_ || !process_ || process_.get() != processGuard ||
            process_->state() == QProcess::NotRunning) {
          return;
        }
        process_->kill();
      });
    }
  }
  if (context_)
    context_->isRunning = false;
}

bool ScriptExecutor::isRunning() const {
  // 根据底层进程状态判断当前是否仍有脚本在执行。
  return process_ && process_->state() != QProcess::NotRunning;
}

void ScriptExecutor::onProcessOutput() {
  // 统一收集 stdout/stderr。运行时事件走结构化通道，普通行转发给界面日志。
  if (!process_ || !context_)
    return;

  processOutputBytes(process_->readAllStandardOutput(), stdoutBuffer_);
  processOutputBytes(process_->readAllStandardError(), stderrBuffer_);
}

void ScriptExecutor::processOutputBytes(const QByteArray &data, QByteArray &buffer) {
  if (data.isEmpty()) {
    return;
  }
  buffer.append(data);
  while (true) {
    const int newline = buffer.indexOf('\n');
    if (newline < 0) {
      break;
    }
    const QByteArray lineBytes = buffer.left(newline);
    buffer.remove(0, newline + 1);
    const QString line = QString::fromUtf8(lineBytes).trimmed();
    if (!line.isEmpty()) {
      processOutputLine(line);
    }
  }
}

void ScriptExecutor::processOutputLine(const QString &line) {
  if (!context_) {
    return;
  }
  if (handleRuntimeEvent(line)) {
    return;
  }

  context_->output.push_back(line.toStdString());
  emit scriptLog(line);
}

bool ScriptExecutor::handleRuntimeEvent(const QString &line) {
  if (!line.startsWith(QString::fromUtf8(kRuntimeEventPrefix))) {
    return false;
  }

  const QString payloadText = line.mid(QString::fromUtf8(kRuntimeEventPrefix).size());
  try {
    const auto event = nlohmann::json::parse(payloadText.toStdString());
    const QString type = jsonString(event, "type");
    if (type == QStringLiteral("dialog")) {
      handleDialogEvent(event);
    } else if (type == QStringLiteral("workflow")) {
      handleWorkflowEvent(event);
    }
  } catch (const std::exception &e) {
    emit scriptLog(QStringLiteral("[runtime] 解析事件失败: %1").arg(e.what()));
  }
  return true;
}

void ScriptExecutor::handleDialogEvent(const nlohmann::json &event) {
  const QString id = jsonString(event, "id");
  const QString kind = jsonString(event, "kind", QStringLiteral("info"));
  const QString title = jsonString(event, "title", QStringLiteral("脚本提示"));
  const QString message = jsonString(event, "message");

  nlohmann::json response = {
      {"id", id.toStdString()},
      {"success", true},
      {"cancelled", false},
  };

  if (kind == QStringLiteral("question")) {
    const auto choice = QMessageBox::question(nullptr, title, message,
                                              QMessageBox::Yes | QMessageBox::No,
                                              QMessageBox::Yes);
    response["response"] = (choice == QMessageBox::Yes);
  } else if (kind == QStringLiteral("input")) {
    QDialog dialog;
    dialog.setWindowTitle(title);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(message, &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto *edit = new QLineEdit(jsonString(event, "default"), &dialog);
    layout->addWidget(edit);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted) {
      response["response"] = edit->text().toStdString();
    } else {
      response["cancelled"] = true;
    }
  } else if (kind == QStringLiteral("multi_field_input")) {
    QDialog dialog;
    dialog.setWindowTitle(title);
    dialog.resize(520, 320);
    auto *layout = new QVBoxLayout(&dialog);
    if (!message.isEmpty()) {
      auto *label = new QLabel(message, &dialog);
      label->setWordWrap(true);
      layout->addWidget(label);
    }
    const auto productParts = productIdAndName(runtimeGlassesProductLabel_);
    auto *deviceLabel = new QLabel(
        QStringLiteral("当前 Agent：%1\n眼镜型号：%2\n眼镜名称：%3\n眼镜 FSN：%4")
            .arg(displayOrDash(runtimeAgentName_),
                 productParts.value(0, QStringLiteral("--")),
                 productParts.value(1, QStringLiteral("--")),
                 displayOrDash(runtimeGlassesFsn_)),
        &dialog);
    deviceLabel->setWordWrap(true);
    deviceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    deviceLabel->setStyleSheet(QStringLiteral(
        "QLabel { background-color: #FFFDF2; border: 1px solid #C8B36A; "
        "padding: 8px; }"));
    layout->addWidget(deviceLabel);

    auto *form = new QFormLayout();
    struct FieldInput {
      QString name;
      QLineEdit *edit = nullptr;
      QComboBox *combo = nullptr;
    };
    std::vector<FieldInput> inputs;
    if (event.contains("fields") && event["fields"].is_array()) {
      for (const auto &field : event["fields"]) {
        const QString name = jsonString(field, "name");
        const QString label = jsonString(field, "label", name);
        QStringList choices = jsonStringList(field, "choices");
        if (choices.isEmpty()) {
          choices = jsonStringList(field, "options");
        }
        if (!choices.isEmpty()) {
          auto *combo = new QComboBox(&dialog);
          combo->addItems(choices);
          const QString defaultValue = jsonString(field, "default");
          const int defaultIndex = combo->findText(defaultValue);
          if (defaultIndex >= 0) {
            combo->setCurrentIndex(defaultIndex);
          }
          form->addRow(label, combo);
          inputs.push_back({name, nullptr, combo});
        } else {
          auto *edit = new QLineEdit(jsonString(field, "default"), &dialog);
          form->addRow(label, edit);
          inputs.push_back({name, edit, nullptr});
        }
      }
    }
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted) {
      nlohmann::json values = nlohmann::json::object();
      for (const auto &input : inputs) {
        if (input.combo) {
          values[input.name.toStdString()] =
              input.combo->currentText().toStdString();
        } else if (input.edit) {
          values[input.name.toStdString()] = input.edit->text().toStdString();
        }
      }
      response["response"] = values;
    } else {
      response["cancelled"] = true;
    }
  } else {
    showTextMessage(kind, title, message);
    response["response"] = true;
  }

  sendRuntimeResponse(response);
}

void ScriptExecutor::handleWorkflowEvent(const nlohmann::json &event) {
  const QString action = jsonString(event, "action");
  if (action == QStringLiteral("clear")) {
    emit workflowCleared();
    return;
  }

  const QString title = jsonString(event, "title", QStringLiteral("脚本流程"));
  const QString message = jsonString(event, "message");
  const QString stepsJson =
      event.contains("steps") ? QString::fromStdString(event["steps"].dump()) : QStringLiteral("[]");
  const bool finished = jsonBool(event, "finished", false);
  const bool success = jsonBool(event, "success", false);
  emit workflowUpdated(title, message, stepsJson, finished, success);
}

void ScriptExecutor::sendRuntimeResponse(const nlohmann::json &response) {
  if (!process_ || process_->state() == QProcess::NotRunning) {
    return;
  }
  QByteArray bytes = QByteArray::fromStdString(response.dump());
  bytes.append('\n');
  process_->write(bytes);
  process_->waitForBytesWritten(1000);
}

void ScriptExecutor::onProcessFinished(int exitCode,
                                       QProcess::ExitStatus exitStatus) {
  // 进程结束后统一折叠成成功/失败结果，并更新上下文状态。
  if (!context_)
    return;

  processOutputBytes({}, stdoutBuffer_);
  processOutputBytes({}, stderrBuffer_);
  if (!stdoutBuffer_.trimmed().isEmpty()) {
    processOutputLine(QString::fromUtf8(stdoutBuffer_).trimmed());
  }
  if (!stderrBuffer_.trimmed().isEmpty()) {
    processOutputLine(QString::fromUtf8(stderrBuffer_).trimmed());
  }
  stdoutBuffer_.clear();
  stderrBuffer_.clear();

  context_->isRunning = false;
  const bool success =
      (!stopRequested_ && exitCode == 0 && exitStatus == QProcess::NormalExit);

  if (stopRequested_) {
    context_->error = "Script stopped by user";
  } else if (!success) {
    context_->error = "Process exited with code " + std::to_string(exitCode);
  }

  emit scriptCompleted(success, QString::fromStdString(context_->error));
  std::cout << "[ScriptExecutor] Finished: " << (success ? "success" : "failed")
            << " (exit " << exitCode << ")" << std::endl;
  stopRequested_ = false;
}

} // namespace recordlab::flowagent::core
