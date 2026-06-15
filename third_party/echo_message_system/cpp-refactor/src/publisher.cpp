#include "publisher.h"
#include "master.h"
#include "logger.h"
#include "context.h"
#include <sstream>

namespace echo {

Publisher::Publisher(const std::string& topic)
    : topic_(topic), context_(ContextManager::getGlobalContext()), port_(0) {
    
    socket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_PUB);
    
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
    
    LOG_INFO("Publisher", "Created publisher for topic '", topic_, "' on port ", port_);
    
    // 向 Master 注册
    registerToMaster();
}

Publisher::~Publisher() {
    if (socket_) {
        socket_->close();
    }
    LOG_INFO("Publisher", "Destroyed publisher for topic '", topic_, "'");
}

void Publisher::publish(const Message::Ptr& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 序列化消息为 JSON
    nlohmann::json j = msg->toJson();
    std::string payload = j.dump();
    
    // ZMQ PUB/SUB 格式: topic + " " + payload
    std::string zmq_msg = topic_ + " " + payload;
    
    zmq::message_t message(zmq_msg.data(), zmq_msg.size());
    socket_->send(message);
    
    LOG_DEBUG("Publisher", "Published message on topic '", topic_, "', size: ", zmq_msg.size());
}

void Publisher::publishRaw(const std::string& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // ZMQ PUB/SUB 格式: topic + " " + data
    std::string zmq_msg = topic_ + " " + data;
    
    zmq::message_t message(zmq_msg.data(), zmq_msg.size());
    socket_->send(message);
    
    LOG_DEBUG("Publisher", "Published raw data on topic '", topic_, "', size: ", zmq_msg.size());
}

void Publisher::registerToMaster() {
    try {
        MasterClient::getInstance().registerPublisher(topic_, port_);
        LOG_INFO("Publisher", "Registered topic '", topic_, "' with Master");
    } catch (const std::exception& e) {
        LOG_WARN("Publisher", "Failed to register with Master: ", e.what());
    }
}

} // namespace echo