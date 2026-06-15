/*
 * DataRegistryServer 实现
 */
#include "recordlab/backend/data_registry_server.h"
#include <chrono>
#include <iostream>
#include <subscriber.h>

namespace recordlab::backend {

DataRegistryServer::DataRegistryServer(const std::string &host, int port,
                                       QObject *parent)
    : QObject(parent), host_(host), port_(port) {}

// 析构时停止订阅器，确保服务退出后不再接收新的注册广播。
DataRegistryServer::~DataRegistryServer() { stop(); }

nlohmann::json DataRegistryServer::registerData(const std::string &dataName,
                                                const std::string &dataType,
                                                int port,
                                                const std::string &nodeName) {
  // 将数据主题登记到内存表中，并在需要时通知上层补充订阅。
  try {
    double now = std::chrono::duration<double>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    registeredData_[dataName] = {{"type", dataType},
                                 {"port", port},
                                 {"node_name", nodeName},
                                 {"registered_at", now}};
    std::cout << "[Registry] Registered data: " << dataName
              << " (type=" << dataType << ", port=" << port
              << ", node=" << nodeName << ")" << std::endl;

    if (onDataRegistered_) {
      try {
        onDataRegistered_(dataName, dataType, port);
      } catch (...) {
      }
    }
    return {{"success", true},
            {"message", "Data '" + dataName + "' registered"},
            {"data_name", dataName}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}, {"data_name", dataName}};
  }
}

nlohmann::json DataRegistryServer::unregisterData(const std::string &dataName) {
  // 按名称移除一个已注册主题，通常在节点下线或重启时调用。
  if (registeredData_.erase(dataName) == 0) {
    return {{"success", false},
            {"message", "Data '" + dataName + "' not found"},
            {"data_name", dataName}};
  }
  return {{"success", true},
          {"message", "Data '" + dataName + "' unregistered"},
          {"data_name", dataName}};
}

std::unordered_map<std::string, nlohmann::json>
DataRegistryServer::getRegisteredData() const {
  // 返回当前已登记主题的快照，供调试或界面展示使用。
  return registeredData_;
}

void DataRegistryServer::registerCallback(DataRegisteredCallback callback) {
  // 注册“有新数据被登记”回调，让外部可以动态扩展订阅集合。
  onDataRegistered_ = std::move(callback);
}

void DataRegistryServer::onRegistrationRequest(const std::string &rawData) {
  // 解析广播请求；当前只识别 register 动作，其余内容静默忽略。
  try {
    auto data = nlohmann::json::parse(rawData);
    if (data.value("action", "") == "register") {
      registerData(data.value("data_name", std::string()),
                   data.value("data_type", std::string()),
                   data.value("port", 0),
                   data.value("node_name", std::string("unknown")));
    }
  } catch (const std::exception &e) {
    std::cerr << "[Registry] Error processing request: " << e.what()
              << std::endl;
  }
}

void DataRegistryServer::start() {
  // 订阅固定注册 topic，开始从 echo 总线接收各节点发来的注册消息。
  try {
    subscriber_ = std::make_unique<echo::Subscriber>(
        "data_registry_requests",
        [this](const std::string &data) { onRegistrationRequest(data); }, true);
    running_ = true;
    std::cout << "[Registry] Subscriber started on " << host_ << ":" << port_
              << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "[Registry] Failed to start: " << e.what() << std::endl;
    running_ = false;
  }
}

void DataRegistryServer::stop() {
  // 释放订阅器并复位运行标志，表示注册服务已完全停止。
  subscriber_.reset();
  running_ = false;
  std::cout << "[Registry] Server stopped" << std::endl;
}

} // namespace recordlab::backend
