/*
 * GlobalState - 全局状态管理
 *
 * 提供脚本可访问的全局变量（线程安全，自动过期）：
 * - record_timer: 录制时长（秒）
 * - time_delay: 时间延迟（毫秒）
 * - motion_status: 运动状态（'idle', 'moving', 'active'）
 *
 */
#pragma once

#include <QMutex>
#include <QString>
#include <chrono>
#include <optional>

namespace recordlab::core {

class GlobalState {
public:
  static constexpr double RECORD_TIMER_TIMEOUT = 5.0;
  static constexpr double TIME_DELAY_TIMEOUT = 5.0;
  static constexpr double MOTION_STATUS_TIMEOUT = 5.0;

  GlobalState() = default;

  // ========== 录制时长 ==========
  void setRecordTimer(double value);
  std::optional<double> getRecordTimer();
  void clearRecordTimer();

  // ========== 时间延迟 ==========
  void setTimeDelay(double value);
  std::optional<double> getTimeDelay();
  void clearTimeDelay();

  // ========== 运动状态 ==========
  void setMotionStatus(const QString &value);
  std::optional<QString> getMotionStatus();
  void clearMotionStatus();

  // ========== 批量 ==========
  void reset();

  // ========== 单例 ==========
  static GlobalState &instance();

private:
  mutable QMutex lock_;

  std::optional<double> recordTimer_;
  double recordTimerTimestamp_ = 0.0;

  std::optional<double> timeDelay_;
  double timeDelayTimestamp_ = 0.0;

  std::optional<QString> motionStatus_;
  double motionStatusTimestamp_ = 0.0;

  static double nowSec() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};

} // namespace recordlab::core
