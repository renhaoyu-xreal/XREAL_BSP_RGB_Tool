/*
 * 消息类型定义
 *
 * 定义所有 Topic 和 Action 的消息格式
 *
 * 所有结构体名、字段名与 Python 版完全一致。
 *
 * 注意：Python 版使用 numpy.ndarray 存储图像，C++ 版使用
 * 裸 uint8_t 向量 + 宽高等元信息。QImage 转换在 UI 层完成。
 */
#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace recordlab::common {

// ============================================================================
// Topic 消息类型
// ============================================================================

/*
 * 图像数据消息
 *
 */
struct ImageMessage {
  std::vector<uint8_t> image;             // 原始图像字节（替代 numpy array）
  int width = 0;                          // 图像宽度 (Python侧由shape获取)
  int height = 0;                         // 图像高度
  int channels = 1;                       // 通道数（GRAY=1, RGB=3, RGBA=4）
  int64_t timestamp_ns = 0;               // 时间戳（纳秒）
  int64_t exposure_start_time_device = 0; // 设备时间戳（纳秒）
  int64_t exposure_start_time_system = 0; // 系统时间戳（纳秒）
  int64_t exposure_duration = 0;          // 曝光时长（纳秒）
  int64_t rolling_shutter_time = 0;       // 卷帘快门时间（纳秒）
  int gain = 0;                           // 增益
  int stride = 0;                         // 步长
  std::string format = "RGB"; // 颜色格式: "RGB", "BGR", "RGBA", "GRAY"

  nlohmann::json toJson() const {
    return {// image 不放 JSON, 走二进制通道
            {"width", width},
            {"height", height},
            {"channels", channels},
            {"timestamp_ns", timestamp_ns},
            {"exposure_start_time_device", exposure_start_time_device},
            {"exposure_start_time_system", exposure_start_time_system},
            {"exposure_duration", exposure_duration},
            {"rolling_shutter_time", rolling_shutter_time},
            {"gain", gain},
            {"stride", stride},
            {"format", format}};
  }

  static ImageMessage fromJson(const nlohmann::json &j) {
    ImageMessage m;
    m.width = j.value("width", 0);
    m.height = j.value("height", 0);
    m.channels = j.value("channels", 1);
    m.timestamp_ns = j.value("timestamp_ns", int64_t(0));
    m.exposure_start_time_device =
        j.value("exposure_start_time_device", int64_t(0));
    m.exposure_start_time_system =
        j.value("exposure_start_time_system", int64_t(0));
    m.exposure_duration = j.value("exposure_duration", int64_t(0));
    m.rolling_shutter_time = j.value("rolling_shutter_time", int64_t(0));
    m.gain = j.value("gain", 0);
    m.stride = j.value("stride", 0);
    m.format = j.value("format", std::string("RGB"));
    return m;
  }
};

/*
 * 双目图像对消息
 *
 */
struct ImagePairMessage {
  ImageMessage left_image;               // 左图
  ImageMessage right_image;              // 右图
  int64_t timestamp_ns = 0;              // 图像对时间戳（纳秒）
  int64_t time_diff_tolerance = 1000000; // 时间差容忍度（纳秒，默认1ms）

  nlohmann::json toJson() const {
    return {{"left_image", left_image.toJson()},
            {"right_image", right_image.toJson()},
            {"timestamp_ns", timestamp_ns},
            {"time_diff_tolerance", time_diff_tolerance}};
  }

  static ImagePairMessage fromJson(const nlohmann::json &j) {
    ImagePairMessage m;
    m.left_image = ImageMessage::fromJson(j.at("left_image"));
    m.right_image = ImageMessage::fromJson(j.at("right_image"));
    m.timestamp_ns = j.value("timestamp_ns", int64_t(0));
    m.time_diff_tolerance = j.value("time_diff_tolerance", int64_t(1000000));
    return m;
  }
};

/*
 * IMU 数据消息
 *
 */
struct ImuMessage {
  int type = 0;             // IMU数据类型（1=陀螺仪，2=加速度计，3=磁力计等）
  int64_t timestamp_ns = 0; // 时间戳（纳秒）
  std::vector<double> data; // [data0, data1, ..., data5]

  nlohmann::json toJson() const {
    return {{"type", type}, {"timestamp_ns", timestamp_ns}, {"data", data}};
  }

  static ImuMessage fromJson(const nlohmann::json &j) {
    ImuMessage m;
    m.type = j.value("type", 0);
    m.timestamp_ns = j.value("timestamp_ns", int64_t(0));
    m.data = j.value("data", std::vector<double>{});
    return m;
  }
};

/*
 * Time 数据结构 - 时间戳消息
 *
 */
struct TimeMessage {
  std::string name;
  int64_t timestamp_ns = 0; // 时间戳（纳秒）
  int64_t duration_ns = 0;  // 持续时间（纳秒）
  std::string status;       // 状态

  nlohmann::json toJson() const {
    return {{"name", name},
            {"timestamp_ns", timestamp_ns},
            {"duration_ns", duration_ns},
            {"status", status}};
  }

  static TimeMessage fromJson(const nlohmann::json &j) {
    TimeMessage m;
    m.name = j.value("name", std::string());
    m.timestamp_ns = j.value("timestamp_ns", int64_t(0));
    m.duration_ns = j.value("duration_ns", int64_t(0));
    m.status = j.value("status", std::string());
    return m;
  }
};

/*
 * 运动状态消息
 *
 */
struct MotionStatusMessage {
  std::string name;
  int64_t timestamp_ns = 0; // 时间戳（纳秒）
  std::string status;       // "none", "static", "moving", "active"

  nlohmann::json toJson() const {
    return {{"name", name}, {"timestamp_ns", timestamp_ns}, {"status", status}};
  }

  static MotionStatusMessage fromJson(const nlohmann::json &j) {
    MotionStatusMessage m;
    m.name = j.value("name", std::string());
    m.timestamp_ns = j.value("timestamp_ns", int64_t(0));
    m.status = j.value("status", std::string());
    return m;
  }
};

/*
 * Pose 数据结构 - 位姿消息
 *
 */
struct PoseMessage {
  std::string name;
  int64_t timestamp_ns = 0; // 时间戳（纳秒）

  struct Vec3 {
    double x = 0, y = 0, z = 0;
  };
  struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
  };

  Vec3 position;
  Quat orientation;

  nlohmann::json toJson() const {
    return {
        {"name", name},
        {"timestamp_ns", timestamp_ns},
        {"position", {{"x", position.x}, {"y", position.y}, {"z", position.z}}},
        {"orientation",
         {{"x", orientation.x},
          {"y", orientation.y},
          {"z", orientation.z},
          {"w", orientation.w}}}};
  }

  static PoseMessage fromJson(const nlohmann::json &j) {
    PoseMessage m;
    m.name = j.value("name", std::string());
    m.timestamp_ns = j.value("timestamp_ns", int64_t(0));
    if (j.contains("position")) {
      auto &p = j["position"];
      m.position = {p.value("x", 0.0), p.value("y", 0.0), p.value("z", 0.0)};
    }
    if (j.contains("orientation")) {
      auto &o = j["orientation"];
      m.orientation = {o.value("x", 0.0), o.value("y", 0.0), o.value("z", 0.0),
                       o.value("w", 1.0)};
    }
    return m;
  }
};

/*
 * 自定义 double 数据结构
 *
 */
struct CustomDoubleMessage {
  std::string name;
  int64_t timestamp_ns = 0;
  double data = 0.0;

  nlohmann::json toJson() const {
    return {{"name", name}, {"timestamp_ns", timestamp_ns}, {"data", data}};
  }

  static CustomDoubleMessage fromJson(const nlohmann::json &j) {
    return {j.value("name", std::string()), j.value("timestamp_ns", int64_t(0)),
            j.value("data", 0.0)};
  }
};

/*
 * 自定义 vector 数据结构
 *
 */
struct CustomVector3dMessage {
  std::string name;
  int64_t timestamp_ns = 0;

  struct Vec3 {
    double x = 0, y = 0, z = 0;
  };
  Vec3 data;

  nlohmann::json toJson() const {
    return {{"name", name},
            {"timestamp_ns", timestamp_ns},
            {"data", {{"x", data.x}, {"y", data.y}, {"z", data.z}}}};
  }

  static CustomVector3dMessage fromJson(const nlohmann::json &j) {
    CustomVector3dMessage m;
    m.name = j.value("name", std::string());
    m.timestamp_ns = j.value("timestamp_ns", int64_t(0));
    if (j.contains("data")) {
      auto &d = j["data"];
      m.data = {d.value("x", 0.0), d.value("y", 0.0), d.value("z", 0.0)};
    }
    return m;
  }
};

// ============================================================================
// 辅助函数
// ============================================================================

/*
 * 创建图像消息
 */
inline ImageMessage createImageMessage(
    std::vector<uint8_t> image, int width, int height, int channels,
    int64_t timestamp_ns, int64_t exposure_start_time_device = 0,
    int64_t exposure_start_time_system = 0, int64_t exposure_duration = 0,
    int64_t rolling_shutter_time = 0, int gain = 0, int stride = 0,
    const std::string &format = "RGB") {
  return ImageMessage{std::move(image),
                      width,
                      height,
                      channels,
                      timestamp_ns,
                      exposure_start_time_device,
                      exposure_start_time_system,
                      exposure_duration,
                      rolling_shutter_time,
                      gain,
                      stride,
                      format};
}

/*
 * 创建 IMU 消息
 *
 * @param type         IMU数据类型（1=陀螺仪，2=加速度计，3=磁力计等）
 * @param timestamp_ns 时间戳（纳秒）
 * @param data         数据列表，通常包含6个浮点数
 */
inline ImuMessage createImuMessage(int type, int64_t timestamp_ns,
                                   std::vector<double> data) {
  return ImuMessage{type, timestamp_ns, std::move(data)};
}

} // namespace recordlab::common
