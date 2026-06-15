/*
 * CameraSharedMemory 实现
 *
 *   - DataReceiverManager._setup_camera_shm       → Writer::create
 *   - DataReceiverProcess._write_camera_to_shm    → Writer::writeFrame
 *   - CameraDisplayThread._attach_shared_memory   → Reader::attach
 *   - CameraDisplayThread._process_camera          → Reader::readLatestFrame
 */

#include "recordlab/common/camera_shared_memory.h"

#include <cstring>
#include <iostream>

namespace recordlab::common {

// ============================================================================
// CameraFrameMeta — 元数据序列化（与 Python 版 little-endian 布局一致）
// ============================================================================

// 以 little-endian 方式写入 32 位字段，保持与 Python 端共享内存布局一致。
static void writeU32(char *buf, uint32_t val) { std::memcpy(buf, &val, 4); }
// 从固定偏移读取 32 位字段，供元数据反序列化复用。
static uint32_t readU32(const char *buf) {
  uint32_t val = 0;
  std::memcpy(&val, buf, 4);
  return val;
}

void CameraFrameMeta::writeTo(char *buf) const {
  // 将元数据写成定长 32 字节头部，方便跨语言稳定解析。
  std::memset(buf, 0, kShmMetaSize);
  writeU32(buf + 0, static_cast<uint32_t>(width));
  writeU32(buf + 4, static_cast<uint32_t>(height));
  writeU32(buf + 8, static_cast<uint32_t>(format));
  writeU32(buf + 12, static_cast<uint32_t>(dataSize));
  writeU32(buf + 16, static_cast<uint32_t>(bytesPerLine));
  writeU32(buf + 20, static_cast<uint32_t>(encodedFormat));
  // [24:32] reserved = 0
}

CameraFrameMeta CameraFrameMeta::readFrom(const char *buf) {
  // 从共享内存中的定长头部恢复出结构化帧元数据。
  CameraFrameMeta m;
  m.width = static_cast<int>(readU32(buf + 0));
  m.height = static_cast<int>(readU32(buf + 4));
  m.format = static_cast<int>(readU32(buf + 8));
  m.dataSize = static_cast<int>(readU32(buf + 12));
  m.bytesPerLine = static_cast<int>(readU32(buf + 16));
  m.encodedFormat = static_cast<int>(readU32(buf + 20));
  return m;
}

// ============================================================================
// CameraSharedMemoryWriter
// ============================================================================

// Memory Layout in QSharedMemory
// [0..sizeOf atomic array]: uint64_t
// slotSeqs_[kCameraCount][kCameraShmSlotCount] [sizeOf atomic array .. EOF]:
// buffers_[kCameraCount][kCameraShmSlotCount][kCameraShmSlotSize]

static constexpr size_t kSlotSeqsSize =
    kCameraCount * kCameraShmSlotCount * sizeof(std::atomic<uint64_t>);
static constexpr size_t kBuffersSize =
    kCameraCount * kCameraShmSlotCount * kCameraShmSlotSize;
static constexpr size_t kTotalShmSize = kSlotSeqsSize + kBuffersSize;

CameraSharedMemoryWriter::CameraSharedMemoryWriter() {
  // 使用固定共享内存 key，确保写端与所有读端都能定位到同一段内存。
  shm_.setKey(QString::fromUtf8(kCameraShmKey));
}

// 析构时断开共享内存映射，避免后续对象复用脏状态。
CameraSharedMemoryWriter::~CameraSharedMemoryWriter() { destroy(); }

bool CameraSharedMemoryWriter::create() {
  // 创建或接管共享内存，并解析出序列号区和图像缓冲区的地址布局。
  if (created_)
    return true;

  try {
    if (shm_.isAttached()) {
      shm_.detach();
    }

    // Create new or attach to existing
    if (!shm_.create(kTotalShmSize)) {
      if (shm_.error() == QSharedMemory::AlreadyExists) {
        if (!shm_.attach()) {
          std::cerr << "[CameraSHM] Failed to attach to existing SHM: "
                    << shm_.errorString().toStdString() << std::endl;
          return false;
        }
      } else {
        std::cerr << "[CameraSHM] Failed to create SHM: "
                  << shm_.errorString().toStdString() << std::endl;
        return false;
      }
    }

    shm_.lock();
    char *basePtr = static_cast<char *>(shm_.data());

    // Initialize pointers
    auto *seqsPtr = reinterpret_cast<std::atomic<uint64_t> *>(basePtr);
    char *bufsPtr = basePtr + kSlotSeqsSize;

    for (int cam = 0; cam < kCameraCount; ++cam) {
      slotSeqs_[cam] = seqsPtr + (cam * kCameraShmSlotCount);
      buffers_[cam] =
          bufsPtr + (cam * kCameraShmSlotCount * kCameraShmSlotSize);

      // Initialize atomic counters to 0 if we created it (or just reset them
      // anyway)
      for (int s = 0; s < kCameraShmSlotCount; ++s) {
        slotSeqs_[cam][s].store(0, std::memory_order_relaxed);
      }
      frameSeq_[cam] = 0;
    }
    // clear buffers
    std::memset(bufsPtr, 0, kBuffersSize);
    shm_.unlock();

    created_ = true;
    std::cout << "[CameraSHM] Created/Attached Writer: " << kCameraCount
              << " cameras × " << kCameraShmSlotCount << " slots × "
              << (kCameraShmSlotSize / 1024) << "KB" << std::endl;
    return true;
  } catch (const std::exception &e) {
    std::cerr << "[CameraSHM] Failed to create Writer: " << e.what()
              << std::endl;
    destroy();
    return false;
  }
}

void CameraSharedMemoryWriter::destroy() {
  // 释放写端映射并清理本地缓存指针与帧序列号。
  for (int cam = 0; cam < kCameraCount; ++cam) {
    buffers_[cam] = nullptr;
    slotSeqs_[cam] = nullptr;
    frameSeq_[cam] = 0;
  }
  if (shm_.isAttached()) {
    shm_.detach();
  }
  created_ = false;
}

uint64_t CameraSharedMemoryWriter::writeFrame(int camIdx,
                                              const CameraFrameMeta &meta,
                                              const char *data, int dataSize) {
  // 将一帧图像写入环形槽位，再通过原子序列号把“最新帧”发布给读端。
  if (!created_ || camIdx < 0 || camIdx >= kCameraCount)
    return 0;
  if (!data || dataSize <= 0)
    return 0;

  // 检查槽位容量: 元数据 + 图像数据
  const int totalSize = kShmMetaSize + dataSize;
  if (totalSize > kCameraShmSlotSize) {
    // 每 100 帧打印一次告警
    if (frameSeq_[camIdx] % 100 == 0) {
      std::cerr << "[CameraSHM] Frame too large: cam=" << camIdx
                << " size=" << totalSize << " slot=" << kCameraShmSlotSize
                << std::endl;
    }
    return 0;
  }

  // 选择槽位（环形轮转，与 Python 版逻辑一致）
  const uint64_t newSeq = frameSeq_[camIdx] + 1;
  const int slotIdx = static_cast<int>(newSeq % kCameraShmSlotCount);
  const int slotOffset = slotIdx * kCameraShmSlotSize;

  char *slotBase = buffers_[camIdx] + slotOffset;

  // 写入元数据 (32 bytes)
  CameraFrameMeta writeMeta = meta;
  writeMeta.dataSize = dataSize;
  writeMeta.writeTo(slotBase);

  // 写入图像数据
  std::memcpy(slotBase + kShmMetaSize, data, dataSize);

  // 更新序列号（release 语义，确保数据写入对 reader 可见）
  slotSeqs_[camIdx][slotIdx].store(newSeq, std::memory_order_release);
  frameSeq_[camIdx] = newSeq;

  // 每 100 帧打印诊断
  if (newSeq % 100 == 1) {
    std::cout << "[CameraSHM] Wrote cam" << camIdx << " frame " << newSeq
              << ": " << meta.width << "x" << meta.height << ", " << dataSize
              << " bytes" << std::endl;
  }

  return newSeq;
}

std::atomic<uint64_t> *CameraSharedMemoryWriter::slotSeqArray(int camIdx) {
  // 返回指定相机的槽位序列号数组，便于测试或低层诊断查看写入进度。
  if (camIdx < 0 || camIdx >= kCameraCount || !slotSeqs_[camIdx])
    return nullptr;
  return slotSeqs_[camIdx];
}

const char *CameraSharedMemoryWriter::bufferBase(int camIdx) const {
  // 返回指定相机原始缓冲区首地址，供零拷贝或诊断代码直接访问。
  if (camIdx < 0 || camIdx >= kCameraCount || !buffers_[camIdx])
    return nullptr;
  return buffers_[camIdx];
}

// ============================================================================
// CameraSharedMemoryReader
// ============================================================================

// 析构时主动解除映射，防止后续读取命中无效共享内存地址。
CameraSharedMemoryReader::~CameraSharedMemoryReader() { detach(); }

bool CameraSharedMemoryReader::attach() {
  // 连接到已有共享内存，并解析出每路相机的元数据区和图像区。
  if (attached_)
    return true;

  shm_.setKey(QString::fromUtf8(kCameraShmKey));

  if (!shm_.attach()) {
    // It's possible the writer hasn't created it yet.
    return false;
  }

  shm_.lock();
  const char *basePtr = static_cast<const char *>(shm_.constData());
  const auto *seqsPtr =
      reinterpret_cast<const std::atomic<uint64_t> *>(basePtr);
  const char *bufsPtr = basePtr + kSlotSeqsSize;

  for (int cam = 0; cam < kCameraCount; ++cam) {
    // Need const_cast for atomic because volatile/const atomic operations
    slotSeqs_[cam] = const_cast<std::atomic<uint64_t> *>(
        seqsPtr + (cam * kCameraShmSlotCount));
    buffers_[cam] = bufsPtr + (cam * kCameraShmSlotCount * kCameraShmSlotSize);
  }
  shm_.unlock();

  attached_ = true;
  std::cout << "[CameraSHM] Reader attached to SHM" << std::endl;
  return true;
}

void CameraSharedMemoryReader::detach() {
  // 解除共享内存映射并清空缓存地址，恢复到未附着状态。
  for (int cam = 0; cam < kCameraCount; ++cam) {
    buffers_[cam] = nullptr;
    slotSeqs_[cam] = nullptr;
  }
  if (shm_.isAttached()) {
    shm_.detach();
  }
  attached_ = false;
  std::cout << "[CameraSHM] Reader detached from SHM" << std::endl;
}

bool CameraSharedMemoryReader::readLatestFrame(int camIdx, uint64_t &lastSeq,
                                               CameraFrameMeta &meta,
                                               QByteArray &data) {
  // 找到比 lastSeq 更新的最新槽位，读取后再次校验避免读到被覆盖的半帧。
  if (!attached_) {
    if (!attach())
      return false;
  }

  if (camIdx < 0 || camIdx >= kCameraCount)
    return false;

  const auto *seqArray = slotSeqs_[camIdx];
  if (!seqArray)
    return false;

  // 查找最新的有效帧（与 Python CameraDisplayThread._process_camera 一致）
  int latestSlotIdx = -1;
  uint64_t latestFrameSeq = lastSeq;

  for (int s = 0; s < kCameraShmSlotCount; ++s) {
    const uint64_t seq = seqArray[s].load(std::memory_order_acquire);
    if (seq > latestFrameSeq) {
      latestFrameSeq = seq;
      latestSlotIdx = s;
    }
  }

  // 没有新帧
  if (latestSlotIdx < 0)
    return false;

  // 读取元数据和数据
  const char *buf = buffers_[camIdx];
  const int slotOffset = latestSlotIdx * kCameraShmSlotSize;
  const char *slotBase = buf + slotOffset;

  meta = CameraFrameMeta::readFrom(slotBase);

  if (meta.width <= 0 || meta.height <= 0 || meta.dataSize <= 0)
    return false;
  if (kShmMetaSize + meta.dataSize > kCameraShmSlotSize)
    return false;

  data = QByteArray(slotBase + kShmMetaSize, meta.dataSize);

  // 验证帧序号仍然有效（避免读取过程中被覆盖）
  const uint64_t verifySeq =
      seqArray[latestSlotIdx].load(std::memory_order_acquire);
  if (verifySeq != latestFrameSeq) {
    // 帧已被覆盖，丢弃
    return false;
  }

  lastSeq = latestFrameSeq;
  return true;
}

} // namespace recordlab::common
