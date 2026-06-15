#pragma once

#include "message.h"
#include <zmq.hpp>
#include <memory>
#include <string>
#include <functional>
#include <thread>
#include <atomic>

namespace echo {

/**
 * @brief 订阅者 - 每个订阅者独立 ZMQ SUB socket 和接收线程
 * 利用 ZMQ 原生 topic 过滤
 */
class Subscriber {
public:
    using Callback = std::function<void(const Message::Ptr&)>;
    using RawCallback = std::function<void(const std::string&)>;
    
    Subscriber(const std::string& topic, Callback callback);
    Subscriber(const std::string& topic, RawCallback callback, bool raw);  // raw=true 表示原始数据回调
    ~Subscriber();
    
    const std::string& getTopic() const { return topic_; }
    
private:
    std::string topic_;
    Callback callback_;
    RawCallback raw_callback_;
    bool use_raw_;
    std::shared_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> socket_;
    std::thread recv_thread_;
    std::atomic<bool> running_;
    
    void receiveLoop();
    void connectToPublishers();
};

} // namespace echo
