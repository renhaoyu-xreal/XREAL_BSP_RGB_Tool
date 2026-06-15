#include "action.h"
#include "logger.h"
#include <thread>
#include <chrono>

void testAction() {
    LOG_INFO("Test", "=== Testing Action ===");
    
    // 创建 Action Server
    auto server = std::make_shared<echo::ActionServer>(
        "/test/count",
        [](uint32_t goal_id, const nlohmann::json& goal,
           std::function<void(const nlohmann::json&)> send_feedback,
           std::atomic<bool>& should_cancel) {
            
            int target = goal["target"];
            LOG_INFO("ActionServer", "Goal ", goal_id, ": count to ", target);
            
            for (int i = 0; i <= target; i++) {
                if (should_cancel) {
                    LOG_INFO("ActionServer", "Goal ", goal_id, " cancelled at ", i);
                    return;
                }
                
                nlohmann::json feedback = {{"current", i}};
                send_feedback(feedback);
                LOG_DEBUG("ActionServer", "Feedback: ", i);
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            LOG_INFO("ActionServer", "Goal ", goal_id, " completed");
        }
    );
    
    // 等待服务器准备好
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 创建 Action Client
    auto client = std::make_shared<echo::ActionClient>("/test/count");
    
    // 发送 goal
    nlohmann::json goal = {{"target", 5}};
    
    uint32_t goal_id = client->sendGoal(
        goal,
        [](uint32_t id, const nlohmann::json& fb) {
            LOG_INFO("ActionClient", "Feedback for goal ", id, ": ", fb.dump());
        },
        [](uint32_t id, const nlohmann::json& res, bool success) {
            LOG_INFO("ActionClient", "Result for goal ", id, ": success=", success);
        }
    );
    
    LOG_INFO("ActionClient", "Sent goal ", goal_id);
    
    // 等待完成
    bool finished = client->waitForResult(goal_id, 10000);
    
    LOG_INFO("Test", finished ? "✓ PASSED" : "✗ FAILED");
}

int main() {
    echo::Logger::getInstance().setLogLevel(echo::LogLevel::INFO);
    
    LOG_INFO("Main", "Starting Action test...");
    LOG_INFO("Main", "Make sure Master is running on port 5590");
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    try {
        testAction();
    } catch (const std::exception& e) {
        LOG_ERROR("Main", "Test failed: ", e.what());
        return 1;
    }
    
    return 0;
}
