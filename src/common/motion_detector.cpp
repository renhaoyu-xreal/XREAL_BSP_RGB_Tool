/*
 * MotionDetector 实现
 */
#include "recordlab/common/motion_detector.h"
#include <algorithm>
#include <iostream>
#include <memory>

namespace recordlab::common {

using json = nlohmann::json;

// ============================================================================
// IncrementalStats
// ============================================================================

IncrementalStats::IncrementalStats(Extractor extractor)
    : extractor_(std::move(extractor)) {}

void IncrementalStats::push(const json &item) {
  // 使用在线算法增量更新均值和平方差，避免每次插入都重扫窗口。
  auto data = extractor_(item);
  windowData_.push_back(item);
  int n = static_cast<int>(windowData_.size());
  for (int i = 0; i < 3; ++i) {
    double delta = data[i] - mean_[i];
    mean_[i] += delta / n;
    double delta2 = data[i] - mean_[i];
    S_[i] += delta * delta2;
  }
}

void IncrementalStats::pop() {
  // 当窗口左侧样本过期时，反向修正均值和方差累计量。
  if (windowData_.empty())
    return;
  auto data = extractor_(windowData_.front());
  int oldSize = static_cast<int>(windowData_.size());
  windowData_.pop_front();
  if (windowData_.empty()) {
    mean_ = {0, 0, 0};
    S_ = {0, 0, 0};
    return;
  }
  for (int i = 0; i < 3; ++i) {
    double oldMean = mean_[i];
    mean_[i] = (mean_[i] * oldSize - data[i]) / (oldSize - 1);
    double deltaOld = data[i] - oldMean;
    double deltaNew = data[i] - mean_[i];
    S_[i] -= deltaOld * deltaNew;
  }
}

void IncrementalStats::clear() {
  // 清空窗口与累计统计量，恢复到初始状态。
  windowData_.clear();
  mean_ = {0, 0, 0};
  S_ = {0, 0, 0};
}

std::array<double, 3> IncrementalStats::variance() const {
  // 至少两个样本时方差才有意义，否则直接返回零向量。
  if (windowData_.size() < 2)
    return {0, 0, 0};
  int n = static_cast<int>(windowData_.size());
  return {S_[0] / (n - 1), S_[1] / (n - 1), S_[2] / (n - 1)};
}

std::array<double, 3> IncrementalStats::std() const {
  // 在方差基础上计算每个轴向的标准差，供运动状态分类复用。
  auto var = variance();
  return {std::sqrt(var[0]), std::sqrt(var[1]), std::sqrt(var[2])};
}

// ============================================================================
// MotionDetector
// ============================================================================

MotionDetector::MotionDetector(double timeWindow, double gyroSigma,
                               double accSigma, bool useGyro, bool useAcc)
    : timeWindowNs_(static_cast<int64_t>(timeWindow * 1e9)),
      gyroSigma_(gyroSigma), accSigma_(accSigma),
      timestampRollbackToleranceNs_(
          std::max(timeWindowNs_, int64_t(100'000'000))) {
  // 将 IMU 消息里的 `data` 字段统一映射为三轴数组，方便 gyro/acc 共用逻辑。
  auto imuExtractor = [](const json &msg) -> std::array<double, 3> {
    auto data = msg.value("data", std::vector<double>{0, 0, 0});
    return {data.size() > 0 ? data[0] : 0.0, data.size() > 1 ? data[1] : 0.0,
            data.size() > 2 ? data[2] : 0.0};
  };

  if (useGyro)
    gyroStats_ = std::make_unique<IncrementalStats>(imuExtractor);
  if (useAcc)
    accStats_ = std::make_unique<IncrementalStats>(imuExtractor);
}

void MotionDetector::addImuMessage(const json &imuData) {
  // 接收一帧 IMU 数据后更新对应滑动窗口，并清理超时样本。
  int64_t currentTs = imuData.value("timestamp_ns", int64_t(0));
  if (hasTimestampRollback(currentTs)) {
    clear();
  }
  if (currentTs > lastTimestampNs_)
    lastTimestampNs_ = currentTs;

  int imuType = imuData.value("type", 0);
  if (imuType == 1 && gyroStats_)
    gyroStats_->push(imuData);
  else if (imuType == 2 && accStats_)
    accStats_->push(imuData);

  removeOldData();
}

json MotionDetector::detect() const {
  // 根据当前窗口的标准差估算运动强度，并返回统一的状态消息。
  bool isEmpty = true;
  int64_t timeSpanNs = 0;

  if (gyroStats_ && !gyroStats_->empty()) {
    isEmpty = false;
    if (gyroStats_->size() >= 2) {
      int64_t front = gyroStats_->front().value("timestamp_ns", int64_t(0));
      int64_t back = gyroStats_->back().value("timestamp_ns", int64_t(0));
      timeSpanNs = std::max(timeSpanNs, back - front);
    }
  }
  if (accStats_ && !accStats_->empty()) {
    isEmpty = false;
    if (accStats_->size() >= 2) {
      int64_t front = accStats_->front().value("timestamp_ns", int64_t(0));
      int64_t back = accStats_->back().value("timestamp_ns", int64_t(0));
      timeSpanNs = std::max(timeSpanNs, back - front);
    }
  }

  if (isEmpty || timeSpanNs < (timeWindowNs_ / 2)) {
    return {{"name", "motion_status"},
            {"timestamp_ns", lastTimestampNs_},
            {"status", motionStateName(MOTION_NONE)}};
  }

  std::vector<int> states;
  if (gyroStats_ && !gyroStats_->empty()) {
    auto s = gyroStats_->std();
    double maxStd = *std::max_element(s.begin(), s.end());
    states.push_back(classifyState(maxStd, gyroSigma_));
  }
  if (accStats_ && !accStats_->empty()) {
    auto s = accStats_->std();
    double maxStd = *std::max_element(s.begin(), s.end());
    states.push_back(classifyState(maxStd, accSigma_));
  }

  int status = states.empty() ? MOTION_STATIC
                              : *std::max_element(states.begin(), states.end());
  return {{"name", "motion_status"},
          {"timestamp_ns", lastTimestampNs_},
          {"status", motionStateName(status)}};
}

int MotionDetector::classifyState(double stdValue, double sigma) {
  // 以 sigma 为基准做三级分类，兼顾静止检测与明显运动识别。
  if (stdValue <= 3.0 * sigma)
    return MOTION_STATIC;
  if (stdValue <= 10.0 * sigma)
    return MOTION_MOVING;
  return MOTION_ACTIVE;
}

json MotionDetector::getStatistics() const {
  // 导出窗口均值、标准差和样本规模，方便调参与问题定位。
  json result = {{"window_size", 0}, {"time_span", 0.0}};
  if (gyroStats_ && !gyroStats_->empty()) {
    auto m = gyroStats_->mean();
    auto s = gyroStats_->std();
    result["gyro_mean"] = {m[0], m[1], m[2]};
    result["gyro_std"] = {s[0], s[1], s[2]};
    result["window_size"] = gyroStats_->size();
  }
  if (accStats_ && !accStats_->empty()) {
    auto m = accStats_->mean();
    auto s = accStats_->std();
    result["acc_mean"] = {m[0], m[1], m[2]};
    result["acc_std"] = {s[0], s[1], s[2]};
  }
  return result;
}

void MotionDetector::clear() {
  // 清空全部统计器，并将时间戳基准恢复到未初始化状态。
  if (gyroStats_)
    gyroStats_->clear();
  if (accStats_)
    accStats_->clear();
  lastTimestampNs_ = 0;
}

bool MotionDetector::hasTimestampRollback(int64_t currentTimestamp) const {
  // 检测时间戳是否发生大幅回退，避免新旧录制数据混入同一统计窗口。
  if (currentTimestamp <= 0 || lastTimestampNs_ <= 0)
    return false;
  return currentTimestamp + timestampRollbackToleranceNs_ < lastTimestampNs_;
}

void MotionDetector::removeOldData() {
  // 维护固定时间窗，只保留距离最新样本不超过 timeWindow 的数据。
  int64_t timeoutNs = lastTimestampNs_ - timeWindowNs_;
  while (gyroStats_ && !gyroStats_->empty()) {
    if (gyroStats_->front().value("timestamp_ns", int64_t(0)) < timeoutNs)
      gyroStats_->pop();
    else
      break;
  }
  while (accStats_ && !accStats_->empty()) {
    if (accStats_->front().value("timestamp_ns", int64_t(0)) < timeoutNs)
      accStats_->pop();
    else
      break;
  }
}

} // namespace recordlab::common
