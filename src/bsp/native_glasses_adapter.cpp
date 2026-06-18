#include "recordlab/bsp/native_glasses_adapter.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <iostream>

#include "recordlab/bsp/xreal_runtime_locator.h"
#include "recordlab/core/compatibility_contract.h"

namespace recordlab::bsp {

namespace {

constexpr char kBridgeMagic[4] = {'R', 'L', 'C', 'B'};
constexpr int kBridgeFrameHeaderSize = 12;
constexpr int kCreateGlassesTimeoutMs = 30000;
constexpr int kCreateGlassesRetryCount = 3;
constexpr int kCreateGlassesRetryDelayMs = 1500;

struct BridgeGlassesHandle {
  int productId = -1;
};

bool isProjectRootCandidate(const QString &path) {
  // 通过关键 vendored 资产判断一个目录是否像有效的工程根目录。
  if (path.isEmpty()) {
    return false;
  }

  const QDir dir(path);
  return QFileInfo::exists(dir.filePath(QString::fromUtf8(
             recordlab::core::compat::kAppConfigRelativePath))) &&
         QFileInfo::exists(dir.filePath(QString::fromUtf8(
             recordlab::core::compat::kAppWheelRelativePath)));
}

nlohmann::json stringListToJsonArray(const QStringList &items) {
  // 将 QStringList 转成 JSON 数组，便于预检结果直接序列化输出。
  nlohmann::json result = nlohmann::json::array();
  for (const auto &item : items) {
    result.push_back(item.toStdString());
  }
  return result;
}

void appendUint32Le(QByteArray &buffer, quint32 value) {
  // 以 little-endian 方式写入 32 位长度字段，用于自定义桥协议。
  buffer.append(static_cast<char>(value & 0xFFu));
  buffer.append(static_cast<char>((value >> 8u) & 0xFFu));
  buffer.append(static_cast<char>((value >> 16u) & 0xFFu));
  buffer.append(static_cast<char>((value >> 24u) & 0xFFu));
}

quint32 readUint32Le(const char *data) {
  // 从桥协议帧头中读取 little-endian 32 位长度字段。
  return static_cast<quint32>(static_cast<unsigned char>(data[0])) |
         (static_cast<quint32>(static_cast<unsigned char>(data[1])) << 8u) |
         (static_cast<quint32>(static_cast<unsigned char>(data[2])) << 16u) |
         (static_cast<quint32>(static_cast<unsigned char>(data[3])) << 24u);
}

QByteArray buildFrame(const nlohmann::json &header, const QByteArray &payload) {
  // 将 header JSON 与二进制 payload 打包成桥进程约定的帧格式。
  const auto headerString = header.dump();
  const QByteArray headerBytes(headerString.data(),
                               static_cast<qsizetype>(headerString.size()));

  QByteArray frame;
  frame.reserve(kBridgeFrameHeaderSize + headerBytes.size() + payload.size());
  frame.append(kBridgeMagic, 4);
  appendUint32Le(frame, static_cast<quint32>(headerBytes.size()));
  appendUint32Le(frame, static_cast<quint32>(payload.size()));
  frame.append(headerBytes);
  frame.append(payload);
  return frame;
}

QString joinPathEntries(const QStringList &entries) {
  // 拼接去重后的 PATH/PYTHONPATH/LD_LIBRARY_PATH 条目。
  QStringList filtered = entries;
  filtered.removeAll(QString());
  filtered.removeDuplicates();
  return filtered.join(QLatin1Char(':'));
}

QString executableCandidate(const QString &rawCandidate) {
  // 解析一个可能是名字或绝对路径的可执行文件候选值。
  const QString candidate = rawCandidate.trimmed();
  if (candidate.isEmpty()) {
    return {};
  }

  if (candidate.contains(QLatin1Char('/'))) {
    return QFileInfo::exists(candidate) ? candidate : QString();
  }

  return QStandardPaths::findExecutable(candidate);
}

QString imageFormatName(int qtFormat) {
  // 将常见 QImage::Format 值映射成桥消息里的格式名称。
  switch (qtFormat) {
  case 24: // QImage::Format_Grayscale8
    return QStringLiteral("grayscale8");
  case 13: // QImage::Format_RGB888
    return QStringLiteral("rgb888");
  case 17: // QImage::Format_RGBA8888
    return QStringLiteral("rgba8888");
  default:
    return QStringLiteral("grayscale8");
  }
}

} // namespace

bool NativeGlassesPreflightReport::canCreateGlasses() const {
  // 只有项目根、wheel、pyi、runtime 和桥脚本都可用时才允许 createGlasses。
  return projectRootDetected && wheelAvailable && pyiAvailable &&
         runtimeQtCompatible && nativeBridgeImplemented;
}

QString NativeGlassesPreflightReport::summary() const {
  // 生成适合日志与 UI 展示的一行预检摘要。
  return QStringLiteral(
             "project-root:%1 | wheel:%2 | pyi:%3 | runtime-root:%4 | "
             "runtime-qt:%5(%6) | required-qt:%7 | bridge:%8 | creatable:%9")
      .arg(projectRootDetected ? QStringLiteral("ok")
                               : QStringLiteral("missing"),
           wheelAvailable ? QStringLiteral("ok") : QStringLiteral("missing"),
           pyiAvailable ? QStringLiteral("ok") : QStringLiteral("missing"),
           runtimeRootAvailable ? QStringLiteral("ok")
                                : QStringLiteral("missing"),
           runtimeQtVersion, runtimeQtSource, requiredQtVersion,
           nativeBridgeImplemented ? QStringLiteral("ready")
                                   : QStringLiteral("missing"),
           canCreateGlasses() ? QStringLiteral("yes") : QStringLiteral("no"));
}

nlohmann::json NativeGlassesPreflightReport::toJson() const {
  // 将完整预检结果序列化成 JSON，供 UI 和脚本直接消费。
  return {
      {"project_root", projectRoot.toStdString()},
      {"wheel_path", wheelPath.toStdString()},
      {"pyi_path", pyiPath.toStdString()},
      {"runtime_root", runtimeRoot.toStdString()},
      {"runtime_site_packages", runtimeSitePackages.toStdString()},
      {"runtime_glasses_server_path", runtimeGlassesServerPath.toStdString()},
      {"current_process_qt", currentProcessQtVersion.toStdString()},
      {"runtime_qt", runtimeQtVersion.toStdString()},
      {"runtime_qt_source", runtimeQtSource.toStdString()},
      {"required_qt", requiredQtVersion.toStdString()},
      {"project_root_detected", projectRootDetected},
      {"wheel_available", wheelAvailable},
      {"pyi_available", pyiAvailable},
      {"runtime_root_available", runtimeRootAvailable},
      {"runtime_site_packages_available", runtimeSitePackagesAvailable},
      {"runtime_glasses_server_available", runtimeGlassesServerAvailable},
      {"runtime_qt_compatible", runtimeQtCompatible},
      {"native_bridge_implemented", nativeBridgeImplemented},
      {"can_create_glasses", canCreateGlasses()},
      {"summary", summary().toStdString()},
      {"blockers", stringListToJsonArray(blockers)},
  };
}

NativeGlassesAdapter::NativeGlassesAdapter(QString projectRoot)
    : projectRoot_(std::move(projectRoot)) {}

// 析构时关闭 bridge 进程和共享内存写端，释放底层资源。
NativeGlassesAdapter::~NativeGlassesAdapter() { shutdownBridge(); }

void NativeGlassesAdapter::setCallbacks(ImuCallback imuCallback,
                                        ImageCallback imageCallback) {
  // 注册 IMU/图像回调，让 bridge 收到的数据可以继续回流到上层。
  imuCallback_ = std::move(imuCallback);
  imageCallback_ = std::move(imageCallback);
}

void NativeGlassesAdapter::clearCallbacks() {
  // 清空回调，避免设备释放后仍往上层投递数据。
  imuCallback_ = nullptr;
  imageCallback_ = nullptr;
}

NativeGlassesPreflightReport NativeGlassesAdapter::preflight() const {
  // 聚合项目根、vendored 资产、runtime 与桥脚本状态，判断是否可 createGlasses。
  NativeGlassesPreflightReport report;
  report.projectRoot = effectiveProjectRoot();
  report.projectRootDetected = !report.projectRoot.isEmpty();

  const auto runtimeInfo = XrealRuntimeLocator::probe(report.projectRoot);
  report.runtimeRoot = runtimeInfo.runtimeRoot;
  report.runtimeSitePackages = runtimeInfo.sitePackagesPath;
  report.runtimeGlassesServerPath = runtimeInfo.glassesServerPath;
  report.currentProcessQtVersion = runtimeInfo.currentProcessQtVersion;
  report.runtimeQtVersion = runtimeInfo.effectiveQtVersion;
  report.runtimeQtSource = runtimeInfo.effectiveQtSource;
  report.requiredQtVersion = requiredVendoredXrealQtVersionString();
  report.runtimeRootAvailable = runtimeInfo.runtimeRootAvailable;
  report.runtimeSitePackagesAvailable = runtimeInfo.sitePackagesAvailable;
  report.runtimeGlassesServerAvailable = runtimeInfo.glassesServerAvailable;
  report.runtimeQtCompatible = runtimeInfo.effectiveQtCompatible();

  if (!report.projectRootDetected) {
    report.blockers.push_back(QStringLiteral(
        "无法定位 RecordLabC 工程根目录，无法解析 vendored XREAL 资产。"));
  } else {
    const QDir rootDir(report.projectRoot);
    report.wheelPath = rootDir.filePath(
        QString::fromUtf8(recordlab::core::compat::kAppWheelRelativePath));
    report.pyiPath = rootDir.filePath(
        QString::fromUtf8(recordlab::core::compat::kAppPyiRelativePath));
    report.wheelAvailable = QFileInfo::exists(report.wheelPath);
    report.pyiAvailable = QFileInfo::exists(report.pyiPath);

    if (!report.wheelAvailable) {
      report.blockers.push_back(QStringLiteral("缺少 vendored XREAL wheel：%1")
                                    .arg(report.wheelPath));
    }
    if (!report.pyiAvailable) {
      report.blockers.push_back(
          QStringLiteral("缺少 vendored XrGlasses.pyi：%1")
              .arg(report.pyiPath));
    }
  }

  report.blockers.append(runtimeInfo.blockers);

  const QString workerScript = bridgeScriptPath();
  if (workerScript.isEmpty() || !QFileInfo::exists(workerScript)) {
    report.blockers.push_back(QStringLiteral("缺少 XREAL bridge worker：%1")
                                  .arg(workerScript.isEmpty()
                                           ? QStringLiteral("<unknown>")
                                           : workerScript));
  }

  const QString pythonProgram = resolveBridgePythonProgram();
  if (pythonProgram.isEmpty()) {
    report.blockers.push_back(
        QStringLiteral("未找到可用的 Python 解释器。请安装 python3.10+，或设置 "
                       "RECORDLABC_XREAL_PYTHON。"));
  }

  report.nativeBridgeImplemented = report.blockers.isEmpty();
  report.blockers.removeDuplicates();
  return report;
}

nlohmann::json NativeGlassesAdapter::createGlasses(void *&opaqueHandle,
                                                   int timeoutMs,
                                                   int retryCount,
                                                   int retryDelayMs) {
  // 创建 bridge 端眼镜对象，并把返回信息包装成本地 opaque handle。
  if (opaqueHandle) {
    closeGlasses(opaqueHandle);
  }

  const auto report = preflight();
  if (!report.canCreateGlasses()) {
    opaqueHandle = nullptr;
    return buildBridgeUnavailableResult(report,
                                        QStringLiteral("createGlasses 失败"));
  }

  nlohmann::json result = {{"success", false},
                           {"message", "create_glasses not attempted"}};
  const int boundedTimeoutMs =
      timeoutMs > 0 ? timeoutMs : kCreateGlassesTimeoutMs;
  const int boundedRetryCount =
      retryCount > 0 ? retryCount : kCreateGlassesRetryCount;
  const int boundedRetryDelayMs =
      retryDelayMs >= 0 ? retryDelayMs : kCreateGlassesRetryDelayMs;
  for (int attempt = 0; attempt < boundedRetryCount; ++attempt) {
    result = sendRequest(QStringLiteral("create_glasses"), {}, {},
                         boundedTimeoutMs);
    if (result.value("success", false)) {
      break;
    }

    const QString message =
        QString::fromStdString(result.value("message", std::string()));
    const bool retryable =
        message.contains(QStringLiteral("No glasses found"),
                         Qt::CaseInsensitive) ||
        message.contains(QStringLiteral("Timed out"), Qt::CaseInsensitive) ||
        message.contains(QStringLiteral("request failed"), Qt::CaseInsensitive);
    if (!retryable || attempt + 1 >= boundedRetryCount) {
      break;
    }

    shutdownBridge();
    if (boundedRetryDelayMs > 0) {
      QThread::msleep(boundedRetryDelayMs);
    }
  }

  if (!result.value("success", false)) {
    opaqueHandle = nullptr;
    return result;
  }

  auto *handle = new BridgeGlassesHandle;
  handle->productId = result.value("product_id", -1);
  opaqueHandle = handle;
  return result;
}

nlohmann::json NativeGlassesAdapter::openGlasses(void *opaqueHandle) {
  // 打开已经 create 成功的眼镜句柄。
  if (!opaqueHandle) {
    return {{"success", false}, {"message", "Glasses not created"}};
  }

  return sendRequest(QStringLiteral("open_glasses"));
}

nlohmann::json NativeGlassesAdapter::startSensors(void *opaqueHandle,
                                                  int sensorMask) {
  // 让 bridge 端启动指定传感器集合。
  if (!opaqueHandle) {
    return {{"success", false}, {"message", "Glasses not created"}};
  }

  return sendRequest(QStringLiteral("start_sensors"),
                     {{"sensor_mask", sensorMask}});
}

nlohmann::json NativeGlassesAdapter::stopSensors(void *opaqueHandle,
                                                 int sensorMask) {
  // 让 bridge 端停止指定传感器集合。
  if (!opaqueHandle) {
    return {{"success", false}, {"message", "Glasses not created"}};
  }

  return sendRequest(QStringLiteral("stop_sensors"),
                     {{"sensor_mask", sensorMask}});
}

nlohmann::json NativeGlassesAdapter::configureGlasses(
    void *opaqueHandle, const nlohmann::json &params) {
  // 透传相机曝光/帧率/增益/显示开关等运行期配置。
  if (!opaqueHandle) {
    return {{"success", false}, {"message", "Glasses not created"}};
  }
  return sendRequest(QStringLiteral("configure_glasses"), params);
}

nlohmann::json NativeGlassesAdapter::glassesState(void *opaqueHandle) {
  // 查询 bridge 端眼镜运行态，用于 BSP Tab 和 raw 抓取前置校验。
  if (!opaqueHandle) {
    return {{"success", false}, {"message", "Glasses not created"}};
  }
  return sendRequest(QStringLiteral("get_glasses_state"));
}

nlohmann::json NativeGlassesAdapter::closeGlasses(void *&opaqueHandle) {
  // 关闭 bridge 端眼镜对象，并释放本地 handle 包装体。
  nlohmann::json result = {{"success", true}, {"message", ""}};
  if (bridgeProcess_ && bridgeProcess_->state() == QProcess::Running) {
    result = sendRequest(QStringLiteral("close_glasses"));
  }

  delete static_cast<BridgeGlassesHandle *>(opaqueHandle);
  opaqueHandle = nullptr;
  return result;
}

nlohmann::json NativeGlassesAdapter::enumerateDevices() {
  // 通过 bridge 枚举 USB 上的眼镜设备，不需要先 createGlasses。
  return sendRequest(QStringLiteral("enumerate_devices"), {}, {}, 5000);
}

QString NativeGlassesAdapter::effectiveProjectRoot() const {
  // 按显式参数、环境变量、编译期路径和可执行文件上层目录依次定位项目根。
  if (isProjectRootCandidate(projectRoot_)) {
    return QDir::cleanPath(projectRoot_);
  }

  const QString envRoot = qEnvironmentVariable("RECORDLABC_ROOT");
  if (isProjectRootCandidate(envRoot)) {
    return QDir::cleanPath(envRoot);
  }

#ifdef RECORDLABC_SOURCE_DIR
  const QString compiledRoot = QString::fromUtf8(RECORDLABC_SOURCE_DIR);
  if (isProjectRootCandidate(compiledRoot)) {
    return QDir::cleanPath(compiledRoot);
  }
#endif

  QDir probe(QCoreApplication::applicationDirPath());
  for (int depth = 0; depth < 6; ++depth) {
    if (isProjectRootCandidate(probe.absolutePath())) {
      return QDir::cleanPath(probe.absolutePath());
    }
    if (!probe.cdUp()) {
      break;
    }
  }

  return {};
}

QString NativeGlassesAdapter::bridgeScriptPath() const {
  // 返回负责实际 XREAL Python 交互的 bridge worker 脚本路径。
  const QString root = effectiveProjectRoot();
  if (root.isEmpty()) {
    return {};
  }
  return QDir(root).filePath(QStringLiteral("scripts/xreal_bridge_worker.py"));
}

QString NativeGlassesAdapter::resolveBridgePythonProgram() const {
  // 解析启动 bridge worker 应使用的 Python 解释器，支持环境变量覆盖。
  const QString envPython =
      executableCandidate(qEnvironmentVariable("RECORDLABC_XREAL_PYTHON"));
  if (!envPython.isEmpty()) {
    return envPython;
  }

  const QString system310 =
      executableCandidate(QStringLiteral("/usr/bin/python3.10"));
  if (!system310.isEmpty()) {
    return system310;
  }

  const QString system3 =
      executableCandidate(QStringLiteral("/usr/bin/python3"));
  if (!system3.isEmpty()) {
    return system3;
  }

  return executableCandidate(QStringLiteral("python3"));
}

QProcessEnvironment
NativeGlassesAdapter::buildBridgeEnvironment(const QString &projectRoot) const {
  // 为 bridge worker 构造隔离环境，包括 runtime、PYTHONPATH 和 Qt 插件路径。
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.remove(QStringLiteral("PYTHONHOME"));
  env.insert(QStringLiteral("RECORDLABC_ROOT"), projectRoot);
  env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));

  const auto runtimeInfo = XrealRuntimeLocator::probe(projectRoot);
  if (!runtimeInfo.runtimeRoot.isEmpty()) {
    env.insert(QStringLiteral("RECORDLABC_XREAL_RUNTIME_ROOT"),
               runtimeInfo.runtimeRoot);
  }
  if (!runtimeInfo.sitePackagesPath.isEmpty()) {
    env.insert(QStringLiteral("RECORDLABC_XREAL_SITE_PACKAGES"),
               runtimeInfo.sitePackagesPath);
  }
  if (!runtimeInfo.glassesServerPath.isEmpty()) {
    env.insert(QStringLiteral("RECORDLABC_XREAL_GLASSES_SERVER"),
               runtimeInfo.glassesServerPath);
  }

  QStringList pythonPathEntries;
  pythonPathEntries << runtimeInfo.sitePackagesPath << projectRoot;
  const QString existingPythonPath = env.value(QStringLiteral("PYTHONPATH"));
  if (!existingPythonPath.isEmpty()) {
    pythonPathEntries << existingPythonPath.split(QLatin1Char(':'),
                                                  Qt::SkipEmptyParts);
  }
  env.insert(QStringLiteral("PYTHONPATH"), joinPathEntries(pythonPathEntries));

  QStringList ldLibraryEntries;
  ldLibraryEntries << runtimeInfo.qtLibDir << runtimeInfo.nativeLibDir;
  const QString existingLd = env.value(QStringLiteral("LD_LIBRARY_PATH"));
  if (!existingLd.isEmpty()) {
    ldLibraryEntries << existingLd.split(QLatin1Char(':'), Qt::SkipEmptyParts);
  }
  env.insert(QStringLiteral("LD_LIBRARY_PATH"),
             joinPathEntries(ldLibraryEntries));

  if (!runtimeInfo.qtPluginsDir.isEmpty()) {
    env.insert(QStringLiteral("QT_PLUGIN_PATH"), runtimeInfo.qtPluginsDir);
    env.insert(
        QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
        QDir(runtimeInfo.qtPluginsDir).filePath(QStringLiteral("platforms")));
  }

  return env;
}

bool NativeGlassesAdapter::ensureBridgeStarted(QString *errorMessage) {
  // 按需启动 bridge worker；若已运行则直接复用。
  if (bridgeProcess_ && bridgeProcess_->state() == QProcess::Running) {
    return true;
  }

  const auto report = preflight();
  if (!report.canCreateGlasses()) {
    const QString message = report.blockers.join(QStringLiteral(" | "));
    if (errorMessage) {
      *errorMessage = message;
    }
    return false;
  }

  shutdownBridge();

  bridgeProcess_ = std::make_unique<QProcess>();
  bridgeProcess_->setWorkingDirectory(report.projectRoot);
  bridgeProcess_->setProcessEnvironment(
      buildBridgeEnvironment(report.projectRoot));
  bridgeProcess_->setProgram(resolveBridgePythonProgram());
  bridgeProcess_->setArguments({bridgeScriptPath(),
                                QStringLiteral("--project-root"),
                                report.projectRoot});

  QObject::connect(bridgeProcess_.get(), &QProcess::readyReadStandardOutput,
                   [this]() { handleStdoutReadyRead(); });
  QObject::connect(bridgeProcess_.get(), &QProcess::readyReadStandardError,
                   [this]() { handleStderrReadyRead(); });
  QObject::connect(bridgeProcess_.get(),
                   qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                   [this](int exitCode, QProcess::ExitStatus exitStatus) {
                     handleBridgeFinished(exitCode, exitStatus);
                   });

  bridgeProcess_->start();
  if (!bridgeProcess_->waitForStarted(5000)) {
    lastBridgeError_ = bridgeProcess_->errorString();
    if (errorMessage) {
      *errorMessage = lastBridgeError_;
    }
    return false;
  }

  lastBridgeError_.clear();
  return true;
}

bool NativeGlassesAdapter::ensureCameraShmWriter() {
  // 按需创建共享内存写端，用于把桥进程相机帧转给 UI 和录制链路。
  if (cameraShmWriter_ && cameraShmWriter_->isCreated()) {
    return true;
  }

  if (!cameraShmWriter_) {
    cameraShmWriter_ =
        std::make_unique<recordlab::common::CameraSharedMemoryWriter>();
  }

  if (cameraShmWriter_->create()) {
    return true;
  }

  std::cerr << "[NativeGlassesAdapter] Failed to create CameraSharedMemoryWriter"
            << std::endl;
  return false;
}

void NativeGlassesAdapter::shutdownBridge() {
  // 停止等待中的请求状态，并彻底关闭 bridge 进程。
  waitingLoop_.clear();
  waitingRequestId_.clear();
  waitingResponse_ = nlohmann::json{};
  bridgeStdoutBuffer_.clear();

  if (!bridgeProcess_) {
    return;
  }

  if (bridgeProcess_->state() == QProcess::Running) {
    bridgeProcess_->closeWriteChannel();
    bridgeProcess_->terminate();
    if (!bridgeProcess_->waitForFinished(3000)) {
      bridgeProcess_->kill();
      bridgeProcess_->waitForFinished(2000);
    }
  }

  bridgeProcess_.reset();
}

nlohmann::json NativeGlassesAdapter::sendRequest(
    const QString &action, const nlohmann::json &payload,
    const QByteArray &binaryPayload, int timeoutMs) {
  // 向 bridge 发送一条请求帧，并同步等待对应 response 或超时。
  if (waitingLoop_) {
    return {{"success", false},
            {"message", "NativeGlassesAdapter request reentry detected"}};
  }

  QString startupError;
  if (!ensureBridgeStarted(&startupError)) {
    return {{"success", false}, {"message", startupError.toStdString()}};
  }

  const QString requestId = QString::number(nextRequestId_++);
  nlohmann::json header = {
      {"type", "request"},
      {"id", requestId.toStdString()},
      {"action", action.toStdString()},
      {"payload", payload},
  };

  const QByteArray frame = buildFrame(header, binaryPayload);
  if (bridgeProcess_->write(frame) != frame.size()) {
    return {{"success", false},
            {"message", "Failed to write request frame to XREAL bridge"}};
  }
  if (!bridgeProcess_->waitForBytesWritten(3000) &&
      bridgeProcess_->bytesToWrite() > 0) {
    return {{"success", false},
            {"message", "Timed out writing request frame to XREAL bridge"}};
  }

  waitingRequestId_ = requestId;
  waitingResponse_ = nlohmann::json{};

  QEventLoop loop;
  QTimer timer;
  waitingLoop_ = &loop;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  timer.start(timeoutMs);
  loop.exec();

  waitingLoop_.clear();
  waitingRequestId_.clear();

  if (!waitingResponse_.is_null() && !waitingResponse_.empty()) {
    return waitingResponse_;
  }

  const QString timeoutMessage =
      lastBridgeError_.isEmpty()
          ? QStringLiteral("Timed out waiting for XREAL bridge response: %1")
                .arg(action)
          : QStringLiteral("XREAL bridge request failed: %1")
                .arg(lastBridgeError_);
  return {{"success", false}, {"message", timeoutMessage.toStdString()}};
}

void NativeGlassesAdapter::handleStdoutReadyRead() {
  // 累积 bridge stdout 数据，并尝试按帧协议解析完整消息。
  if (!bridgeProcess_) {
    return;
  }

  bridgeStdoutBuffer_.append(bridgeProcess_->readAllStandardOutput());
  parseOutputFrames();
}

void NativeGlassesAdapter::handleStderrReadyRead() {
  // 记录 bridge stderr 的最新错误文本，供请求失败时回显。
  if (!bridgeProcess_) {
    return;
  }

  const QByteArray data = bridgeProcess_->readAllStandardError();
  const QString text = QString::fromLocal8Bit(data).trimmed();
  if (!text.isEmpty()) {
    lastBridgeError_ = text;
    std::cerr << "[NativeGlassesAdapter] bridge stderr: " << text.toStdString()
              << std::endl;
  }
}

void NativeGlassesAdapter::handleBridgeFinished(
    int exitCode, QProcess::ExitStatus exitStatus) {
  // bridge 进程退出时记录错误并唤醒任何正在等待响应的请求。
  if (lastBridgeError_.isEmpty()) {
    lastBridgeError_ =
        QStringLiteral("XREAL bridge exited (code=%1, status=%2)")
            .arg(exitCode)
            .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal")
                                                    : QStringLiteral("crash"));
  }

  if (waitingLoop_) {
    waitingLoop_->quit();
  }
}

void NativeGlassesAdapter::parseOutputFrames() {
  // 从 stdout 缓冲中按自定义帧协议逐个拆出完整消息。
  while (bridgeStdoutBuffer_.size() >= kBridgeFrameHeaderSize) {
    if (std::memcmp(bridgeStdoutBuffer_.constData(), kBridgeMagic, 4) != 0) {
      const int magicPos =
          bridgeStdoutBuffer_.indexOf(QByteArray(kBridgeMagic, 4));
      if (magicPos < 0) {
        bridgeStdoutBuffer_.clear();
        return;
      }
      bridgeStdoutBuffer_.remove(0, magicPos);
      if (bridgeStdoutBuffer_.size() < kBridgeFrameHeaderSize) {
        return;
      }
    }

    const quint32 headerSize =
        readUint32Le(bridgeStdoutBuffer_.constData() + 4);
    const quint32 payloadSize =
        readUint32Le(bridgeStdoutBuffer_.constData() + 8);
    const qsizetype totalSize = kBridgeFrameHeaderSize +
                                static_cast<qsizetype>(headerSize) +
                                static_cast<qsizetype>(payloadSize);
    if (bridgeStdoutBuffer_.size() < totalSize) {
      return;
    }

    const QByteArray headerBytes = bridgeStdoutBuffer_.mid(
        kBridgeFrameHeaderSize, static_cast<qsizetype>(headerSize));
    const QByteArray payloadBytes = bridgeStdoutBuffer_.mid(
        kBridgeFrameHeaderSize + static_cast<qsizetype>(headerSize),
        static_cast<qsizetype>(payloadSize));
    bridgeStdoutBuffer_.remove(0, totalSize);

    try {
      const auto header = nlohmann::json::parse(std::string(
          headerBytes.constData(), static_cast<size_t>(headerBytes.size())));
      handleBridgeMessage(header, payloadBytes);
    } catch (const std::exception &e) {
      lastBridgeError_ =
          QStringLiteral("Failed to parse bridge message: %1").arg(e.what());
    }
  }
}

void NativeGlassesAdapter::handleBridgeMessage(const nlohmann::json &header,
                                               const QByteArray &payloadBytes) {
  // 根据消息类型分流 response 和 event，并在 camera event 中写共享内存。
  const std::string type = header.value("type", std::string{});
  if (type == "response") {
    const QString responseId =
        QString::fromStdString(header.value("id", std::string{}));
    if (!waitingRequestId_.isEmpty() && responseId == waitingRequestId_) {
      if (header.contains("result") && header["result"].is_object()) {
        waitingResponse_ = header["result"];
      } else {
        waitingResponse_ = header;
      }
      if (waitingLoop_) {
        waitingLoop_->quit();
      }
    }
    return;
  }

  if (type != "event") {
    return;
  }

  const std::string eventName = header.value("event", std::string{});
  if (eventName == "imu") {
    if (imuCallback_ && header.contains("payload") &&
        header["payload"].is_object()) {
      imuCallback_(header["payload"]);
    }
    return;
  }

  if (eventName == "imu_batch") {
    if (imuCallback_ && header.contains("payload") &&
        header["payload"].is_object()) {
      const auto &payload = header["payload"];
      if (payload.contains("items") && payload["items"].is_array()) {
        for (const auto &item : payload["items"]) {
          if (item.is_object()) {
            imuCallback_(item);
          }
        }
      }
    }
    return;
  }

  if (eventName != "camera" || !imageCallback_ || !header.contains("payload") ||
      !header["payload"].is_object()) {
    return;
  }

  // 相机帧节流：限制 base64 编码+JSON 构建频率（20Hz）
  // 减少 bsp_main_subnode 主线程负载，防止 bridge stdout 管道背压
  {
    const auto nowNs =
        std::chrono::steady_clock::now().time_since_epoch().count();
    if (nowNs - lastCameraCallbackNs_ < kCameraCallbackIntervalNs) {
      return; // 跳过此帧
    }
    lastCameraCallbackNs_ = nowNs;
  }

  const auto &payload = header["payload"];
  nlohmann::json imageMessage = {
      {"timestamp", payload.value("timestamp", 0)},
      {"cam_data", nlohmann::json::object()},
  };

  if (!payload.contains("cams") || !payload["cams"].is_array()) {
    imageCallback_(imageMessage);
    return;
  }

  for (const auto &camMeta : payload["cams"]) {
    if (!camMeta.is_object()) {
      continue;
    }

    const int index = camMeta.value("index", -1);
    const int width = camMeta.value("width", 0);
    const int height = camMeta.value("height", 0);
    const int bytesPerLine = camMeta.value("bytes_per_line", width);
    const int dataOffset = camMeta.value("data_offset", -1);
    const int dataSize = camMeta.value("data_size", 0);
    if (index < 0 || width <= 0 || height <= 0 || dataOffset < 0 ||
        dataSize <= 0 || dataOffset + dataSize > payloadBytes.size()) {
      continue;
    }

    const QByteArray rawBytes = payloadBytes.mid(dataOffset, dataSize);
    recordlab::common::CameraFrameMeta meta;
    meta.width = width;
    meta.height = height;
    meta.format = camMeta.value("qt_format", 24);
    meta.dataSize = rawBytes.size();
    meta.bytesPerLine = bytesPerLine;

    bool isJpeg = false;
    if (camMeta.contains("encoded_format") &&
        camMeta["encoded_format"].is_string()) {
      isJpeg = (camMeta["encoded_format"] == "JPEG");
    }
    meta.encodedFormat = isJpeg ? 1 : 0;

    uint64_t seq = 0;
    if (ensureCameraShmWriter()) {
      seq = cameraShmWriter_->writeFrame(index, meta, rawBytes.constData(),
                                         rawBytes.size());
    }

    nlohmann::json camInfo = {
        {"exposure_start_time_device",
         camMeta.value("exposure_start_time_device", 0LL)},
        {"exposure_start_time_system",
         camMeta.value("exposure_start_time_system", 0LL)},
        {"exposure_duration", camMeta.value("exposure_duration", 0LL)},
        {"rolling_shutter_time", camMeta.value("rolling_shutter_time", 0LL)},
        {"stride", camMeta.value("stride", bytesPerLine)},
        {"gain", camMeta.value("gain", 0)},
        {"temperature", camMeta.value("temperature", 0.0)},
    };
    if (camMeta.contains("source_image_idx")) {
      camInfo["source_image_idx"] = camMeta["source_image_idx"];
    }

    // 无论是否写入成功，都通过轻量信号传递 metadata 给 UI（没有巨大的 base64
    // 数据）
    camInfo["image_raw"] = {
        {"width", width},
        {"height", height},
        {"format",
         camMeta.value("format", imageFormatName(meta.format).toStdString())},
        {"qt_format", meta.format},
        {"bytes_per_line", bytesPerLine},
        {"shm_seq", seq} // 通知 UI 确实有写共享内存
    };
    if (camMeta.contains("encoded_format") &&
        camMeta["encoded_format"].is_string()) {
      camInfo["image_raw"]["encoded_format"] = camMeta["encoded_format"];
    }

    imageMessage["cam_data"][std::to_string(index)] = camInfo;
  }

  imageCallback_(imageMessage);
}

nlohmann::json NativeGlassesAdapter::buildBridgeUnavailableResult(
    const NativeGlassesPreflightReport &report,
    const QString &prefixMessage) const {
  // 将预检阻塞项压缩成标准失败结果，供 create/open 等入口复用。
  const QString message =
      report.blockers.isEmpty()
          ? prefixMessage
          : QStringLiteral("%1：%2").arg(
                prefixMessage, report.blockers.join(QStringLiteral(" | ")));
  return {
      {"success", false},
      {"message", message.toStdString()},
      {"preflight", report.toJson()},
  };
}

} // namespace recordlab::bsp
