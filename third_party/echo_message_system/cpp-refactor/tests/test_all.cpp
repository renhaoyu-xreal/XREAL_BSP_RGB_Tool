#include "publisher.h"
#include "subscriber.h"
#include "service.h"
#include "action.h"
#include "message.h"
#include "logger.h"
#include <thread>
#include <chrono>
#include <atomic>

std::atomic<int> test_passed{0};
std::atomic<int> test_failed{0};

void testFourMessageTypes() {
    LOG_INFO("Test", "\n=== Testing Four Message Types ===");
    
    std::atomic<int> imu_count{0}, latency_count{0}, image_count{0}, binary_count{0};
    
    // 订阅者
    auto imu_sub = std::make_shared<echo::Subscriber>("/data/imu",
        [&](const echo::Message::Ptr& msg) { imu_count++; });
    
    auto latency_sub = std::make_shared<echo::Subscriber>("/data/latency",
        [&](const echo::Message::Ptr& msg) { latency_count++; });
    
    auto image_sub = std::make_shared<echo::Subscriber>("/data/image",
        [&](const echo::Message::Ptr& msg) { image_count++; });
    
    auto binary_sub = std::make_shared<echo::Subscriber>("/data/binary",
        [&](const echo::Message::Ptr& msg) { binary_count++; });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    
    // 发布者
    auto imu_pub = std::make_shared<echo::Publisher>("/data/imu");
    auto latency_pub = std::make_shared<echo::Publisher>("/data/latency");
    auto image_pub = std::make_shared<echo::Publisher>("/data/image");
    auto binary_pub = std::make_shared<echo::Publisher>("/data/binary");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    
    // 发布消息
    for (int i = 0; i < 5; i++) {
        // IMU
        auto imu_msg = std::make_shared<echo::ImuMessage>(
            "/data/imu", i, 1, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0);
        imu_pub->publish(imu_msg);
        
        // Latency
        auto latency_msg = std::make_shared<echo::LatencyMessage>(
            "/data/latency", i, 2, i*0.1, i*0.2, i*0.3, i*0.4, i*0.5, i*0.6);
        latency_pub->publish(latency_msg);
        
        // Image
        std::vector<uint8_t> img_data(100, i);
        auto image_msg = std::make_shared<echo::ImageMessage>(
            "/data/image", img_data, "test.jpg");
        image_pub->publish(image_msg);
        
        // Binary
        std::vector<uint8_t> bin_data(50, i);
        auto binary_msg = std::make_shared<echo::BinaryMessage>(
            "/data/binary", bin_data);
        binary_pub->publish(binary_msg);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    LOG_INFO("Test", "IMU messages: ", imu_count.load());
    LOG_INFO("Test", "Latency messages: ", latency_count.load());
    LOG_INFO("Test", "Image messages: ", image_count.load());
    LOG_INFO("Test", "Binary messages: ", binary_count.load());
    
    bool passed = (imu_count >= 4 && latency_count >= 4 && 
                   image_count >= 4 && binary_count >= 4);
    
    if (passed) {
        LOG_INFO("Test", "✓ Four Message Types Test PASSED");
        test_passed++;
    } else {
        LOG_ERROR("Test", "✗ Four Message Types Test FAILED");
        test_failed++;
    }
}

void testMultipleSubscribers() {
    LOG_INFO("Test", "\n=== Testing Multiple Independent Subscribers ===");
    
    std::atomic<int> sub1_count{0}, sub2_count{0}, sub3_count{0};
    
    // 3个独立订阅者，每个有独立的 socket
    auto sub1 = std::make_shared<echo::Subscriber>("/multi/test",
        [&](const echo::Message::Ptr& msg) { sub1_count++; });
    
    auto sub2 = std::make_shared<echo::Subscriber>("/multi/test",
        [&](const echo::Message::Ptr& msg) { sub2_count++; });
    
    auto sub3 = std::make_shared<echo::Subscriber>("/multi/test",
        [&](const echo::Message::Ptr& msg) { sub3_count++; });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    
    auto pub = std::make_shared<echo::Publisher>("/multi/test");
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    
    for (int i = 0; i < 10; i++) {
        auto msg = std::make_shared<echo::ImuMessage>(
            "/multi/test", i, 1, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0);
        pub->publish(msg);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    LOG_INFO("Test", "Subscriber 1: ", sub1_count.load());
    LOG_INFO("Test", "Subscriber 2: ", sub2_count.load());
    LOG_INFO("Test", "Subscriber 3: ", sub3_count.load());
    
    bool passed = (sub1_count >= 8 && sub2_count >= 8 && sub3_count >= 8);
    
    if (passed) {
        LOG_INFO("Test", "✓ Multiple Subscribers Test PASSED");
        test_passed++;
    } else {
        LOG_ERROR("Test", "✗ Multiple Subscribers Test FAILED");
        test_failed++;
    }
}

int main() {
    echo::Logger::getInstance().setLogLevel(echo::LogLevel::INFO);
    
    LOG_INFO("Main",   "╔════════════════════════════════════════╗");
    LOG_INFO("Main",   "║  Echo Message System - Full Test Suite ║");
    LOG_INFO("Main",   "╚════════════════════════════════════════╝\n");
    
    LOG_INFO("Main", "Make sure Master is running on port 5590");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    try {
        testFourMessageTypes();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        testMultipleSubscribers();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
    } catch (const std::exception& e) {
        LOG_ERROR("Main", "Test failed with exception: ", e.what());
        return 1;
    }
    
    LOG_INFO("Main",   "╔════════════════════════════════════════╗");
    LOG_INFO("Main",   "║           Test Results                 ║");
    LOG_INFO("Main",   "╠════════════════════════════════════════╣");
    LOG_INFO("Main",   "║  Passed: ", test_passed.load(), "                             ║");
    LOG_INFO("Main",   "║  Failed: ", test_failed.load(), "                             ║");
    LOG_INFO("Main",   "╚════════════════════════════════════════╝\n");
    
    return test_failed.load() > 0 ? 1 : 0;
}

