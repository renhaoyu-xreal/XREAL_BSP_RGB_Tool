#include "subscriber.h"
#include "master.h"
#include "logger.h"
#include "context.h"
#include <set>
#include <thread>
#include <chrono>

namespace echo {

Subscriber::Subscriber(const std::string& topic, Callback callback)
    : topic_(topic), callback_(std::move(callback)), use_raw_(false),
      context_(ContextManager::getGlobalContext()), running_(true) {
    
    socket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_SUB);
    
    // 设置 ZMQ topic 过滤
    socket_->setsockopt(ZMQ_SUBSCRIBE, topic_.c_str(), topic_.size());
    
    LOG_INFO("Subscriber", "Created subscriber for topic '", topic_, "'");
    
    // 连接到发布者
    connectToPublishers();
    
    // 启动接收线程
    recv_thread_ = std::thread(&Subscriber::receiveLoop, this);
}

Subscriber::Subscriber(const std::string& topic, RawCallback callback, bool raw)
    : topic_(topic), raw_callback_(std::move(callback)), use_raw_(raw),
      context_(ContextManager::getGlobalContext()), running_(true) {
    
    socket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_SUB);
    
    // 设置 ZMQ topic 过滤
    socket_->setsockopt(ZMQ_SUBSCRIBE, topic_.c_str(), topic_.size());
    
    LOG_INFO("Subscriber", "Created subscriber for topic '", topic_, "' (raw mode)");
    
    // 连接到发布者
    connectToPublishers();
    
    // 启动接收线程
    recv_thread_ = std::thread(&Subscriber::receiveLoop, this);
}

Subscriber::~Subscriber() {
    running_ = false;
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
    if (socket_) {
        socket_->close();
    }
    LOG_INFO("Subscriber", "Destroyed subscriber for topic '", topic_, "'");
}

void Subscriber::connectToPublishers() {
    try {
        auto publishers = MasterClient::getInstance().queryPublishers(topic_);
        for (const auto& [address, port] : publishers) {
            std::string endpoint = "tcp://" + address + ":" + std::to_string(port);
            socket_->connect(endpoint);
            LOG_INFO("Subscriber", "Connected to publisher at ", endpoint);
        }
    } catch (const std::exception& e) {
        LOG_WARN("Subscriber", "Failed to connect to publishers: ", e.what());
    }
}

void Subscriber::receiveLoop() {
    LOG_INFO("Subscriber", "Receive thread started for topic '", topic_, "'");
    
    std::set<std::string> connected_endpoints;
    int reconnect_counter = 0;
    
    while (running_) {
        try {
            // 每 500 次循环尝试重新连接一次（约 500ms） 确保能及时发现新加入的发布者
            if (reconnect_counter++ >= 500) {
                reconnect_counter = 0;
                try {
                    auto publishers = MasterClient::getInstance().queryPublishers(topic_);
                    for (const auto& [address, port] : publishers) {
                        std::string endpoint = "tcp://" + address + ":" + std::to_string(port);
                        if (connected_endpoints.find(endpoint) == connected_endpoints.end()) {
                            socket_->connect(endpoint);
                            connected_endpoints.insert(endpoint);
                            LOG_DEBUG("Subscriber", "Connected to publisher at ", endpoint);
                        }
                    }
                } catch (...) {
                    // 忽略连接错误，继续接收
                }
            }
            
            // 批量 drain：一次把 socket 里所有积压消息全部读完，
            // 避免每轮只收一条 + sleep 导致吞吐瓶颈。
            // 上限 4000 条防止在消息风暴时长期独占 CPU。
            bool received_any = false;
            for (int batch = 0; batch < 4000; ++batch) {
                zmq::message_t zmq_msg;
                auto result = socket_->recv(&zmq_msg, ZMQ_DONTWAIT);
                if (!result) break;
                received_any = true;
                
                std::string msg_str(static_cast<char*>(zmq_msg.data()), zmq_msg.size());
                
                // 解析: topic + " " + payload
                size_t space_pos = msg_str.find(' ');
                if (space_pos == std::string::npos) {
                    LOG_WARN("Subscriber", "Invalid message format");
                    continue;
                }
                
                std::string received_topic = msg_str.substr(0, space_pos);
                std::string payload = msg_str.substr(space_pos + 1);
                
                if (received_topic != topic_) {
                    continue;
                }
                
                // 如果使用原始数据回调
                if (use_raw_) {
                    if (raw_callback_) {
                        raw_callback_(payload);
                        LOG_DEBUG("Subscriber", "Delivered raw data on topic '", topic_, "'");
                    }
                } else {
                    // 反序列化
                    nlohmann::json j = nlohmann::json::parse(payload);
                    Message::Ptr msg = MessageFactory::fromJson(j);
                    
                    if (msg && callback_) {
                        callback_(msg);
                        LOG_DEBUG("Subscriber", "Delivered message on topic '", topic_, "'");
                    }
                }
            }
            
            if (!received_any) {
                // 没有消息时短暂休眠，1ms 足够保证低延迟唤醒。
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Subscriber", "Error in receive loop: ", e.what());
        }
    }
    
    LOG_INFO("Subscriber", "Receive thread stopped for topic '", topic_, "'");
}

} // namespace echo