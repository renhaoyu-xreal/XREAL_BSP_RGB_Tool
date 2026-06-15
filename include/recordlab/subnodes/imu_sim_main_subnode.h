/*
 * ImuSimMainSubnode - IMU 仿真子节点
 *
 * 继承 MainSubnode，用仿真设备替代真实 IMU 设备
 * 生成正弦/噪声数据用于测试
 *
 */
#pragma once

#include "recordlab/subnodes/main_subnode.h"

#include <atomic>
#include <memory>
#include <thread>

namespace recordlab::subnodes {

// ============================================================================
// ImuSimDevice — IMU 仿真设备
// ============================================================================

class ImuSimDevice : public BaseDevice {
public:
  explicit ImuSimDevice(double frequency = 200.0);
  ~ImuSimDevice() override;

  nlohmann::json initialize(const nlohmann::json &params) override;
  nlohmann::json start(const nlohmann::json &params = {}) override;
  nlohmann::json stop(const nlohmann::json &params = {}) override;
  nlohmann::json release() override;

  void setImuDataCallback(ImuCallback cb) override {
    imuCallback_ = std::move(cb);
  }
  void setImageDataCallback(ImageCallback cb) override {
    imageCallback_ = std::move(cb);
  }

private:
  void generateLoop();

  double frequency_; // Hz
  ImuCallback imuCallback_;
  ImageCallback imageCallback_;
  std::atomic<bool> running_{false};
  std::thread genThread_;
};

// ============================================================================
// ImuSimMainSubnode
// ============================================================================

class ImuSimMainSubnode : public MainSubnode {
  Q_OBJECT

public:
  explicit ImuSimMainSubnode(const QString &name = "imu_simulation",
                             const QString &subnodeHost = "127.0.0.1",
                             int goalPort = 5692, int feedbackPort = 5693,
                             const QString &rootPath = "./output",
                             double frequency = 200.0,
                             QObject *parent = nullptr);
  ~ImuSimMainSubnode() override;

private:
  std::unique_ptr<ImuSimDevice> simDevice_;
};

// 独立可执行文件入口函数。
int imuSimMainSubnodeMain(int argc, char *argv[]);

} // namespace recordlab::subnodes
