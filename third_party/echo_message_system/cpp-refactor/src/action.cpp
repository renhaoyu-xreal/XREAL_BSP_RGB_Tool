#include "action.h"
#include "logger.h"
#include <chrono>
#include <vector>

namespace echo {

// ==================== ActionServer ====================

ActionServer::ActionServer(const std::string& action_name, GoalCallback callback)
    : action_name_(action_name), goal_callback_(std::move(callback)) {
    
    // 创建 Goal Service (用于接收 goal 请求)
    goal_service_ = std::make_unique<ServiceServer>(
        action_name_ + "/send_goal",
        [this](const nlohmann::json& req) { return handleGoal(req); }
    );
    
    // 创建 Cancel Service
    cancel_service_ = std::make_unique<ServiceServer>(
        action_name_ + "/cancel",
        [this](const nlohmann::json& req) { return handleCancel(req); }
    );
    
    // 创建 Feedback Publisher
    feedback_pub_ = std::make_unique<Publisher>(action_name_ + "/feedback");
    
    // 创建 Result Publisher
    result_pub_ = std::make_unique<Publisher>(action_name_ + "/result");
    
    LOG_INFO("ActionServer", "Created action server for '", action_name_, "'");
}

ActionServer::~ActionServer() {
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(goals_mutex_);
        for (auto& [id, thread] : goal_threads_) {
            if (thread.joinable()) {
                threads.push_back(std::move(thread));
            }
        }
        goal_threads_.clear();
        cancel_flags_.clear();
    }
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    LOG_INFO("ActionServer", "Destroyed action server for '", action_name_, "'");
}

nlohmann::json ActionServer::handleGoal(const nlohmann::json& request) {
    static uint32_t next_goal_id = 1;
    uint32_t goal_id = next_goal_id++;
    
    nlohmann::json goal = request["goal"];
    
    LOG_INFO("ActionServer", "Received goal ", goal_id, " for action '", action_name_, "'");
    
    // 创建 cancel flag
    {
        std::lock_guard<std::mutex> lock(goals_mutex_);
        cancel_flags_[goal_id] = false;
        goal_threads_[goal_id] = std::thread();
    }
    
    // 启动执行线程
    std::thread exec_thread(&ActionServer::executeGoal, this, goal_id, goal);
    
    {
        std::lock_guard<std::mutex> lock(goals_mutex_);
        auto it = goal_threads_.find(goal_id);
        if (it != goal_threads_.end()) {
            it->second = std::move(exec_thread);
        } else if (exec_thread.joinable()) {
            exec_thread.detach();
        }
    }
    
    return {{"goal_id", goal_id}, {"accepted", true}};
}

nlohmann::json ActionServer::handleCancel(const nlohmann::json& request) {
    uint32_t goal_id = request["goal_id"];
    
    std::lock_guard<std::mutex> lock(goals_mutex_);
    if (cancel_flags_.find(goal_id) != cancel_flags_.end()) {
        cancel_flags_[goal_id] = true;
        LOG_INFO("ActionServer", "Cancelled goal ", goal_id, " for action '", action_name_, "'");
        return {{"success", true}};
    }
    
    return {{"success", false}, {"error", "Goal not found"}};
}

void ActionServer::executeGoal(uint32_t goal_id, const nlohmann::json& goal) {
    auto send_feedback = [this, goal_id](const nlohmann::json& feedback) {
        publishFeedback(goal_id, feedback);
    };
    
    std::atomic<bool>* should_cancel = nullptr;
    {
        std::lock_guard<std::mutex> lock(goals_mutex_);
        auto it = cancel_flags_.find(goal_id);
        if (it == cancel_flags_.end()) {
            return;
        }
        should_cancel = &it->second;
    }
    
    try {
        goal_callback_(goal_id, goal, send_feedback, *should_cancel);
    } catch (const std::exception& e) {
        LOG_ERROR("ActionServer", "Goal ", goal_id, " execution failed: ", e.what());
        publishResult(goal_id, nlohmann::json{{"error", e.what()}}, false);
    }
    
    // 清理
    {
        std::lock_guard<std::mutex> lock(goals_mutex_);
        cancel_flags_.erase(goal_id);
        auto it = goal_threads_.find(goal_id);
        if (it != goal_threads_.end()) {
            if (it->second.joinable()) {
                it->second.detach();
            }
            goal_threads_.erase(it);
        }
    }
}

void ActionServer::publishFeedback(uint32_t goal_id, const nlohmann::json& feedback) {
    nlohmann::json fb_msg = {
        {"goal_id", goal_id},
        {"feedback", feedback}
    };
    
    feedback_pub_->publishRaw(fb_msg.dump());
    LOG_DEBUG("ActionServer", "Published feedback for goal ", goal_id);
}

void ActionServer::publishResult(uint32_t goal_id, const nlohmann::json& result, bool success) {
    nlohmann::json res_msg = {
        {"goal_id", goal_id},
        {"result", result},
        {"success", success}
    };
    
    result_pub_->publishRaw(res_msg.dump());
    LOG_INFO("ActionServer", "Published result for goal ", goal_id, ", success: ", success);
}

// ==================== ActionClient ====================

ActionClient::ActionClient(const std::string& action_name)
    : action_name_(action_name) {
    
    // 创建 Service Clients
    goal_client_ = std::make_unique<ServiceClient>(action_name_ + "/send_goal");
    cancel_client_ = std::make_unique<ServiceClient>(action_name_ + "/cancel");
    
    // 创建 Subscribers（使用原始数据模式）
    feedback_sub_ = std::make_unique<Subscriber>(
        action_name_ + "/feedback",
        [this](const std::string& data) { handleFeedbackRaw(data); },
        true
    );
    
    result_sub_ = std::make_unique<Subscriber>(
        action_name_ + "/result",
        [this](const std::string& data) { handleResultRaw(data); },
        true
    );
    
    LOG_INFO("ActionClient", "Created action client for '", action_name_, "'");
}

ActionClient::~ActionClient() {
    LOG_INFO("ActionClient", "Destroyed action client for '", action_name_, "'");
}

uint32_t ActionClient::sendGoal(const nlohmann::json& goal, FeedbackCallback fb_cb, ResultCallback res_cb) {
    nlohmann::json request = {{"goal", goal}};
    nlohmann::json response = goal_client_->call(request);
    
    if (!response["accepted"].get<bool>()) {
        throw std::runtime_error("Goal rejected");
    }
    
    uint32_t goal_id = response["goal_id"];
    bool has_pending_result = false;
    nlohmann::json pending_result;
    bool pending_success = false;

    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (fb_cb) feedback_callbacks_[goal_id] = fb_cb;
        if (res_cb) result_callbacks_[goal_id] = res_cb;
        auto pending_it = pending_results_.find(goal_id);
        if (pending_it != pending_results_.end()) {
            has_pending_result = true;
            pending_result = pending_it->second.result;
            pending_success = pending_it->second.success;
            pending_results_.erase(pending_it);
            result_received_[goal_id] = true;
        } else {
            result_received_[goal_id] = false;
        }
    }

    const bool is_check_goal =
        goal.contains("cmd") &&
        goal["cmd"].is_string() &&
        goal["cmd"].get<std::string>() == "check";
    if (!is_check_goal) {
        LOG_INFO("ActionClient", "Sent goal ", goal_id, " to action '", action_name_, "'");
    }

    if (has_pending_result && res_cb) {
        res_cb(goal_id, pending_result, pending_success);
    }
    
    return goal_id;
}

void ActionClient::cancelGoal(uint32_t goal_id) {
    nlohmann::json request = {{"goal_id", goal_id}};
    cancel_client_->call(request);
    
    LOG_INFO("ActionClient", "Cancelled goal ", goal_id);
}

bool ActionClient::waitForResult(uint32_t goal_id, int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    
    while (true) {
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            if (result_received_[goal_id]) {
                return true;
            }
        }
        
        if (timeout_ms > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start
            ).count();
            if (elapsed >= timeout_ms) {
                return false;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void ActionClient::handleFeedbackRaw(const std::string& data) {
    nlohmann::json fb_msg = nlohmann::json::parse(data);
    
    uint32_t goal_id = fb_msg["goal_id"];
    nlohmann::json feedback = fb_msg["feedback"];

    FeedbackCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        auto it = feedback_callbacks_.find(goal_id);
        if (it != feedback_callbacks_.end()) {
            callback = it->second;
        }
    }

    if (callback) {
        callback(goal_id, feedback);
    }
}

void ActionClient::handleResultRaw(const std::string& data) {
    nlohmann::json res_msg = nlohmann::json::parse(data);
    
    uint32_t goal_id = res_msg["goal_id"];
    nlohmann::json result = res_msg["result"];
    bool success = res_msg["success"];

    ResultCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        auto it = result_callbacks_.find(goal_id);
        if (it != result_callbacks_.end()) {
            callback = it->second;
            result_callbacks_.erase(it);
            feedback_callbacks_.erase(goal_id);
        } else {
            pending_results_[goal_id] = PendingResult{result, success};
        }
        result_received_[goal_id] = true;
    }

    if (callback) {
        callback(goal_id, result, success);
    }
}

} // namespace echo
