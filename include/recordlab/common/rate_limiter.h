/*
 * 频率限制器
 *
 * 用于限制不同类型消息的处理频率
 *
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace recordlab::common {

class RateLimiter {
public:
  explicit RateLimiter(double frequency)
      : frequency_(frequency),
        intervalNs_(frequency > 0 ? static_cast<int64_t>(1.0 / frequency * 1e9)
                                  : 0) {}

  /*
   * 检查是否允许处理这条数据
   *
   * @param data JSON 格式的数据，可包含 "timestamp_ns" 和 "type" 字段
   * @return true 表示可以处理，false 表示应被限流丢弃
   */
  bool check(const nlohmann::json &data) {
    if (frequency_ <= 0)
      return true;

    int64_t currentTimeNs = 0;
    int msgType = 0;

    if (data.is_object()) {
      if (data.contains("timestamp_ns") &&
          data["timestamp_ns"].is_number_integer()) {
        currentTimeNs = data["timestamp_ns"].get<int64_t>();
      } else if (data.contains("timestamp") &&
                 data["timestamp"].is_number_float()) {
        currentTimeNs =
            static_cast<int64_t>(data["timestamp"].get<double>() * 1e9);
      } else {
        currentTimeNs = nowNs();
      }
      msgType = data.value("type", 0);
    } else {
      currentTimeNs = nowNs();
    }

    auto &last = lastTimestampNs_[msgType];
    if (currentTimeNs - last >= intervalNs_) {
      last = currentTimeNs;
      return true;
    }
    return false;
  }

  void reset() { lastTimestampNs_.clear(); }

private:
  double frequency_;
  int64_t intervalNs_;
  std::unordered_map<int, int64_t> lastTimestampNs_;

  static int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};

} // namespace recordlab::common
