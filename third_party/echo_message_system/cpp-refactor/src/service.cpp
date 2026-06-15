#include "service.h"
#include "master.h"
#include "logger.h"
#include "context.h"

namespace echo {

// ==================== ServiceServer ====================

ServiceServer::ServiceServer(const std::string& service_name, Callback callback)
    : service_name_(service_name), callback_(std::move(callback)),
      context_(ContextManager::getGlobalContext()), port_(0), running_(true) {
    
    socket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_REP);
    
    // 绑定到随机端口
    socket_->bind("tcp://*:0");
    
    // 获取实际端口
    char endpoint[256];
    size_t size = sizeof(endpoint);
    socket_->getsockopt(ZMQ_LAST_ENDPOINT, endpoint, &size);
    std::string ep(endpoint);
    auto pos = ep.rfind(':');
    if (pos != std::string::npos) {
        port_ = std::stoi(ep.substr(pos + 1));
    }
    
    LOG_INFO("ServiceServer", "Created service '", service_name_, "' on port ", port_);
    
    // 向 Master 注册
    registerToMaster();
    
    // 启动服务线程
    service_thread_ = std::thread(&ServiceServer::serviceLoop, this);
}

ServiceServer::~ServiceServer() {
    running_ = false;
    if (service_thread_.joinable()) {
        service_thread_.join();
    }
    if (socket_) {
        socket_->close();
    }
    LOG_INFO("ServiceServer", "Destroyed service '", service_name_, "'");
}

void ServiceServer::serviceLoop() {
    LOG_INFO("ServiceServer", "Service thread started for '", service_name_, "'");
    
    while (running_) {
        try {
            zmq::message_t request_msg;
            auto result = socket_->recv(&request_msg, ZMQ_DONTWAIT);
            
            if (result) {
                std::string req_str(static_cast<char*>(request_msg.data()), request_msg.size());
                nlohmann::json request = nlohmann::json::parse(req_str);
                
                LOG_DEBUG("ServiceServer", "Received request for service '", service_name_, "'");
                
                // 调用回调处理
                nlohmann::json response;
                try {
                    response = callback_(request);
                    response["success"] = true;
                } catch (const std::exception& e) {
                    response["success"] = false;
                    response["error"] = e.what();
                }
                
                // 发送响应
                std::string resp_str = response.dump();
                zmq::message_t response_msg(resp_str.data(), resp_str.size());
                socket_->send(response_msg);
                
                LOG_DEBUG("ServiceServer", "Sent response for service '", service_name_, "'");
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } catch (const std::exception& e) {
            LOG_ERROR("ServiceServer", "Error in service loop: ", e.what());
        }
    }
    
    LOG_INFO("ServiceServer", "Service thread stopped for '", service_name_, "'");
}

void ServiceServer::registerToMaster() {
    try {
        MasterClient::getInstance().registerService(service_name_, port_);
        LOG_INFO("ServiceServer", "Registered service '", service_name_, "' with Master");
    } catch (const std::exception& e) {
        LOG_WARN("ServiceServer", "Failed to register with Master: ", e.what());
    }
}

// ==================== ServiceClient ====================

ServiceClient::ServiceClient(const std::string& service_name)
    : service_name_(service_name), context_(ContextManager::getGlobalContext()) {
    
    socket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_REQ);
    int linger_ms = 0;
    socket_->setsockopt(ZMQ_LINGER, &linger_ms, sizeof(linger_ms));
    
    // 连接到服务器
    connectToServer();
    
    LOG_INFO("ServiceClient", "Created client for service '", service_name_, "'");
}

ServiceClient::~ServiceClient() {
    if (socket_) {
        socket_->close();
    }
    LOG_INFO("ServiceClient", "Destroyed client for service '", service_name_, "'");
}

void ServiceClient::connectToServer() {
    auto services = MasterClient::getInstance().queryService(service_name_);
    if (!services.empty()) {
        // 选择第一个匹配的服务实例
        auto& [svc_name, address, port] = services[0];
        std::string endpoint = "tcp://" + address + ":" + std::to_string(port);
        socket_->connect(endpoint);
        LOG_INFO("ServiceClient", "Connected to service '", service_name_, "' at ", endpoint);
    } else {
        throw std::runtime_error("Service not found: " + service_name_);
    }
}

nlohmann::json ServiceClient::call(const nlohmann::json& request, int timeout_ms) {
    std::lock_guard<std::mutex> lock(call_mutex_);

    const auto reconnect = [this]() {
        try {
            if (socket_) {
                int linger_ms = 0;
                socket_->setsockopt(ZMQ_LINGER, &linger_ms, sizeof(linger_ms));
                socket_->close();
            }
        } catch (...) {
        }

        socket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_REQ);
        int linger_ms = 0;
        socket_->setsockopt(ZMQ_LINGER, &linger_ms, sizeof(linger_ms));
        connectToServer();
    };

    try {
        socket_->setsockopt(ZMQ_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
        socket_->setsockopt(ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

        std::string req_str = request.dump();
        zmq::message_t request_msg(req_str.data(), req_str.size());
        socket_->send(request_msg);

        LOG_DEBUG("ServiceClient", "Sent request to service '", service_name_, "'");

        zmq::message_t response_msg;
        auto result = socket_->recv(&response_msg);

        if (!result) {
            throw std::runtime_error("Service call timeout: " + service_name_);
        }

        std::string resp_str(static_cast<char*>(response_msg.data()), response_msg.size());
        nlohmann::json response = nlohmann::json::parse(resp_str);

        LOG_DEBUG("ServiceClient", "Received response from service '", service_name_, "'");
        return response;
    } catch (...) {
        try {
            reconnect();
        } catch (...) {
        }
        throw;
    }
}

} // namespace echo
