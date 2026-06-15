#include "service.h"
#include "logger.h"
#include <thread>
#include <chrono>

void testService() {
    LOG_INFO("Test", "=== Testing Service ===");
    
    // 创建服务端
    auto server = std::make_shared<echo::ServiceServer>(
        "/test/add",
        [](const nlohmann::json& req) -> nlohmann::json {
            int a = req["a"];
            int b = req["b"];
            int result = a + b;
            LOG_INFO("ServiceServer", "Computing ", a, " + ", b, " = ", result);
            return {{"result", result}};
        }
    );
    
    // 等待服务器准备好
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 创建客户端
    auto client = std::make_shared<echo::ServiceClient>("/test/add");
    
    // 测试调用
    bool all_passed = true;
    for (int i = 0; i < 5; i++) {
        nlohmann::json request = {{"a", i}, {"b", i * 10}};
        nlohmann::json response = client->call(request);
        
        int expected = i + i * 10;
        int result = response["result"];
        
        LOG_INFO("ServiceClient", "Called service: ", i, " + ", i * 10, " = ", result);
        
        if (result != expected) {
            LOG_ERROR("Test", "Result mismatch: expected ", expected, ", got ", result);
            all_passed = false;
        }
    }
    
    LOG_INFO("Test", all_passed ? "✓ PASSED" : "✗ FAILED");
}

int main() {
    echo::Logger::getInstance().setLogLevel(echo::LogLevel::INFO);
    
    LOG_INFO("Main", "Starting Service test...");
    LOG_INFO("Main", "Make sure Master is running on port 5590");
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    try {
        testService();
    } catch (const std::exception& e) {
        LOG_ERROR("Main", "Test failed: ", e.what());
        return 1;
    }
    
    return 0;
}
