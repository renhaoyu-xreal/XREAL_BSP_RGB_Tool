/*
 * 数据注册客户端 - 实现
 *
 */

#include "recordlab/common/data_registry_client.h"
#include <iostream>
#include <publisher.h>

namespace recordlab::common {

DataRegistryClient::DataRegistryClient(const std::string &registryHost,
                                       int registryPort)
    : host_(registryHost), port_(registryPort) {
  // 构造阶段只建立发布端，真正的注册动作在 registerData 时按需发送。
  try {
    // 创建 Publisher 用于发送注册请求
    // topic = "data_registry_requests"
    publisher_ = std::make_unique<echo::Publisher>("data_registry_requests");
    std::cout << "[RegistryClient] Connected to registry on port "
              << registryPort << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "[RegistryClient] Failed to connect to registry: " << e.what()
              << std::endl;
  }
}

DataRegistryClient::~DataRegistryClient() = default;

DataRegistryResult
DataRegistryClient::registerData(const std::string &dataName,
                                 const std::string &dataType, int port,
                                 const std::string &nodeName) {
  // 将当前节点提供的数据主题广播给注册中心，便于接收端动态发现新流。
  try {
    if (!publisher_) {
      std::cerr << "[RegistryClient] Publisher not initialized" << std::endl;
      return {false, "Publisher not initialized", dataName};
    }

    // 发送注册请求消息
    nlohmann::json request = {{"action", "register"},
                              {"data_name", dataName},
                              {"data_type", dataType},
                              {"port", port},
                              {"node_name", nodeName}};

    publisher_->publishRaw(request.dump());
    std::cout << "[RegistryClient] Sent registration request for " << dataName
              << " (type=" << dataType << ", port=" << port << ") from "
              << nodeName << std::endl;

    return {true, "Registration request sent for " + dataName, dataName};

  } catch (const std::exception &e) {
    std::cerr << "[RegistryClient] Exception while registering " << dataName
              << ": " << e.what() << std::endl;
    return {false, e.what(), dataName};
  }
}

} // namespace recordlab::common
