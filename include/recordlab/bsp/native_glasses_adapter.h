/*
 * native_glasses_adapter.h
 *
 * 这是 BSP 真机链路进入真正 ABI 集成前的“原生适配层门面”。
 *
 * 当前它先承担三件事：
 * 1. 统一解析 RecordLabC 自己的 vendored XREAL 资产路径；
 * 2. 统一输出 createGlasses 前的预检报告；
 * 3. 在原生 ABI 尚未接入前，给出明确、结构化、可复用的阻塞结果。
 *
 * 之所以提前把这层抽出来，是为了避免：
 * - UI 一套判断
 * - 子节点一套判断
 * - 文档再写一套判断
 *
 * 之后真正接入 ABI 时，只需要把这个类从“预检/阻塞门面”继续扩成
 * “真实设备访问层”，而不是再推倒一轮接口。
 */
#pragma once

#include <QByteArray>
#include <QEventLoop>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

#include "recordlab/bsp/xreal_sdk_compat.h"
#include "recordlab/common/camera_shared_memory.h"

namespace recordlab::bsp {

struct NativeGlassesPreflightReport {
  QString projectRoot;
  QString wheelPath;
  QString pyiPath;
  QString runtimeRoot;
  QString runtimeSitePackages;
  QString runtimeGlassesServerPath;
  QString currentProcessQtVersion;
  QString runtimeQtVersion;
  QString runtimeQtSource;
  QString requiredQtVersion;
  bool projectRootDetected = false;
  bool wheelAvailable = false;
  bool pyiAvailable = false;
  bool runtimeRootAvailable = false;
  bool runtimeSitePackagesAvailable = false;
  bool runtimeGlassesServerAvailable = false;
  bool runtimeQtCompatible = false;
  bool nativeBridgeImplemented = false;
  QStringList blockers;

  // 当前是否已经满足“可以真正尝试 createGlasses”的前提。
  bool canCreateGlasses() const;
  // 供日志/页面直接展示的短摘要。
  QString summary() const;
  // 供 CLI / 调试工具输出的结构化结果。
  nlohmann::json toJson() const;
};

class NativeGlassesAdapter {
public:
  explicit NativeGlassesAdapter(QString projectRoot = {});
  ~NativeGlassesAdapter();

  NativeGlassesPreflightReport preflight() const;

  using ImuCallback = std::function<void(const nlohmann::json &)>;
  using ImageCallback = std::function<void(const nlohmann::json &)>;

  void setCallbacks(ImuCallback imuCallback, ImageCallback imageCallback);
  void clearCallbacks();

  nlohmann::json createGlasses(void *&opaqueHandle, int timeoutMs = 30000,
                               int retryCount = 3, int retryDelayMs = 1500);
  nlohmann::json openGlasses(void *opaqueHandle);
  nlohmann::json startSensors(void *opaqueHandle, int sensorMask);
  nlohmann::json stopSensors(void *opaqueHandle, int sensorMask);
  nlohmann::json configureGlasses(void *opaqueHandle,
                                  const nlohmann::json &params);
  nlohmann::json glassesState(void *opaqueHandle);
  nlohmann::json closeGlasses(void *&opaqueHandle);

  /// 通过 bridge 枚举 USB 上的眼镜设备，返回 product_id 列表。
  /// 不需要先 createGlasses，可随时调用（会自动启动 bridge）。
  nlohmann::json enumerateDevices();

private:
  QString effectiveProjectRoot() const;
  QString bridgeScriptPath() const;
  QString resolveBridgePythonProgram() const;
  bool ensureBridgeStarted(QString *errorMessage = nullptr);
  bool ensureCameraShmWriter();
  void shutdownBridge();
  QProcessEnvironment buildBridgeEnvironment(const QString &projectRoot) const;
  nlohmann::json sendRequest(const QString &action,
                             const nlohmann::json &payload = {},
                             const QByteArray &binaryPayload = {},
                             int timeoutMs = 10000);
  void handleStdoutReadyRead();
  void handleStderrReadyRead();
  void handleBridgeFinished(int exitCode, QProcess::ExitStatus exitStatus);
  void parseOutputFrames();
  void handleBridgeMessage(const nlohmann::json &header,
                           const QByteArray &payloadBytes);
  nlohmann::json
  buildBridgeUnavailableResult(const NativeGlassesPreflightReport &report,
                               const QString &prefixMessage) const;

  QString projectRoot_;
  ImuCallback imuCallback_;
  ImageCallback imageCallback_;
  std::unique_ptr<QProcess> bridgeProcess_;
  QByteArray bridgeStdoutBuffer_;
  QString lastBridgeError_;
  quint64 nextRequestId_ = 1;
  QString waitingRequestId_;
  nlohmann::json waitingResponse_;
  QPointer<QEventLoop> waitingLoop_;

  // 相机帧节流：限制 base64 编码 + JSON 构建的频率
  int64_t lastCameraCallbackNs_ = 0;
  static constexpr int64_t kCameraCallbackIntervalNs =
      33'333'333; // 30Hz

  // IPC 共享内存写入端
  std::unique_ptr<recordlab::common::CameraSharedMemoryWriter> cameraShmWriter_;
};

} // namespace recordlab::bsp
