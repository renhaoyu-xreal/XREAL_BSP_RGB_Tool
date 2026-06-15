/*
 * ImuSimMainSubnode 实现
 */
#include "recordlab/subnodes/imu_sim_main_subnode.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTimer>

#include <chrono>
#include <cmath>
#include <iostream>
#include <csignal>

namespace recordlab::subnodes {

using json = nlohmann::json;

// ============================================================================
// ImuSimDevice
// ============================================================================

ImuSimDevice::ImuSimDevice(double frequency) : frequency_(frequency) {}

// 析构时保证仿真线程被停止并回收。
ImuSimDevice::~ImuSimDevice() { release(); }

json ImuSimDevice::initialize(const json & /*params*/) {
  // 初始化仿真设备本身不需要额外硬件，只记录频率并返回成功。
  std::cout << "[ImuSimDevice] Initialized at " << frequency_ << " Hz"
            << std::endl;
  return {{"success", true}, {"message", ""}};
}

json ImuSimDevice::start(const json &) {
  // 启动后台生成线程，持续输出模拟 IMU 数据。
  if (running_)
    return {{"success", true}, {"message", "Already running"}};
  running_ = true;
  genThread_ = std::thread(&ImuSimDevice::generateLoop, this);
  std::cout << "[ImuSimDevice] Started" << std::endl;
  return {{"success", true}, {"message", ""}};
}

json ImuSimDevice::stop(const json &) {
  // 停止生成线程并等待其退出，确保不会继续写录制数据。
  running_ = false;
  if (genThread_.joinable())
    genThread_.join();
  std::cout << "[ImuSimDevice] Stopped" << std::endl;
  return {{"success", true}, {"message", ""}};
}

json ImuSimDevice::release() {
  // release 直接复用 stop 逻辑，保持接口和真实设备一致。
  stop({});
  return {{"success", true}, {"message", ""}};
}

void ImuSimDevice::generateLoop() {
  // 按设定频率合成 gyro/acc 数据，模拟一个持续运行的 IMU 设备。
  auto interval = std::chrono::duration<double>(1.0 / frequency_);
  int64_t counter = 0;

  while (running_) {
    auto now = std::chrono::steady_clock::now();
    int64_t tsNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       now.time_since_epoch())
                       .count();

    double t = counter * (1.0 / frequency_);

    // Generate IMU0 gyro (type=1)
    if (imuCallback_) {
      json gyroMsg = {{"type", 1},
                      {"timestamp_ns", tsNs},
                      {"data",
                       {std::sin(2 * M_PI * 0.5 * t) * 0.1,  // gx
                        std::cos(2 * M_PI * 0.3 * t) * 0.05, // gy
                        std::sin(2 * M_PI * 0.7 * t) * 0.02, // gz
                        0.0, 0.0, 0.0}}};
      imuCallback_(gyroMsg);

      // Generate IMU0 acc (type=2)
      json accMsg = {
          {"type", 2},
          {"timestamp_ns", tsNs},
          {"data",
           {std::sin(2 * M_PI * 0.1 * t) * 0.5,        // ax
            9.81 + std::cos(2 * M_PI * 0.2 * t) * 0.1, // ay (gravity)
            std::sin(2 * M_PI * 0.15 * t) * 0.3,       // az
            0.0, 0.0, 0.0}}};
      imuCallback_(accMsg);
    }

    counter++;
    std::this_thread::sleep_until(now + interval);
  }
}

// ============================================================================
// ImuSimMainSubnode
// ============================================================================

ImuSimMainSubnode::ImuSimMainSubnode(const QString &name,
                                     const QString &subnodeHost, int goalPort,
                                     int feedbackPort, const QString &rootPath,
                                     double frequency, QObject *parent)
    : MainSubnode(name, subnodeHost, goalPort, feedbackPort, rootPath, nullptr,
                  parent) {
  simDevice_ = std::make_unique<ImuSimDevice>(frequency);

  // Set as the device for MainSubnode
  device_ = simDevice_.get();
  device_->setImuDataCallback(
      [this](const json &msg) { imuDataCallback(msg); });

  // Create IMU writer for recording
  addImuWriter(0, std::make_unique<CsvDataWriter>("imu0_data.csv"));

  std::cout << "[" << name.toStdString() << "] ImuSimMainSubnode initialized @ "
            << frequency << "Hz" << std::endl;
}

ImuSimMainSubnode::~ImuSimMainSubnode() = default;

int imuSimMainSubnodeMain(int argc, char *argv[]) {
  // 独立子节点入口：解析参数、安装信号处理并启动仿真主子节点。
  QCoreApplication app(argc, argv);

  auto signalHandler = [](int signum) {
    std::cout << "Received signal " << signum << ", shutting down..."
              << std::endl;
    QCoreApplication::quit();
  };
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  QCommandLineParser parser;
  parser.addOption({"name", "Node name", "imu_simulation"});
  parser.addOption({"host", "Host address", "127.0.0.1"});
  parser.addOption({"goal-port", "Goal port", "5690"});
  parser.addOption({"feedback-port", "Feedback port", "5691"});
  parser.addOption({"root-path", "Data root path", "./output"});
  parser.process(app);

  QString name = parser.value("name");
  if (name.isEmpty())
    name = "imu_simulation";
  QString host = parser.value("host");
  if (host.isEmpty())
    host = "127.0.0.1";
  int goalPort = parser.value("goal-port").toInt();
  if (goalPort == 0)
    goalPort = 5690;
  int feedbackPort = parser.value("feedback-port").toInt();
  if (feedbackPort == 0)
    feedbackPort = 5691;
  QString rootPath = parser.value("root-path");
  if (rootPath.isEmpty())
    rootPath = "./output";

  QTimer ticker;
  QObject::connect(&ticker, &QTimer::timeout, []() {});
  ticker.start(500);

  ImuSimMainSubnode subnode(name, host, goalPort, feedbackPort, rootPath);
  subnode.createMainPublishers();

  auto result = subnode.connect();
  if (!result.value("success", false)) {
    std::cerr << "Failed to connect: "
              << result.value("message", std::string("Unknown error"))
              << std::endl;
    return 1;
  }

  std::cout << "[" << name.toStdString() << "] SubNode ready" << std::endl;
  return app.exec();
}

} // namespace recordlab::subnodes
