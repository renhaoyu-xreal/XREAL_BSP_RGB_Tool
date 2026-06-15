#pragma once

#include "message.h"
#include <zmq.hpp>
#include <memory>
#include <string>
#include <mutex>

namespace echo {

/**
 * @brief 发布者 - 每个发布者独立 ZMQ PUB socket
 */
class Publisher {
public:
    explicit Publisher(const std::string& topic);
    ~Publisher();
    
    void publish(const Message::Ptr& msg);
    void publishRaw(const std::string& data);  // 发布原始字符串数据
    int getPort() const { return port_; }
    const std::string& getTopic() const { return topic_; }
    
private:
    std::string topic_;
    std::shared_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> socket_;
    int port_;
    std::mutex mutex_;
    
    void registerToMaster();
};

} // namespace echo
