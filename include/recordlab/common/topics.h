/*
 * Topic 名称和端口常量定义
 *
 * 只包含传感器数据相关的 Topic，不包含 Agent 内部通信
 *
 * 所有常量名、字符串值、端口号与 Python 版完全一致。
 */
#pragma once

#include <QString>
#include <stdexcept>
#include <unordered_map>

namespace recordlab::common {

// ============================================================================
// Topic 名称
// ============================================================================

// 传感器数据 Topics
inline constexpr const char *TOPIC_CAMERA = "camera_data";
inline constexpr const char *TOPIC_IMU = "imu_data";
inline constexpr const char *TOPIC_IMU1 = "imu1_data";
inline constexpr const char *TOPIC_POSE = "pose_data";
inline constexpr const char *TOPIC_MOTION_STATUS = "motion_status"; // 运动状态
inline constexpr const char *TOPIC_RECORD_TIMER = "record_timer"; // 录制计时器
inline constexpr const char *TOPIC_TIME_DELAY = "time_delay";     // 时间延迟
inline constexpr const char *TOPIC_ANDROID_IMU = "android_imu_data";

// ============================================================================
// 端口配置
// ============================================================================

// Topic Ports (Publisher/Subscriber) - 使用 MainSubnode 的端口
// MainSubnode 在这些端口发布数据，DataReceiver 在这些端口订阅
inline constexpr int PORT_IMU =
    16510; // IMU数据端口（陀螺仪+加速度计+磁力计+温度）
inline constexpr int PORT_IMU1 = 16511;   // IMU1数据端口（备用IMU）
inline constexpr int PORT_CAMERA = 16515; // 图像数据端口
inline constexpr int PORT_RECORD_TIMER =
    16520;                                    // 录制计时器端口（录制时长信息）
inline constexpr int PORT_TIME_DELAY = 16521; // 时间延迟端口（时间同步信息）
inline constexpr int PORT_MOTION_STATUS =
    16525; // 运动状态端口（静止/运动/活跃）
inline constexpr int PORT_ANDROID_IMU = 16512; // Android IMU 数据端口

// 默认主机
inline constexpr const char *DEFAULT_HOST = "127.0.0.1";

// ============================================================================
// 辅助函数
// ============================================================================

/*
 * 获取 Topic 的 ZMQ 地址
 *
 * @param topicName Topic 名称
 * @param host      主机地址（默认 127.0.0.1）
 * @return          ZMQ TCP 地址字符串，如 "tcp://127.0.0.1:16510"
 */
inline QString getTopicAddress(const QString &topicName,
                               const QString &host = DEFAULT_HOST) {
  static const std::unordered_map<std::string, int> portMap = {
      {TOPIC_CAMERA, PORT_CAMERA},
      {TOPIC_IMU, PORT_IMU},
      {TOPIC_IMU1, PORT_IMU1},
      {TOPIC_MOTION_STATUS, PORT_MOTION_STATUS},
      {TOPIC_RECORD_TIMER, PORT_RECORD_TIMER},
      {TOPIC_TIME_DELAY, PORT_TIME_DELAY},
      {TOPIC_ANDROID_IMU, PORT_ANDROID_IMU},
  };

  auto it = portMap.find(topicName.toStdString());
  if (it == portMap.end()) {
    throw std::invalid_argument("Unknown topic: " + topicName.toStdString());
  }
  return QStringLiteral("tcp://%1:%2").arg(host).arg(it->second);
}

} // namespace recordlab::common
