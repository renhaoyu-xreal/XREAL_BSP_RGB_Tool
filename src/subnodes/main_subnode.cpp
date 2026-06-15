/*
 * MainSubnode 完整实现
 *
 * 增强内容:
 * - imuDataCallback: time_delay发布 + motion_status发布 + 录制(data0-5格式)
 * - imageDataCallback: QImage序列化 + 相机快照
 * - ImageDataWriter::saveImageData: 实际 PGM P5 写入 + timestamps.txt +
 * metadata.txt
 * - on_release/on_estop/on_check 虚函数实现
 */
#include "recordlab/subnodes/main_subnode.h"
#include "recordlab/common/camera_shared_memory.h"
#include "recordlab/common/motion_detector.h"
#include "recordlab/common/topics.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStringList>
#include <QThread>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace recordlab::subnodes {

using json = nlohmann::json;
using namespace recordlab::common;

static int64_t nowNs() {
  // 使用单调时钟生成纳秒时间戳，避免系统时间跳变影响录制计时。
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

namespace {

struct ImuStreamProbe {
  uint64_t count = 0;
  int64_t lastArrivalNs = 0;
  int64_t lastTimestampNs = 0;
  int64_t maxArrivalGapNs = 0;
  int64_t maxTimestampGapNs = 0;
  int64_t maxTimestampBacktrackNs = 0;
};

void logImuProbeStats(const char *tag, ImuStreamProbe &probe,
                      int64_t arrivalNs, int64_t timestampNs, int imuType) {
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
              << " imu_type=" << imuType
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

std::vector<std::string> deriveCsvFieldNames(const json &row) {
  static constexpr std::array<const char *, 8> kImuFieldOrder = {
      "timestamp_ns", "type",  "data0", "data1",
      "data2",        "data3", "data4", "data5"};

  std::vector<std::string> fields;
  bool hasImuPayload = false;
  for (const char *field : kImuFieldOrder) {
    if (std::string(field).rfind("data", 0) == 0 && row.contains(field)) {
      hasImuPayload = true;
      break;
    }
  }

  if (row.contains("timestamp_ns") && row.contains("type") && hasImuPayload) {
    for (const char *field : kImuFieldOrder) {
      if (row.contains(field)) {
        fields.emplace_back(field);
      }
    }
  }

  for (const auto &[key, _] : row.items()) {
    if (std::find(fields.begin(), fields.end(), key) == fields.end()) {
      fields.push_back(key);
    }
  }

  return fields;
}

// ============================================================================
// 运行时路径与 Python worker 辅助函数
// ============================================================================

bool isProjectRootCandidate(const QString &path) {
  // 通过关键配置和运行时脚本判断某个目录是否像有效的项目根目录。
  if (path.isEmpty())
    return false;

  const QDir dir(path);
  return QFileInfo::exists(dir.filePath("config/agents_config.json")) &&
         QFileInfo::exists(
             dir.filePath("scripts/runtime/run_recordlab_script.py"));
}

QString resolveProjectRootPath() {
  // 依次尝试环境变量、编译期根目录和可执行文件上层目录来定位项目根。
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

QString resolveStorageRootPath(const QString &configuredRootPath) {
  // 将相对 root_path 解析到项目根下，绝对路径则原样保留。
  const QString cleaned = QDir::cleanPath(configuredRootPath.trimmed());
  if (cleaned.isEmpty()) {
    return cleaned;
  }

  if (QDir::isAbsolutePath(cleaned)) {
    return cleaned;
  }

  const QString projectRoot = resolveProjectRootPath();
  if (!projectRoot.isEmpty()) {
    return QDir(projectRoot).filePath(cleaned);
  }

  return QFileInfo(cleaned).absoluteFilePath();
}

QProcessEnvironment buildWorkerEnvironment(const QString &projectRoot) {
  // 为录制辅助 worker 构造统一环境变量，确保其能找到脚本和第三方依赖。
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert(QStringLiteral("RECORDLABC_ROOT"), projectRoot);
  env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));

  QStringList pythonPathEntries;
  pythonPathEntries << projectRoot
                    << QDir(projectRoot)
                           .filePath(QStringLiteral(
                               "third_party/echo_message_system/python"));

  const QString existingPythonPath = env.value(QStringLiteral("PYTHONPATH"));
  if (!existingPythonPath.isEmpty()) {
    pythonPathEntries << existingPythonPath.split(QLatin1Char(':'),
                                                  Qt::SkipEmptyParts);
  }

  pythonPathEntries.removeDuplicates();
  env.insert(QStringLiteral("PYTHONPATH"),
             pythonPathEntries.join(QLatin1Char(':')));
  return env;
}

} // namespace

// ============================================================================
// CameraSnapshotWorker — C++ 原生双目快照工作器
//
// 这一版直接对齐旧的 scripts/common/camera_snapshot_worker.py：
// 1. 保存 cam_start / cam_end / cam_{n}min.png；
// 2. 每秒记录一次双目 RGB 均值到 camera_rgb.csv；
// 3. 只缓存“最新帧”，避免把主采集线程拖慢。
// ============================================================================

class CameraSnapshotWorker {
public:
  explicit CameraSnapshotWorker(const QString &saveDir)
      : saveDir_(saveDir),
        cam0SnapshotsDir_(QDir(saveDir).filePath("cam0/snapshots")),
        cam1SnapshotsDir_(QDir(saveDir).filePath("cam1/snapshots")),
        csvPath_(QDir(saveDir).filePath("camera_rgb.csv")) {}

  // 析构时停止后台线程并补写结束快照，保证录制目录完整。
  ~CameraSnapshotWorker() { stop(); }

  void start() {
    // 创建快照目录和 CSV 文件后，启动后台线程定期保存快照与 RGB 均值。
    if (running_.exchange(true)) {
      std::cout << "[CameraSnapshotWorker] Already running" << std::endl;
      return;
    }

    QDir().mkpath(cam0SnapshotsDir_);
    QDir().mkpath(cam1SnapshotsDir_);
    initCsv();

    workerThread_ = std::thread(&CameraSnapshotWorker::mainLoop, this);
    std::cout << "[CameraSnapshotWorker] Started: " << saveDir_.toStdString()
              << std::endl;
  }

  void stop() {
    // 停止快照线程前补写结束快照和最后一次 RGB 统计。
    if (!running_.exchange(false)) {
      return;
    }

    saveEndSnapshot();
    logRgbValues();

    if (workerThread_.joinable()) {
      workerThread_.join();
    }

    closeCsv();
    std::cout << "[CameraSnapshotWorker] Stopped" << std::endl;
  }

  void updateFrame(int camIdx, const QImage &frame) {
    // 接收主线程提供的最新帧副本，供后台快照线程异步保存。
    if (camIdx < 0 || camIdx >= static_cast<int>(latestFrames_.size()) ||
        frame.isNull()) {
      return;
    }

    std::lock_guard<std::mutex> lock(frameMutex_);
    latestFrames_[camIdx] = frame.copy();
  }

private:
  void initCsv() {
    // 初始化 camera_rgb.csv 并写入表头。
    csvFile_.setFileName(csvPath_);
    if (!csvFile_.open(QIODevice::WriteOnly | QIODevice::Text |
                       QIODevice::Truncate)) {
      std::cerr << "[CameraSnapshotWorker] Failed to open CSV: "
                << csvPath_.toStdString() << std::endl;
      return;
    }

    csvStream_.setDevice(&csvFile_);
    csvStream_ << "timestamps_ns,r_left_mean,g_left_mean,b_left_mean,"
                  "r_right_mean,g_right_mean,b_right_mean\n";
    csvStream_.flush();
  }

  void closeCsv() {
    // 刷新并关闭 RGB 统计 CSV 文件。
    if (!csvFile_.isOpen()) {
      return;
    }
    csvStream_.flush();
    csvFile_.close();
  }

  std::array<QImage, 2> snapshotFrames() const {
    // 获取当前双目最新帧的线程安全副本。
    std::lock_guard<std::mutex> lock(frameMutex_);
    return {latestFrames_[0].copy(), latestFrames_[1].copy()};
  }

  static std::tuple<double, double, double>
  calculateRgbMean(const QImage &image) {
    // 计算一张图像的 RGB 均值，供 camera_rgb.csv 统计使用。
    if (image.isNull()) {
      return {-1.0, -1.0, -1.0};
    }

    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull()) {
      return {-1.0, -1.0, -1.0};
    }

    const int width = rgba.width();
    const int height = rgba.height();
    const int bytesPerLine = rgba.bytesPerLine();
    const auto *bits = rgba.constBits();
    if (!bits || width <= 0 || height <= 0 || bytesPerLine <= 0) {
      return {-1.0, -1.0, -1.0};
    }

    quint64 rSum = 0;
    quint64 gSum = 0;
    quint64 bSum = 0;
    for (int row = 0; row < height; ++row) {
      const auto *line = bits + row * bytesPerLine;
      for (int col = 0; col < width; ++col) {
        const int offset = col * 4;
        rSum += static_cast<quint64>(line[offset + 0]);
        gSum += static_cast<quint64>(line[offset + 1]);
        bSum += static_cast<quint64>(line[offset + 2]);
      }
    }

    const double pixelCount = static_cast<double>(width) * height;
    if (pixelCount <= 0.0) {
      return {-1.0, -1.0, -1.0};
    }

    return {static_cast<double>(rSum) / pixelCount,
            static_cast<double>(gSum) / pixelCount,
            static_cast<double>(bSum) / pixelCount};
  }

  void writeRgbRow(int64_t timestampNs, double rLeft, double gLeft,
                   double bLeft, double rRight, double gRight, double bRight) {
    // 以一行 CSV 的形式记录当前时刻双目图像的 RGB 均值。
    if (!csvFile_.isOpen()) {
      return;
    }

    auto writeValue = [this](double value) {
      if (value >= 0.0) {
        csvStream_ << QString::number(value, 'f', 2);
      } else {
        csvStream_ << "-1";
      }
    };

    csvStream_ << timestampNs << ",";
    writeValue(rLeft);
    csvStream_ << ",";
    writeValue(gLeft);
    csvStream_ << ",";
    writeValue(bLeft);
    csvStream_ << ",";
    writeValue(rRight);
    csvStream_ << ",";
    writeValue(gRight);
    csvStream_ << ",";
    writeValue(bRight);
    csvStream_ << "\n";
    csvStream_.flush();
  }

  void logRgbValues() {
    // 采样当前双目最新帧并追加一条 RGB 统计记录。
    const auto frames = snapshotFrames();
    const auto [rLeft, gLeft, bLeft] = calculateRgbMean(frames[0]);
    const auto [rRight, gRight, bRight] = calculateRgbMean(frames[1]);
    writeRgbRow(nowNs(), rLeft, gLeft, bLeft, rRight, gRight, bRight);
  }

  bool saveSnapshot(const QString &name) {
    // 将当前双目最新帧分别保存成命名快照文件。
    int successCount = 0;
    const auto frames = snapshotFrames();

    for (int camIdx = 0; camIdx < static_cast<int>(frames.size()); ++camIdx) {
      const QImage &frame = frames[camIdx];
      if (frame.isNull()) {
        std::cout << "[CameraSnapshotWorker] cam" << camIdx
                  << " has no frame for " << name.toStdString() << std::endl;
        continue;
      }

      const QString snapshotDir =
          (camIdx == 0) ? cam0SnapshotsDir_ : cam1SnapshotsDir_;
      const QString path =
          QDir(snapshotDir).filePath(QString("%1.png").arg(name));

      if (frame.save(path, "PNG")) {
        ++successCount;
      } else {
        std::cerr << "[CameraSnapshotWorker] Failed to save cam" << camIdx
                  << " snapshot: " << path.toStdString() << std::endl;
      }
    }

    return successCount > 0;
  }

  void saveStartSnapshot() {
    // 启动后等待首批帧到达，再写入 cam_start 快照。
    QElapsedTimer timer;
    timer.start();

    // 和 Python 版保持一致：给设备最多 5 秒填充第一批帧。
    while (timer.elapsed() < 5000 && running_.load()) {
      const auto frames = snapshotFrames();
      if (!frames[0].isNull() || !frames[1].isNull()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (saveSnapshot(QStringLiteral("cam_start"))) {
      lastSnapshotMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    }
  }

  void saveEndSnapshot() { saveSnapshot(QStringLiteral("cam_end")); }

  void mainLoop() {
    // 后台循环每秒记录 RGB 均值，并按分钟周期写入快照。
    saveStartSnapshot();
    logRgbValues();
    lastRgbLogMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();

    while (running_.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!running_.load()) {
        break;
      }

      const qint64 currentMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();

      if (currentMs - lastRgbLogMs_ >= rgbLogIntervalMs_) {
        logRgbValues();
        lastRgbLogMs_ = currentMs;
      }

      if (currentMs - lastSnapshotMs_ >= snapshotIntervalMs_) {
        ++snapshotCount_;
        const QString name = QStringLiteral("cam_%1min").arg(snapshotCount_);
        if (saveSnapshot(name)) {
          lastSnapshotMs_ = currentMs;
        }
      }
    }
  }

  QString saveDir_;
  QString cam0SnapshotsDir_;
  QString cam1SnapshotsDir_;
  QString csvPath_;

  mutable std::mutex frameMutex_;
  std::array<QImage, 2> latestFrames_;

  QFile csvFile_;
  QTextStream csvStream_;
  std::atomic<bool> running_{false};
  std::thread workerThread_;

  qint64 snapshotIntervalMs_ = 60'000;
  qint64 rgbLogIntervalMs_ = 1'000;
  qint64 lastSnapshotMs_ = 0;
  qint64 lastRgbLogMs_ = 0;
  int snapshotCount_ = 0;
};

namespace {

QByteArray payloadBytes(const json &payload) {
  // 从 payload 中提取原始图像字节，兼容普通字符串和 base64 编码两种来源。
  if (!payload.is_object() || !payload.contains("data") ||
      !payload["data"].is_string()) {
    return {};
  }

  const std::string raw = payload["data"].get<std::string>();
  if (raw.empty()) {
    return {};
  }

  if (payload.value("data_encoding", std::string()) == std::string("base64")) {
    return QByteArray::fromBase64(QByteArray::fromStdString(raw));
  }
  return QByteArray::fromStdString(raw);
}

QImage imageFromPayload(const json &payload) {
  // 按 encoded/raw 两条路径把图像 payload 还原成 QImage。
  if (!payload.is_object()) {
    return {};
  }

  // 先提取原始字节（可能是 base64 编码的）
  const QByteArray rawBytes = payloadBytes(payload);
  if (rawBytes.isEmpty()) {
    return {};
  }

  // 优先处理编码格式（如 JPEG），与 data_monitor_widget.cpp 行为一致
  if (payload.contains("encoded_format") &&
      payload["encoded_format"].is_string()) {
    QImage encodedImage;
    const QByteArray encodedFormat =
        QByteArray::fromStdString(payload["encoded_format"].get<std::string>());
    if (encodedImage.loadFromData(rawBytes, encodedFormat.constData())) {
      return encodedImage;
    }
  }

  // 回退到 raw pixel 路径
  const int width = payload.value("width", 0);
  const int height = payload.value("height", 0);
  if (width <= 0 || height <= 0) {
    return {};
  }

  int channels = payload.value("channels", 1);
  QImage::Format format = QImage::Format_Grayscale8;

  if (payload.contains("format")) {
    const auto &formatValue = payload["format"];
    if (formatValue.is_string()) {
      const QString formatName =
          QString::fromStdString(formatValue.get<std::string>()).toUpper();
      if (formatName == QStringLiteral("RGBA")) {
        format = QImage::Format_RGBA8888;
        channels = 4;
      } else if (formatName == QStringLiteral("RGB")) {
        format = QImage::Format_RGB888;
        channels = 3;
      } else {
        format = QImage::Format_Grayscale8;
        channels = 1;
      }
    } else if (formatValue.is_number_integer()) {
      const int qtFormatValue = formatValue.get<int>();
      if (qtFormatValue == static_cast<int>(QImage::Format_RGBA8888)) {
        format = QImage::Format_RGBA8888;
        channels = 4;
      } else if (qtFormatValue == static_cast<int>(QImage::Format_RGB888)) {
        format = QImage::Format_RGB888;
        channels = 3;
      } else {
        format = QImage::Format_Grayscale8;
        channels = 1;
      }
    }
  } else if (channels == 4) {
    format = QImage::Format_RGBA8888;
  } else if (channels == 3) {
    format = QImage::Format_RGB888;
  }

  const int rowBytes = width * std::max(1, channels);
  int sourceBytesPerLine =
      payload.value("bytes_per_line", payload.value("stride", rowBytes));
  if (sourceBytesPerLine <= 0) {
    sourceBytesPerLine = rowBytes;
  }

  const qsizetype minimumSize =
      static_cast<qsizetype>(sourceBytesPerLine) * height;
  if (rawBytes.size() < minimumSize) {
    return {};
  }

  QImage image(width, height, format);
  if (image.isNull()) {
    return {};
  }

  const int copyBytes =
      std::min(rowBytes, static_cast<int>(image.bytesPerLine()));
  for (int row = 0; row < height; ++row) {
    std::memcpy(image.scanLine(row),
                rawBytes.constData() + row * sourceBytesPerLine,
                static_cast<size_t>(copyBytes));
  }
  return image;
}

QImage extractSnapshotImage(const json &camInfo) {
  // 从相机信息对象中优先提取 image_raw，必要时回退到 image 字段。
  if (!camInfo.is_object()) {
    return {};
  }

  if (camInfo.contains("image_raw")) {
    QImage image = imageFromPayload(camInfo["image_raw"]);
    if (!image.isNull()) {
      return image;
    }
  }
  if (camInfo.contains("image")) {
    return imageFromPayload(camInfo["image"]);
  }
  return {};
}

constexpr const char *RGB_IMAGE_DIRNAME = "rgb0";

qint64 rgbCaptureTimestampNs(const json &camInfo, const json &imageMessage) {
  // RGB BMP、metadata 和 timestamps 使用同一个曝光中点时间戳。
  const qint64 cached = camInfo.value("canonical_timestamp_ns", qint64(0));
  if (cached > 0) {
    return cached;
  }

  const qint64 exposureStart =
      camInfo.value("exposure_start_time_device", qint64(0));
  const qint64 exposureDuration = camInfo.value("exposure_duration", qint64(0));
  const qint64 rollingShutter = camInfo.value("rolling_shutter_time", qint64(0));
  if (exposureStart > 0) {
    return ((exposureStart * 2) + exposureDuration + rollingShutter + 1) / 2;
  }

  const qint64 messageTimestamp = imageMessage.value("timestamp", qint64(0));
  if (messageTimestamp > 0) {
    return messageTimestamp;
  }
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

QString rgbCaptureFilename(qint64 timestampNs) {
  return QStringLiteral("%1.bmp").arg(timestampNs, 20, 10, QLatin1Char('0'));
}

json imagePayloadFromSharedMemory(
    recordlab::common::CameraSharedMemoryReader *reader, int camIdx,
    uint64_t &lastSeq, const json &imageInfo, QImage *decodedImage = nullptr) {
  // 按需从共享内存回读一帧图像，并重建可写盘/可解码的 payload 结构。
  if (!reader) {
    return json::object();
  }

  uint64_t requestedSeq = 0;
  if (imageInfo.is_object()) {
    requestedSeq = imageInfo.value("shm_seq", uint64_t(0));
  }
  if (requestedSeq > 0 && requestedSeq > lastSeq) {
    lastSeq = requestedSeq - 1;
  }

  recordlab::common::CameraFrameMeta meta;
  QByteArray frameData;
  uint64_t readSeq = lastSeq;
  if (!reader->readLatestFrame(camIdx, readSeq, meta, frameData)) {
    return json::object();
  }
  lastSeq = readSeq;

  json payload = {
      {"width", meta.width},
      {"height", meta.height},
      {"qt_format", meta.format},
      {"bytes_per_line", meta.bytesPerLine},
      {"shm_seq", readSeq},
      {"data", std::string(frameData.constData(),
                            static_cast<size_t>(frameData.size()))},
  };

  if (imageInfo.is_object() && imageInfo.contains("format")) {
    payload["format"] = imageInfo["format"];
  } else if (meta.encodedFormat == 1) {
    payload["format"] = "grayscale8";
  }

  if (meta.encodedFormat == 1) {
    payload["encoded_format"] = "JPEG";
  }

  if (decodedImage) {
    *decodedImage = imageFromPayload(payload);
  }

  return payload;
}

} // namespace

// ============================================================================
// RgbImageDataWriter — 单路 RGB BMP 写入
// ============================================================================

RgbImageDataWriter::RgbImageDataWriter(int bufferSize)
    : bufferSize_(bufferSize) {}

RgbImageDataWriter::~RgbImageDataWriter() { close(); }

bool RgbImageDataWriter::open(const std::string &folderPath) {
  close();
  folderPath_ = folderPath;
  QDir().mkpath(QDir(QString::fromStdString(folderPath_))
                    .filePath(QString::fromUtf8(RGB_IMAGE_DIRNAME)));

  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    savedFrameCount_ = 0;
    lastFrameFilename_.clear();
  }

  stopEvent_ = false;
  isOpen_ = true;
  saveThread_ = std::thread(&RgbImageDataWriter::saveWorker, this);
  std::cout << "[RgbImageDataWriter] Opened: " << folderPath_ << std::endl;
  return true;
}

bool RgbImageDataWriter::writeData(const json &imageMessage) {
  // 有界队列：RGB 帧过密时宁可丢帧，也不阻塞采集回调。
  if (!isOpen_) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (static_cast<int>(saveQueue_.size()) >= bufferSize_) {
      std::cerr << "[RgbImageDataWriter] Save queue full, dropping frame"
                << std::endl;
      return false;
    }
    saveQueue_.push(imageMessage);
  }
  queueCv_.notify_one();
  return true;
}

void RgbImageDataWriter::saveWorker() {
  while (!stopEvent_ || !saveQueue_.empty()) {
    json msg;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
        return !saveQueue_.empty() || stopEvent_.load();
      });
      if (saveQueue_.empty()) {
        continue;
      }
      msg = std::move(saveQueue_.front());
      saveQueue_.pop();
    }
    saveImageData(msg);
  }
}

void RgbImageDataWriter::saveImageData(const json &imageMessage) {
  // 目录结构对齐 Python 版：
  //   rgb0/<timestamp>.bmp
  //   rgb0/metadata.txt
  //   rgb0/timestamps.txt
  if (folderPath_.empty() || !imageMessage.is_object()) {
    return;
  }

  const auto camData = imageMessage.value("cam_data", json::object());
  if (!camData.is_object() || !camData.contains("0") ||
      !camData["0"].is_object()) {
    return;
  }

  const auto &camInfo = camData["0"];
  const json imageRaw = camInfo.value("image_raw", camInfo.value("image", json{}));
  QImage image = imageFromPayload(imageRaw);
  if (image.isNull()) {
    return;
  }

  image = image.convertToFormat(QImage::Format_RGB888);
  if (image.isNull()) {
    return;
  }

  const qint64 timestampNs = rgbCaptureTimestampNs(camInfo, imageMessage);
  const QString filename = rgbCaptureFilename(timestampNs);
  const QString rgbDir =
      QDir(QString::fromStdString(folderPath_))
          .filePath(QString::fromUtf8(RGB_IMAGE_DIRNAME));
  QDir().mkpath(rgbDir);
  const QString imagePath = QDir(rgbDir).filePath(filename);

  if (!image.save(imagePath, "BMP")) {
    std::cerr << "[RgbImageDataWriter] Failed to save frame: "
              << imagePath.toStdString() << std::endl;
    return;
  }

  QFile metadataFile(QDir(rgbDir).filePath(QStringLiteral("metadata.txt")));
  if (metadataFile.open(QIODevice::Append | QIODevice::Text)) {
    QTextStream stream(&metadataFile);
    stream << filename << " " << timestampNs << " "
           << camInfo.value("exposure_duration", qint64(0)) << " "
           << camInfo.value("gain", 0.0) << " "
           << camInfo.value("exposure_start_time_system", qint64(0)) << " "
           << camInfo.value("exposure_start_time_device", qint64(0)) << " "
           << camInfo.value("rolling_shutter_time", qint64(0)) << " "
           << camInfo.value("stride", camInfo.value("bytes_per_line", 0)) << " "
           << QString::number(camInfo.value("temperature", 0.0), 'f', 2)
           << "\n";
  }

  QFile timestampsFile(QDir(rgbDir).filePath(QStringLiteral("timestamps.txt")));
  if (timestampsFile.open(QIODevice::Append | QIODevice::Text)) {
    QTextStream stream(&timestampsFile);
    stream << filename << " " << timestampNs << "\n";
  }

  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    ++savedFrameCount_;
    lastFrameFilename_ =
        std::string(RGB_IMAGE_DIRNAME) + "/" + filename.toStdString();
  }
}

void RgbImageDataWriter::close() {
  if (!isOpen_ && !saveThread_.joinable()) {
    return;
  }

  stopEvent_ = true;
  queueCv_.notify_all();
  if (saveThread_.joinable()) {
    saveThread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    std::queue<json> empty;
    saveQueue_.swap(empty);
  }

  isOpen_ = false;
  std::cout << "[RgbImageDataWriter] Closed" << std::endl;
}

int RgbImageDataWriter::savedFrameCount() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  return savedFrameCount_;
}

std::string RgbImageDataWriter::lastFrameFilename() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  return lastFrameFilename_;
}

// ============================================================================
// CsvDataWriter
// ============================================================================

CsvDataWriter::CsvDataWriter(const std::string &filename, int bufferSize)
    : filename_(filename), bufferSize_(bufferSize) {}

// 析构时确保写线程退出并把缓存数据刷盘。
CsvDataWriter::~CsvDataWriter() { close(); }

bool CsvDataWriter::open(const std::string &folderPath) {
  // 打开目标 CSV 文件并启动后台写线程。
  if (isOpen_)
    close();
  QDir().mkpath(QString::fromStdString(folderPath));
  std::string fullPath = folderPath + "/" + filename_;
  fileHandle_.open(fullPath, std::ios::out | std::ios::trunc);
  if (!fileHandle_.is_open())
    return false;
  isOpen_ = true;
  headerWritten_ = false;
  stopEvent_ = false;
  writeThread_ = std::thread(&CsvDataWriter::writeWorker, this);
  std::cout << "[CsvWriter] Opened: " << fullPath << std::endl;
  return true;
}

bool CsvDataWriter::writeData(const json &data) {
  // 将一条记录写入内存缓冲，达到阈值后交给后台线程批量落盘。
  if (!isOpen_ || stopEvent_)
    return false;
  buffer_.push_back(data);
  if (static_cast<int>(buffer_.size()) >= bufferSize_) {
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      writeQueue_.push(buffer_);
    }
    queueCv_.notify_one();
    buffer_.clear();
  }
  return true;
}

void CsvDataWriter::writeWorker() {
  // 后台线程负责批量写入 CSV 行，减少主采集线程阻塞。
  while (!stopEvent_ || !writeQueue_.empty()) {
    std::vector<json> batch;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
        return !writeQueue_.empty() || stopEvent_.load();
      });
      if (writeQueue_.empty())
        continue;
      batch = std::move(writeQueue_.front());
      writeQueue_.pop();
    }
    if (batch.empty())
      continue;

    // Write header from first row's keys
    if (!headerWritten_ && batch[0].is_object()) {
      fieldNames_ = deriveCsvFieldNames(batch[0]);
      for (size_t i = 0; i < fieldNames_.size(); ++i) {
        fileHandle_ << fieldNames_[i];
        if (i + 1 < fieldNames_.size())
          fileHandle_ << ",";
      }
      fileHandle_ << "\n";
      headerWritten_ = true;
    }

    // Write rows
    for (auto &row : batch) {
      for (size_t i = 0; i < fieldNames_.size(); ++i) {
        if (row.contains(fieldNames_[i])) {
          auto &v = row[fieldNames_[i]];
          if (v.is_number_float())
            fileHandle_ << v.get<double>();
          else if (v.is_number_integer())
            fileHandle_ << v.get<int64_t>();
          else if (v.is_string())
            fileHandle_ << v.get<std::string>();
          else
            fileHandle_ << v.dump();
        }
        if (i + 1 < fieldNames_.size())
          fileHandle_ << ",";
      }
      fileHandle_ << "\n";
    }
    writeCount_ += static_cast<int>(batch.size());
    fileHandle_.flush();
  }
}

void CsvDataWriter::close() {
  // 刷出剩余缓冲并等待后台写线程退出。
  if (!buffer_.empty()) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    writeQueue_.push(buffer_);
    buffer_.clear();
    queueCv_.notify_one();
  }
  stopEvent_ = true;
  queueCv_.notify_all();
  if (writeThread_.joinable())
    writeThread_.join();
  if (fileHandle_.is_open()) {
    fileHandle_.close();
    std::cout << "[CsvWriter] Closed (wrote " << writeCount_ << " rows)"
              << std::endl;
  }
  isOpen_ = false;
  headerWritten_ = false;
  writeCount_ = 0;
}

// ============================================================================
// ImageDataWriter — 含实际 PGM P5 写入
// ============================================================================

ImageDataWriter::ImageDataWriter(const std::string &filename, int bufferSize)
    : filename_(filename), bufferSize_(bufferSize) {}

// 析构时等待保存线程退出，防止图像文件写一半被中断。
ImageDataWriter::~ImageDataWriter() { close(); }

bool ImageDataWriter::open(const std::string &folderPath) {
  // 创建双目图像输出目录并启动后台保存线程。
  folderPath_ = folderPath;
  camCounters_.clear();
  for (int i = 0; i < CAM_NUM; ++i) {
    QString camDir =
        QString::fromStdString(folderPath) + QString("/cam%1/images").arg(i);
    QDir().mkpath(camDir);
    camCounters_[i] = 0;
  }
  stopEvent_ = false;
  saveThread_ = std::thread(&ImageDataWriter::saveWorker, this);
  isOpen_ = true;
  std::cout << "[ImageDataWriter] Opened: " << folderPath << std::endl;
  return true;
}

bool ImageDataWriter::writeData(const json &imageMessage) {
  // 将图像消息压入保存队列；队列满时直接拒绝，优先保证实时性。
  if (!isOpen_)
    return false;
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (static_cast<int>(saveQueue_.size()) >= bufferSize_)
      return false;
    saveQueue_.push(imageMessage);
  }
  queueCv_.notify_one();
  return true;
}

void ImageDataWriter::saveWorker() {
  // 后台线程持续从队列取图像消息并写入磁盘。
  while (!stopEvent_ || !saveQueue_.empty()) {
    json msg;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
        return !saveQueue_.empty() || stopEvent_.load();
      });
      if (saveQueue_.empty())
        continue;
      msg = std::move(saveQueue_.front());
      saveQueue_.pop();
    }
    saveImageData(msg);
  }
}

void ImageDataWriter::saveImageData(const json &imageMessage) {
  // 将双目图像保存为 PGM，并同步维护 timestamps.txt 与 metadata.txt。
  /*
   * 保存双目灰度图像为 PGM P5 格式
   *
   * 目录结构: cam{idx}/images/m{counter:07d}.pgm
   * 元数据:   cam{idx}/images/timestamps.txt, metadata.txt
   *
   * PGM P5 格式:
   *   P5\n{width} {height}\n255\n{binary pixel data}
   *
   * 兼容要求：
   * - timestamps.txt / metadata.txt 的内容与 Python 版保持一致；
   * - 使用文件名 m0000000.pgm，而不是内部计数值；
   * - metadata 记录 exposure middle time、gain、stride 等完整字段。
   */
  auto camData = imageMessage.value("cam_data", json::object());

  for (int idx = 0; idx < CAM_NUM; ++idx) {
    std::string key = std::to_string(idx);
    if (!camData.contains(key))
      continue;
    auto &camInfo = camData[key];

    auto imageRaw = camInfo.value("image_raw", camInfo.value("image", json{}));
    if (imageRaw.is_null() || !imageRaw.is_object())
      continue;

    QImage image = imageFromPayload(imageRaw);
    if (image.isNull()) {
      continue;
    }
    image = image.convertToFormat(QImage::Format_Grayscale8);
    const int width = image.width();
    const int height = image.height();
    if (width <= 0 || height <= 0) {
      continue;
    }

    int counter = camCounters_[idx]++;
    char filename[32];
    std::snprintf(filename, sizeof(filename), "m%07d.pgm", counter);

    std::string camDir = folderPath_ + "/cam" + std::to_string(idx) + "/images";
    std::string pgmPath = camDir + "/" + filename;

    // Write PGM P5
    std::ofstream pgm(pgmPath, std::ios::binary);
    if (pgm.is_open()) {
      // Header
      pgm << "P5\n" << width << " " << height << "\n255\n";
      // Pixel data
      for (int row = 0; row < height; ++row) {
        const char *rowData =
            reinterpret_cast<const char *>(image.constScanLine(row));
        pgm.write(rowData, static_cast<std::streamsize>(width));
      }
      pgm.close();
    }

    // Append to timestamps.txt
    std::string tsPath = camDir + "/timestamps.txt";
    std::ofstream tsFile(tsPath, std::ios::app);
    if (tsFile.is_open()) {
      const double exposureStartTimeDevice =
          camInfo.value("exposure_start_time_device", 0.0);
      const double exposureDuration = camInfo.value("exposure_duration", 0.0);
      const double rollingShutterTime =
          camInfo.value("rolling_shutter_time", 0.0);
      const double exposureMiddleTime = exposureStartTimeDevice +
                                        exposureDuration / 2.0 +
                                        rollingShutterTime / 2.0;

      tsFile << filename << " " << std::fixed << (exposureMiddleTime * 1e-9)
             << "\n";
      tsFile.close();
    }

    // Append to metadata.txt (exposure, gain, etc.)
    std::string metaPath = camDir + "/metadata.txt";
    std::ofstream metaFile(metaPath, std::ios::app);
    if (metaFile.is_open()) {
      const double exposureStartTimeDevice =
          camInfo.value("exposure_start_time_device", 0.0);
      const double exposureStartTimeSystem =
          camInfo.value("exposure_start_time_system", 0.0);
      const double exposureDuration = camInfo.value("exposure_duration", 0.0);
      const double rollingShutterTime =
          camInfo.value("rolling_shutter_time", 0.0);
      const double gain = camInfo.value("gain", 0.0);
      const double stride = camInfo.value("stride", 0.0);
      const double exposureMiddleTime = exposureStartTimeDevice +
                                        exposureDuration / 2.0 +
                                        rollingShutterTime / 2.0;

      metaFile << filename << " " << exposureMiddleTime << " "
               << exposureDuration << " " << gain << " "
               << exposureStartTimeSystem << " " << exposureStartTimeDevice
               << " " << rollingShutterTime << " " << stride << "\n";
      metaFile.close();
    }
  }
}

void ImageDataWriter::close() {
  // 停止后台保存线程并输出本次累计保存帧数。
  if (!isOpen_)
    return;
  stopEvent_ = true;
  queueCv_.notify_all();
  if (saveThread_.joinable())
    saveThread_.join();
  int total = 0;
  for (auto &[_, c] : camCounters_)
    total += c;
  std::cout << "[ImageDataWriter] Closed (saved " << total << " frames)"
            << std::endl;
  isOpen_ = false;
  camCounters_.clear();
}

// ============================================================================
// MainSubnode
// ============================================================================

const std::unordered_map<int, int> MainSubnode::IMU_TYPE_TO_INDEX = {
    {1, 0}, {2, 0}, {3, 0}, {12, 0}, // IMU0: gyro(1), acc(2), mag(3), temp(12)
    {4, 1}, {5, 1}, {13, 1}          // IMU1: gyro(4), acc(5), temp(13)
};

MainSubnode::MainSubnode(const QString &name, const QString &subnodeHost,
                         int goalPort, int feedbackPort,
                         const QString &rootPath, BaseDevice *device,
                         QObject *parent)
    : BaseSubnode(name, subnodeHost, goalPort, feedbackPort, rootPath, parent),
      imuDataPort_(PORT_IMU), imageDataPort_(PORT_CAMERA),
      recordTimerPort_(PORT_RECORD_TIMER), timeDelayPort_(PORT_TIME_DELAY),
      motionStatusPort_(PORT_MOTION_STATUS), device_(device) {
  // 主子节点负责把设备回调接入通用发布/录制框架，并注册全部设备命令。
  if (device_) {
    device_->setImuDataCallback(
        [this](const json &msg) { imuDataCallback(msg); });
    device_->setImageDataCallback(
        [this](const json &msg) { imageDataCallback(msg); });
  }
  rootPath_ = resolveStorageRootPath(rootPath_);
  if (!rootPath_.isEmpty()) {
    QDir().mkpath(rootPath_);
  }
  registerDeviceCommands();
  std::cout << "[" << name.toStdString() << "] MainSubnode initialized"
            << " (root_path=" << rootPath_.toStdString() << ")"
            << std::endl;
}

MainSubnode::~MainSubnode() {
  // 析构时确保录制和外部 worker 都被正确收尾。
  if (recordFlag_) {
    cmdStopRecord(0, "stop_record", {});
  }
  stopExternalWorker(screenCaptureWorkerProcess_,
                     QStringLiteral("ScreenCaptureWorker"));
  stopExternalWorker(micRecordWorkerProcess_,
                     QStringLiteral("MicRecordWorker"));
}

bool MainSubnode::shouldPublish(int64_t currentNs, int64_t &lastNs,
                                int64_t intervalNs) {
  // 基于时间间隔决定当前状态消息是否需要再次发布。
  if (currentNs - lastNs >= intervalNs) {
    lastNs = currentNs;
    return true;
  }
  return false;
}

void MainSubnode::resetMotionStatusState(const QString &reason) {
  // 清空运动检测器和相关缓存，开始新的设备会话前重建状态基线。
  if (motionDetector_) {
    motionDetector_->clear();
  }

  lastMotionStatusPublishNs_ = 0;
  lastMotionStatusValue_.clear();

  if (!reason.isEmpty()) {
    std::cout << "[" << name_.toStdString()
              << "] Motion status state reset: " << reason.toStdString()
              << std::endl;
  }
}

void MainSubnode::syncImageAliases(json &imageMessage) const {
  // 统一补齐 image/image_raw 双字段，降低上下游对单一字段名的耦合。
  if (!imageMessage.is_object() || !imageMessage.contains("cam_data") ||
      !imageMessage["cam_data"].is_object()) {
    return;
  }

  for (auto &item : imageMessage["cam_data"].items()) {
    auto &camInfo = item.value();
    if (!camInfo.is_object()) {
      continue;
    }

    // 旧版 UI 和录制链路会同时读 image / image_raw，
    // 所以这里统一补齐双字段，降低后续上下游对单一字段名的耦合。
    if (camInfo.contains("image_raw") && !camInfo.contains("image")) {
      camInfo["image"] = camInfo["image_raw"];
    } else if (camInfo.contains("image") && !camInfo.contains("image_raw") &&
               camInfo["image"].is_object()) {
      camInfo["image_raw"] = camInfo["image"];
    }
  }
}

bool MainSubnode::startScreenCaptureWorker(const QString &recordPath) {
  // 启动屏幕录制辅助 worker，并通过 ready-file 完成启动握手。
  stopExternalWorker(screenCaptureWorkerProcess_,
                     QStringLiteral("ScreenCaptureWorker"));

  const QString projectRoot = resolveProjectRootPath();
  if (projectRoot.isEmpty()) {
    std::cerr << "[" << name_.toStdString()
              << "] Failed to start ScreenCaptureWorker: project root not found"
              << std::endl;
    return false;
  }

  const QString workerScript =
      QDir(projectRoot).filePath("scripts/runtime/run_recording_worker.py");
  if (!QFileInfo::exists(workerScript)) {
    std::cerr
        << "[" << name_.toStdString()
        << "] Failed to start ScreenCaptureWorker: worker script missing: "
        << workerScript.toStdString() << std::endl;
    return false;
  }

  auto process = std::make_unique<QProcess>();
  const QString readyFile =
      QDir(recordPath).filePath(QStringLiteral(".screen_capture_worker.ready"));
  QFile::remove(readyFile);
  process->setWorkingDirectory(projectRoot);
  process->setProcessEnvironment(buildWorkerEnvironment(projectRoot));
  process->setProgram(QStringLiteral("python3"));
  process->setArguments({workerScript, QStringLiteral("--project-root"),
                         projectRoot, QStringLiteral("--worker"),
                         QStringLiteral("screen_capture"),
                         QStringLiteral("--save-dir"), recordPath,
                         QStringLiteral("--ready-file"), readyFile});

  QObject::connect(process.get(), &QProcess::readyReadStandardError, this,
                   [this, proc = process.get()]() {
                     const QByteArray data = proc->readAllStandardError();
                     if (!data.trimmed().isEmpty()) {
                       std::cerr << "[" << name_.toStdString()
                                 << "] ScreenCaptureWorker stderr: "
                                 << data.toStdString();
                     }
                   });
  QObject::connect(process.get(), &QProcess::readyReadStandardOutput, this,
                   [this, proc = process.get()]() {
                     const QByteArray data = proc->readAllStandardOutput();
                     if (!data.trimmed().isEmpty()) {
                       std::cout << "[" << name_.toStdString()
                                 << "] ScreenCaptureWorker stdout: "
                                 << data.toStdString();
                     }
                   });

  process->start();
  if (!process->waitForStarted(5000)) {
    std::cerr << "[" << name_.toStdString()
              << "] Failed to start ScreenCaptureWorker: "
              << process->errorString().toStdString() << std::endl;
    QFile::remove(readyFile);
    return false;
  }

  if (!waitForExternalWorkerReady(process.get(), readyFile,
                                  QStringLiteral("ScreenCaptureWorker"), 5000)) {
    stopExternalWorker(process, QStringLiteral("ScreenCaptureWorker"), 3000);
    return false;
  }

  screenCaptureWorkerProcess_ = std::move(process);
  std::cout << "[" << name_.toStdString()
            << "] ScreenCaptureWorker started: " << recordPath.toStdString()
            << std::endl;
  return true;
}

bool MainSubnode::startMicRecordWorker(const QString &recordPath) {
  // 启动麦克风录制辅助 worker，并通过 ready-file 完成启动握手。
  stopExternalWorker(micRecordWorkerProcess_,
                     QStringLiteral("MicRecordWorker"));

  const QString projectRoot = resolveProjectRootPath();
  if (projectRoot.isEmpty()) {
    std::cerr << "[" << name_.toStdString()
              << "] Failed to start MicRecordWorker: project root not found"
              << std::endl;
    return false;
  }

  const QString workerScript =
      QDir(projectRoot).filePath("scripts/runtime/run_recording_worker.py");
  if (!QFileInfo::exists(workerScript)) {
    std::cerr << "[" << name_.toStdString()
              << "] Failed to start MicRecordWorker: worker script missing: "
              << workerScript.toStdString() << std::endl;
    return false;
  }

  auto process = std::make_unique<QProcess>();
  const QString readyFile =
      QDir(recordPath).filePath(QStringLiteral(".mic_record_worker.ready"));
  QFile::remove(readyFile);
  process->setWorkingDirectory(projectRoot);
  process->setProcessEnvironment(buildWorkerEnvironment(projectRoot));
  process->setProgram(QStringLiteral("python3"));
  process->setArguments({workerScript, QStringLiteral("--project-root"),
                         projectRoot, QStringLiteral("--worker"),
                         QStringLiteral("mic_record"),
                         QStringLiteral("--save-dir"), recordPath,
                         QStringLiteral("--ready-file"), readyFile});

  QObject::connect(process.get(), &QProcess::readyReadStandardError, this,
                   [this, proc = process.get()]() {
                     const QByteArray data = proc->readAllStandardError();
                     if (!data.trimmed().isEmpty()) {
                       std::cerr << "[" << name_.toStdString()
                                 << "] MicRecordWorker stderr: "
                                 << data.toStdString();
                     }
                   });
  QObject::connect(process.get(), &QProcess::readyReadStandardOutput, this,
                   [this, proc = process.get()]() {
                     const QByteArray data = proc->readAllStandardOutput();
                     if (!data.trimmed().isEmpty()) {
                       std::cout << "[" << name_.toStdString()
                                 << "] MicRecordWorker stdout: "
                                 << data.toStdString();
                     }
                   });

  process->start();
  if (!process->waitForStarted(5000)) {
    std::cerr << "[" << name_.toStdString()
              << "] Failed to start MicRecordWorker: "
              << process->errorString().toStdString() << std::endl;
    QFile::remove(readyFile);
    return false;
  }

  if (!waitForExternalWorkerReady(process.get(), readyFile,
                                  QStringLiteral("MicRecordWorker"), 5000)) {
    stopExternalWorker(process, QStringLiteral("MicRecordWorker"), 3000);
    return false;
  }

  micRecordWorkerProcess_ = std::move(process);
  std::cout << "[" << name_.toStdString()
            << "] MicRecordWorker started: " << recordPath.toStdString()
            << std::endl;
  return true;
}

bool MainSubnode::waitForExternalWorkerReady(QProcess *process,
                                             const QString &readyFile,
                                             const QString &workerName,
                                             int timeoutMs) {
  // 轮询 ready-file 结果，判断外部 worker 是否已经完成初始化。
  if (!process) {
    QFile::remove(readyFile);
    return false;
  }

  QElapsedTimer timer;
  timer.start();

  while (timer.elapsed() < timeoutMs) {
    QFile file(readyFile);
    if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      const QString content = QString::fromUtf8(file.readAll());
      file.close();
      QFile::remove(readyFile);

      const QStringList lines = content.split('\n');
      const QString state = lines.value(0).trimmed().toLower();
      const QString detail = lines.mid(1).join(QStringLiteral("\n")).trimmed();

      if (state == QStringLiteral("ready")) {
        return true;
      }

      if (state == QStringLiteral("error")) {
        std::cerr << "[" << name_.toStdString() << "] "
                  << workerName.toStdString()
                  << " startup failed: " << detail.toStdString() << std::endl;
        return false;
      }
    }

    if (process->state() == QProcess::NotRunning) {
      std::cerr << "[" << name_.toStdString() << "] "
                << workerName.toStdString()
                << " exited before readiness handshake: exitCode="
                << process->exitCode() << " status="
                << static_cast<int>(process->exitStatus()) << std::endl;
      QFile::remove(readyFile);
      return false;
    }

    QThread::msleep(50);
  }

  std::cerr << "[" << name_.toStdString() << "] "
            << workerName.toStdString()
            << " readiness handshake timed out: " << readyFile.toStdString()
            << std::endl;
  QFile::remove(readyFile);
  return false;
}

void MainSubnode::stopExternalWorker(std::unique_ptr<QProcess> &process,
                                     const QString &workerName, int timeoutMs) {
  // 尝试优雅停止外部 worker；超时仍未退出时升级为 kill。
  if (!process) {
    return;
  }

  if (process->state() != QProcess::NotRunning) {
    process->terminate();
    if (!process->waitForFinished(timeoutMs)) {
      std::cerr << "[" << name_.toStdString() << "] "
                << workerName.toStdString() << " did not exit in time, killing"
                << std::endl;
      process->kill();
      process->waitForFinished(5000);
    }
  }

  const QByteArray remainingStdout = process->readAllStandardOutput();
  if (!remainingStdout.trimmed().isEmpty()) {
    std::cout << "[" << name_.toStdString() << "] " << workerName.toStdString()
              << " stdout: " << remainingStdout.toStdString();
  }

  const QByteArray remainingStderr = process->readAllStandardError();
  if (!remainingStderr.trimmed().isEmpty()) {
    std::cerr << "[" << name_.toStdString() << "] " << workerName.toStdString()
              << " stderr: " << remainingStderr.toStdString();
  }

  process.reset();
}

void MainSubnode::registerDeviceCommands() {
  // 注册主设备支持的标准命令，统一复用 BaseSubnode 的动作入口。
  registerCmd("init_device",
              [this](uint32_t g, const std::string &c, const json &p) {
                return cmdInitDevice(g, c, p);
              });
  registerCmd("start_device",
              [this](uint32_t g, const std::string &c, const json &p) {
                return cmdStartDevice(g, c, p);
              });
  registerCmd("stop_device",
              [this](uint32_t g, const std::string &c, const json &p) {
                return cmdStopDevice(g, c, p);
              });
  registerCmd("control_device",
              [this](uint32_t g, const std::string &c, const json &p) {
                return cmdControlDevice(g, c, p);
              });
  registerCmd("release_device",
              [this](uint32_t g, const std::string &c, const json &p) {
                return cmdReleaseDevice(g, c, p);
              });
  registerCmd("start_record",
              [this](uint32_t g, const std::string &c, const json &p) {
                return cmdStartRecord(g, c, p);
              });
  registerCmd("stop_record",
              [this](uint32_t g, const std::string &c, const json &p) {
                return cmdStopRecord(g, c, p);
              });
  registerCmd("delete_record",
              [this](uint32_t g, const std::string &c, const json &p) {
                return cmdDeleteRecord(g, c, p);
              });
  registerCmd("get_runtime_state",
              [this](uint32_t g, const std::string &c, const json &p) {
                return cmdGetRuntimeState(g, c, p);
              });
}

void MainSubnode::createMainPublishers() {
  // 创建主数据流发布器，包括 IMU、图像、录制计时、延迟和运动状态。
  createPublisher(imuDataPort_, "imu_data", "json", 0, false);
  createPublisher(imageDataPort_, TOPIC_CAMERA, "json", 0, false);
  createPublisher(recordTimerPort_, "record_timer", "json", 0, false);
  createPublisher(timeDelayPort_, "time_delay", "json", 0, false);
  createPublisher(motionStatusPort_, "motion_status", "json", 0, true);
  createPublisher(PORT_NVIZ_TREE, TOPIC_NVIZ_TREE, "json", 0, false);
  std::this_thread::sleep_for(std::chrono::seconds(1));
}

void MainSubnode::addImuWriter(int imuIndex,
                               std::unique_ptr<CsvDataWriter> writer) {
  // 为指定 IMU 索引挂接一个 CSV 写入器。
  imuDataWriters_[imuIndex] = std::move(writer);
}

void MainSubnode::setImageWriter(std::unique_ptr<ImageDataWriter> writer) {
  // 设置图像写入器，用于录制期间落盘双目图像。
  imageDataWriter_ = std::move(writer);
}

void MainSubnode::setRgbImageWriter(
    std::unique_ptr<RgbImageDataWriter> writer) {
  // 设置单路 RGB 写入器，用于 RGB 模式下保存 BMP + sidecar。
  rgbImageDataWriter_ = std::move(writer);
}

nlohmann::json MainSubnode::recordingState() {
  // 返回当前录制状态快照，供 BSP RGB 页面轮询展示。
  QMutexLocker stateLocker(&recordStateLock_);

  json state = {
      {"is_recording", recordFlag_.load()},
      {"record_path", recordPath_.isEmpty() ? nullptr
                                             : json(recordPath_.toStdString())},
      {"dataset_name", nullptr},
      {"image_mode", recordImageMode_.isEmpty()
                         ? nullptr
                         : json(recordImageMode_.toStdString())},
      {"frame_count", 0},
      {"last_frame_file", nullptr},
  };

  if (!recordPath_.isEmpty()) {
    const QString relative =
        QDir(rootPath_).relativeFilePath(recordPath_);
    state["dataset_name"] =
        relative.isEmpty() ? QFileInfo(recordPath_).fileName().toStdString()
                           : relative.toStdString();
  }

  if (recordImageMode_ == QStringLiteral("rgb") && rgbImageDataWriter_) {
    state["frame_count"] = rgbImageDataWriter_->savedFrameCount();
    const auto lastFrame = rgbImageDataWriter_->lastFrameFilename();
    if (!lastFrame.empty()) {
      state["last_frame_file"] = lastFrame;
    }
  }

  return state;
}

// ==================== Device commands ====================

json MainSubnode::cmdInitDevice(uint32_t, const std::string &,
                                const json &params) {
  // 调用底层设备 initialize，并在成功后重置运动状态基线。
  if (!device_)
    return {{"success", false}, {"message", "No device"}};
  try {
    auto result = device_->initialize(params);
    if (result.value("success", false)) {
      resetMotionStatusState(QStringLiteral("init_device"));
    }
    return result;
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json MainSubnode::cmdStartDevice(uint32_t, const std::string &,
                                 const json &params) {
  // 启动底层设备数据流，并在成功后清理旧运动状态缓存。
  if (!device_)
    return {{"success", false}, {"message", "No device"}};
  try {
    const json normalizedParams = params.is_object() ? params : json::object();
    auto result = device_->start(normalizedParams);
    if (result.value("success", false)) {
      resetMotionStatusState(QStringLiteral("start_device"));
    }
    return result;
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json MainSubnode::cmdStopDevice(uint32_t goalId, const std::string &cmd,
                                const json &params) {
  // 停止设备前若仍在录制，则先完成 stop_record 收尾。
  if (!device_)
    return {{"success", false}, {"message", "No device"}};
  try {
    if (recordFlag_) {
      auto r = cmdStopRecord(goalId, cmd, {});
      if (!r.value("success", false))
        return r;
    }
    const json normalizedParams = params.is_object() ? params : json::object();
    auto result = device_->stop(normalizedParams);
    if (result.value("success", false)) {
      resetMotionStatusState(QStringLiteral("stop_device"));
    }
    return result;
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json MainSubnode::cmdControlDevice(uint32_t, const std::string &,
                                   const json &params) {
  // 将控制参数直接透传给底层设备控制接口。
  if (!device_)
    return {{"success", false}, {"message", "No device"}};
  try {
    return device_->control(params);
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json MainSubnode::cmdReleaseDevice(uint32_t, const std::string &,
                                   const json &) {
  // 释放设备资源，并同步重置运动状态缓存。
  if (!device_)
    return {{"success", false}, {"message", "No device"}};
  try {
    auto result = device_->release();
    if (result.value("success", false)) {
      resetMotionStatusState(QStringLiteral("release_device"));
    }
    return result;
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json MainSubnode::cmdStartRecord(uint32_t, const std::string &,
                                 const json &params) {
  // 启动一次新的录制会话，准备目录、writer 和辅助 worker。
  try {
    QMutexLocker stateLocker(&recordStateLock_);
    if (recordFlag_) {
      return {{"success", false}, {"message", "Already recording"}};
    }
    if (recordFinalizing_) {
      return {{"success", false}, {"message", "上次录制正在收尾，请等待"}};
    }

    std::string datasetName = params.value("dataset_name", std::string());
    if (datasetName.empty())
      return {{"success", false}, {"message", "Missing dataset_name"}};

    const QString recordPath = QDir(rootPath_).filePath(
        QDir::cleanPath(QString::fromStdString(datasetName)));
    std::cout << "[" << name_.toStdString()
              << "] start_record: rootPath_=" << rootPath_.toStdString()
              << " dataset_name=" << datasetName
              << " → recordPath=" << recordPath.toStdString() << std::endl;
    QDir().mkpath(recordPath);
    recordPath_ = recordPath;

    // Open all IMU writers
    std::vector<CsvDataWriter *> openedImuWriters;
    for (auto &[imuIdx, writer] : imuDataWriters_) {
      if (!writer->open(recordPath.toStdString())) {
        for (auto *openedWriter : openedImuWriters) {
          if (openedWriter) {
            openedWriter->close();
          }
        }
        recordPath_.clear();
        return {
            {"success", false},
            {"message", "Failed to open IMU writer " + std::to_string(imuIdx)}};
      }
      openedImuWriters.push_back(writer.get());
    }

    // Open image writers if requested. SLAM PGM 与 RGB BMP 是两条互斥路径。
    bool enableImage = params.value("enable_image_recording", false);
    bool enableRgb = params.value("enable_rgb_recording", false);
    if (enableImage && enableRgb) {
      for (auto *openedWriter : openedImuWriters) {
        if (openedWriter) {
          openedWriter->close();
        }
      }
      recordPath_.clear();
      return {{"success", false},
              {"message", "SLAM image recording and RGB recording cannot both be enabled"}};
    }

    if (enableImage && imageDataWriter_) {
      if (!imageDataWriter_->open(recordPath.toStdString())) {
        for (auto *openedWriter : openedImuWriters) {
          if (openedWriter) {
            openedWriter->close();
          }
        }
        recordPath_.clear();
        return {{"success", false}, {"message", "Failed to open image writer"}};
      }
    }

    if (enableRgb) {
      if (!rgbImageDataWriter_) {
        for (auto *openedWriter : openedImuWriters) {
          if (openedWriter) {
            openedWriter->close();
          }
        }
        if (imageDataWriter_) {
          imageDataWriter_->close();
        }
        recordPath_.clear();
        return {{"success", false},
                {"message", "No RGB image writer configured"}};
      }
      if (!rgbImageDataWriter_->open(recordPath.toStdString())) {
        for (auto *openedWriter : openedImuWriters) {
          if (openedWriter) {
            openedWriter->close();
          }
        }
        if (imageDataWriter_) {
          imageDataWriter_->close();
        }
        recordPath_.clear();
        return {{"success", false}, {"message", "Failed to open RGB image writer"}};
      }
    }

    recordImageMode_ = enableRgb   ? QStringLiteral("rgb")
                       : enableImage ? QStringLiteral("slam")
                                     : QString();
    recordFlag_ = true;
    startRecordTimestampNs_ = nowNs();
    lastRecordTimerPublishNs_ = 0;
    lastRecordTimerStatus_.clear();
    lastCameraSnapshotFeedNs_ = {0, 0};

    // 和旧版 Python 一致：
    // 1. 双目快照由主节点内置 worker 直接吃主线程里的最新帧；
    // 2. 屏幕截图、麦克风录音继续走辅助 worker，但这些 worker 已经
    //    vendored 到新工程里，不再依赖旧 RecordLab。
    if (params.value("enable_camera_snapshot", false)) {
      cameraSnapshotWorker_ =
          std::make_unique<CameraSnapshotWorker>(recordPath);
      cameraSnapshotWorker_->start();
    }
    QStringList helperStartupWarnings;
    if (params.value("enable_screen_capture", false) &&
        !startScreenCaptureWorker(recordPath)) {
      helperStartupWarnings.push_back(
          QStringLiteral("Failed to start ScreenCaptureWorker"));
    }
    if (params.value("enable_mic_recording", false) &&
        !startMicRecordWorker(recordPath)) {
      helperStartupWarnings.push_back(
          QStringLiteral("Failed to start MicRecordWorker"));
    }

    if (!helperStartupWarnings.isEmpty()) {
      const QString warningText = helperStartupWarnings.join(QStringLiteral("; "));
      std::cerr << "[" << name_.toStdString()
                << "] Recording helper warning: "
                << warningText.toStdString() << std::endl;
      QFile infoFile(QDir(recordPath).filePath(QStringLiteral("record_info.txt")));
      if (infoFile.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&infoFile);
        stream << "Recording helper warning: " << warningText << "\n";
      }
    }

    std::cout << "[" << name_.toStdString()
              << "] Recording started: " << recordPath.toStdString()
              << std::endl;
    std::string message = "Recording started: " + recordPath.toStdString();
    if (!helperStartupWarnings.isEmpty()) {
      message += " (warning: " + helperStartupWarnings.join(QStringLiteral("; ")).toStdString() + ")";
    }
    return {{"success", true}, {"message", message}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json MainSubnode::cmdStopRecord(uint32_t, const std::string &, const json &) {
  // 停止录制并串行完成 writer、快照线程和外部 worker 的最终收尾。
  bool finalizationOwner = false;
  json result;
  try {
    std::unique_ptr<CameraSnapshotWorker> cameraSnapshotWorker;
    std::unique_ptr<QProcess> screenCaptureWorkerProcess;
    std::unique_ptr<QProcess> micRecordWorkerProcess;
    {
      QMutexLocker stateLocker(&recordStateLock_);
      if (!recordFlag_) {
        if (recordFinalizing_) {
          return {{"success", false},
                  {"message", "Previous recording is still finalizing"}};
        }
        return {{"success", false}, {"message", "Not recording"}};
      }

      recordFlag_ = false;
      recordFinalizing_ = true;
      finalizationOwner = true;
      lastRecordTimerPublishNs_ = 0;
      lastRecordTimerStatus_.clear();

      cameraSnapshotWorker = std::move(cameraSnapshotWorker_);
      screenCaptureWorkerProcess = std::move(screenCaptureWorkerProcess_);
      micRecordWorkerProcess = std::move(micRecordWorkerProcess_);
    }

    for (auto &[_, writer] : imuDataWriters_)
      writer->close();
    if (imageDataWriter_)
      imageDataWriter_->close();
    if (rgbImageDataWriter_)
      rgbImageDataWriter_->close();

    if (cameraSnapshotWorker) {
      cameraSnapshotWorker->stop();
    }
    stopExternalWorker(screenCaptureWorkerProcess,
                       QStringLiteral("ScreenCaptureWorker"));
    stopExternalWorker(micRecordWorkerProcess,
                       QStringLiteral("MicRecordWorker"));

    if (!recordPath_.isEmpty())
      onRecordStopped();

    std::cout << "[" << name_.toStdString() << "] Recording stopped"
              << std::endl;
    result = {{"success", true}, {"message", ""}};
  } catch (const std::exception &e) {
    result = {{"success", false}, {"message", e.what()}};
  }

  if (finalizationOwner) {
    QMutexLocker stateLocker(&recordStateLock_);
    recordImageMode_.clear();
    recordFinalizing_ = false;
  }

  return result;
}

json MainSubnode::cmdDeleteRecord(uint32_t, const std::string &,
                                  const json &params) {
  // 删除指定数据集目录，或删除最近一次录制目录。
  std::string dsName = params.value("dataset_name", std::string());
  QString targetPath;
  if (!dsName.empty()) {
    targetPath = QDir(rootPath_).filePath(QString::fromStdString(dsName));
  } else if (!recordPath_.isEmpty()) {
    targetPath = recordPath_;
  } else {
    return {{"success", false},
            {"message", "No dataset_name and no recent record"}};
  }
  QDir dir(targetPath);
  if (dir.exists()) {
    dir.removeRecursively();
    std::cout << "[" << name_.toStdString()
              << "] Deleted: " << targetPath.toStdString() << std::endl;
    return {{"success", true},
            {"message", "Deleted: " + targetPath.toStdString()}};
  }
  return {{"success", false},
          {"message", "Path not found: " + targetPath.toStdString()}};
}

json MainSubnode::cmdGetRuntimeState(uint32_t, const std::string &,
                                     const json &) {
  // 返回录制状态、时间延迟和运动状态等运行时快照。
  json result = {
      {"success", true},
      {"message", "Runtime state snapshot"},
      {"recording", recordFlag_.load()},
      {"time_delay_ms", 0.0},
      {"motion_status",
       lastMotionStatusValue_.empty() ? std::string("none")
                                      : lastMotionStatusValue_},
  };
  result["record_state"] = recordingState();

  if (recordFlag_.load() && startRecordTimestampNs_ > 0) {
    result["record_timer"] =
        static_cast<double>(nowNs() - startRecordTimestampNs_) / 1e9;
  } else {
    result["record_timer"] = nullptr;
  }

  return result;
}

void MainSubnode::onRecordStopped() {
  // 供派生类在录制结束后追加自定义收尾逻辑。
  // Hook for subclasses (e.g., save config files)
}

// ==================== BaseSubnode overrides ====================

json MainSubnode::onRelease() {
  // 子节点释放时确保先停录制，再停设备并释放底层资源。
  try {
    if (recordFlag_) {
      auto r = cmdStopRecord(0, "", {});
      if (!r.value("success", false))
        return r;
    }
    if (device_) {
      auto r = device_->stop({});
      if (!r.value("success", false))
        return r;
      r = device_->release();
      if (!r.value("success", false))
        return r;
    }
    resetMotionStatusState(QStringLiteral("on_release"));
    return {{"success", true}, {"message", ""}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json MainSubnode::onEstop() {
  // 急停优先停止录制；若未在录制中则仅重置运动状态。
  try {
    std::cout << "[" << name_.toStdString() << "] Emergency stopping..."
              << std::endl;
    if (recordFlag_) {
      return cmdStopRecord(0, "", {});
    }
    resetMotionStatusState(QStringLiteral("on_estop"));
    return {{"success", true}, {"message", ""}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json MainSubnode::onCheck() {
  // 调用底层设备 health check，向上层返回统一 success/message 结构。
  try {
    if (device_) {
      auto r = device_->check();
      if (!r.value("success", false))
        return r;
      if (!r.contains("message")) {
        r["message"] = "";
      }
      return r;
    }
    return {{"success", true}, {"message", ""}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

// ==================== Data Callbacks ====================

void MainSubnode::imuDataCallback(const json &message) {
  // IMU 主回调：发布原始数据、时间延迟、运动状态，并在录制时写 CSV/计时。
  /*
   * IMU 数据回调: 发布 → 检测 → 录制
   *
   * 格式: {type, timestamp_ns, data: [6]}
   */
  try {
    static std::atomic<uint64_t> imuCallbackCount{0};
    static ImuStreamProbe imuProbe;
    const uint64_t callbackCount = ++imuCallbackCount;
    if (callbackCount <= 100 || callbackCount % 1000 == 0) {
      std::cout << "[" << name_.toStdString()
                << "][IMU_DATA_CALLBACK] count=" << callbackCount
                << " message=" << message.dump() << std::endl;
    }
    int64_t currentTimestampNs = nowNs();
    logImuProbeStats(name_.toStdString().c_str(), imuProbe, currentTimestampNs,
                     message.value("timestamp_ns", int64_t(0)),
                     message.value("type", 0));

    // 1. 发布 IMU 数据到实时预览总线。这里必须全量发布，保持与旧
    // Python 版本一致：IMU0-gyro/acc 等单路都应能达到约 1000Hz。
    // UI 侧另有抽帧，避免高频数据直接压到主线程。
    publish(imuDataPort_, message);

    // 2. 发布时间延迟 (10Hz)
    if (shouldPublish(currentTimestampNs, lastTimeDelayPublishNs_,
                      timeDelayPublishIntervalNs_)) {
      publish(timeDelayPort_, {{"name", "time_delay"},
                               {"timestamp_ns", currentTimestampNs},
                               {"time_delay_ns", 0},
                               {"status", ""}});
    }

    // 3. 运动检测 + 发布 motion_status (10Hz)
    if (motionDetector_) {
      motionDetector_->addImuMessage(message);
      auto motionMsg = motionDetector_->detect();
      std::string motionStatus = motionMsg.value("status", std::string(""));

      if (motionStatus != lastMotionStatusValue_ ||
          shouldPublish(currentTimestampNs, lastMotionStatusPublishNs_,
                        motionStatusPublishIntervalNs_)) {
        lastMotionStatusPublishNs_ = currentTimestampNs;
        lastMotionStatusValue_ = motionStatus;
        publish(motionStatusPort_, motionMsg);
      }
    }

    // 4. 录制
    if (recordFlag_) {
      std::string status;

      if (!imuDataWriters_.empty()) {
        auto dataArray =
            message.value("data", std::vector<double>{0, 0, 0, 0, 0, 0});
        int imuType = message.value("type", 0);

        // 确定 IMU 索引
        auto it = IMU_TYPE_TO_INDEX.find(imuType);
        int imuIdx = (it != IMU_TYPE_TO_INDEX.end()) ? it->second : -1;

        // 转换为录制格式 (data0-data5)
        json recordData = {
            {"timestamp_ns", message.value("timestamp_ns", int64_t(0))},
            {"type", imuType},
            {"data0", dataArray.size() > 0 ? dataArray[0] : 0.0},
            {"data1", dataArray.size() > 1 ? dataArray[1] : 0.0},
            {"data2", dataArray.size() > 2 ? dataArray[2] : 0.0},
            {"data3", dataArray.size() > 3 ? dataArray[3] : 0.0},
            {"data4", dataArray.size() > 4 ? dataArray[4] : 0.0},
            {"data5", dataArray.size() > 5 ? dataArray[5] : 0.0}};

        // 写入对应 IMU 写入器
        auto writerIt = imuDataWriters_.find(imuIdx);
        if (writerIt == imuDataWriters_.end())
          writerIt = imuDataWriters_.find(-1); // fallback
        if (writerIt != imuDataWriters_.end()) {
          if (!writerIt->second->writeData(recordData))
            status = "write failed";
        } else {
          status = "no writer for IMU" + std::to_string(imuIdx);
        }
      } else {
        status = "no writer";
      }

      // 发布录制计时 (5Hz)
      if (startRecordTimestampNs_ > 0) {
        int64_t durationNs = currentTimestampNs - startRecordTimestampNs_;
        if (status != lastRecordTimerStatus_ ||
            shouldPublish(currentTimestampNs, lastRecordTimerPublishNs_,
                          recordTimerPublishIntervalNs_)) {
          lastRecordTimerPublishNs_ = currentTimestampNs;
          lastRecordTimerStatus_ = status;
          publish(recordTimerPort_, {{"name", "record_timer"},
                                     {"timestamp_ns", currentTimestampNs},
                                     {"duration_ns", durationNs},
                                     {"status", status}});
        }
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "[" << name_.toStdString()
              << "] IMU callback error: " << e.what() << std::endl;
  }
}

void MainSubnode::imageDataCallback(const json &message) {
  // 图像主回调：发布 metadata、按需回读共享内存，并在录制时写图像数据。
  /*
   * 图像数据回调: 发布 → 快照 → 录制
   *
   * 当前 bridge 会把图像主体写入共享内存，回调消息里只保留 metadata + shm_seq。
   * 因此：
   * 1. UI 仍直接消费轻量 metadata；
   * 2. 只有在“相机快照 / 图像录制”确实开启时，才按需从共享内存回读图像主体。
   */
  try {
    const int64_t currentTimestampNs = nowNs();
    const bool rgbRecording =
        recordFlag_ && rgbImageDataWriter_ &&
        recordImageMode_ == QStringLiteral("rgb");
    const bool needsHydratedFrames =
        cameraSnapshotWorker_ != nullptr || (recordFlag_ && imageDataWriter_) ||
        rgbRecording;
    json hydratedMessage = message;

    // 1. 发布图像数据 — 直接转发 bridge 的已编码数据。
    // Bridge worker 已完成序列化（JPEG base64），UI 端
    // data_monitor_widget::imageFromPayload 能直接解码显示。
    // 不做 JSON 深拷贝、不做 JPEG 解码→重编码，避免阻塞主线程。
    if (shouldPublish(currentTimestampNs, lastCameraPublishNs_,
                      cameraPublishIntervalNs_)) {
      if (message.contains("cam_data") && message["cam_data"].is_object() &&
          !message["cam_data"].empty()) {
        publish(imageDataPort_, message);
      }
    }

    // 2. 喂帧给 CameraSnapshotWorker（仅录制时存在）。
    // 这里需要解码 JPEG → QImage，但有 5Hz 频率限制，开销可接受。
    if (needsHydratedFrames &&
        message.contains("cam_data") && message["cam_data"].is_object()) {
      if (!cameraShmReader_) {
        cameraShmReader_ =
            std::make_unique<recordlab::common::CameraSharedMemoryReader>();
      }
      if (!cameraShmReader_->isAttached()) {
        cameraShmReader_->attach();
      }

      auto &hydratedCamData = hydratedMessage["cam_data"];
      const auto &camData = message["cam_data"];
      bool hydratedAnyFrame = false;

      for (int camIdx = 0; camIdx < 2; ++camIdx) {
        const std::string camKey = std::to_string(camIdx);
        if (!camData.contains(camKey) || !camData[camKey].is_object()) {
          continue;
        }

        QImage hydratedFrame;
        const json imageInfo = camData[camKey].value("image_raw", json::object());
        const json hydratedImage = imagePayloadFromSharedMemory(
            cameraShmReader_.get(), camIdx, lastCameraRecordReadSeq_[camIdx],
            imageInfo, &hydratedFrame);
        if (hydratedImage.is_object() && !hydratedImage.empty()) {
          hydratedCamData[camKey]["image_raw"] = hydratedImage;
          hydratedAnyFrame = true;
        }
        if (hydratedFrame.isNull()) {
          hydratedFrame = extractSnapshotImage(camData[camKey]);
        }

        if (cameraSnapshotWorker_ &&
            shouldPublish(currentTimestampNs, lastCameraSnapshotFeedNs_[camIdx],
                          cameraSnapshotFeedIntervalNs_)) {
          if (!hydratedFrame.isNull()) {
            cameraSnapshotWorker_->updateFrame(camIdx, hydratedFrame);
          }
        }
      }

      if (hydratedAnyFrame) {
        syncImageAliases(hydratedMessage);
      }
    }

    // 3. 图像录制。
    if (recordFlag_ && imageDataWriter_) {
      imageDataWriter_->writeData(hydratedMessage);
    }
    if (rgbRecording) {
      rgbImageDataWriter_->writeData(hydratedMessage);
    }
  } catch (const std::exception &e) {
    std::cerr << "[" << name_.toStdString()
              << "] Image callback error: " << e.what() << std::endl;
  }
}

} // namespace recordlab::subnodes
