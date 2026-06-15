#pragma once

#include <zmq.hpp>
#include <memory>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

namespace echo {

/**
 * @brief 服务端 - 使用 ZMQ REP socket
 */
class ServiceServer {
public:
    using Callback = std::function<nlohmann::json(const nlohmann::json&)>;
    
    ServiceServer(const std::string& service_name, Callback callback);
    ~ServiceServer();
    
    int getPort() const { return port_; }
    const std::string& getName() const { return service_name_; }
    
private:
    std::string service_name_;
    Callback callback_;
    std::shared_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> socket_;
    int port_;
    std::thread service_thread_;
    std::atomic<bool> running_;
    
    void serviceLoop();
    void registerToMaster();
};

/**
 * @brief 服务客户端 - 使用 ZMQ REQ socket
 */
class ServiceClient {
public:
    explicit ServiceClient(const std::string& service_name);
    ~ServiceClient();
    
    nlohmann::json call(const nlohmann::json& request, int timeout_ms = 5000);
    
private:
    std::string service_name_;
    std::shared_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> socket_;
    std::mutex call_mutex_;

    void connectToServer();
};

} // namespace echo
