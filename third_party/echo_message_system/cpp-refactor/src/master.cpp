#include "master.h"
#include "logger.h"
#include "context.h"
#include <thread>
#include <chrono>

namespace echo {

// ==================== Master ====================

Master::Master(int port) : port_(port), running_(false) {
    context_ = ContextManager::getGlobalContext();
    socket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_REP);
}

Master::~Master() {
    stop();
}

void Master::start() {
    std::string endpoint = "tcp://*:" + std::to_string(port_);
    socket_->bind(endpoint);
    running_ = true;
    
    LOG_INFO("Master", "Master started on port ", port_);
    
    while (running_) {
        try {
            zmq::message_t request_msg;
            auto result = socket_->recv(&request_msg, ZMQ_DONTWAIT);
            
            if (result) {
                std::string req_str(static_cast<char*>(request_msg.data()), request_msg.size());
                nlohmann::json request = nlohmann::json::parse(req_str);
                
                nlohmann::json response;
                handleRequest(request, response);
                
                std::string resp_str = response.dump();
                zmq::message_t response_msg(resp_str.data(), resp_str.size());
                socket_->send(response_msg);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Master", "Error handling request: ", e.what());
        }
    }
    
    LOG_INFO("Master", "Master stopped");
}

void Master::stop() {
    running_ = false;
    if (socket_) {
        socket_->close();
    }
}

void Master::handleRequest(const nlohmann::json& request, nlohmann::json& response) {
    std::string type = request["type"];
    
    if (type == "register_publisher") {
        registerPublisher(request, response);
    } else if (type == "query_publishers") {
        queryPublishers(request, response);
    } else if (type == "register_service") {
        registerService(request, response);
    } else if (type == "query_service") {
        queryService(request, response);
    } else {
        response["success"] = false;
        response["error"] = "Unknown request type";
    }
}

void Master::registerPublisher(const nlohmann::json& req, nlohmann::json& resp) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    std::string topic = req["topic"];
    std::string address = req.value("address", "127.0.0.1");
    int port = req["port"];
    
    publishers_[topic].push_back({address, port});
    
    LOG_INFO("Master", "Registered publisher for topic '", topic, "' at ", address, ":", port);
    
    resp["success"] = true;
}

void Master::queryPublishers(const nlohmann::json& req, nlohmann::json& resp) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    std::string pattern = req["topic"];
    
    nlohmann::json pubs = nlohmann::json::array();
    
    // 支持通配符查询
    for (const auto& [topic, publishers] : publishers_) {
        if (StringUtils::matchPattern(topic, pattern)) {
            for (const auto& pub : publishers) {
                pubs.push_back({
                    {"address", pub.address},
                    {"port", pub.port},
                    {"topic", topic}
                });
            }
        }
    }
    
    resp["publishers"] = pubs;
    resp["success"] = true;
}

void Master::registerService(const nlohmann::json& req, nlohmann::json& resp) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    std::string service_name = req["service_name"];
    std::string address = req.value("address", "127.0.0.1");
    int port = req["port"];
    
    services_[service_name] = {address, port};
    
    LOG_INFO("Master", "Registered service '", service_name, "' at ", address, ":", port);
    
    resp["success"] = true;
}

void Master::queryService(const nlohmann::json& req, nlohmann::json& resp) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    std::string pattern = req["service_name"];
    
    // 支持通配符查询
    nlohmann::json services = nlohmann::json::array();
    bool found = false;
    
    for (const auto& [service_name, svc] : services_) {
        if (StringUtils::matchPattern(service_name, pattern)) {
            services.push_back({
                {"service_name", service_name},
                {"address", svc.address},
                {"port", svc.port}
            });
            found = true;
        }
    }
    
    if (found) {
        resp["services"] = services;
        resp["success"] = true;
    } else {
        resp["success"] = false;
        resp["error"] = "No matching services found";
    }
}

// ==================== MasterClient ====================

MasterClient::MasterClient(const std::string& master_address, int master_port)
    : master_address_(master_address), master_port_(master_port) {
    
    context_ = ContextManager::getGlobalContext();
    resetSocketLocked();
}

MasterClient::~MasterClient() {
    if (socket_) {
        socket_->close();
    }
}

MasterClient& MasterClient::getInstance() {
    static MasterClient instance;
    return instance;
}

nlohmann::json MasterClient::sendRequest(const nlohmann::json& request) {
    std::lock_guard<std::mutex> lock(socket_mutex_);

    try {
        std::string req_str = request.dump();
        zmq::message_t request_msg(req_str.data(), req_str.size());
        auto send_result = socket_->send(request_msg);
        if (!send_result) {
            throw std::runtime_error("Master request send timeout");
        }

        zmq::message_t response_msg;
        auto recv_result = socket_->recv(&response_msg);
        if (!recv_result) {
            throw std::runtime_error("Master request receive timeout");
        }

        std::string resp_str(static_cast<char*>(response_msg.data()), response_msg.size());
        return nlohmann::json::parse(resp_str);
    } catch (...) {
        // REQ socket 在超时/半开状态后很容易进入不可复用状态。
        // 这里直接重建 socket，避免后续请求永久卡死。
        resetSocketLocked();
        throw;
    }
}

void MasterClient::resetSocketLocked() {
    if (socket_) {
        try {
            socket_->close();
        } catch (...) {
        }
    }

    socket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_REQ);

    int linger = 0;
    socket_->setsockopt(ZMQ_LINGER, &linger, sizeof(linger));
    socket_->setsockopt(ZMQ_SNDTIMEO, &request_timeout_ms_, sizeof(request_timeout_ms_));
    socket_->setsockopt(ZMQ_RCVTIMEO, &request_timeout_ms_, sizeof(request_timeout_ms_));

    std::string endpoint = "tcp://" + master_address_ + ":" + std::to_string(master_port_);
    socket_->connect(endpoint);

    LOG_INFO("MasterClient", "Connected to Master at ", endpoint,
             " (timeout=", request_timeout_ms_, "ms)");
}

bool MasterClient::registerPublisher(const std::string& topic, int port) {
    nlohmann::json request = {
        {"type", "register_publisher"},
        {"topic", topic},
        {"address", "127.0.0.1"},
        {"port", port}
    };
    
    nlohmann::json response = sendRequest(request);
    return response.value("success", false);
}

std::vector<std::pair<std::string, int>> MasterClient::queryPublishers(const std::string& topic) {
    nlohmann::json request = {
        {"type", "query_publishers"},
        {"topic", topic}
    };
    
    nlohmann::json response = sendRequest(request);
    
    std::vector<std::pair<std::string, int>> result;
    if (response.contains("publishers")) {
        for (const auto& pub : response["publishers"]) {
            result.emplace_back(pub["address"], pub["port"]);
        }
    }
    
    return result;
}

bool MasterClient::registerService(const std::string& service_name, int port) {
    nlohmann::json request = {
        {"type", "register_service"},
        {"service_name", service_name},
        {"address", "127.0.0.1"},
        {"port", port}
    };
    
    nlohmann::json response = sendRequest(request);
    return response.value("success", false);
}

std::vector<std::tuple<std::string, std::string, int>> MasterClient::queryService(const std::string& service_name) {
    nlohmann::json request = {
        {"type", "query_service"},
        {"service_name", service_name}
    };
    
    nlohmann::json response = sendRequest(request);
    std::vector<std::tuple<std::string, std::string, int>> results;
    
    if (response.value("success", false) && response.contains("services")) {
        for (const auto& service : response["services"]) {
            results.emplace_back(
                service["service_name"],
                service["address"],
                service["port"]
            );
        }
    }
    
    return results;
}

} // namespace echo
