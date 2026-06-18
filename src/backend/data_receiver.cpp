/*
 * DataReceiver 实现
 *
 * 这一层要同时处理“原始接收频率”和“UI 展示频率”，两者不能混为一谈：
 *
 * 1. 原始接收频率
 *    每条 topic 消息一到，就立即进入 trackDataReception()。
 *    这里维护的是设备真实接收节奏，列表里显示的 Hz 应该来自这一层。
 * 2. UI 展示节流
 *    dataUpdated 信号会做轻量抽帧，避免 1000Hz IMU 直接把主线程打满。
 *    这层只影响 UI 开销，不该影响频率统计。
 * 3. 相机特殊通道
 *    图像本体走共享内存，DataReceiver 这里只转发 metadata，通知显示线程去读最新帧。
 */
#include "recordlab/backend/data_receiver.h"
#include "recordlab/common/topics.h"

#include <subscriber.h>

#include <QByteArray>
#include <QHash>

#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <set>
#include <thread>

namespace recordlab::backend {

using json = nlohmann::json;
using namespace recordlab::common;

static double nowSec() {
  // 使用单调时钟统计接收频率和 UI 抽帧节奏，避免系统时间变化干扰。
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ============================================================================
// DataReceiverProcess
// ============================================================================

DataReceiverProcess::DataReceiverProcess(const std::string &host,
                                         QObject *parent)
    : QThread(parent), host_(host) {}

// 析构时停止接收线程和订阅器，确保退出时没有残留的总线回调。
DataReceiverProcess::~DataReceiverProcess() { stopReceiver(); }

void DataReceiverProcess::stopReceiver() {
  // 请求线程退出并等待收尾，然后释放全部 topic 订阅。
  running_ = false;
  if (isRunning())
    wait(5000);
  subscribers_.clear();
}

void DataReceiverProcess::run() {
  // 后台线程启动后先订阅标准 topic，再进入轻量轮询等待阶段。
  running_ = true;

  // DataReceiver 和页面生命周期解耦，标准 topic 在后台始终保持订阅。
  subscribeTopic(TOPIC_CAMERA, PORT_CAMERA);
  subscribeTopic(TOPIC_IMU, PORT_IMU);
  subscribeTopic(TOPIC_MOTION_STATUS, PORT_MOTION_STATUS);
  subscribeTopic(TOPIC_RECORD_TIMER, PORT_RECORD_TIMER);
  subscribeTopic(TOPIC_TIME_DELAY, PORT_TIME_DELAY);

  std::cout << "[DataReceiver] Running, subscribed to " << subscribers_.size()
            << " topics" << std::endl;

  while (running_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  subscribers_.clear();
  std::cout << "[DataReceiver] Stopped" << std::endl;
}

void DataReceiverProcess::subscribeTopic(const std::string &topicName,
                                         int /*port*/) {
  // 为指定 topic 创建 echo 订阅器，消息到达后统一交给 onDataReceived 解析。
  try {
    auto sub = std::make_unique<echo::Subscriber>(
        topicName,
        [this, topicName](const std::string &raw) {
          try {
            // IMU 热路径优化：直接传原始字符串，避免在 subscriber recv
            // 线程里做 json::parse（该操作在 7000 msg/s 下成为瓶颈）。
            if (topicName == TOPIC_IMU) {
              onImuRawReceived(raw);
            } else {
              auto data = json::parse(raw);
              onDataReceived(topicName, data);
            }
          } catch (...) {
          }
        },
        true);
    subscribers_[topicName] = std::move(sub);
  } catch (const std::exception &e) {
    std::cerr << "[DataReceiver] Subscribe failed for " << topicName << ": "
              << e.what() << std::endl;
  }
}

void DataReceiverProcess::onImuRawReceived(const std::string &raw) {
  // IMU hot path: extract fields directly from raw JSON string,
  // skipping json::parse + deep copy to bring per-message cost from ~900us to ~50us.
  const double ts = nowSec();

  // Fast extract "type" field (integer)
  int imuType = 0;
  {
    static const char kTypeKey[] = "\"type\":";
    const auto pos = raw.find(kTypeKey);
    if (pos != std::string::npos) {
      const char *p = raw.data() + pos + sizeof(kTypeKey) - 1;
      while (*p == ' ') ++p;
      bool neg = false;
      if (*p == '-') { neg = true; ++p; }
      while (*p >= '0' && *p <= '9') {
        imuType = imuType * 10 + (*p - '0');
        ++p;
      }
      if (neg) imuType = -imuType;
    }
  }

  const std::string dataName = imuTypeToName(imuType);
  if (dataName.empty())
    return;

  // Fast extract "timestamp_ns" field
  int64_t tsNs = 0;
  {
    static const char kTsKey[] = "\"timestamp_ns\":";
    const auto pos = raw.find(kTsKey);
    if (pos != std::string::npos) {
      const char *p = raw.data() + pos + sizeof(kTsKey) - 1;
      while (*p == ' ') ++p;
      while (*p >= '0' && *p <= '9') {
        tsNs = tsNs * 10 + (*p - '0');
        ++p;
      }
    }
  }
  const double dataTs = tsNs > 0 ? tsNs / 1e9 : ts;

  // Fast extract "data":[...] array (up to 6 doubles)
  double vals[6] = {0, 0, 0, 0, 0, 0};
  {
    static const char kDataKey[] = "\"data\":[";
    const auto pos = raw.find(kDataKey);
    if (pos != std::string::npos) {
      const char *p = raw.data() + pos + sizeof(kDataKey) - 1;
      for (int i = 0; i < 6; ++i) {
        while (*p == ' ' || *p == ',') ++p;
        if (*p == ']' || *p == '\0') break;
        char *end = nullptr;
        vals[i] = std::strtod(p, &end);
        p = end;
      }
    }
  }

  // Construct lightweight UI value
  json value;
  if (imuType == 12 || imuType == 13) { // temperature
    value = {{"value", vals[0]}};
  } else {
    value = {{"x", vals[0]}, {"y", vals[1]}, {"z", vals[2]}};
  }

  trackDataReception(dataName, dataTs);
  const double freq = getFrequency(dataName);

  double &lastSend = lastSendTime_[dataName];
  if (dataTs - lastSend >= uiSendIntervalForData(dataName)) {
    lastSend = dataTs;
    enqueuePendingEmit(
        {QString::fromStdString(dataName), value, dataTs, freq});
  }
}

void DataReceiverProcess::onDataReceived(const std::string &topicName,
                                         const json &data) {
  // 为每条原始消息记录到达时间，并更新“最新值缓存 + 待发射队列”。
  double ts = nowSec();

  // 每个 topic 保留一份最近收到的原始消息，供页面同步“最新值”使用。
  {
    QMutexLocker locker(&cacheLock_);
    dataCache_[topicName] = {{"data", data}, {"timestamp", ts}};
  }

  convertAndEmit(topicName, data, ts);
}

void DataReceiverProcess::convertAndEmit(const std::string &topicName,
                                         const json &data, double timestamp) {
  // 按 topic 类型把原始消息转为 UI 友好的统一结构，并执行频率统计与抽帧。
  // IMU data
  if (topicName == TOPIC_IMU) {
    int imuType = data.value("type", 0);
    std::string dataName = imuTypeToName(imuType);
    if (dataName.empty())
      return;

    int64_t tsNs = data.value("timestamp_ns", int64_t(0));
    double dataTs = tsNs > 0 ? tsNs / 1e9 : timestamp;

    auto values = data.value("data", std::vector<double>{});
    json value;
    if (imuType == 12 || imuType == 13) { // temperature
      value = {{"value", values.size() > 0 ? values[0] : 0.0}};
    } else {
      value = {{"x", values.size() > 0 ? values[0] : 0.0},
               {"y", values.size() > 1 ? values[1] : 0.0},
               {"z", values.size() > 2 ? values[2] : 0.0}};
    }

    trackDataReception(dataName, dataTs);
    double freq = getFrequency(dataName);

    // 频率已经基于原始消息计算完毕；这里再做 UI 抽帧，只是为了减轻主线程负担。
    double &lastSend = lastSendTime_[dataName];
    if (dataTs - lastSend >= uiSendIntervalForData(dataName)) {
      lastSend = dataTs;
      // 不再 emit 信号（避免跨线程 QueuedConnection 积压），
      // 改为存入 pendingEmits_ 缓冲，由 DataReceiverManager 定时 poll。
      enqueuePendingEmit(
          {QString::fromStdString(dataName), value, dataTs, freq});
    }
  }
  // Record timer
  else if (topicName == TOPIC_RECORD_TIMER) {
    trackDataReception(topicName, timestamp);
    const double freq = getFrequency(topicName);
    double durationNs = data.value("duration_ns", 0.0);
    double elapsed = durationNs / 1e9;
    enqueuePendingEmit({QString::fromStdString(topicName), {{"value", elapsed}},
                       timestamp, freq});
  }
  // Time delay
  else if (topicName == TOPIC_TIME_DELAY) {
    trackDataReception(topicName, timestamp);
    const double freq = getFrequency(topicName);
    double delayNs = data.value("time_delay_ns", 0.0);
    double delayMs = delayNs / 1e6;
    enqueuePendingEmit({QString::fromStdString(topicName), {{"value", delayMs}},
                       timestamp, freq});
  }
  // Motion status
  else if (topicName == TOPIC_MOTION_STATUS) {
    trackDataReception(topicName, timestamp);
    const double freq = getFrequency(topicName);
    enqueuePendingEmit({QString::fromStdString(topicName),
                        {{"value", data.value("status", std::string())}},
                        timestamp, freq});
  }
  // Camera — 图像本体不再走 JSON/base64，而是由底层直接写共享内存。
  // 这里仅发送轻量 metadata，让显示线程知道有新帧和对应参数。
  else if (topicName == TOPIC_CAMERA) {
    trackDataReception(topicName, timestamp);

    // 仅处理元数据（无 Base64 处理，因为画面由 NativeGlassesAdapter 直接写入
    // QSharedMemory）
    handleCameraDataLight(data);
  }
  // Custom data
  else if (registeredCustomData_.count(topicName)) {
    auto &info = registeredCustomData_[topicName];
    std::string dataType = info.value("type", std::string("double"));
    json value;
    if (dataType == "vector") {
      value = {{"x", data.value("data0", 0.0)},
               {"y", data.value("data1", 0.0)},
               {"z", data.value("data2", 0.0)}};
    } else {
      value = {{"value", data.value("data0", 0.0)}};
    }
    trackDataReception(topicName, timestamp);
    double freq = getFrequency(topicName);
    enqueuePendingEmit(
        {QString::fromStdString(topicName), value, timestamp, freq});
  }
}

void DataReceiverProcess::enqueuePendingEmit(PendingEmit item) {
  // 把待发往主线程的数据放入有界队列，队列过长时丢弃最旧元素保实时性。
  QMutexLocker locker(&pendingLock_);
  pendingEmits_.push_back(std::move(item));
  while (pendingEmits_.size() > maxPendingEmitCount_) {
    pendingEmits_.pop_front();
    const double now = nowSec();
    if (now - lastPendingOverflowLogSec_ >= 2.0) {
      lastPendingOverflowLogSec_ = now;
      std::cerr << "[DataReceiver] pending queue overflow, dropping oldest items"
                << std::endl;
    }
  }
}

double DataReceiverProcess::uiSendIntervalForData(
    const std::string &dataName) const {
  // 为不同数据类型返回各自的 UI 抽帧间隔，避免高频流压垮界面线程。
  if (dataName.rfind("IMU", 0) == 0) {
    return imuSendInterval_;
  }
  if (dataName.rfind("Android-", 0) == 0) {
    return imuSendInterval_;
  }
  if (dataName == TOPIC_CAMERA) {
    return cameraSendInterval_;
  }
  if (registeredCustomData_.count(dataName) > 0) {
    return customSendInterval_;
  }
  return defaultSendInterval_;
}

void DataReceiverProcess::trackDataReception(const std::string &dataName,
                                             double timestamp) {
  // 基于滑动时间窗统计接收频率，并处理时间戳回退导致的窗口污染。
  QMutexLocker locker(&freqLock_);
  auto &times = receptionTimes_[dataName];

  // 时间戳大幅回退通常意味着设备重启、数据流重连或旧缓存混入。
  // 这里直接清窗口，避免旧时间轴把频率和曲线一起带歪。
  if (!times.empty() && timestamp < times.back() - 1.0) {
    times.clear();
    frequencies_[dataName] = 0.0;
  }

  if (!times.empty() && std::abs(timestamp - times.back()) < 1e-6) {
    return;
  }

  times.push_back(timestamp);
  double cutoff = timestamp - frequencyWindow_;
  while (!times.empty() && times.front() < cutoff)
    times.pop_front();

  if (times.size() >= 3) {
    int n = std::min(static_cast<int>(times.size()), 50);
    double span = times.back() - times[times.size() - n];
    if (span > 0.001)
      frequencies_[dataName] = (n - 1) / span;
  }

  if (times.size() > 5000) {
    std::deque<double> trimmed(times.end() - 2000, times.end());
    times.swap(trimmed);
  }
}

/*
 * handleCameraDataLight
 *
 * 这里只做“共享内存帧的索引整理”，不搬运图像本体。UI 端真正需要的是：
 * - 时间戳
 * - 每个相机的分辨率 / bytes_per_line
 * - gain / exposure / rolling shutter 等调试信息
 * - shm_seq，确认共享内存里确实出现了更新
 */
void DataReceiverProcess::handleCameraDataLight(const json &data) {
  // 从相机消息中提炼 UI 真正关心的 metadata，而不搬运图像本体。
  if (!data.contains("cam_data") || !data["cam_data"].is_object())
    return;

  double timestamp = 0.0;
  if (data.contains("timestamp")) {
    const double rawTimestamp = data.value("timestamp", 0.0);
    timestamp = rawTimestamp > 1e12 ? rawTimestamp / 1e9 : rawTimestamp;
  }
  if (timestamp <= 0.0 && data.contains("timestamp_ns")) {
    timestamp = data.value("timestamp_ns", 0.0) / 1e9;
  }
  if (timestamp <= 0.0) {
    timestamp = nowSec();
  }

  json camMeta = json::object();
  try {
    for (auto it = data["cam_data"].begin(); it != data["cam_data"].end();
         ++it) {
      if (!it.value().is_object()) {
        continue;
      }

      json meta = {
          {"gain", it.value().value("gain", 0)},
          {"exposure_duration", it.value().value("exposure_duration", 0LL)},
          {"rolling_shutter_time",
           it.value().value("rolling_shutter_time", 0LL)},
      };

      if (it.value().contains("image_raw") && it.value()["image_raw"].is_object()) {
        const auto &imageRaw = it.value()["image_raw"];
        meta["width"] = imageRaw.value("width", 0);
        meta["height"] = imageRaw.value("height", 0);
        meta["bytes_per_line"] = imageRaw.value("bytes_per_line", 0);
        meta["shm_seq"] = imageRaw.value("shm_seq", 0ULL);
      }

      camMeta[it.key()] = meta;
    }
  } catch (...) {
  }

  const double freq = getFrequency(TOPIC_CAMERA);
  enqueuePendingEmit({QString::fromStdString(TOPIC_CAMERA),
                      {{"timestamp", timestamp},
                       {"shm", true},
                       {"cam_meta", camMeta}},
                      timestamp, freq});
}

void DataReceiverProcess::registerCustomData(const std::string &dataName,
                                             const std::string &dataType,
                                             int port) {
  // 记录自定义数据类型并立即建立订阅，让 UI 能动态接入新增 topic。
  registeredCustomData_[dataName] = {{"type", dataType}, {"port", port}};
  subscribeTopic(dataName, port);
}

json DataReceiverProcess::getData(const std::string &topicName) const {
  // 返回某个 topic 最近收到的一条原始缓存数据。
  QMutexLocker locker(&cacheLock_);
  auto it = dataCache_.find(topicName);
  return it != dataCache_.end() ? it->second : json{};
}

double DataReceiverProcess::getFrequency(const std::string &dataName) const {
  // 读取最近统计出的接收频率，供 UI 频率指示器展示。
  QMutexLocker locker(&freqLock_);
  auto it = frequencies_.find(dataName);
  return it != frequencies_.end() ? it->second : 0.0;
}

std::size_t DataReceiverProcess::pendingDataCount() const {
  // 返回待发往主线程的数据条数，供主线程动态调整每轮拉取批量。
  QMutexLocker locker(&pendingLock_);
  return pendingEmits_.size();
}

std::deque<PendingEmit> DataReceiverProcess::takePendingData(
    std::size_t maxCount) {
  // 批量取出待处理数据，支持限制单轮拉取数量以控制主线程开销。
  QMutexLocker locker(&pendingLock_);
  std::deque<PendingEmit> result;
  if (maxCount == 0 || pendingEmits_.size() <= maxCount) {
    result.swap(pendingEmits_);
    return result;
  }

  const auto splitIt =
      pendingEmits_.begin() + static_cast<std::ptrdiff_t>(maxCount);
  result.insert(result.end(), pendingEmits_.begin(), splitIt);
  pendingEmits_.erase(pendingEmits_.begin(), splitIt);
  return result;
}

// ============================================================================
// DataReceiverManager
// ============================================================================

DataReceiverManager::DataReceiverManager(const std::string &host,
                                         QObject *parent)
    : QObject(parent), host_(host) {}

// 析构时停止轮询器和后台接收线程，确保管理器销毁后不再发信号。
DataReceiverManager::~DataReceiverManager() { stop(); }

void DataReceiverManager::start() {
  // 在主线程中创建轮询定时器，并启动后台接收线程。
  if (receiver_)
    return;

  receiver_ = std::make_unique<DataReceiverProcess>(host_);
  // 不再用 signal/slot 跨线程（QueuedConnection 会积压），
  // 改用定时器主线程轮询。100ms 对高频 IMU 曲线会肉眼发涩，
  // 这里提到约 30Hz，让显示链路尽快消费已收到的数据。
  pollTimer_ = new QTimer(this);
  pollTimer_->setInterval(33);
  connect(pollTimer_, &QTimer::timeout, this,
          &DataReceiverManager::pollReceiverData);
  pollTimer_->start();
  receiver_->start();
}

void DataReceiverManager::stop() {
  // 先停轮询器，再停后台接收线程，避免在销毁过程中仍访问 receiver_。
  if (pollTimer_) {
    pollTimer_->stop();
    pollTimer_->deleteLater();
    pollTimer_ = nullptr;
  }
  if (!receiver_)
    return;
  receiver_->stopReceiver();
  receiver_.reset();
}

bool DataReceiverManager::isRunning() const {
  // 反映后台接收线程是否仍处于运行状态。
  return receiver_ && receiver_->isRunning();
}

void DataReceiverManager::pollReceiverData() {
  // 模拟 Python 版 queue 轮询逻辑：为曲线保留样本，为列表只保留最新值。
  if (!receiver_)
    return;

  // 对齐原版 Python update_cache_from_queue：
  // 1. 当前被曲线订阅的数据保留本轮所有抽样点，避免图像/状态类 coalesce
  //    把曲线压成“断断续续”的回放；
  // 2. 但 UI 的 dataUpdated 只发每路最新值，避免列表/文本框跟着每个点重刷；
  // 3. 相机仍只保留本轮最后一帧，优先保证实时性。
  const std::size_t pendingCount = receiver_->pendingDataCount();
  std::size_t maxBatchSize = 400;
  if (pendingCount > 160) {
    maxBatchSize = 2500;
  } else if (pendingCount > 60) {
    maxBatchSize = 1000;
  }

  auto pending = receiver_->takePendingData(maxBatchSize);
  std::set<std::string> subscribedNames;
  {
    QMutexLocker locker(&bufferLock_);
    subscribedNames = subscribedNames_;
  }

  QHash<QString, PendingEmit> emitItemsByName;
  QHash<QString, PendingEmit> latestItemsByName;
  std::optional<PendingEmit> latestCameraItem;

  for (auto &item : pending) {
    if (item.dataName == QStringLiteral("camera_data")) {
      latestCameraItem = item;
      continue;
    }
    if (subscribedNames.count(item.dataName.toStdString()) > 0) {
      applyDataUpdate(item.dataName, item.value, item.timestamp, item.frequency);
      emitItemsByName.insert(item.dataName, item);
      continue;
    }
    latestItemsByName.insert(item.dataName, item);
  }

  for (auto it = latestItemsByName.cbegin(); it != latestItemsByName.cend();
       ++it) {
    const auto &item = it.value();
    applyDataUpdate(item.dataName, item.value, item.timestamp, item.frequency);
    emitItemsByName.insert(item.dataName, item);
  }

  for (auto it = emitItemsByName.cbegin(); it != emitItemsByName.cend(); ++it) {
    const auto &item = it.value();
    emit dataUpdated(item.dataName, item.value, item.timestamp, item.frequency);
  }

  if (latestCameraItem.has_value()) {
    const auto &item = latestCameraItem.value();
    applyDataUpdate(item.dataName, item.value, item.timestamp, item.frequency);
    emit dataUpdated(item.dataName, item.value, item.timestamp, item.frequency);
  }
}

void DataReceiverManager::applyDataUpdate(const QString &dataName,
                                          const json &value, double timestamp,
                                          double frequency) {
  // 将新数据同步进 latestData_ 和可选的曲线缓冲区，并处理时间回退与降采样。
  std::string name = dataName.toStdString();
  double previousTimestamp = 0.0;
  bool hasPreviousTimestamp = false;

  // Update latest
  {
    QMutexLocker locker(&dataLock_);
    auto it = latestData_.find(name);
    if (it != latestData_.end() && it->second.is_object()) {
      previousTimestamp = it->second.value("timestamp", 0.0);
      hasPreviousTimestamp = previousTimestamp > 0.0;
    }
    latestData_[name] = {{"value", value}, {"timestamp", timestamp}};
    latestFrequencies_[name] = frequency;
  }

  // Update display buffer if subscribed
  {
    QMutexLocker locker(&bufferLock_);
    if (subscribedNames_.count(name)) {
      auto &buf = displayBuffer_[name];
      if (hasPreviousTimestamp && timestamp < previousTimestamp - 1.0) {
        buf.clear();
        lastBufferTimestamp_.erase(name);
      }
      bool shouldAppend = true;
      auto lastBufferIt = lastBufferTimestamp_.find(name);
      if (lastBufferIt != lastBufferTimestamp_.end()) {
        if (timestamp < lastBufferIt->second - 1.0) {
          buf.clear();
          lastBufferTimestamp_.erase(lastBufferIt);
        } else if ((timestamp - lastBufferIt->second) < downsampleInterval_) {
          shouldAppend = false;
          if (!buf.empty()) {
            buf.back() = {{"value", value}, {"timestamp", timestamp}};
          }
        }
      }

      if (shouldAppend) {
        buf.push_back({{"value", value}, {"timestamp", timestamp}});
        lastBufferTimestamp_[name] = timestamp;
      }
      while (static_cast<int>(buf.size()) > maxBufferSize_)
        buf.pop_front();
    }
  }

}

json DataReceiverManager::getLatestData(const std::string &dataName) const {
  // 返回主线程缓存中的最新值快照。
  QMutexLocker locker(&dataLock_);
  auto it = latestData_.find(dataName);
  return it != latestData_.end() ? it->second : json{};
}

double DataReceiverManager::getFrequency(const std::string &dataName) const {
  // 返回主线程侧最近记录的接收频率。
  QMutexLocker locker(&dataLock_);
  auto it = latestFrequencies_.find(dataName);
  return it != latestFrequencies_.end() ? it->second : 0.0;
}

void DataReceiverManager::registerCustomData(const std::string &dataName,
                                             const std::string &dataType,
                                             int port) {
  // 将自定义数据注册转发给后台线程，并同步向外发出 dataRegistered 提示。
  if (receiver_)
    receiver_->registerCustomData(dataName, dataType, port);
  emit dataRegistered(QString::fromStdString(dataName),
                      QString::fromStdString(dataType), port);
}

void DataReceiverManager::subscribeData(const std::string &dataName) {
  // 为指定数据名开启曲线缓存，后续 poll 时会保留该数据的全部抽样点。
  QMutexLocker locker(&bufferLock_);
  subscribedNames_.insert(dataName);
  if (!displayBuffer_.count(dataName)) {
    displayBuffer_[dataName] = {};
  }
  lastBufferTimestamp_.erase(dataName);
}

void DataReceiverManager::unsubscribeData(const std::string &dataName) {
  // 停止保留某一路数据的曲线样本，并释放对应缓冲区。
  QMutexLocker locker(&bufferLock_);
  subscribedNames_.erase(dataName);
  displayBuffer_.erase(dataName);
  lastBufferTimestamp_.erase(dataName);
}

std::vector<json>
DataReceiverManager::getDisplayBuffer(const std::string &dataName) const {
  // 取出当前曲线缓冲并立即清空，匹配“消费式读取”语义。
  QMutexLocker locker(&bufferLock_);
  auto it = displayBuffer_.find(dataName);
  if (it == displayBuffer_.end())
    return {};
  std::vector<json> result{it->second.begin(), it->second.end()};
  it->second.clear();
  return result;
}

std::vector<std::string> DataReceiverManager::getDataNameList() const {
  // 返回当前预定义的数据名称列表，供下拉框和默认订阅使用。
  return {"IMU0-gyro",
          "IMU0-acc",
          "IMU0-mag",
          "IMU0-temperature",
          "IMU1-gyro",
          "IMU1-acc",
          "IMU1-temperature",
          "Android-gyro",
          "Android-acc",
          "Android-mag",
          "Android-temperature"};
}

} // namespace recordlab::backend
