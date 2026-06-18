/*
 * DataReceiver - 数据接收器
 *
 * 订阅所有传感器 Topics（IMU/Camera/Timer/Delay/Motion），
 * 缓存最新数据，转换格式，跟踪频率。
 *
 * C++ 版本使用 echo::Subscriber + Qt 信号替代 multiprocessing.Queue。
 * 相机数据通过共享内存环形缓冲传输（与 Python 版架构一致）。
 *
 */
#pragma once

#include "recordlab/common/camera_shared_memory.h"

#include <QMutex>
#include <QObject>
#include <QSharedMemory>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

#include "recordlab/core/qt_json_compat.h"

namespace echo {
class Subscriber;
}

namespace recordlab::backend {

// IMU 类型映射
inline std::string imuTypeToName(int type) {
  static const std::unordered_map<int, std::string> map = {
      {1, "IMU0-gyro"},         {2, "IMU0-acc"},  {3, "IMU0-mag"},
      {12, "IMU0-temperature"}, {4, "IMU1-gyro"}, {5, "IMU1-acc"},
      {13, "IMU1-temperature"}};
  auto it = map.find(type);
  return it != map.end() ? it->second : "";
}

// 数据缓冲项（替代跨线程 Qt 信号，由 DataReceiverManager 定时 poll）
struct PendingEmit {
  QString dataName;
  nlohmann::json value;
  double timestamp = 0.0;
  double frequency = 0.0;
};

// ============================================================================
// DataReceiverProcess — 数据接收进程
// ============================================================================

class DataReceiverProcess : public QThread {
  Q_OBJECT

public:
  explicit DataReceiverProcess(const std::string &host = "127.0.0.1",
                               QObject *parent = nullptr);
  ~DataReceiverProcess() override;

  void stopReceiver();

  // 自定义数据注册
  void registerCustomData(const std::string &dataName,
                          const std::string &dataType, int port);

  // 不再管理相机的 Base64 共享内存写入，改由 NativeGlassesAdapter 写入
  // 相机流退化为轻量元数据通知
  void handleCameraDataLight(const nlohmann::json &data);

  // 数据查询
  nlohmann::json getData(const std::string &topicName) const;
  double getFrequency(const std::string &dataName) const;

  // 批量取出待处理数据（替代信号，由 Manager 定时调用）
  std::size_t pendingDataCount() const;
  std::deque<PendingEmit> takePendingData(std::size_t maxCount);

signals:
  // 数据更新信号（UI 线程接收）
  void dataUpdated(const QString &dataName, const nlohmann::json &value,
                   double timestamp, double frequency);

  // 相机帧信号（轻量通知，图像通过共享内存）
  void cameraFrameReady(int camIndex, double timestamp);

protected:
  void run() override;

private:
  void subscribeTopic(const std::string &topicName, int port);
  void onDataReceived(const std::string &topicName, const nlohmann::json &data);
  void onImuRawReceived(const std::string &raw);
  void convertAndEmit(const std::string &topicName, const nlohmann::json &data,
                      double timestamp);
  void enqueuePendingEmit(PendingEmit item);
  double uiSendIntervalForData(const std::string &dataName) const;
  void trackDataReception(const std::string &dataName, double timestamp);
  bool writeCameraToShm(const nlohmann::json &data);

  std::string host_;

  std::atomic<bool> running_{false};

  // Subscribers
  std::unordered_map<std::string, std::unique_ptr<echo::Subscriber>>
      subscribers_;

  // Data cache
  mutable QMutex cacheLock_;
  std::unordered_map<std::string, nlohmann::json> dataCache_;

  // Frequency tracking
  mutable QMutex freqLock_;
  std::unordered_map<std::string, std::deque<double>> receptionTimes_;
  std::unordered_map<std::string, double> frequencies_;
  double frequencyWindow_ = 1.0;

  // Rate limiting (20Hz per topic to UI)
  std::unordered_map<std::string, double> lastSendTime_;
  double imuSendInterval_ = 0.01;
  double customSendInterval_ = 0.01;
  double defaultSendInterval_ = 0.05;
  double cameraSendInterval_ = 0.05;

  // Custom data
  std::unordered_map<std::string, nlohmann::json> registeredCustomData_;

  // 线程安全的待处理数据缓冲（替代跨线程 Qt 信号）
  mutable QMutex pendingLock_;
  std::deque<PendingEmit> pendingEmits_;
  std::size_t maxPendingEmitCount_ = 2000;
  double lastPendingOverflowLogSec_ = 0.0;
};

// ============================================================================
// DataReceiverManager — 数据接收管理器 (主线程)
// ============================================================================

class DataReceiverManager : public QObject {
  Q_OBJECT

public:
  explicit DataReceiverManager(const std::string &host = "127.0.0.1",
                               QObject *parent = nullptr);
  ~DataReceiverManager() override;

  void start();
  void stop();
  bool isRunning() const;

  // 数据查询
  nlohmann::json getLatestData(const std::string &dataName) const;
  double getFrequency(const std::string &dataName) const;

  // 自定义数据
  void registerCustomData(const std::string &dataName,
                          const std::string &dataType, int port);

  // Display buffer (for charts)
  void subscribeData(const std::string &dataName);
  void unsubscribeData(const std::string &dataName);
  std::vector<nlohmann::json>
  getDisplayBuffer(const std::string &dataName) const;

  // 预定义数据名称列表
  std::vector<std::string> getDataNameList() const;

signals:
  void dataRegistered(const QString &dataName, const QString &dataType,
                      int port);
  void dataUpdated(const QString &dataName, const nlohmann::json &value,
                   double timestamp, double frequency);

private slots:
  void pollReceiverData();

private:
  void applyDataUpdate(const QString &dataName, const nlohmann::json &value,
                       double timestamp, double frequency);
  std::unique_ptr<DataReceiverProcess> receiver_;
  QTimer *pollTimer_ = nullptr;

  std::string host_;

  // Latest data
  mutable QMutex dataLock_;
  std::unordered_map<std::string, nlohmann::json> latestData_;
  std::unordered_map<std::string, double> latestFrequencies_;

  // Display buffer
  mutable QMutex bufferLock_;
  mutable std::unordered_map<std::string, std::deque<nlohmann::json>>
      displayBuffer_;
  std::unordered_map<std::string, double> lastBufferTimestamp_;
  std::set<std::string> subscribedNames_;
  int maxBufferSize_ = 600;
  double downsampleInterval_ = 0.01;
};

} // namespace recordlab::backend
