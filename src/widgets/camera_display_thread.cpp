/*
 * CameraDisplayThread 实现
 *
 */

#include "recordlab/widgets/camera_display_thread.h"
#include "recordlab/common/camera_shared_memory.h"

#include <QImage>
#include <QtGlobal>

#include <chrono>
#include <cmath>
#include <iostream>

namespace recordlab::widgets {

using recordlab::common::CameraFrameMeta;
using recordlab::common::CameraSharedMemoryReader;

static double perfTimeSec() {
  // 使用单调时钟统计显示线程中的 FPS 和发帧节流时间。
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ============================================================================
// 构造 / 析构
// ============================================================================

CameraDisplayThread::CameraDisplayThread(CameraSharedMemoryReader *reader,
                                         QSize targetSize, double maxDisplayFps,
                                         QObject *parent)
    : QThread(parent), reader_(reader), targetSize_(targetSize) {
  // 将最大显示帧率换算成最小发帧间隔，后续统一在 processCamera 中限流。
  emitIntervalS_ = maxDisplayFps <= 0.0 ? 0.0 : (1.0 / maxDisplayFps);
}

// 析构时确保显示线程退出，避免共享内存读操作落到已释放对象。
CameraDisplayThread::~CameraDisplayThread() { stopThread(); }

void CameraDisplayThread::stopThread() {
  // 尝试正常等待线程退出；极端情况下再强制 terminate。
  running_ = false;
  if (!wait(3000)) {
    std::cerr << "[CameraDisplayThread] Force terminating..." << std::endl;
    terminate();
    wait();
  }
}

void CameraDisplayThread::setTargetSize(const QSize &size) {
  // 在 UI 线程修改目标尺寸时加锁，避免与后台缩放并发冲突。
  std::lock_guard<std::mutex> lock(targetSizeMutex_);
  targetSize_ = size;
}

void CameraDisplayThread::markFrameConsumed(int camIdx) {
  // UI 消费完一帧后清除背压标记，允许后台继续发送新预览图。
  if (camIdx >= 0 && camIdx < 2) {
    uiPending_[camIdx].store(false, std::memory_order_release);
  }
}

QImage CameraDisplayThread::latestSourceImage(int camIdx) const {
  // 返回最近一次成功解码的原始图像，供点击放大查看使用。
  if (camIdx < 0 || camIdx >= 2) {
    return {};
  }
  std::lock_guard<std::mutex> lock(latestSourceImageMutex_);
  return latestSourceImages_[camIdx];
}

// ============================================================================
// 主循环
// ============================================================================

void CameraDisplayThread::run() {
  // 后台循环持续读取双目共享内存，并按设定速率向 UI 发预览帧。
  std::cout << "[CameraDisplayThread] Starting..." << std::endl;

  if (!reader_) {
    std::cerr << "[CameraDisplayThread] No reader, stopping." << std::endl;
    return;
  }

  running_ = true;
  lastFpsTime_ = perfTimeSec();

  int loopCount = 0;
  double lastLogTime = perfTimeSec();

  while (running_) {
    ++loopCount;

    // 处理双目相机
    processCamera(0);
    processCamera(1);

    // FPS 统计
    const double now = perfTimeSec();
    if (now - lastFpsTime_ >= 1.0) {
      fps_ = frameCount_ / (now - lastFpsTime_);
      frameCount_ = 0;
      lastFpsTime_ = now;
    }

    // 每秒打印一次循环统计
    if (now - lastLogTime >= 1.0) {
      lastLogTime = now;
      loopCount = 0;
    }

    // 与原版后台预览节奏保持接近，避免相机刷新抢占曲线/UI 主线程预算。
    msleep(16);
  }

  std::cout << "[CameraDisplayThread] Stopped" << std::endl;
}

// ============================================================================
// 处理单个相机
// ============================================================================

void CameraDisplayThread::processCamera(int camIdx) {
  // 读取一路相机最新帧，做解码、缩放和统计后再发给 UI 主线程。
  CameraFrameMeta meta;
  QByteArray frameData;
  uint64_t prevSeq = lastFrameSeq_[camIdx];

  // 尝试读取最新帧
  if (!reader_->readLatestFrame(camIdx, prevSeq, meta, frameData)) {
    return; // 无新帧
  }

  const double now = perfTimeSec();

  // 帧率上限
  if (emitIntervalS_ > 0 && (now - lastEmitTime_[camIdx]) < emitIntervalS_) {
    // 更新序列号但不处理（跳过旧帧，只保留最新）
    lastFrameSeq_[camIdx] = prevSeq;
    return;
  }

  // 背压控制：若上一帧还没被 UI 消费，直接跳过
  if (uiPending_[camIdx].load(std::memory_order_acquire)) {
    lastFrameSeq_[camIdx] = prevSeq;
    return;
  }

  // 解码图像
  QImage image;
  if (meta.encodedFormat == 1) {
    // JPEG 编码：使用 QImage::loadFromData 解码
    if (!image.loadFromData(
            reinterpret_cast<const uchar *>(frameData.constData()),
            frameData.size(), "JPEG")) {
      lastFrameSeq_[camIdx] = prevSeq;
      return;
    }
  } else {
    // Raw pixel 数据
    if (meta.width <= 0 || meta.height <= 0)
      return;
    const auto format = static_cast<QImage::Format>(meta.format);
    if (format == QImage::Format_Invalid)
      return;
    int bpl = meta.bytesPerLine > 0 ? meta.bytesPerLine : meta.width;
    if (frameData.size() < bpl * meta.height) {
      lastFrameSeq_[camIdx] = prevSeq;
      return;
    }
    // 必须 copy，因为 frameData 会在下次读取时失效
    image = QImage(reinterpret_cast<const uchar *>(frameData.constData()),
                   meta.width, meta.height, bpl, format)
                .copy();
  }

  if (image.isNull()) {
    lastFrameSeq_[camIdx] = prevSeq;
    return;
  }

  {
    std::lock_guard<std::mutex> lock(latestSourceImageMutex_);
    latestSourceImages_[camIdx] = image;
  }

  QSize targetSize;
  {
    std::lock_guard<std::mutex> lock(targetSizeMutex_);
    targetSize = targetSize_;
  }
  if (targetSize.width() <= 0 || targetSize.height() <= 0) {
    targetSize = QSize(680, 420);
  }

  // 预览图优先保证刷新率，使用 FastTransformation。
  QImage scaled =
      image.scaled(targetSize, Qt::KeepAspectRatio, Qt::FastTransformation);

  double meanValue = 0.0;
  double stdValue = 0.0;
  {
    QImage statsImage = scaled;
    if (statsImage.format() != QImage::Format_Grayscale8) {
      statsImage = statsImage.convertToFormat(QImage::Format_Grayscale8);
    }
    if (!statsImage.isNull()) {
      const int width = statsImage.width();
      const int height = statsImage.height();
      const int bytesPerLine = statsImage.bytesPerLine();
      if (width > 0 && height > 0 && bytesPerLine > 0) {
        double sum = 0.0;
        double sumSquares = 0.0;
        const double pixelCount = static_cast<double>(width) * height;
        for (int row = 0; row < height; ++row) {
          const auto *line =
              reinterpret_cast<const uchar *>(statsImage.constScanLine(row));
          for (int col = 0; col < width; ++col) {
            const double pixel = static_cast<double>(line[col]);
            sum += pixel;
            sumSquares += pixel * pixel;
          }
        }
        meanValue = sum / pixelCount;
        const double variance =
            qMax(0.0, sumSquares / pixelCount - meanValue * meanValue);
        stdValue = std::sqrt(variance);
      }
    }
  }

  // 设置背压标记
  uiPending_[camIdx].store(true, std::memory_order_release);

  // 发送信号到 UI 主线程
  if (camIdx == 0) {
    emit leftFrameReady(scaled, meanValue, stdValue);
  } else {
    emit rightFrameReady(scaled, meanValue, stdValue);
  }

  lastEmitTime_[camIdx] = perfTimeSec();
  lastFrameSeq_[camIdx] = prevSeq;
  ++frameCount_;
}

} // namespace recordlab::widgets
