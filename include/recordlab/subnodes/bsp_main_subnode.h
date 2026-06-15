/*
 * BspMainSubnode - BSP 硬件数据采集节点
 *
 * 完整类层次（与 Python 版 1:1 对应）:
 *
 *   GlassesFactory   — 线程安全的 XrGlasses Qt 对象管理（信号/槽在主线程执行）
 *   XrGlassesSSHManager — SSH/ping/reboot 连接管理
 *   BspDevice : BaseDevice — BSP 设备接口（枚举/打开/IMU+Cam 回调）
 *   BspRecordingSubnode : MainSubnode — 录制子节点（record_info.txt +
 * glass_config.json）
 *
 */
#pragma once

#include "recordlab/bsp/native_glasses_adapter.h"
#include "recordlab/subnodes/main_subnode.h"

#include <QMutex>
#include <QProcess>
#include <QWaitCondition>

#include <functional>
#include <memory>
#include <mutex>
#include <set>

namespace recordlab::subnodes {

// ============================================================================
// GlassesFactory — 线程安全的眼镜设备对象工厂
// ============================================================================

class GlassesFactory : public QObject {
  Q_OBJECT

public:
  explicit GlassesFactory(QObject *parent = nullptr);
  ~GlassesFactory() override;

  // 从子线程调用，在主线程安全执行
  nlohmann::json createGlasses();
  nlohmann::json openGlasses();
  nlohmann::json startSensors(int sensorMask);
  nlohmann::json stopSensors(int sensorMask);
  nlohmann::json configureGlasses(const nlohmann::json &params);
  nlohmann::json glassesState();
  nlohmann::json closeGlasses();
  nlohmann::json enumerateDevices();

  // 回调管理
  using ImuCallback = std::function<void(const nlohmann::json &)>;
  using ImageCallback = std::function<void(const nlohmann::json &)>;
  void setCallbacks(ImuCallback imuCb, ImageCallback imageCb);
  void disconnectCallbacks();

  // 设备指针 (SDK 集成点)
  void *glasses() const { return glasses_; }

signals:
  void createGlassesSignal();
  void openGlassesSignal();
  void startSensorsSignal(int mask);
  void stopSensorsSignal(int mask);
  void configureGlassesSignal(QString paramsJson);
  void queryGlassesStateSignal();
  void closeGlassesSignal();
  void enumerateDevicesSignal();

private slots:
  void onCreateGlassesSlot();
  void onOpenGlassesSlot();
  void onStartSensorsSlot(int mask);
  void onStopSensorsSlot(int mask);
  void onConfigureGlassesSlot(QString paramsJson);
  void onQueryGlassesStateSlot();
  void onCloseGlassesSlot();
  void onEnumerateDevicesSlot();

private:
  void setResult(const nlohmann::json &result);

  void *glasses_ = nullptr; // XrGlasses SDK object (opaque)
  ImuCallback imuCallback_;
  ImageCallback imageCallback_;

  QMutex mutex_;
  QWaitCondition waitCondition_;
  nlohmann::json pendingResult_;
  std::unique_ptr<recordlab::bsp::NativeGlassesAdapter> adapter_;
};

// ============================================================================
// XrGlassesSSHManager — SSH/ping 连接管理
// ============================================================================

class XrGlassesSSHManager {
public:
  static constexpr const char *DEFAULT_HOSTNAME = "169.254.2.1";
  static constexpr int DEFAULT_PORT = 22;
  static constexpr const char *DEFAULT_USERNAME = "root";
  static constexpr const char *DEFAULT_PASSWORD = "xreal2017";

  explicit XrGlassesSSHManager(const std::string &hostname = DEFAULT_HOSTNAME,
                               int port = DEFAULT_PORT,
                               const std::string &username = DEFAULT_USERNAME,
                               const std::string &password = DEFAULT_PASSWORD);

  // 连接检测
  bool ping() const;
  bool waitUntilServerStarts(int intervalMs = 1000,
                             int timeoutMs = 120000) const;
  void checkAndWaitRestarted();

  // SSH 命令执行
  nlohmann::json executeCommand(const std::string &command,
                                int timeoutMs = 5000) const;

  // 文件传输 (SCP)
  nlohmann::json copyFileFromGlasses(const std::string &remotePath,
                                     const std::string &localPath) const;
  nlohmann::json copyFileToGlasses(const std::string &localPath,
                                   const std::string &remotePath) const;

  // ADB
  nlohmann::json adbPull(const std::string &remotePath,
                         const std::string &localPath) const;

  // Accessors
  const std::string &hostname() const { return hostname_; }
  int port() const { return port_; }
  const std::string &username() const { return username_; }
  const std::string &password() const { return password_; }

private:
  std::string hostname_;
  int port_;
  std::string username_;
  std::string password_;
};

// ============================================================================
// BspDevice — BSP 设备接口
// ============================================================================

class BspDevice : public BaseDevice {
public:
  explicit BspDevice();
  ~BspDevice() override;

  void setSshRequired(bool required) { sshRequired_ = required; }
  bool sshRequired() const { return sshRequired_; }

  nlohmann::json initialize(const nlohmann::json &params) override;
  nlohmann::json start(const nlohmann::json &params = {}) override;
  nlohmann::json stop(const nlohmann::json &params = {}) override;
  nlohmann::json release() override;
  nlohmann::json control(const nlohmann::json &params) override;
  nlohmann::json check() override;
  nlohmann::json runtimeState() const;
  nlohmann::json latestRgbFrameMeta() const;
  nlohmann::json latestTemperatures() const;
  nlohmann::json captureRawFrame(const nlohmann::json &params);
  nlohmann::json rebootViaSshAndWait(int disconnectTimeoutMs = 30000,
                                     int reconnectTimeoutMs = 10000,
                                     int pollIntervalMs = 2000);

  bool isStarted() const { return isStarted_; }
  const std::string &cameraMode() const { return cameraMode_; }

  void setImuDataCallback(ImuCallback cb) override {
    imuCallback_ = std::move(cb);
  }
  void setImageDataCallback(ImageCallback cb) override {
    imageCallback_ = std::move(cb);
  }

  XrGlassesSSHManager &sshManager() { return sshManager_; }
  GlassesFactory &factory() { return factory_; }

private:
  // 内部 SDK 回调 → 统一 JSON 格式
  void onImuData(const nlohmann::json &rawImuData);
  void onCamData(const nlohmann::json &rawCamData);

  ImuCallback imuCallback_;
  ImageCallback imageCallback_;
  XrGlassesSSHManager sshManager_;
  GlassesFactory factory_;

  bool isInitialized_ = false;
  bool isStarted_ = false;
  std::string cameraMode_ = "slam";
  mutable std::mutex latestRgbFrameMutex_;
  nlohmann::json latestRgbFrameMeta_;
  mutable std::mutex latestTemperaturesMutex_;
  nlohmann::json latestTemperatures_;
  double rgbFps_ = 15.0;
  bool rgbAutoExposure_ = false;
  int rgbExposureUs_ = 400;
  nlohmann::json rgbGain_;

  bool sshRequired_ = true;

  // 启动的传感器集合: 0=Imu, 1=Slam, 2=Rgb, 3=Display
  static constexpr int SENSOR_IMU = 0x01;
  static constexpr int SENSOR_SLAM = 0x02;
  static constexpr int SENSOR_RGB = 0x04;
  static constexpr int SENSOR_DISPLAY = 0x08;
  int startSensorMask_ = SENSOR_IMU | SENSOR_SLAM;
};

// ============================================================================
// BspRecordingSubnode — BSP 录制子节点（带配置获取）
// ============================================================================

class BspRecordingSubnode : public MainSubnode {
  Q_OBJECT

public:
  explicit BspRecordingSubnode(const QString &name = "BspMainSubnode",
                               const QString &subnodeHost = "127.0.0.1",
                               int goalPort = 5690, int feedbackPort = 5691,
                               const QString &rootPath = "./output",
                               BaseDevice *device = nullptr,
                               QObject *parent = nullptr);
  ~BspRecordingSubnode() override;

  void setSshManager(XrGlassesSSHManager *mgr) { sshManager_ = mgr; }

  // 覆盖
  nlohmann::json onCheck() override;

protected:
  void onRecordStopped() override;

private:
  void saveRecordInfo();
  void fetchGlassConfig();
  void registerBspCommands();

  nlohmann::json cmdGetGlassesConfig(uint32_t goalId, const std::string &cmd,
                                     const nlohmann::json &params);
  nlohmann::json cmdGetFirmwareVersion(uint32_t goalId, const std::string &cmd,
                                       const nlohmann::json &params);
  nlohmann::json cmdGetBspRuntimeState(uint32_t goalId,
                                       const std::string &cmd,
                                       const nlohmann::json &params);
  nlohmann::json cmdCaptureRawFrame(uint32_t goalId, const std::string &cmd,
                                    const nlohmann::json &params);

  XrGlassesSSHManager *sshManager_ = nullptr;
};

// ============================================================================
// main() 入口函数声明
// ============================================================================
int bspMainSubnodeMain(int argc, char *argv[]);

} // namespace recordlab::subnodes
