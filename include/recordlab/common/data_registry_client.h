/*
 * 数据注册客户端 - 供 node 调用以注册自定义数据
 *
 * 这个模块在 common 中，可以被 subnodes 依赖，避免 subnodes 依赖 backend
 *
 */
#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

// 前向声明 echo::Publisher
namespace echo {
class Publisher;
}

namespace recordlab::common {

struct DataRegistryResult {
  bool success = false;
  std::string message;
  std::string dataName;
};

class DataRegistryClient {
public:
  /*
   * 初始化注册客户端
   *
   * @param registryHost 注册服务器地址（保留参数，实际不使用）
   * @param registryPort 注册服务器端口
   */
  explicit DataRegistryClient(const std::string &registryHost = "127.0.0.1",
                              int registryPort = 5750);
  ~DataRegistryClient();

  /*
   * 向注册服务器注册自定义数据
   *
   * @param dataName  数据名称
   * @param dataType  数据类型（"double" 或 "vector"）
   * @param port      数据所在端口
   * @param nodeName  node 名称
   * @return          注册结果
   */
  DataRegistryResult registerData(const std::string &dataName,
                                  const std::string &dataType, int port,
                                  const std::string &nodeName = "unknown");

private:
  std::string host_;
  int port_;
  std::unique_ptr<echo::Publisher> publisher_;
};

} // namespace recordlab::common
