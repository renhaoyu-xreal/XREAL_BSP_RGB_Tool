#pragma once

#include <zmq.hpp>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <memory>
#include <tuple>
#include <nlohmann/json.hpp>

namespace echo {

/**
 * @brief ROS 风格的 Master 服务
 * 负责服务发现和地址查询
 */
class Master {
public:
    explicit Master(int port = 5590);
    ~Master();
    
    void start();
    void stop();
    
private:
    int port_;
    std::shared_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> socket_;
    bool running_;
    
    struct PublisherInfo {
        std::string address;
        int port;
    };
    
    struct ServiceInfo {
        std::string address;
        int port;
    };
    
    std::map<std::string, std::vector<PublisherInfo>> publishers_;  // topic -> publishers
    std::map<std::string, ServiceInfo> services_;                    // service_name -> service info
    std::mutex data_mutex_;
    
    void handleRequest(const nlohmann::json& request, nlohmann::json& response);
    void registerPublisher(const nlohmann::json& req, nlohmann::json& resp);
    void queryPublishers(const nlohmann::json& req, nlohmann::json& resp);
    void registerService(const nlohmann::json& req, nlohmann::json& resp);
    void queryService(const nlohmann::json& req, nlohmann::json& resp);
};

/**
 * @brief Master 客户端 - 用于向 Master 注册和查询
 */
class MasterClient {
public:
    explicit MasterClient(const std::string& master_address = "127.0.0.1", int master_port = 5590);
    ~MasterClient();
    
    bool registerPublisher(const std::string& topic, int port);
    std::vector<std::pair<std::string, int>> queryPublishers(const std::string& topic);
    bool registerService(const std::string& service_name, int port);
    std::vector<std::tuple<std::string, std::string, int>> queryService(const std::string& service_name);
    
    static MasterClient& getInstance();
    
private:
    std::string master_address_;
    int master_port_;
    std::shared_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> socket_;
    std::mutex socket_mutex_;
    int request_timeout_ms_ = 1000;
    
    nlohmann::json sendRequest(const nlohmann::json& request);
    void resetSocketLocked();
};

} // namespace echo
