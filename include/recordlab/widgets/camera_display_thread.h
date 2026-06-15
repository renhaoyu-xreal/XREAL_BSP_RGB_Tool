/*
 * CameraDisplayThread — 专用相机显示线程
 *
 *
 * 架构：
 * 1. 从共享内存读取 raw bytes / JPEG（零拷贝）
 * 2. 在后台线程完成：raw → QImage → scaled → QPixmap
 * 3. 通过信号发送到 UI 主线程，主线程只做 setPixmap
 *
 * 性能优势：
 * - UI 主线程解放，只做 setPixmap（< 1ms）
 * - 所有重操作在后台线程并行处理
 * - 背压控制：若上一帧还没被 UI 消费，跳过当前帧
 */
#pragma once

#include <QImage>
#include <QSize>
#include <QThread>

#include <atomic>
#include <mutex>

namespace recordlab::common {
class CameraSharedMemoryReader;
}

namespace recordlab::widgets {

class CameraDisplayThread : public QThread {
  Q_OBJECT

public:
  /// @param reader   共享内存读取器（生命周期由调用者管理）
  /// @param targetSize  目标缩放尺寸（默认 640×400，与 Python 一致）
  /// @param maxDisplayFps  最大显示帧率（默认 24fps，与 Python 一致）
  explicit CameraDisplayThread(
      recordlab::common::CameraSharedMemoryReader *reader,
      QSize targetSize = {640, 400}, double maxDisplayFps = 30.0,
      QObject *parent = nullptr);

  ~CameraDisplayThread() override;

  /// 停止线程（从外部调用）
  void stopThread();

  /// 设置目标显示尺寸
  void setTargetSize(const QSize &size);

  /// UI 线程在完成 setPixmap 后调用，释放该路相机的背压标记
  void markFrameConsumed(int camIdx);

  /// 获取最新一帧原始图像，用于点击放大查看。
  QImage latestSourceImage(int camIdx) const;

signals:
  /// 左目相机预览图已就绪（QImage 可安全跨线程传输）
  void leftFrameReady(const QImage &previewImage, double meanValue,
                      double stdValue);

  /// 右目相机预览图已就绪
  void rightFrameReady(const QImage &previewImage, double meanValue,
                       double stdValue);

protected:
  void run() override;

private:
  /// 处理单个相机的图像
  void processCamera(int camIdx);

  recordlab::common::CameraSharedMemoryReader *reader_;
  QSize targetSize_;
  mutable std::mutex targetSizeMutex_;

  // 运行控制
  std::atomic<bool> running_{false};

  // 帧序列号跟踪（每路相机上次处理的序列号）
  uint64_t lastFrameSeq_[2] = {0, 0};

  // 背压控制：若上一帧还没被 UI 消费就跳过
  std::atomic<bool> uiPending_[2] = {{false}, {false}};

  // 帧率限制
  double emitIntervalS_ = 0.0;
  double lastEmitTime_[2] = {0.0, 0.0};

  // FPS 统计
  int frameCount_ = 0;
  double lastFpsTime_ = 0.0;
  double fps_ = 0.0;

  mutable std::mutex latestSourceImageMutex_;
  QImage latestSourceImages_[2];
};

} // namespace recordlab::widgets
