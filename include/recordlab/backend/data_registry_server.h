/*
 * DataRegistryServer - 数据注册服务器
 *
 * 允许 node 动态注册自定义数据
 *
 */
#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

namespace echo {
class Subscriber;
}

namespace recordlab::backend {

class DataRegistryServer : public QObject {
  Q_OBJECT

public:
  using DataRegisteredCallback = std::function<void(
      const std::string &dataName, const std::string &dataType, int port)>;

  explicit DataRegistryServer(const std::string &host = "127.0.0.1",
                              int port = 5750, QObject *parent = nullptr);
  ~DataRegistryServer() override;

  nlohmann::json registerData(const std::string &dataName,
                              const std::string &dataType, int port,
                              const std::string &nodeName = "unknown");

  nlohmann::json unregisterData(const std::string &dataName);

  std::unordered_map<std::string, nlohmann::json> getRegisteredData() const;

  void registerCallback(DataRegisteredCallback callback);

  void start();
  void stop();

private:
  void onRegistrationRequest(const std::string &rawData);

  std::string host_;
  int port_;
  bool running_ = false;

  std::unordered_map<std::string, nlohmann::json> registeredData_;
  DataRegisteredCallback onDataRegistered_;

  std::unique_ptr<echo::Subscriber> subscriber_;
};

} // namespace recordlab::backend
