/*
 * GlobalState 实现
 */
#include "recordlab/core/global_state.h"

namespace recordlab::core {

// ========== 录制时长 ==========
// 写入最新录制时长，并记录本次更新时间，供后续超时失效判断使用。
void GlobalState::setRecordTimer(double value) {
  QMutexLocker locker(&lock_);
  recordTimer_ = value;
  recordTimerTimestamp_ = nowSec();
}

// 读取录制时长；如果缓存已过期，则在返回前自动清空以避免展示陈旧数据。
std::optional<double> GlobalState::getRecordTimer() {
  QMutexLocker locker(&lock_);
  if (recordTimer_.has_value()) {
    if (nowSec() - recordTimerTimestamp_ > RECORD_TIMER_TIMEOUT) {
      recordTimer_.reset();
    }
  }
  return recordTimer_;
}

// 主动清空录制时长缓存，通常在录制结束或全局复位时调用。
void GlobalState::clearRecordTimer() {
  QMutexLocker locker(&lock_);
  recordTimer_.reset();
  recordTimerTimestamp_ = 0.0;
}

// ========== 时间延迟 ==========
// 更新当前链路延迟值，并同步刷新对应的时间戳。
void GlobalState::setTimeDelay(double value) {
  QMutexLocker locker(&lock_);
  timeDelay_ = value;
  timeDelayTimestamp_ = nowSec();
}

// 返回最近的延迟测量结果；如果超出允许窗口则返回空值。
std::optional<double> GlobalState::getTimeDelay() {
  QMutexLocker locker(&lock_);
  if (timeDelay_.has_value()) {
    if (nowSec() - timeDelayTimestamp_ > TIME_DELAY_TIMEOUT) {
      timeDelay_.reset();
    }
  }
  return timeDelay_;
}

// 清空延迟缓存，防止旧结果污染新一轮流程。
void GlobalState::clearTimeDelay() {
  QMutexLocker locker(&lock_);
  timeDelay_.reset();
  timeDelayTimestamp_ = 0.0;
}

// ========== 运动状态 ==========
// 写入最近一次运动状态分类结果，例如静止、轻动或剧烈运动。
void GlobalState::setMotionStatus(const QString &value) {
  QMutexLocker locker(&lock_);
  motionStatus_ = value;
  motionStatusTimestamp_ = nowSec();
}

// 获取最近的运动状态；状态过旧时自动回退为空，提示上层重新等待数据。
std::optional<QString> GlobalState::getMotionStatus() {
  QMutexLocker locker(&lock_);
  if (motionStatus_.has_value()) {
    if (nowSec() - motionStatusTimestamp_ > MOTION_STATUS_TIMEOUT) {
      motionStatus_.reset();
    }
  }
  return motionStatus_;
}

// 清空运动状态缓存，常用于设备断开或流程重置。
void GlobalState::clearMotionStatus() {
  QMutexLocker locker(&lock_);
  motionStatus_.reset();
  motionStatusTimestamp_ = 0.0;
}

// ========== 批量 ==========
// 一次性清理全部全局缓存与时间戳，恢复到初始状态。
void GlobalState::reset() {
  QMutexLocker locker(&lock_);
  recordTimer_.reset();
  recordTimerTimestamp_ = 0.0;
  timeDelay_.reset();
  timeDelayTimestamp_ = 0.0;
  motionStatus_.reset();
  motionStatusTimestamp_ = 0.0;
}

// ========== 单例 ==========
// 返回进程级单例，供多个模块共享轻量级运行时状态。
GlobalState &GlobalState::instance() {
  static GlobalState s;
  return s;
}

} // namespace recordlab::core
