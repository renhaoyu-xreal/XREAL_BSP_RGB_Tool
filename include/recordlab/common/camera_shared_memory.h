#pragma once

#include <QByteArray>
#include <QSharedMemory>
#include <QString>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace recordlab::common {

// 常量（与 Python 版 _CAMERA_* 常量完全一致）
inline constexpr int kCameraCount = 2;
inline constexpr int kCameraShmSlotCount = 4;
inline constexpr int kCameraShmSlotSize = 2 * 1024 * 1024; // 2MB/slot

// 跨进程共享内存的 Key
inline constexpr char kCameraShmKey[] = "RecordLabC_CameraSHM";

// 元数据头大小（与 Python 版完全一致）
inline constexpr int kShmMetaSize = 32;

/*
 * 元数据布局（32 字节，little-endian，与 Python 版完全一致）：
 * [0:4]   width           (uint32)
 * [4:8]   height          (uint32)
 * [8:12]  format          (uint32, QImage::Format 枚举值)
 * [12:16] data_size       (uint32, 实际图像数据字节数)
 * [16:20] bytes_per_line  (uint32)
 * [20:24] encoded_format  (uint32, 0=raw pixels, 1=JPEG)
 * [24:32] reserved        (8 bytes)
 */
struct CameraFrameMeta {
  int width = 0;
  int height = 0;
  int format = 0;   // QImage::Format
  int dataSize = 0; // 实际图像数据字节数
  int bytesPerLine = 0;
  int encodedFormat = 0; // 0 = raw pixels, 1 = JPEG

  // 序列化到 32 字节缓冲区
  void writeTo(char *buf) const;
  // 从 32 字节缓冲区反序列化
  static CameraFrameMeta readFrom(const char *buf);
};

// ============================================================================
// CameraSharedMemoryWriter — 写入端（bsp_main_subnode / NativeGlassesAdapter
// 使用）
// ============================================================================

class CameraSharedMemoryWriter {
public:
  CameraSharedMemoryWriter();
  ~CameraSharedMemoryWriter();

  // 不可拷贝
  CameraSharedMemoryWriter(const CameraSharedMemoryWriter &) = delete;
  CameraSharedMemoryWriter &
  operator=(const CameraSharedMemoryWriter &) = delete;

  /// 创建 OS 级共享内存缓冲区
  bool create();

  /// 释放缓冲区
  void destroy();

  /// 是否已建立进程间共享内存
  bool isCreated() const { return created_; }

  /// 写入一帧图像数据到指定相机的环形缓冲
  /// @return 写入的帧序列号，0 表示失败
  uint64_t writeFrame(int camIdx, const CameraFrameMeta &meta, const char *data,
                      int dataSize);

  /// 获取槽位序列号数组指针（供 Reader 使用）
  std::atomic<uint64_t> *slotSeqArray(int camIdx);

  /// 获取缓冲区基地址（供 Reader 使用）
  const char *bufferBase(int camIdx) const;

  int slotSize() const { return kCameraShmSlotSize; }
  int slotCount() const { return kCameraShmSlotCount; }

private:
  bool created_ = false;

  QSharedMemory shm_;

  // 指向 shm_.data() 内的存储区域
  char *buffers_[kCameraCount] = {nullptr, nullptr};
  std::atomic<uint64_t> *slotSeqs_[kCameraCount] = {nullptr, nullptr};

  // 当前帧序列号（局部递增）
  uint64_t frameSeq_[kCameraCount] = {0, 0};
};

// ============================================================================
// CameraSharedMemoryReader — 读取端（UI 预览线程 / CameraDisplayThread
// 使用）
// ============================================================================

class CameraSharedMemoryReader {
public:
  CameraSharedMemoryReader() = default;
  ~CameraSharedMemoryReader();

  /// 附着到进程间的共享内存缓冲区
  bool attach();

  /// 断开
  void detach();

  /// 读取指定相机的最新帧
  /// @param camIdx     相机索引（0 或 1）
  /// @param lastSeq    上次已处理的帧序列号（输入/输出）
  /// @param meta       输出：帧元数据
  /// @param data       输出：帧数据
  /// @return           true = 有新帧，false = 无新帧
  bool readLatestFrame(int camIdx, uint64_t &lastSeq, CameraFrameMeta &meta,
                       QByteArray &data);

  bool isAttached() const { return attached_; }

private:
  bool attached_ = false;

  QSharedMemory shm_;

  // 指向 shm_.data() 内的存储区域
  const char *buffers_[kCameraCount] = {nullptr, nullptr};
  std::atomic<uint64_t> *slotSeqs_[kCameraCount] = {nullptr, nullptr};
};

} // namespace recordlab::common
