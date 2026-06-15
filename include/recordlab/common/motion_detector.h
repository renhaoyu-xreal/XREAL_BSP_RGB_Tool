/*
 * MotionDetector - 运动检测模块
 *
 * 基于 Welford 增量统计算法实现滑动窗口的均值/方差计算，
 * 用于 IMU 数据的运动状态检测。
 *
 * 包含：
 * - IncrementalStats: 增量统计类（Welford 算法, O(1) 复杂度）
 * - MotionDetector: 运动检测器（基于时间窗口）
 *
 */
#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <string>

namespace recordlab::common {

// 运动状态常量
constexpr int MOTION_NONE = 0;   // 数据不足
constexpr int MOTION_STATIC = 1; // 静止
constexpr int MOTION_MOVING = 2; // 运动
constexpr int MOTION_ACTIVE = 3; // 活跃

inline const char *motionStateName(int state) {
  switch (state) {
  case MOTION_NONE:
    return "none";
  case MOTION_STATIC:
    return "static";
  case MOTION_MOVING:
    return "moving";
  case MOTION_ACTIVE:
    return "active";
  default:
    return "unknown";
  }
}

// ============================================================================
// IncrementalStats — Welford 增量统计 (3D 向量)
// ============================================================================

class IncrementalStats {
public:
  using Extractor =
      std::function<std::array<double, 3>(const nlohmann::json &)>;

  explicit IncrementalStats(Extractor extractor);

  void push(const nlohmann::json &item);
  void pop();
  void clear();

  const nlohmann::json &front() const { return windowData_.front(); }
  const nlohmann::json &back() const { return windowData_.back(); }
  int size() const { return static_cast<int>(windowData_.size()); }
  bool empty() const { return windowData_.empty(); }

  std::array<double, 3> mean() const { return mean_; }
  std::array<double, 3> variance() const;
  std::array<double, 3> std() const;

private:
  Extractor extractor_;
  std::deque<nlohmann::json> windowData_;
  std::array<double, 3> mean_ = {0, 0, 0};
  std::array<double, 3> S_ = {0, 0, 0};
};

// ============================================================================
// MotionDetector — 运动检测器
// ============================================================================

class MotionDetector {
public:
  explicit MotionDetector(double timeWindow = 2.0, double gyroSigma = 0.01,
                          double accSigma = 0.1, bool useGyro = true,
                          bool useAcc = true);

  void addImuMessage(const nlohmann::json &imuData);
  nlohmann::json detect() const;
  nlohmann::json getStatistics() const;
  void clear();

private:
  static int classifyState(double stdValue, double sigma);
  void removeOldData();
  bool hasTimestampRollback(int64_t currentTimestamp) const;

  int64_t timeWindowNs_;
  double gyroSigma_;
  double accSigma_;
  int64_t lastTimestampNs_ = 0;
  int64_t timestampRollbackToleranceNs_;

  std::unique_ptr<IncrementalStats> gyroStats_;
  std::unique_ptr<IncrementalStats> accStats_;
};

} // namespace recordlab::common
