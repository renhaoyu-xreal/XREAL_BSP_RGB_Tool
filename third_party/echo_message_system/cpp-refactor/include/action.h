#pragma once

#include "service.h"
#include "publisher.h"
#include "subscriber.h"
#include <memory>
#include <string>
#include <functional>
#include <thread>
#include <map>
#include <mutex>
#include <atomic>

namespace echo {

/**
 * @brief Action 服务端
 * Goal/Cancel 通过 Service 实现
 * Result/Feedback 通过 Publisher 实现
 */
class ActionServer {
public:
    using GoalCallback = std::function<void(uint32_t goal_id, const nlohmann::json& goal,
                                           std::function<void(const nlohmann::json&)> send_feedback,
                                           std::atomic<bool>& should_cancel)>;
    
    ActionServer(const std::string& action_name, GoalCallback callback);
    ~ActionServer();
    
    void publishFeedback(uint32_t goal_id, const nlohmann::json& feedback);
    void publishResult(uint32_t goal_id, const nlohmann::json& result, bool success);
    
private:
    std::string action_name_;
    GoalCallback goal_callback_;
    
    std::unique_ptr<ServiceServer> goal_service_;
    std::unique_ptr<ServiceServer> cancel_service_;
    std::unique_ptr<Publisher> feedback_pub_;
    std::unique_ptr<Publisher> result_pub_;
    
    std::map<uint32_t, std::thread> goal_threads_;
    std::map<uint32_t, std::atomic<bool>> cancel_flags_;
    std::mutex goals_mutex_;
    
    nlohmann::json handleGoal(const nlohmann::json& request);
    nlohmann::json handleCancel(const nlohmann::json& request);
    void executeGoal(uint32_t goal_id, const nlohmann::json& goal);
};

/**
 * @brief Action 客户端
 */
class ActionClient {
public:
    using FeedbackCallback = std::function<void(uint32_t goal_id, const nlohmann::json& feedback)>;
    using ResultCallback = std::function<void(uint32_t goal_id, const nlohmann::json& result, bool success)>;
    
    explicit ActionClient(const std::string& action_name);
    ~ActionClient();
    
    uint32_t sendGoal(const nlohmann::json& goal, FeedbackCallback fb_cb = nullptr, ResultCallback res_cb = nullptr);
    void cancelGoal(uint32_t goal_id);
    bool waitForResult(uint32_t goal_id, int timeout_ms = 0);
    
private:
    struct PendingResult {
        nlohmann::json result;
        bool success = false;
    };

    std::string action_name_;
    
    std::unique_ptr<ServiceClient> goal_client_;
    std::unique_ptr<ServiceClient> cancel_client_;
    std::unique_ptr<Subscriber> feedback_sub_;
    std::unique_ptr<Subscriber> result_sub_;
    
    std::map<uint32_t, FeedbackCallback> feedback_callbacks_;
    std::map<uint32_t, ResultCallback> result_callbacks_;
    std::map<uint32_t, bool> result_received_;
    std::map<uint32_t, PendingResult> pending_results_;
    std::mutex callbacks_mutex_;
    
    void handleFeedbackRaw(const std::string& data);
    void handleResultRaw(const std::string& data);
};

} // namespace echo
