/*
 * BspMainSubnode 完整实现
 */
#include "recordlab/subnodes/bsp_main_subnode.h"
#include "recordlab/common/motion_detector.h"
#include "recordlab/common/topics.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>

#include <chrono>
#include <csignal>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

namespace recordlab::subnodes {

using json = nlohmann::json;
using namespace recordlab::common;

static constexpr int BUFFER_SIZE = 50;
static const char *IMU0_FILENAME = "imu_0.csv";
static const char *IMU1_FILENAME = "imu_1.csv";
static const char *IMAGE_FILENAME = "image.txt";
static const char *CAMERA_MODE_SLAM = "slam";
static const char *CAMERA_MODE_RGB = "rgb";
static const char *DEFAULT_RAW_TARGET_SUBDIR = "rgb0/raw_data";
static const char *DEFAULT_RAW_REMOTE_PATH = "/usrdata/test1.raw";
static const char *DEFAULT_RAW_REMOTE_FRAME_PATH = "/usrdata/frame.txt";
static const char *DEFAULT_RAW_REMOTE_TIMESTAMP_PATH = "/usrdata/timestamp.txt";
static const char *REMOTE_CAMERATEST_PATH = "/usrdata/cameratest";
static constexpr double DEFAULT_RGB_FPS = 15.0;
static constexpr bool DEFAULT_RGB_AUTO_EXPOSURE = false;
static constexpr int DEFAULT_RGB_EXPOSURE_US = 400;
static constexpr int DEFAULT_RAW_RESOLUTION = 8;
static constexpr int DEFAULT_RAW_EXPOSURE_MODE = 0;
static constexpr int DEFAULT_RAW_EXPOSURE_VALUE = 1;
static constexpr int DEFAULT_RAW_GAIN = 1;
static constexpr int DEFAULT_RAW_FILE_READY_TIMEOUT_MS = 12000;

namespace {

struct ImuSourceProbe {
  uint64_t count = 0;
  int64_t lastArrivalNs = 0;
  int64_t lastTimestampNs = 0;
  int64_t maxArrivalGapNs = 0;
  int64_t maxTimestampGapNs = 0;
  int64_t maxTimestampBacktrackNs = 0;
};

std::optional<double> jsonNumberIfPresent(const json &value, const char *key) {
  if (!value.is_object() || !value.contains(key)) {
    return std::nullopt;
  }
  const auto &field = value.at(key);
  if (!field.is_number()) {
    return std::nullopt;
  }
  return field.get<double>();
}

std::optional<double> firstAvailableRgbTemperature(const BspDevice *device) {
  if (!device) {
    return std::nullopt;
  }

  if (const auto latestFrameTemp =
          jsonNumberIfPresent(device->latestRgbFrameMeta(), "temperature")) {
    return latestFrameTemp;
  }
  return jsonNumberIfPresent(device->latestTemperatures(), "rgb_temperature");
}

void logImuSourceProbe(const char *tag, ImuSourceProbe &probe,
                       int64_t arrivalNs, int64_t timestampNs, int imuIdx) {
  ++probe.count;
  int64_t arrivalGapNs = 0;
  int64_t timestampGapNs = 0;
  if (probe.lastArrivalNs > 0) {
    arrivalGapNs = arrivalNs - probe.lastArrivalNs;
    if (arrivalGapNs > probe.maxArrivalGapNs) {
      probe.maxArrivalGapNs = arrivalGapNs;
    }
  }
  if (probe.lastTimestampNs > 0) {
    timestampGapNs = timestampNs - probe.lastTimestampNs;
    if (timestampGapNs > probe.maxTimestampGapNs) {
      probe.maxTimestampGapNs = timestampGapNs;
    }
    if (timestampGapNs < 0) {
      const int64_t backtrackNs = -timestampGapNs;
      if (backtrackNs > probe.maxTimestampBacktrackNs) {
        probe.maxTimestampBacktrackNs = backtrackNs;
      }
    }
  }
  probe.lastArrivalNs = arrivalNs;
  probe.lastTimestampNs = timestampNs;

  if (probe.count <= 20 || probe.count % 1000 == 0) {
    std::cout << "[" << tag << "] count=" << probe.count
              << " imu_idx=" << imuIdx
              << " arrival_gap_ms=" << static_cast<double>(arrivalGapNs) / 1e6
              << " timestamp_gap_ms="
              << static_cast<double>(timestampGapNs) / 1e6
              << " max_arrival_gap_ms="
              << static_cast<double>(probe.maxArrivalGapNs) / 1e6
              << " max_timestamp_gap_ms="
              << static_cast<double>(probe.maxTimestampGapNs) / 1e6
              << " max_timestamp_backtrack_ms="
              << static_cast<double>(probe.maxTimestampBacktrackNs) / 1e6
              << std::endl;
  }
}

QStringList legacySshOptions()
{
  // 兼容旧眼镜设备 SSH 配置，放宽 host key 与算法限制。
  return {
      QStringLiteral("-o"),
      QStringLiteral("StrictHostKeyChecking=no"),
      QStringLiteral("-o"),
      QStringLiteral("UserKnownHostsFile=/dev/null"),
      QStringLiteral("-o"),
      QStringLiteral("HostKeyAlgorithms=+ssh-rsa"),
      QStringLiteral("-o"),
      QStringLiteral("PubkeyAcceptedAlgorithms=+ssh-rsa"),
      QStringLiteral("-o"),
      QStringLiteral("ConnectTimeout=2"),
  };
}

constexpr unsigned long kGlassesFactoryOpTimeoutMs = 30000;

QString projectRootPath()
{
#ifdef RECORDLABC_SOURCE_DIR
  return QString::fromUtf8(RECORDLABC_SOURCE_DIR);
#else
  return QCoreApplication::applicationDirPath();
#endif
}

QString rawCaptureScriptPath()
{
  return QDir(projectRootPath())
      .filePath(QStringLiteral("subnodes/bsp_shell/capture_raw_frame.sh"));
}

QString localCameratestPath()
{
  return QDir(projectRootPath()).filePath(QStringLiteral("third_party/cameratest"));
}

QString buildCaptureFilename(qint64 timestampNs, const QString &suffix)
{
  QString normalized = suffix.startsWith(QLatin1Char('.')) ? suffix
                                                           : QStringLiteral(".") + suffix;
  return QStringLiteral("%1%2")
      .arg(timestampNs, 20, 10, QLatin1Char('0'))
      .arg(normalized);
}

qint64 captureTimestampNsFromMeta(const json &meta)
{
  const qint64 exposureStart =
      meta.value("exposure_start_time_device", qint64(0));
  const qint64 exposureDuration = meta.value("exposure_duration", qint64(0));
  const qint64 rollingShutter = meta.value("rolling_shutter_time", qint64(0));
  if (exposureStart <= 0) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
  return ((exposureStart * 2) + exposureDuration + rollingShutter + 1) / 2;
}

QString normalizedSidecarText(const QString &text,
                              const QString &fallback = QString())
{
  QStringList lines;
  for (const QString &line : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                        Qt::SkipEmptyParts)) {
    const QString trimmed = line.trimmed();
    if (!trimmed.isEmpty()) {
      lines.push_back(trimmed);
    }
  }
  if (lines.isEmpty()) {
    return fallback;
  }
  return lines.join(QLatin1Char(' '));
}

json runScript(const QString &program, const QStringList &args, int timeoutMs)
{
  QProcess process;
  process.start(program, args);
  if (!process.waitForFinished(timeoutMs)) {
    process.kill();
    process.waitForFinished(1000);
    return {{"success", false}, {"message", "script timeout"}};
  }
  return {{"success", process.exitCode() == 0},
          {"stdout", process.readAllStandardOutput().toStdString()},
          {"stderr", process.readAllStandardError().toStdString()},
          {"returncode", process.exitCode()}};
}

} // namespace

// ============================================================================
// GlassesFactory
// ============================================================================

GlassesFactory::GlassesFactory(QObject *parent)
    : QObject(parent),
      adapter_(std::make_unique<recordlab::bsp::NativeGlassesAdapter>()) {
  // 信号槽连接: 从子线程发信号 → 在主线程执行槽
  connect(this, &GlassesFactory::createGlassesSignal, this,
          &GlassesFactory::onCreateGlassesSlot, Qt::QueuedConnection);
  connect(this, &GlassesFactory::openGlassesSignal, this,
          &GlassesFactory::onOpenGlassesSlot, Qt::QueuedConnection);
  connect(this, &GlassesFactory::startSensorsSignal, this,
          &GlassesFactory::onStartSensorsSlot, Qt::QueuedConnection);
  connect(this, &GlassesFactory::stopSensorsSignal, this,
          &GlassesFactory::onStopSensorsSlot, Qt::QueuedConnection);
  connect(this, &GlassesFactory::configureGlassesSignal, this,
          &GlassesFactory::onConfigureGlassesSlot, Qt::QueuedConnection);
  connect(this, &GlassesFactory::queryGlassesStateSignal, this,
          &GlassesFactory::onQueryGlassesStateSlot, Qt::QueuedConnection);
  connect(this, &GlassesFactory::closeGlassesSignal, this,
          &GlassesFactory::onCloseGlassesSlot, Qt::QueuedConnection);
  connect(this, &GlassesFactory::enumerateDevicesSignal, this,
          &GlassesFactory::onEnumerateDevicesSlot, Qt::QueuedConnection);
}

GlassesFactory::~GlassesFactory() {
  // 析构时断开回调并尝试关闭仍持有的眼镜句柄。
  disconnectCallbacks();
  if (glasses_) {
    closeGlasses();
  }
}

json GlassesFactory::createGlasses() {
  // 通过主线程槽函数创建眼镜对象，并同步等待返回结果。
  QMutexLocker locker(&mutex_);
  pendingResult_ = json{};
  emit createGlassesSignal();
  if (!waitCondition_.wait(&mutex_, kGlassesFactoryOpTimeoutMs)) {
    std::cerr << "[GlassesFactory] Create glasses timeout" << std::endl;
    return {{"success", false}, {"message", "Timeout creating glasses"}};
  }
  json result = pendingResult_;
  pendingResult_ = json{};
  return result.empty() ? json{{"success", false}, {"message", "No result"}}
                        : result;
}

json GlassesFactory::openGlasses() {
  // 请求主线程打开眼镜连接，并同步等待执行结果。
  QMutexLocker locker(&mutex_);
  pendingResult_ = json{};
  emit openGlassesSignal();
  if (!waitCondition_.wait(&mutex_, kGlassesFactoryOpTimeoutMs)) {
    return {{"success", false}, {"message", "Timeout opening glasses"}};
  }
  json result = pendingResult_;
  pendingResult_ = json{};
  return result.empty() ? json{{"success", false}, {"message", "No result"}}
                        : result;
}

json GlassesFactory::startSensors(int sensorMask) {
  // 在主线程中启动指定传感器集合，避免跨线程直接触碰 SDK 对象。
  QMutexLocker locker(&mutex_);
  pendingResult_ = json{};
  emit startSensorsSignal(sensorMask);
  if (!waitCondition_.wait(&mutex_, kGlassesFactoryOpTimeoutMs)) {
    return {{"success", false}, {"message", "Timeout starting sensors"}};
  }
  json result = pendingResult_;
  pendingResult_ = json{};
  return result.empty() ? json{{"success", false}, {"message", "No result"}}
                        : result;
}

json GlassesFactory::stopSensors(int sensorMask) {
  // 在主线程中停止指定传感器集合，并同步返回结果。
  QMutexLocker locker(&mutex_);
  pendingResult_ = json{};
  emit stopSensorsSignal(sensorMask);
  if (!waitCondition_.wait(&mutex_, kGlassesFactoryOpTimeoutMs)) {
    return {{"success", false}, {"message", "Timeout stopping sensors"}};
  }
  json result = pendingResult_;
  pendingResult_ = json{};
  return result.empty() ? json{{"success", false}, {"message", "No result"}}
                        : result;
}

json GlassesFactory::configureGlasses(const json &params) {
  QMutexLocker locker(&mutex_);
  pendingResult_ = json{};
  emit configureGlassesSignal(QString::fromStdString(params.dump()));
  if (!waitCondition_.wait(&mutex_, kGlassesFactoryOpTimeoutMs)) {
    return {{"success", false}, {"message", "Timeout configuring glasses"}};
  }
  json result = pendingResult_;
  pendingResult_ = json{};
  return result.empty() ? json{{"success", false}, {"message", "No result"}}
                        : result;
}

json GlassesFactory::glassesState() {
  QMutexLocker locker(&mutex_);
  pendingResult_ = json{};
  emit queryGlassesStateSignal();
  if (!waitCondition_.wait(&mutex_, kGlassesFactoryOpTimeoutMs)) {
    return {{"success", false}, {"message", "Timeout querying glasses state"}};
  }
  json result = pendingResult_;
  pendingResult_ = json{};
  return result.empty() ? json{{"success", false}, {"message", "No result"}}
                        : result;
}

json GlassesFactory::closeGlasses() {
  // 在主线程关闭眼镜句柄，确保 SDK 生命周期和 Qt 线程模型一致。
  QMutexLocker locker(&mutex_);
  pendingResult_ = json{};
  emit closeGlassesSignal();
  if (!waitCondition_.wait(&mutex_, kGlassesFactoryOpTimeoutMs)) {
    return {{"success", false}, {"message", "Timeout closing glasses"}};
  }
  json result = pendingResult_;
  pendingResult_ = json{};
  return result.empty() ? json{{"success", false}, {"message", "No result"}}
                        : result;
}

void GlassesFactory::setCallbacks(ImuCallback imuCb, ImageCallback imageCb) {
  // 把上层 IMU/图像回调桥接到底层 NativeGlassesAdapter。
  imuCallback_ = std::move(imuCb);
  imageCallback_ = std::move(imageCb);
  adapter_->setCallbacks(imuCallback_, imageCallback_);
  std::cout << "[GlassesFactory] Callbacks set" << std::endl;
}

void GlassesFactory::disconnectCallbacks() {
  // 解除回调绑定，防止 release 后仍有数据回流到已销毁对象。
  imuCallback_ = nullptr;
  imageCallback_ = nullptr;
  adapter_->clearCallbacks();
}

void GlassesFactory::onCreateGlassesSlot() {
  // 真正执行 createGlasses 的槽函数，运行在拥有 adapter 的线程中。
  try {
    std::cout << "[GlassesFactory] Creating glasses via NativeGlassesAdapter..."
              << std::endl;
    setResult(adapter_->createGlasses(glasses_));
  } catch (const std::exception &e) {
    glasses_ = nullptr;
    setResult({{"success", false}, {"message", e.what()}});
  }
}

void GlassesFactory::onOpenGlassesSlot() {
  // 真正执行 openGlasses 的槽函数。
  try {
    std::cout << "[GlassesFactory] Opening glasses via NativeGlassesAdapter..."
              << std::endl;
    setResult(adapter_->openGlasses(glasses_));
  } catch (const std::exception &e) {
    setResult({{"success", false}, {"message", e.what()}});
  }
}

void GlassesFactory::onStartSensorsSlot(int mask) {
  // 真正执行 startSensors 的槽函数。
  try {
    std::cout << "[GlassesFactory] Starting sensors via NativeGlassesAdapter"
              << " (mask=" << mask << ")" << std::endl;
    setResult(adapter_->startSensors(glasses_, mask));
  } catch (const std::exception &e) {
    setResult({{"success", false}, {"message", e.what()}});
  }
}

void GlassesFactory::onStopSensorsSlot(int mask) {
  // 真正执行 stopSensors 的槽函数。
  try {
    std::cout << "[GlassesFactory] Stopping sensors via NativeGlassesAdapter"
              << " (mask=" << mask << ")" << std::endl;
    setResult(adapter_->stopSensors(glasses_, mask));
  } catch (const std::exception &e) {
    setResult({{"success", false}, {"message", e.what()}});
  }
}

void GlassesFactory::onConfigureGlassesSlot(QString paramsJson) {
  try {
    const json params = json::parse(paramsJson.toStdString());
    setResult(adapter_->configureGlasses(glasses_, params));
  } catch (const std::exception &e) {
    setResult({{"success", false}, {"message", e.what()}});
  }
}

void GlassesFactory::onQueryGlassesStateSlot() {
  try {
    setResult(adapter_->glassesState(glasses_));
  } catch (const std::exception &e) {
    setResult({{"success", false}, {"message", e.what()}});
  }
}

void GlassesFactory::onCloseGlassesSlot() {
  // 真正执行 closeGlasses 的槽函数。
  try {
    std::cout << "[GlassesFactory] Closing glasses via NativeGlassesAdapter..."
              << std::endl;
    setResult(adapter_->closeGlasses(glasses_));
  } catch (const std::exception &e) {
    setResult({{"success", false}, {"message", e.what()}});
  }
}

json GlassesFactory::enumerateDevices() {
  // 通过主线程槽函数枚举 USB 设备，并同步等待返回结果。
  QMutexLocker locker(&mutex_);
  pendingResult_ = json{};
  emit enumerateDevicesSignal();
  if (!waitCondition_.wait(&mutex_, kGlassesFactoryOpTimeoutMs)) {
    return {{"success", false}, {"message", "Timeout enumerating devices"}};
  }
  json result = pendingResult_;
  pendingResult_ = json{};
  return result.empty() ? json{{"success", false}, {"message", "No result"}}
                        : result;
}

void GlassesFactory::onEnumerateDevicesSlot() {
  // 真正执行 enumerateDevices 的槽函数。
  try {
    setResult(adapter_->enumerateDevices());
  } catch (const std::exception &e) {
    setResult({{"success", false}, {"message", e.what()}});
  }
}

void GlassesFactory::setResult(const json &result) {
  // 保存最近一次操作结果并唤醒等待线程。
  QMutexLocker locker(&mutex_);
  pendingResult_ = result;
  waitCondition_.wakeAll();
}

// ============================================================================
// XrGlassesSSHManager
// ============================================================================

XrGlassesSSHManager::XrGlassesSSHManager(const std::string &hostname, int port,
                                         const std::string &username,
                                         const std::string &password)
    : hostname_(hostname), port_(port), username_(username),
      password_(password) {}

bool XrGlassesSSHManager::ping() const {
  // 用系统 ping 粗略探测眼镜设备当前是否可达。
  QProcess proc;
  proc.start("ping", {"-c", "1", "-W", "1", QString::fromStdString(hostname_)});
  if (!proc.waitForFinished(2000)) {
    proc.kill();
    return false;
  }
  return proc.exitCode() == 0;
}

bool XrGlassesSSHManager::waitUntilServerStarts(int intervalMs,
                                                int timeoutMs) const {
  // 轮询 SSH 登录测试命令，等待眼镜上的 SSH 服务就绪。
  std::cout << "[SSHManager] Waiting for SSH server at " << hostname_ << "..."
            << std::endl;
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  int attempt = 0;

  while (std::chrono::steady_clock::now() < deadline) {
    ++attempt;
    QProcess ssh;
    QStringList args = {
        QStringLiteral("-p"),
        QString::fromStdString(password_),
        QStringLiteral("ssh"),
    };
    args << legacySshOptions() << QStringLiteral("-p") << QString::number(port_)
         << QStringLiteral("%1@%2")
                .arg(QString::fromStdString(username_),
                     QString::fromStdString(hostname_))
         << QStringLiteral("echo ok");
    ssh.start(QStringLiteral("sshpass"), args);
    if (ssh.waitForFinished(5000) && ssh.exitCode() == 0) {
      std::cout << "[SSHManager] ✓ SSH connection successful" << std::endl;
      return true;
    }
    if (attempt == 1 || attempt % 5 == 0) {
      const auto stderrText = ssh.readAllStandardError().trimmed();
      std::cout << "[SSHManager] SSH not ready yet (attempt " << attempt
                << ")";
      if (!stderrText.isEmpty()) {
        std::cout << ": " << stderrText.toStdString();
      }
      std::cout << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
  }
  std::cerr << "[SSHManager] SSH connection timeout after " << timeoutMs << "ms"
            << std::endl;
  return false;
}

void XrGlassesSSHManager::checkAndWaitRestarted() {
  // 检查旧 gina_server 是否残留；若有则重启眼镜并等待 SSH 恢复。
  std::cout << "[SSHManager] Checking glasses device state..." << std::endl;
  if (!waitUntilServerStarts()) {
    throw std::runtime_error("SSH server did not become ready");
  }

  // Check for gina_server process
  auto result =
      executeCommand("ps -ef | grep [g]ina_server | grep -v grep", 5000);
  std::string pidLine = result.value("stdout", std::string(""));

  if (!pidLine.empty()) {
    std::cout << "[SSHManager] Found gina_server running: " << pidLine
              << std::endl;
    std::cout << "[SSHManager] Rebooting glasses to clean up..." << std::endl;
    executeCommand("rm -rf /bin/pilot; /sbin/reboot", 5000);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!waitUntilServerStarts()) {
      throw std::runtime_error("SSH server did not come back after reboot");
    }
    std::cout << "[SSHManager] ✓ Glasses reboot complete" << std::endl;
  }
}

json XrGlassesSSHManager::executeCommand(const std::string &command,
                                         int timeoutMs) const {
  // 通过 sshpass + ssh 在眼镜上执行一条命令，并返回 stdout/stderr。
  QProcess ssh;
  QStringList args = {
      QStringLiteral("-p"),
      QString::fromStdString(password_),
      QStringLiteral("ssh"),
  };
  args << legacySshOptions() << QStringLiteral("-p") << QString::number(port_)
       << QStringLiteral("%1@%2")
              .arg(QString::fromStdString(username_),
                   QString::fromStdString(hostname_))
       << QString::fromStdString(command);
  ssh.start(QStringLiteral("sshpass"), args);
  if (!ssh.waitForFinished(timeoutMs)) {
    ssh.kill();
    return {{"success", false}, {"message", "SSH command timeout"}};
  }
  return {{"success", ssh.exitCode() == 0},
          {"stdout", ssh.readAllStandardOutput().toStdString()},
          {"stderr", ssh.readAllStandardError().toStdString()},
          {"returncode", ssh.exitCode()}};
}

json XrGlassesSSHManager::copyFileFromGlasses(
    const std::string &remotePath, const std::string &localPath) const {
  // 通过 SCP 将眼镜上的文件拉回本地。
  QProcess scp;
  QStringList args = {
      QStringLiteral("-p"),
      QString::fromStdString(password_),
      QStringLiteral("scp"),
      QStringLiteral("-O"),
  };
  args << legacySshOptions() << QStringLiteral("-P") << QString::number(port_)
       << QStringLiteral("%1@%2:%3")
              .arg(QString::fromStdString(username_),
                   QString::fromStdString(hostname_),
                   QString::fromStdString(remotePath))
       << QString::fromStdString(localPath);
  scp.start(QStringLiteral("sshpass"), args);
  if (!scp.waitForFinished(30000)) {
    scp.kill();
    return {{"success", false}, {"message", "SCP timeout"}};
  }
  return {{"success", scp.exitCode() == 0},
          {"message", scp.exitCode() == 0
                          ? "File copied"
                          : scp.readAllStandardError().toStdString()}};
}

json XrGlassesSSHManager::copyFileToGlasses(
    const std::string &localPath, const std::string &remotePath) const {
  // 通过 SCP 将本地文件上传到眼镜设备。
  QProcess scp;
  QStringList args = {
      QStringLiteral("-p"),
      QString::fromStdString(password_),
      QStringLiteral("scp"),
      QStringLiteral("-O"),
  };
  args << legacySshOptions() << QStringLiteral("-P") << QString::number(port_)
       << QString::fromStdString(localPath)
       << QStringLiteral("%1@%2:%3")
              .arg(QString::fromStdString(username_),
                   QString::fromStdString(hostname_),
                   QString::fromStdString(remotePath));
  scp.start(QStringLiteral("sshpass"), args);
  if (!scp.waitForFinished(30000)) {
    scp.kill();
    return {{"success", false}, {"message", "SCP upload timeout"}};
  }
  return {{"success", scp.exitCode() == 0},
          {"message", scp.exitCode() == 0
                          ? "File uploaded"
                          : scp.readAllStandardError().toStdString()}};
}

json XrGlassesSSHManager::adbPull(const std::string &remotePath,
                                  const std::string &localPath) const {
  // 备用路径：通过 adb pull 从眼镜拉文件到本地。
  QProcess adb;
  adb.start("adb", {"pull", QString::fromStdString(remotePath),
                    QString::fromStdString(localPath)});
  if (!adb.waitForFinished(30000)) {
    adb.kill();
    return {{"success", false}, {"message", "ADB pull timeout"}};
  }
  return {{"success", adb.exitCode() == 0},
          {"message", adb.exitCode() == 0
                          ? "File pulled"
                          : adb.readAllStandardError().toStdString()}};
}

// ============================================================================
// BspDevice
// ============================================================================

BspDevice::BspDevice() { std::cout << "[BspDevice] Initialized" << std::endl; }

// 析构时确保眼镜句柄、传感器和回调都被正确释放。
BspDevice::~BspDevice() { release(); }

json BspDevice::initialize(const json &params) {
  // 完成 SSH 预清理、create/open glasses 以及数据回调绑定。
  try {
    isInitialized_ = false;
    isStarted_ = false;
    std::cout << "[BspDevice] Initializing..." << std::endl;

    const bool skipSsh = params.value("skip_ssh", false);
    const bool sshOptional = params.value("ssh_optional", false);
    if (params.contains("ssh_required")) {
      sshRequired_ = params.value("ssh_required", true);
    } else if (skipSsh || sshOptional) {
      sshRequired_ = false;
    }

    // 1. SSH 检查: 确保 gina_server 不在运行
    if (sshRequired_) {
      if (!params.value("skip_restart_check", false)) {
        sshManager_.checkAndWaitRestarted();
      } else if (!sshManager_.waitUntilServerStarts(1000, 30000)) {
        return {{"success", false}, {"message", "SSH server did not become ready"}};
      }
    } else {
      std::cout << "[BspDevice] SSH optional: skipping SSH precheck" << std::endl;
    }

    // 2. 在主线程创建 glasses
    auto result = factory_.createGlasses();
    if (!result.value("success", false))
      return result;

    // 3. 设置 IMU/Camera 回调
    factory_.setCallbacks(imuCallback_ ? [this](const json &d) { onImuData(d); }
                                       : GlassesFactory::ImuCallback{},
                          imageCallback_
                              ? [this](const json &d) { onCamData(d); }
                              : GlassesFactory::ImageCallback{});

    // 4. 在主线程打开 glasses
    result = factory_.openGlasses();
    if (!result.value("success", false)) {
      release();
      return result;
    }

    isInitialized_ = true;
    std::cout << "[BspDevice] Initialized successfully" << std::endl;
    return {{"success", true}, {"message", ""}};
  } catch (const std::exception &e) {
    try {
      release();
    } catch (...) {
    }
    std::cerr << "[BspDevice] Initialize error: " << e.what() << std::endl;
    return {{"success", false}, {"message", e.what()}};
  }
}

json BspDevice::start(const json &params) {
  // 启动需要的传感器集合，让 IMU/图像数据真正开始回流。
  try {
    if (!isInitialized_)
      return {{"success", false}, {"message", "Not initialized"}};
    if (isStarted_)
      return {{"success", true}, {"message", "Already started"}};

    const std::string requestedMode =
      params.value("camera_mode", std::string(CAMERA_MODE_SLAM));
    const bool cameraDisabled =
      requestedMode == "none" || requestedMode == "imu" ||
      requestedMode == "imu_only";
    if (!cameraDisabled && requestedMode != CAMERA_MODE_SLAM &&
      requestedMode != CAMERA_MODE_RGB) {
      return {{"success", false},
              {"message", "Invalid camera_mode: " + requestedMode}};
    }

    const int dataSensorMask =
        SENSOR_IMU |
        (cameraDisabled
             ? 0
             : (requestedMode == CAMERA_MODE_RGB ? SENSOR_RGB : SENSOR_SLAM));
    startSensorMask_ = dataSensorMask;

    auto result = factory_.startSensors(dataSensorMask);
    if (!result.value("success", false))
      return result;

    if (!cameraDisabled) {
      json config = json::object();
      if (requestedMode == CAMERA_MODE_RGB) {
        rgbFps_ = params.value("rgb_fps", DEFAULT_RGB_FPS);
        rgbAutoExposure_ =
            params.value("rgb_auto_exposure", DEFAULT_RGB_AUTO_EXPOSURE);
        rgbExposureUs_ =
            params.value("rgb_exposure", DEFAULT_RGB_EXPOSURE_US);
        config["rgb_fps"] = rgbFps_;
        config["rgb_auto_exposure"] = rgbAutoExposure_;
        config["rgb_exposure"] = rgbExposureUs_;
        if (params.contains("rgb_gain")) {
          rgbGain_ = params["rgb_gain"];
          config["rgb_gain"] = rgbGain_;
        }
      } else {
        if (params.contains("slam_fps")) {
          config["slam_fps"] = params["slam_fps"];
        }
        if (params.contains("exposure")) {
          config["exposure"] = params["exposure"];
        }
        if (params.contains("auto_exposure")) {
          config["auto_exposure"] = params["auto_exposure"];
        }
      }
      if (!config.empty()) {
        auto configResult = factory_.configureGlasses(config);
        if (!configResult.value("success", false)) {
          factory_.stopSensors(startSensorMask_);
          return configResult;
        }
      }
    }

    const bool enableDisplay = params.value("enable_display", false);
    if (enableDisplay && cameraDisabled) {
      std::cout << "[BspDevice] Display is disabled in IMU-only/no-system mode"
                << std::endl;
    } else if (enableDisplay) {
      auto displayResult =
          factory_.configureGlasses({{"enable_display", true}});
      if (displayResult.value("success", false)) {
        startSensorMask_ |= SENSOR_DISPLAY;
      } else {
        std::cerr << "[BspDevice] enable_display failed during start: "
                  << displayResult.value("message", std::string())
                  << std::endl;
      }
    }

    isStarted_ = true;
    cameraMode_ = cameraDisabled ? "none" : requestedMode;
    std::cout << "[BspDevice] Started" << std::endl;
    return {{"success", true}, {"message", ""}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json BspDevice::stop(const json &) {
  // 停止当前启用的传感器集合。
  try {
    auto result = factory_.stopSensors(startSensorMask_);
    if (!result.value("success", false))
      return result;
    isStarted_ = false;
    std::cout << "[BspDevice] Stopped" << std::endl;
    return {{"success", true}, {"message", ""}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json BspDevice::release() {
  // 断开回调并关闭眼镜句柄，彻底释放设备资源。
  try {
    factory_.disconnectCallbacks();
    if (factory_.glasses()) {
      if (isStarted_) {
        auto result = factory_.stopSensors(startSensorMask_);
        if (!result.value("success", true)) {
          std::cerr << "[BspDevice] stopSensors during release: "
                    << result.value("message", std::string()) << std::endl;
        }
      }
      auto closeResult = factory_.closeGlasses();
      if (!closeResult.value("success", true)) {
        std::cerr << "[BspDevice] closeGlasses during release: "
                  << closeResult.value("message", std::string()) << std::endl;
      }
    }
    isInitialized_ = false;
    isStarted_ = false;
    cameraMode_ = CAMERA_MODE_SLAM;
    {
      std::lock_guard<std::mutex> lock(latestRgbFrameMutex_);
      latestRgbFrameMeta_ = json::object();
    }
    std::cout << "[BspDevice] Released" << std::endl;
    return {{"success", true}, {"message", ""}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json BspDevice::control(const json &params) {
  // 处理运行期控制参数，如显示开关、曝光和传感器组合调整。
  try {
    // slam_fps
    if (params.contains("slam_fps")) {
      double fps = params.value("slam_fps", 30.0);
      // TODO: factory_.glasses()->setFrameRate(Slam, fps);
      std::cout << "[BspDevice] Set SLAM FPS: " << fps << std::endl;
    }
    // exposure
    if (params.contains("exposure")) {
      double exposure = params.value("exposure", 0.0);
      // TODO: factory_.glasses()->setExposure(Slam, exposure);
      std::cout << "[BspDevice] Set exposure: " << exposure << std::endl;
    }
    // auto_exposure
    if (params.contains("auto_exposure")) {
      bool autoExp = params.value("auto_exposure", true);
      // TODO: factory_.glasses()->setAutoExposure(Slam, autoExp);
      std::cout << "[BspDevice] Set auto exposure: " << autoExp << std::endl;
    }
    // enable_display
    if (params.contains("enable_display")) {
      bool enable = params.value("enable_display", false);
      auto result = json{{"success", true}, {"message", ""}};
      if (enable) {
        if (cameraMode_ == "none") {
          return {{"success", false},
                  {"message",
                   "Display is disabled in IMU-only/no-system mode"}};
        }
        result = factory_.startSensors(SENSOR_DISPLAY);
        if (!result.value("success", false)) {
          return result;
        }
        startSensorMask_ |= SENSOR_DISPLAY;
      } else {
        result = factory_.stopSensors(SENSOR_DISPLAY);
        if (!result.value("success", false)) {
          return result;
        }
        startSensorMask_ &= ~SENSOR_DISPLAY;
      }
    }
    json config = json::object();
    for (const auto *key :
         {"slam_fps", "exposure", "auto_exposure", "rgb_fps",
          "rgb_exposure", "rgb_auto_exposure", "rgb_gain"}) {
      if (params.contains(key)) {
        config[key] = params[key];
      }
    }
    if (params.contains("rgb_fps")) {
      rgbFps_ = params.value("rgb_fps", rgbFps_);
    }
    if (params.contains("rgb_auto_exposure")) {
      rgbAutoExposure_ = params.value("rgb_auto_exposure", rgbAutoExposure_);
    }
    if (params.contains("rgb_exposure")) {
      rgbExposureUs_ = params.value("rgb_exposure", rgbExposureUs_);
    }
    if (params.contains("rgb_gain")) {
      rgbGain_ = params["rgb_gain"];
    }
    if (!config.empty()) {
      auto result = factory_.configureGlasses(config);
      if (!result.value("success", false)) {
        return result;
      }
    }
    return {{"success", true}, {"message", ""}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json BspDevice::check() {
  // 统一使用 SDK USB 枚举检测眼镜是否物理连接。
  // 所有节点（BSP / Helen）共用同一套逻辑，不再区分 sshRequired_。
  try {
    auto enumResult = factory_.enumerateDevices();

    if (!enumResult.value("success", false)) {
      // bridge 不可用（未启动/崩溃/SDK 异常）
      return {{"success", false},
              {"message", enumResult.value("message", std::string("SDK enumerate failed"))},
              {"product_ids", json::array()},
              {"device_count", 0}};
    }

    auto productIds = enumResult.value("product_ids", json::array());
    const int deviceCount = static_cast<int>(productIds.size());

    if (deviceCount == 0) {
      return {{"success", false},
              {"message", "No glasses connected"},
              {"product_ids", json::array()},
              {"device_count", 0}};
    }

    // 返回成功 + 设备 ID 列表
    const int firstProductId = productIds[0].get<int>();
    json glassesState = nullptr;
    if (factory_.glasses()) {
      glassesState = factory_.glassesState();
    }
    json result = {{"success", true},
                   {"message", ""},
                   {"product_ids", productIds},
                   {"product_id", firstProductId},
                   {"device_count", deviceCount},
                   {"fsn", glassesState.is_object()
                               ? glassesState.value("fsn", std::string())
                               : std::string()}};

    // 补充 SSH 可达性信息（仅供诊断，不影响 check 成功与否）
    if (sshRequired_) {
      result["ssh_reachable"] = sshManager_.ping();
    }

    return result;
  } catch (const std::exception &e) {
    return {{"success", false},
            {"message", e.what()},
            {"product_ids", json::array()},
            {"device_count", 0}};
  }
}

json BspDevice::runtimeState() const {
  json glassesState = nullptr;
  if (factory_.glasses()) {
    glassesState = const_cast<GlassesFactory &>(factory_).glassesState();
  }

  json deviceState = {
      {"connected", isInitialized_},
      {"initialized", isInitialized_},
      {"started", isStarted_},
      {"is_opened", glassesState.is_object()
                        ? glassesState.value("is_opened", false)
                        : false},
      {"glasses_type", glassesState.is_object()
                           ? glassesState.value("glasses_type", std::string())
                           : std::string()},
      {"fsn", glassesState.is_object()
                  ? glassesState.value("fsn", std::string())
                  : std::string()},
      {"mcu_firmware_version",
       glassesState.is_object()
           ? glassesState.value("mcu_firmware_version", std::string())
           : std::string()},
      {"has_rgb_sensor", glassesState.is_object()
                             ? glassesState.value("has_rgb_sensor", false)
                             : false},
      {"active_sensors", glassesState.is_object()
                             ? glassesState.value("active_sensors", json::array())
                             : json::array()},
  };

  json rgbConfig = {
      {"rgb_cam_sn", glassesState.is_object()
                         ? glassesState.value("rgb_cam_sn", std::string())
                         : std::string()},
      {"fps", rgbFps_},
      {"auto_exposure", rgbAutoExposure_},
      {"exposure", rgbExposureUs_},
      {"gain", rgbGain_.is_null() ? nullptr : rgbGain_},
  };

  json state = {{"success", true},
                {"message", "BSP runtime state"},
                {"command", "get_bsp_runtime_state"},
                {"device", deviceState},
                {"temperatures", latestTemperatures()},
                {"camera_mode", cameraMode_},
                {"rgb_config", rgbConfig},
                {"glasses", glassesState}};
  state["latest_frame"] = latestRgbFrameMeta();
  return state;
}

json BspDevice::latestRgbFrameMeta() const {
  std::lock_guard<std::mutex> lock(latestRgbFrameMutex_);
  return latestRgbFrameMeta_.is_object() ? latestRgbFrameMeta_ : json::object();
}

json BspDevice::latestTemperatures() const {
  std::lock_guard<std::mutex> lock(latestTemperaturesMutex_);
  return latestTemperatures_.is_object() ? latestTemperatures_ : json::object();
}

json BspDevice::rebootViaSshAndWait(int disconnectTimeoutMs,
                                    int reconnectTimeoutMs,
                                    int pollIntervalMs) {
  if (!sshRequired_) {
    return {{"success", false}, {"message", "SSH disabled for this device"}};
  }
  auto rebootResult = sshManager_.executeCommand("/sbin/reboot", 3000);
  if (!rebootResult.value("success", false)) {
    const std::string stderrText =
        rebootResult.value("stderr", std::string());
    const std::string stdoutText =
        rebootResult.value("stdout", std::string());
    if (stderrText.find("closed") == std::string::npos &&
        stdoutText.find("closed") == std::string::npos) {
      return rebootResult;
    }
  }

  auto disconnectDeadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(disconnectTimeoutMs);
  while (std::chrono::steady_clock::now() < disconnectDeadline) {
    if (!sshManager_.ping()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
  }

  if (!sshManager_.waitUntilServerStarts(pollIntervalMs, reconnectTimeoutMs)) {
    return {{"success", false},
            {"message", "SSH server did not come back after reboot"}};
  }
  std::this_thread::sleep_for(std::chrono::seconds(1));
  return {{"success", true}, {"message", ""}};
}

json BspDevice::captureRawFrame(const json &params) {
  if (!sshRequired_) {
    return {{"success", false}, {"message", "SSH disabled for this device"}};
  }
  const QString remoteRaw = QString::fromStdString(
      params.value("remote_path", std::string(DEFAULT_RAW_REMOTE_PATH)));
  const QString remoteFrame = QString::fromStdString(params.value(
      "remote_frame_path", std::string(DEFAULT_RAW_REMOTE_FRAME_PATH)));
  const QString remoteTimestamp = QString::fromStdString(params.value(
      "remote_timestamp_path", std::string(DEFAULT_RAW_REMOTE_TIMESTAMP_PATH)));
  const double readyTimeoutS = params.value("file_ready_timeout_s", 12.0);
  const double pollIntervalS = params.value("poll_interval_s", 0.1);

  QStringList args;
  args << QString::fromStdString(sshManager_.hostname())
       << QString::number(sshManager_.port())
       << QString::fromStdString(sshManager_.username())
       << QString::fromStdString(sshManager_.password()) << localCameratestPath()
       << QString::fromUtf8(REMOTE_CAMERATEST_PATH) << remoteRaw << remoteFrame
       << remoteTimestamp
       << (params.value("cleanup_remote_before_capture", true)
               ? QStringLiteral("1")
               : QStringLiteral("0"))
       << QString::number(readyTimeoutS, 'f', 3)
       << QString::number(pollIntervalS, 'f', 3)
       << QString::number(params.value("raw_resolution", DEFAULT_RAW_RESOLUTION))
       << QString::number(
              params.value("raw_exposure_mode", DEFAULT_RAW_EXPOSURE_MODE))
       << QString::number(
              params.value("raw_exposure_value", DEFAULT_RAW_EXPOSURE_VALUE))
       << QString::number(params.value("raw_gain", DEFAULT_RAW_GAIN));

  const int scriptTimeoutMs =
      std::max(static_cast<int>(readyTimeoutS * 1000.0) + 10000, 15000);
  auto result = runScript(rawCaptureScriptPath(), args, scriptTimeoutMs);
  if (!result.value("success", false)) {
    const std::string stderrText = result.value("stderr", std::string());
    const std::string stdoutText = result.value("stdout", std::string());
    return {{"success", false},
            {"message", stderrText.empty() ? stdoutText : stderrText}};
  }

  QString stdoutText =
      QString::fromStdString(result.value("stdout", std::string())).trimmed();
  QStringList lines = stdoutText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  bool ok = false;
  const qint64 remoteSize =
      lines.isEmpty() ? 0 : lines.last().trimmed().toLongLong(&ok);
  if (!ok || remoteSize <= 0) {
    return {{"success", false}, {"message", "invalid raw capture size"}};
  }
  return {{"success", true},
          {"message", ""},
          {"remote_path", remoteRaw.toStdString()},
          {"remote_frame_path", remoteFrame.toStdString()},
          {"remote_timestamp_path", remoteTimestamp.toStdString()},
          {"remote_size", remoteSize}};
}

void BspDevice::onImuData(const json &rawImuData) {
  // 将 SDK 原生 IMU 格式转成主链路统一的 type/timestamp/data 结构。
  /*
   * BSP IMU回调
   *
   * rawImuData 来自 XrGlasses SDK: {imu_idx, hmd_time_ns,
   *   hasGyro, gyro[3], hasAcc, acc[3], hasMag, mag[3], temperature}
   *
   * 转换为标准格式: {type, timestamp_ns, data[6]}
   * IMU0: type 1(gyro), 2(acc), 3(mag), 12(temp)
   * IMU1: type 4(gyro), 5(acc), 13(temp)
   */
  if (!imuCallback_)
    return;
  try {
    static ImuSourceProbe sourceProbe;
    const int64_t arrivalNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
    int64_t timestampNs = rawImuData.value("hmd_time_ns", int64_t(0));
    int imuIdx = rawImuData.value("imu_idx", 0);
    logImuSourceProbe("BspDevice::onImuData", sourceProbe, arrivalNs,
                      timestampNs, imuIdx);

    int gyroType, accType, tempType;
    int magType = -1;
    if (imuIdx == 1) {
      gyroType = 4;
      accType = 5;
      tempType = 13;
    } else {
      gyroType = 1;
      accType = 2;
      magType = 3;
      tempType = 12;
    }

    auto gyro = rawImuData.value("gyro", std::vector<double>{0, 0, 0});
    auto acc = rawImuData.value("acc", std::vector<double>{0, 0, 0});
    auto mag = rawImuData.value("mag", std::vector<double>{0, 0, 0});
    double temp = rawImuData.value("temperature", 0.0);
    {
      std::lock_guard<std::mutex> lock(latestTemperaturesMutex_);
      latestTemperatures_[imuIdx == 1 ? "imu1_temperature"
                                      : "imu0_temperature"] = temp;
    }

    bool hasGyro = rawImuData.value("hasGyro", false) && gyro.size() >= 3;
    bool hasAcc = rawImuData.value("hasAcc", false) && acc.size() >= 3;
    bool hasMag = rawImuData.value("hasMag", false) && mag.size() >= 3;

    if (hasGyro) {
      imuCallback_({{"type", gyroType},
                    {"timestamp_ns", timestampNs},
                    {"data", {gyro[0], gyro[1], gyro[2], 0.0, 0.0, 0.0}}});
      imuCallback_({{"type", tempType},
                    {"timestamp_ns", timestampNs},
                    {"data", {temp, 0.0, 0.0, 0.0, 0.0, 0.0}}});
    }
    if (hasAcc) {
      imuCallback_({{"type", accType},
                    {"timestamp_ns", timestampNs},
                    {"data", {acc[0], acc[1], acc[2], 0.0, 0.0, 0.0}}});
    }
    if (hasMag && magType >= 0) {
      imuCallback_({{"type", magType},
                    {"timestamp_ns", timestampNs},
                    {"data", {mag[0], mag[1], mag[2], 0.0, 0.0, 0.0}}});
    }
  } catch (const std::exception &e) {
    std::cerr << "[BspDevice] IMU callback error: " << e.what() << std::endl;
  }
}

void BspDevice::onCamData(const json &rawCamData) {
  // 将 SDK 原生相机消息转换成主链路使用的 cam_data 结构。
  /*
   * BSP 图像回调
   *
   * 转换为标准格式: {timestamp, cam_data: {0: {image_raw, exposure_*, gain,
   * ...}}}
   */
  if (!imageCallback_)
    return;
  try {
    json camMessage;
    camMessage["timestamp"] = rawCamData.value("timestamp", 0.0);
    camMessage["cam_data"] = json::object();

    auto camData = rawCamData.value("cam_data", json::object());
    for (auto &[key, camInfo] : camData.items()) {
      // Forward the cam info as-is (image bytes handled by MainSubnode)
      camMessage["cam_data"][key] = camInfo;
      if (cameraMode_ == CAMERA_MODE_RGB && key == "0" && camInfo.is_object()) {
        json snapshot = camInfo;
        snapshot["timestamp_ns"] = rawCamData.value("timestamp", int64_t(0));
        snapshot["received_at_monotonic_ns"] =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (camInfo.contains("temperature")) {
          std::lock_guard<std::mutex> tempLock(latestTemperaturesMutex_);
          latestTemperatures_["rgb_temperature"] = camInfo["temperature"];
        }
        std::lock_guard<std::mutex> lock(latestRgbFrameMutex_);
        latestRgbFrameMeta_ = snapshot;
      }
    }
    imageCallback_(camMessage);
  } catch (const std::exception &e) {
    std::cerr << "[BspDevice] Camera callback error: " << e.what() << std::endl;
  }
}

// ============================================================================
// BspRecordingSubnode
// ============================================================================

BspRecordingSubnode::BspRecordingSubnode(const QString &name,
                                         const QString &subnodeHost,
                                         int goalPort, int feedbackPort,
                                         const QString &rootPath,
                                         BaseDevice *device, QObject *parent)
    : MainSubnode(name, subnodeHost, goalPort, feedbackPort, rootPath, device,
                  parent) {
  // 在主录制子节点基础上补充 BSP 专属命令和录制后收尾逻辑。
  registerBspCommands();
  std::cout
      << "[" << name.toStdString()
      << "] BspRecordingSubnode initialized (New Version with glass_config)"
      << std::endl;
}

BspRecordingSubnode::~BspRecordingSubnode() = default;

void BspRecordingSubnode::registerBspCommands() {
  // 注册 BSP 专属命令，如获取 glass_config 和固件版本。
  registerCmd("check",
              [this](uint32_t, const std::string &, const json &params) {
                if (auto *bspDevice = dynamic_cast<BspDevice *>(device_)) {
                  const bool skipSsh = params.value("skip_ssh", false);
                  const bool sshOptional = params.value("ssh_optional", false);
                  if (params.contains("ssh_required")) {
                    bspDevice->setSshRequired(params.value("ssh_required", true));
                  } else if (skipSsh || sshOptional) {
                    bspDevice->setSshRequired(false);
                  }
                }
                return this->check();
              });
  registerCmd("get_glasses_config",
              [this](uint32_t g, const std::string &c, const json &p) {
                return cmdGetGlassesConfig(g, c, p);
              });
  registerCmd("get_firmware_version",
              [this](uint32_t g, const std::string &c, const json &p) {
                return cmdGetFirmwareVersion(g, c, p);
              });
  registerCmd("get_bsp_runtime_state",
              [this](uint32_t g, const std::string &c, const json &p) {
                return cmdGetBspRuntimeState(g, c, p);
              });
  registerCmd("capture_raw_frame",
              [this](uint32_t g, const std::string &c, const json &p) {
                return cmdCaptureRawFrame(g, c, p);
              });
}

json BspRecordingSubnode::onCheck() {
  // 先复用 MainSubnode/device 检查，再补充 BSP/SSH 侧约束。
  if (!sshManager_) {
    return {{"success", false}, {"message", "No SSH manager"}};
  }

  // 先复用 MainSubnode/device 侧的检查，这里面已经聚合了：
  // 1. XREAL vendored 资产是否可用
  // 2. Qt 运行时是否满足当前原生库要求
  // 3. SSH 是否可达
  auto baseResult = MainSubnode::onCheck();
  if (!baseResult.value("success", false)) {
    return baseResult;
  }

  baseResult["success"] = true;
  baseResult["message"] = "BSP connected";
  return baseResult;
}

json BspRecordingSubnode::cmdGetGlassesConfig(uint32_t, const std::string &,
                                              const json &params) {
  // 通过 SSH 将眼镜上的 glass_config.json 拉到本地指定位置。
  auto *bspDevice = dynamic_cast<BspDevice *>(device_);
  if (bspDevice && !bspDevice->sshRequired()) {
    return {{"success", false}, {"message", "SSH disabled for this device"}};
  }
  if (!sshManager_)
    return {{"success", false}, {"message", "No SSH manager"}};
  std::string localPath =
      params.value("local_path", std::string("./glass_config.json"));
  std::string remotePath =
      params.value("remote_path", std::string("/data/glass_config.json"));
  return sshManager_->copyFileFromGlasses(remotePath, localPath);
}

json BspRecordingSubnode::cmdGetFirmwareVersion(uint32_t, const std::string &,
                                                const json &) {
  // 通过 SSH 读取眼镜当前固件版本。
  auto *bspDevice = dynamic_cast<BspDevice *>(device_);
  if (bspDevice && !bspDevice->sshRequired()) {
    return {{"success", false}, {"message", "SSH disabled for this device"}};
  }
  if (!sshManager_)
    return {{"success", false}, {"message", "No SSH manager"}};
  auto result = sshManager_->executeCommand("cat /etc/firmware_version");
  if (result.value("success", false)) {
    return {{"success", true},
            {"version", result.value("stdout", std::string("unknown"))}};
  }
  return result;
}

json BspRecordingSubnode::cmdGetBspRuntimeState(uint32_t, const std::string &,
                                                const json &) {
  auto *bspDevice = dynamic_cast<BspDevice *>(device_);
  if (!bspDevice) {
    return {{"success", false}, {"message", "No BSP device"}};
  }
  json state = bspDevice->runtimeState();
  state["command"] = "get_bsp_runtime_state";
  state["record_state"] = recordingState();
  return state;
}

json BspRecordingSubnode::cmdCaptureRawFrame(uint32_t, const std::string &,
                                             const json &params) {
  auto *bspDevice = dynamic_cast<BspDevice *>(device_);
  if (!bspDevice) {
    return {{"success", false},
            {"message", "No BSP raw capture device available"},
            {"command", "capture_raw_frame"}};
  }
  if (!bspDevice->sshRequired()) {
    return {{"success", false},
            {"message", "SSH disabled for this device"},
            {"command", "capture_raw_frame"}};
  }
  if (!params.value("direct_capture_without_start_device", false)) {
    if (!bspDevice->isStarted()) {
      return {{"success", false},
              {"message", "Device not started"},
              {"command", "capture_raw_frame"}};
    }
    if (bspDevice->cameraMode() != CAMERA_MODE_RGB) {
      return {{"success", false},
              {"message", "Device is not in RGB mode"},
              {"command", "capture_raw_frame"}};
    }
  }

  json frameMeta = bspDevice->latestRgbFrameMeta();
  const auto preCaptureRgbTemperature =
      jsonNumberIfPresent(frameMeta, "temperature");
  if (frameMeta.empty() &&
      !params.value("direct_capture_without_start_device", false)) {
    return {{"success", false},
            {"message", "No RGB frame metadata snapshot available"},
            {"command", "capture_raw_frame"}};
  }

  const std::string datasetName =
      params.value("dataset_name", std::string("raw_capture"));
  const QString targetSubdir = QString::fromStdString(
      params.value("target_subdir", std::string(DEFAULT_RAW_TARGET_SUBDIR)));
  const qint64 timestampNs =
      frameMeta.empty()
          ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count()
          : captureTimestampNsFromMeta(frameMeta);
  const QString rawFilename = buildCaptureFilename(timestampNs, QStringLiteral(".raw"));
  const QString captureDir =
      QDir(rootPath_).filePath(QString::fromStdString(datasetName));
  const QString imageDir = QDir(captureDir).filePath(targetSubdir);
  QDir().mkpath(imageDir);
  const QString localRawPath = QDir(imageDir).filePath(rawFilename);

  if (QFileInfo::exists(localRawPath)) {
    return {{"success", true},
            {"message", "Raw frame already exists: " + localRawPath.toStdString()},
            {"command", "capture_raw_frame"},
            {"capture_dir", captureDir.toStdString()},
            {"raw_file", localRawPath.toStdString()},
            {"canonical_timestamp_ns", timestampNs},
            {"skipped_existing", true}};
  }

  const bool releasedForCapture =
      bspDevice->isStarted() &&
      !params.value("direct_capture_without_start_device", false);
  if (releasedForCapture) {
    auto releaseResult = bspDevice->release();
    if (!releaseResult.value("success", false)) {
      return {{"success", false},
              {"message", "release_device: " +
                              releaseResult.value("message", std::string())},
              {"command", "capture_raw_frame"}};
    }
    auto rebootResult = bspDevice->rebootViaSshAndWait();
    if (!rebootResult.value("success", false)) {
      return {{"success", false},
              {"message", "reboot_before_capture: " +
                              rebootResult.value("message", std::string())},
              {"command", "capture_raw_frame"}};
    }
  }

  json captureResult = bspDevice->captureRawFrame(params);
  if (!captureResult.value("success", false)) {
    return {{"success", false},
            {"message", "capture_raw_via_cameratest: " +
                            captureResult.value("message", std::string())},
            {"command", "capture_raw_frame"}};
  }

  auto fetchRaw = sshManager_->copyFileFromGlasses(
      params.value("remote_path", std::string(DEFAULT_RAW_REMOTE_PATH)),
      localRawPath.toStdString());
  if (!fetchRaw.value("success", false)) {
    QFile::remove(localRawPath);
    return {{"success", false},
            {"message", "download_raw_file: " +
                            fetchRaw.value("message", std::string())},
            {"command", "capture_raw_frame"}};
  }

  QString frameFilePath;
  QString glassTimestampFilePath;
  const QString frameTmp = QDir(imageDir).filePath(QStringLiteral(".frame.tmp"));
  const QString timestampTmp =
      QDir(imageDir).filePath(QStringLiteral(".glass_timestamp.tmp"));
  const std::string remoteFrame = captureResult.value(
      "remote_frame_path", std::string(DEFAULT_RAW_REMOTE_FRAME_PATH));
  const std::string remoteTimestamp = captureResult.value(
      "remote_timestamp_path", std::string(DEFAULT_RAW_REMOTE_TIMESTAMP_PATH));
  auto fetchFrame = sshManager_->copyFileFromGlasses(remoteFrame, frameTmp.toStdString());
  if (fetchFrame.value("success", false)) {
    auto fetchTimestamp =
        sshManager_->copyFileFromGlasses(remoteTimestamp, timestampTmp.toStdString());
    QFile frameFile(frameTmp);
    QFile timestampFile(timestampTmp);
    QString frameText;
    QString timestampText = QStringLiteral("__MISSING__");
    if (frameFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
      frameText = QString::fromUtf8(frameFile.readAll());
    }
    if (fetchTimestamp.value("success", false) &&
        timestampFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
      timestampText = QString::fromUtf8(timestampFile.readAll());
    }
    frameFilePath = QDir(imageDir).filePath(QStringLiteral("frame.txt"));
    glassTimestampFilePath =
        QDir(imageDir).filePath(QStringLiteral("glass_timestamp.txt"));
    QFile frameOut(frameFilePath);
    if (frameOut.open(QIODevice::Append | QIODevice::Text)) {
      QTextStream stream(&frameOut);
      stream << rawFilename << " " << normalizedSidecarText(frameText) << "\n";
    }
    QFile timestampOut(glassTimestampFilePath);
    if (timestampOut.open(QIODevice::Append | QIODevice::Text)) {
      QTextStream stream(&timestampOut);
      stream << rawFilename << " "
             << normalizedSidecarText(timestampText, QStringLiteral("__MISSING__"))
             << "\n";
    }
  }
  QFile::remove(frameTmp);
  QFile::remove(timestampTmp);

  QString metadataFilePath;
  if (!frameMeta.empty()) {
    metadataFilePath = QDir(imageDir).filePath(QStringLiteral("metadata.txt"));
    QFile metadataFile(metadataFilePath);
    if (metadataFile.open(QIODevice::Append | QIODevice::Text)) {
      QTextStream stream(&metadataFile);
      stream << rawFilename << " " << timestampNs << " "
             << frameMeta.value("exposure_duration", qint64(0)) << " "
             << frameMeta.value("gain", 0.0) << " "
             << frameMeta.value("exposure_start_time_system", qint64(0)) << " "
             << frameMeta.value("exposure_start_time_device", qint64(0)) << " "
             << frameMeta.value("rolling_shutter_time", qint64(0)) << " "
             << frameMeta.value("stride", 0) << " "
             << QString::number(frameMeta.value("temperature", 0.0), 'f', 2)
             << "\n";
    }
  }

  bool restoreSuccess = true;
  std::string restoreMessage;
  std::optional<double> restoredFirstRgbTemperature;
  if (releasedForCapture) {
    auto rebootResult = bspDevice->rebootViaSshAndWait();
    restoreSuccess = rebootResult.value("success", false);
    restoreMessage = rebootResult.value("message", std::string());
    if (restoreSuccess) {
      auto initResult = bspDevice->initialize({{"skip_restart_check", true}});
      restoreSuccess = initResult.value("success", false);
      restoreMessage = initResult.value("message", std::string());
    }
    if (restoreSuccess) {
      auto startResult = bspDevice->start({{"camera_mode", CAMERA_MODE_RGB}});
      restoreSuccess = startResult.value("success", false);
      restoreMessage = startResult.value("message", std::string());
      if (restoreSuccess) {
        restoredFirstRgbTemperature = firstAvailableRgbTemperature(bspDevice);
      }
    }
  }

  std::optional<double> averageRgbTemperature;
  if (preCaptureRgbTemperature && restoredFirstRgbTemperature) {
    averageRgbTemperature =
        (*preCaptureRgbTemperature + *restoredFirstRgbTemperature) / 2.0;
  } else if (preCaptureRgbTemperature) {
    averageRgbTemperature = *preCaptureRgbTemperature;
  } else if (restoredFirstRgbTemperature) {
    averageRgbTemperature = *restoredFirstRgbTemperature;
  }

  json response = {{"success", restoreSuccess},
                   {"message",
                    restoreSuccess
                        ? "Raw frame captured: " + localRawPath.toStdString()
                        : "Raw frame captured, but failed to restore RGB: " +
                              restoreMessage},
                   {"command", "capture_raw_frame"},
                   {"capture_dir", captureDir.toStdString()},
                   {"raw_file", localRawPath.toStdString()},
                   {"metadata_file", nullptr},
                   {"frame_file", nullptr},
                   {"glass_timestamp_file", nullptr},
                   {"remote_size", captureResult.value("remote_size", 0)},
                   {"canonical_timestamp_ns", timestampNs},
                   {"pre_capture_rgb_temperature", nullptr},
                   {"restored_first_rgb_temperature", nullptr},
                   {"average_rgb_temperature", nullptr},
                   {"rgb_restore_success", nullptr},
                   {"rgb_restore_message", restoreMessage}};
  if (!metadataFilePath.isEmpty()) {
    response["metadata_file"] = metadataFilePath.toStdString();
  }
  if (!frameFilePath.isEmpty()) {
    response["frame_file"] = frameFilePath.toStdString();
  }
  if (!glassTimestampFilePath.isEmpty()) {
    response["glass_timestamp_file"] = glassTimestampFilePath.toStdString();
  }
  if (releasedForCapture) {
    response["rgb_restore_success"] = restoreSuccess;
  }
  if (preCaptureRgbTemperature) {
    response["pre_capture_rgb_temperature"] = *preCaptureRgbTemperature;
  }
  if (restoredFirstRgbTemperature) {
    response["restored_first_rgb_temperature"] = *restoredFirstRgbTemperature;
  }
  if (averageRgbTemperature) {
    response["average_rgb_temperature"] = *averageRgbTemperature;
  }
  return response;
}

void BspRecordingSubnode::onRecordStopped() {
  // 录制结束后补充保存 record_info 和 glass_config 等 BSP 附加产物。
  if (recordPath_.isEmpty())
    return;
  saveRecordInfo();
  fetchGlassConfig();
}

void BspRecordingSubnode::saveRecordInfo() {
  // 通过 getprop 获取眼镜系统信息，并追加到 record_info.txt。
  /*
   * 通过 SSH 执行 getprop 并保存到 record_info.txt
   */
  auto *bspDevice = dynamic_cast<BspDevice *>(device_);
  if (bspDevice && !bspDevice->sshRequired()) {
    return;
  }
  if (!sshManager_) {
    std::cerr << "[BSP] No SSH manager, skipping record_info.txt" << std::endl;
    return;
  }
  try {
    auto result =
        sshManager_->executeCommand("/usr/usrdata/bin/getprop", 10000);
    QString infoPath = recordPath_ + "/record_info.txt";
    QFile file(infoPath);
    if (file.open(QIODevice::Append | QIODevice::Text)) {
      QTextStream stream(&file);
      stream << QDateTime::currentDateTime().toString("yyyyMMddHHmmss") << "\n";
      stream << QString::fromStdString(result.value("stdout", std::string("")));
      std::string stderrStr = result.value("stderr", std::string(""));
      if (!stderrStr.empty()) {
        stream << "\n[stderr]\n" << QString::fromStdString(stderrStr);
      }
      file.close();
      std::cout << "[BSP] Saved record_info.txt with getprop output"
                << std::endl;
    }
  } catch (const std::exception &e) {
    std::cerr << "[BSP] Failed to save record_info.txt: " << e.what()
              << std::endl;
  }
}

void BspRecordingSubnode::fetchGlassConfig() {
  // 优先通过 SCP 获取 glass_config；失败时退回旧 shell 脚本。
  /*
   * 通过 SCP 从眼镜获取 glass_config.json
   */
  auto *bspDevice = dynamic_cast<BspDevice *>(device_);
  if (bspDevice && !bspDevice->sshRequired()) {
    return;
  }
  if (!sshManager_ || recordPath_.isEmpty())
    return;
  try {
    std::string localConfig = recordPath_.toStdString() + "/glass_config.json";

    // 方法1: 直接 SCP 获取
    auto result = sshManager_->copyFileFromGlasses("/data/glass_config.json",
                                                   localConfig);
    if (result.value("success", false)) {
      std::cout << "[BSP] glass_config.json fetched successfully" << std::endl;
    } else {
      // 方法2: 尝试使用 shell 脚本
      QProcess script;
      script.start("bash", {"subnodes/nviz_node/shell/gf_3dof_end_record.sh",
                            recordPath_});
      if (script.waitForFinished(60000) && script.exitCode() == 0) {
        std::cout << "[BSP] glass_config.json fetched via shell script"
                  << std::endl;
      } else {
        std::cerr << "[BSP] Failed to fetch glass_config.json" << std::endl;
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "[BSP] Failed to fetch glass_config.json: " << e.what()
              << std::endl;
  }
}

// ============================================================================
// main() 入口
// ============================================================================

int bspMainSubnodeMain(int argc, char *argv[]) {
  // 独立 BSP 子节点入口：解析参数、创建设备与录制子节点并进入 Qt 事件循环。
  QCoreApplication app(argc, argv);

  // 信号处理
  auto signalHandler = [](int signum) {
    std::cout << "Received signal " << signum << ", shutting down..."
              << std::endl;
    QCoreApplication::quit();
  };
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  // 解析命令行参数
  QCommandLineParser parser;
  parser.addOption({"name", "Node name", "BspMainSubnode"});
  parser.addOption({"host", "Host address", "127.0.0.1"});
  parser.addOption({"goal-port", "Goal port", "5690"});
  parser.addOption({"feedback-port", "Feedback port", "5691"});
  parser.addOption({"root-path", "Data root path", "./output"});
  parser.addOption({"ssh-optional", "Allow embedded glasses without SSH"});
  parser.addOption({"preflight", "只执行 BSP/XREAL 本地预检并输出 JSON 后退出"});
  parser.process(app);

  QString name = parser.value("name");
  if (name.isEmpty())
    name = "BspMainSubnode";
  QString host = parser.value("host");
  if (host.isEmpty())
    host = "127.0.0.1";
  int goalPort = parser.value("goal-port").toInt();
  if (goalPort == 0)
    goalPort = 5690;
  int feedbackPort = parser.value("feedback-port").toInt();
  if (feedbackPort == 0)
    feedbackPort = 5691;
  QString rootPath = parser.value("root-path");
  if (rootPath.isEmpty())
    rootPath = "./output";

  if (parser.isSet("preflight")) {
    const recordlab::bsp::NativeGlassesAdapter adapter;
    const auto report = adapter.preflight();
    std::cout << report.toJson().dump(2) << std::endl;
    return report.canCreateGlasses() ? 0 : 2;
  }

  // Python 层事件循环 ticker (确保 signal handler 可以被调用)
  QTimer ticker;
  QObject::connect(&ticker, &QTimer::timeout, []() {});
  ticker.start(500);

  // 创建运动检测器
  auto motionDetector = std::make_unique<recordlab::common::MotionDetector>(
      0.5, 0.03, 0.1, true, false);

  // 创建双 IMU 写入器
  auto imuWriter0 = std::make_unique<CsvDataWriter>(IMU0_FILENAME, BUFFER_SIZE);
  auto imuWriter1 = std::make_unique<CsvDataWriter>(IMU1_FILENAME, BUFFER_SIZE);
  auto imageWriter =
      std::make_unique<ImageDataWriter>(IMAGE_FILENAME, BUFFER_SIZE);
  auto rgbImageWriter = std::make_unique<RgbImageDataWriter>(BUFFER_SIZE);

  // 创建 BSP 设备
  BspDevice device;
  if (parser.isSet("ssh-optional")) {
    device.setSshRequired(false);
  }

  // 创建录制子节点
  BspRecordingSubnode subnode(name, host, goalPort, feedbackPort, rootPath,
                              &device);

  // 设置 IMU 写入器
  subnode.addImuWriter(0, std::move(imuWriter0));
  subnode.addImuWriter(1, std::move(imuWriter1));
  subnode.setImageWriter(std::move(imageWriter));
  subnode.setRgbImageWriter(std::move(rgbImageWriter));

  // 设置运动检测器
  subnode.setMotionDetector(motionDetector.get());

  // 设置 SSH 管理器
  subnode.setSshManager(&device.sshManager());

  // 创建发布器
  subnode.createMainPublishers();

  // 连接
  auto result = subnode.connect();
  if (!result.value("success", false)) {
    std::cerr << "Failed to connect: "
              << result.value("message", std::string("Unknown error"))
              << std::endl;
    return 1;
  }

  std::cout << "[" << name.toStdString() << "] SubNode ready" << std::endl;

  int exitCode = app.exec();
  std::cout << "Qt application exited with code: " << exitCode << std::endl;

  try {
    subnode.disconnect();
    subnode.release();
  } catch (const std::exception &e) {
    std::cerr << "Cleanup error: " << e.what() << std::endl;
  }

  return exitCode;
}

} // namespace recordlab::subnodes
