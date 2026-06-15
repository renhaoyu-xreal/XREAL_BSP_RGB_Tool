/*
 * MainSubnode - 数据采集与录制框架
 *
 * 包含：
 * - CsvDataWriter：CSV 批量写入器（缓冲区 + 后台线程）
 * - ImageDataWriter：PGM 图像写入器（双目 + 后台线程）
 * - BaseDevice：设备抽象基类
 * - MainSubnode：数据采集主节点（继承 BaseSubnode）
 *
 */
#pragma once

#include "recordlab/subnodes/base_subnode.h"

#include <QDir>
#include <QImage>
#include <QMutex>
#include <QProcess>

#include <array>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace recordlab::common {
class MotionDetector;
class CameraSharedMemoryReader;
}

namespace recordlab::subnodes {

// 前向声明：
// CameraSnapshotWorker 是 C++ 版双目相机快照工作器，对齐旧版
// scripts/common/camera_snapshot_worker.py 的职责。
// 这里先只暴露不透明指针，避免把内部线程与文件写入细节泄漏到头文件。
class CameraSnapshotWorker;

// ============================================================================
// CsvDataWriter — CSV 批量写入器
// ============================================================================

class CsvDataWriter {
public:
  explicit CsvDataWriter(const std::string &filename = "imu_data.csv",
                         int bufferSize = 3500);
  ~CsvDataWriter();

  void setFilename(const std::string &filename) { filename_ = filename; }
  bool open(const std::string &folderPath);
  bool writeData(const nlohmann::json &data);
  void close();

private:
  void writeWorker();

  std::string filename_;
  int bufferSize_;

  std::ofstream fileHandle_;
  bool isOpen_ = false;
  bool headerWritten_ = false;
  std::vector<std::string> fieldNames_;

  std::vector<nlohmann::json> buffer_;
  std::queue<std::vector<nlohmann::json>> writeQueue_;
  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::atomic<bool> stopEvent_{false};
  std::thread writeThread_;
  int writeCount_ = 0;
};

// ============================================================================
// ImageDataWriter — PGM 图像写入器
// ============================================================================

class ImageDataWriter {
public:
  static constexpr int CAM_NUM = 2;

  explicit ImageDataWriter(const std::string &filename = "image.txt",
                           int bufferSize = 100);
  ~ImageDataWriter();

  void setFilename(const std::string &filename) { filename_ = filename; }
  bool open(const std::string &folderPath);
  bool writeData(const nlohmann::json &imageMessage);
  void close();

private:
  void saveWorker();
  void saveImageData(const nlohmann::json &imageMessage);

  std::string filename_;
  int bufferSize_;
  std::string folderPath_;
  bool isOpen_ = false;

  std::unordered_map<int, int> camCounters_; // cam_idx -> incremental_id

  std::queue<nlohmann::json> saveQueue_;
  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::atomic<bool> stopEvent_{false};
  std::thread saveThread_;
};

// ============================================================================
// RgbImageDataWriter — 单路 RGB BMP 图像写入器
// ============================================================================

class RgbImageDataWriter {
public:
  explicit RgbImageDataWriter(int bufferSize = 100);
  ~RgbImageDataWriter();

  bool open(const std::string &folderPath);
  bool writeData(const nlohmann::json &imageMessage);
  void close();

  int savedFrameCount() const;
  std::string lastFrameFilename() const;

private:
  void saveWorker();
  void saveImageData(const nlohmann::json &imageMessage);

  int bufferSize_;
  std::string folderPath_;
  bool isOpen_ = false;

  std::queue<nlohmann::json> saveQueue_;
  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::atomic<bool> stopEvent_{false};
  std::thread saveThread_;

  mutable std::mutex stateMutex_;
  int savedFrameCount_ = 0;
  std::string lastFrameFilename_;
};

// ============================================================================
// BaseDevice — 设备抽象基类
// ============================================================================

class BaseDevice {
public:
  using ImuCallback = std::function<void(const nlohmann::json &)>;
  using ImageCallback = std::function<void(const nlohmann::json &)>;

  virtual ~BaseDevice() = default;

  virtual nlohmann::json initialize(const nlohmann::json &params) = 0;
  virtual nlohmann::json start(const nlohmann::json &params = {}) = 0;
  virtual nlohmann::json stop(const nlohmann::json &params = {}) = 0;
  virtual nlohmann::json release() = 0;

  virtual void setImuDataCallback(ImuCallback cb) = 0;
  virtual void setImageDataCallback(ImageCallback cb) = 0;

  virtual nlohmann::json control(const nlohmann::json & /*params*/) {
    return {{"success", false}, {"message", "Control not implemented"}};
  }
  virtual nlohmann::json check() {
    return {{"success", true}, {"message", ""}};
  }
};

// ============================================================================
// MainSubnode — 数据采集与录制主节点
// ============================================================================

class MainSubnode : public BaseSubnode {
  Q_OBJECT

public:
  // IMU type 到 IMU 索引的映射
  static const std::unordered_map<int, int> IMU_TYPE_TO_INDEX;

  explicit MainSubnode(const QString &name = "MainSubnode",
                       const QString &subnodeHost = "127.0.0.1",
                       int goalPort = 5690, int feedbackPort = 5691,
                       const QString &rootPath = "./output",
                       BaseDevice *device = nullptr, QObject *parent = nullptr);
  ~MainSubnode() override;

  // ========== 设备命令 ==========

  nlohmann::json cmdInitDevice(uint32_t goalId, const std::string &cmd,
                               const nlohmann::json &params);
  nlohmann::json cmdStartDevice(uint32_t goalId, const std::string &cmd,
                                const nlohmann::json &params);
  nlohmann::json cmdStopDevice(uint32_t goalId, const std::string &cmd,
                               const nlohmann::json &params);
  nlohmann::json cmdControlDevice(uint32_t goalId, const std::string &cmd,
                                  const nlohmann::json &params);
  nlohmann::json cmdReleaseDevice(uint32_t goalId, const std::string &cmd,
                                  const nlohmann::json &params);
  nlohmann::json cmdStartRecord(uint32_t goalId, const std::string &cmd,
                                const nlohmann::json &params);
  nlohmann::json cmdStopRecord(uint32_t goalId, const std::string &cmd,
                               const nlohmann::json &params);
  nlohmann::json cmdDeleteRecord(uint32_t goalId, const std::string &cmd,
                                 const nlohmann::json &params);
  nlohmann::json cmdGetRuntimeState(uint32_t goalId, const std::string &cmd,
                                    const nlohmann::json &params);

  // ========== Publishers ==========

  void createMainPublishers();

  // ========== 数据写入器 ==========

  void addImuWriter(int imuIndex, std::unique_ptr<CsvDataWriter> writer);
  void setImageWriter(std::unique_ptr<ImageDataWriter> writer);
  void setRgbImageWriter(std::unique_ptr<RgbImageDataWriter> writer);
  void setMotionDetector(common::MotionDetector *detector) {
    motionDetector_ = detector;
  }
  nlohmann::json recordingState();

  // ========== BaseSubnode 虚函数 ==========
  nlohmann::json onRelease() override;
  nlohmann::json onEstop() override;
  nlohmann::json onCheck() override;

protected:
  // 录制停止 hook（子类可覆盖）
  virtual void onRecordStopped();

  // 数据回调（设备调用）
  void imuDataCallback(const nlohmann::json &message);
  void imageDataCallback(const nlohmann::json &message);

private:
  void registerDeviceCommands();
  bool shouldPublish(int64_t nowNs, int64_t &lastNs, int64_t intervalNs);
  void resetMotionStatusState(const QString &reason = QString());
  void syncImageAliases(nlohmann::json &imageMessage) const;
  bool startScreenCaptureWorker(const QString &recordPath);
  bool startMicRecordWorker(const QString &recordPath);
  bool waitForExternalWorkerReady(QProcess *process, const QString &readyFile,
                                  const QString &workerName,
                                  int timeoutMs = 5000);
  void stopExternalWorker(std::unique_ptr<QProcess> &process,
                          const QString &workerName, int timeoutMs = 15000);

  // 录制状态
  QMutex recordStateLock_;
  std::atomic<bool> recordFlag_{false};
  bool recordFinalizing_ = false;
  int64_t startRecordTimestampNs_ = 0;

  // 发布端口
  int imuDataPort_;
  int imageDataPort_;
  int recordTimerPort_;
  int timeDelayPort_;
  int motionStatusPort_;

  // 写入器
  std::unordered_map<int, std::unique_ptr<CsvDataWriter>> imuDataWriters_;
  std::unique_ptr<ImageDataWriter> imageDataWriter_;
  std::unique_ptr<RgbImageDataWriter> rgbImageDataWriter_;
  QString recordImageMode_;

  // 运动检测
  common::MotionDetector *motionDetector_ = nullptr;
  std::string lastMotionStatusValue_;

  // 发布频率控制
  int64_t timeDelayPublishIntervalNs_ = 100'000'000;    // 10Hz
  int64_t motionStatusPublishIntervalNs_ = 100'000'000; // 10Hz
  int64_t recordTimerPublishIntervalNs_ = 200'000'000;  // 5Hz
  int64_t lastTimeDelayPublishNs_ = 0;
  int64_t lastMotionStatusPublishNs_ = 0;
  int64_t lastRecordTimerPublishNs_ = 0;

  // CameraSnapshotWorker 只需要低频喂“最新帧”，不需要每帧都深拷贝。
  int64_t cameraPublishIntervalNs_ = 33'333'333;       // 30Hz，与原版实时预览一致
  int64_t cameraSnapshotFeedIntervalNs_ = 200'000'000; // 5Hz
  int64_t lastCameraPublishNs_ = 0;
  std::array<int64_t, 2> lastCameraSnapshotFeedNs_{{0, 0}};
  std::string lastRecordTimerStatus_;

  // 辅助录制链路：
  // 1. CameraSnapshotWorker 由 C++ 原生实现，负责接住主数据回调里的最新帧。
  // 2. ScreenCaptureWorker / MicRecordWorker 继续复用本地 Python 脚本，
  //    但启动与停止完全由新工程 C++ 主节点托管。
  std::unique_ptr<CameraSnapshotWorker> cameraSnapshotWorker_;
  std::unique_ptr<QProcess> screenCaptureWorkerProcess_;
  std::unique_ptr<QProcess> micRecordWorkerProcess_;
  std::unique_ptr<recordlab::common::CameraSharedMemoryReader> cameraShmReader_;
  std::array<uint64_t, 2> lastCameraRecordReadSeq_{{0, 0}};

protected:
  // 子类可访问
  BaseDevice *device_ = nullptr;
  QString recordPath_;
};

} // namespace recordlab::subnodes
