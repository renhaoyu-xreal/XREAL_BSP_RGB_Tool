#include "publisher.h"
#include "subscriber.h"
#include "message.h"
#include "logger.h"
#include <thread>
#include <chrono>
#include <atomic>

std::atomic<int> received_count{0};

void testPubSub() {
    LOG_INFO("Test", "=== Testing Pub/Sub ===");
    
    // 创建订阅者
    auto sub = std::make_shared<echo::Subscriber>(
        "/test/imu",
        [](const echo::Message::Ptr& msg) {
            auto imu_msg = std::dynamic_pointer_cast<echo::ImuMessage>(msg);
            if (imu_msg) {
                const double* data = imu_msg->getData();
                LOG_INFO("Subscriber", "Received IMU data: [",
                         data[0], ", ", data[1], ", ", data[2], ", ",
                         data[3], ", ", data[4], ", ", data[5], "]");
                received_count++;
            }
        }
    );
    
    // 等待订阅者连接
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    // 创建发布者
    auto pub = std::make_shared<echo::Publisher>("/test/imu");
    
    // 等待发布者准备好并让连接建立
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    
    // 发布消息
    for (int i = 0; i < 10; i++) {
        auto msg = std::make_shared<echo::ImuMessage>(
            "/test/imu",
            std::chrono::system_clock::now().time_since_epoch().count(),
            1,
            i * 1.0, i * 1.1, i * 1.2,
            i * 2.0, i * 2.1, i * 2.2
        );
        pub->publish(msg);
        LOG_INFO("Publisher", "Published message ", i);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // 等待接收
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    LOG_INFO("Test", "Received ", received_count.load(), " messages");
    LOG_INFO("Test", received_count.load() >= 8 ? "✓ PASSED" : "✗ FAILED");
}

int main() {
    echo::Logger::getInstance().setLogLevel(echo::LogLevel::INFO);
    
    LOG_INFO("Main", "Starting Pub/Sub test...");
    LOG_INFO("Main", "Make sure Master is running on port 5590");
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    try {
        testPubSub();
    } catch (const std::exception& e) {
        LOG_ERROR("Main", "Test failed: ", e.what());
        return 1;
    }
    
    return 0;
}
