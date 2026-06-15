#pragma once

#include <zmq.hpp>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include "master.h"

namespace echo {

/**
 * @brief 参数服务器 - 基于ROS Master实现的参数管理系统
 * 负责参数的存储、查询和更新
 * 
 * 支持的操作:
 * - set_param: 设置参数
 * - get_param: 获取参数
 * - delete_param: 删除参数
 * - list_params: 列出所有参数
 * - search_param: 搜索参数
 * - has_param: 检查参数是否存在
 */
class ParameterServer {
public:
    explicit ParameterServer(int port = 5591);
    ~ParameterServer();
    
    /**
     * @brief 启动参数服务器
     */
    void start();
    
    /**
     * @brief 停止参数服务器
     */
    void stop();
    
    /**
     * @brief 注册到ROS Master
     */
    void registerToMaster(const std::string& master_address = "127.0.0.1", int master_port = 5590);
    
    bool isRunning() const { return running_; }
    int getPort() const { return port_; }
    
private:
    int port_;
    std::shared_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> socket_;
    std::unique_ptr<zmq::socket_t> sub_socket_;  // 用于订阅参数更新
    bool running_;
    
    // 参数存储: 使用嵌套JSON支持参数分组 (如 /robot/name)
    std::map<std::string, nlohmann::json> parameters_;
    std::mutex params_mutex_;
    
    // 处理请求
    void handleRequest(const nlohmann::json& request, nlohmann::json& response);
    
    // 参数操作
    void setParam(const nlohmann::json& req, nlohmann::json& resp);
    void getParam(const nlohmann::json& req, nlohmann::json& resp);
    void deleteParam(const nlohmann::json& req, nlohmann::json& resp);
    void listParams(const nlohmann::json& req, nlohmann::json& resp);
    void searchParam(const nlohmann::json& req, nlohmann::json& resp);
    void hasParam(const nlohmann::json& req, nlohmann::json& resp);
};

/**
 * @brief 参数客户端 - 用于与参数服务器交互
 */
class ParameterClient {
public:
    /**
     * @brief 构造参数客户端
     * @param param_server_address 参数服务器地址
     * @param param_server_port 参数服务器端口
     */
    explicit ParameterClient(const std::string& param_server_address = "127.0.0.1", 
                            int param_server_port = 5591);
    
    ~ParameterClient();
    
    /**
     * @brief 设置参数
     * @param param_name 参数名称 (如 "/robot/name")
     * @param value 参数值 (支持所有JSON类型)
     * @return 是否设置成功
     */
    bool setParam(const std::string& param_name, const nlohmann::json& value);
    
    /**
     * @brief 获取参数
     * @param param_name 参数名称
     * @param value 存储参数值的引用
     * @return 是否获取成功
     */
    bool getParam(const std::string& param_name, nlohmann::json& value);
    
    /**
     * @brief 获取参数 (模板版本，支持类型转换)
     * @param param_name 参数名称
     * @return 参数值
     */
    template<typename T>
    T getParam(const std::string& param_name) {
        nlohmann::json value;
        if (getParam(param_name, value)) {
            return value.get<T>();
        }
        throw std::runtime_error("Parameter not found: " + param_name);
    }
    
    /**
     * @brief 删除参数
     * @param param_name 参数名称
     * @return 是否删除成功
     */
    bool deleteParam(const std::string& param_name);
    
    /**
     * @brief 列出所有参数
     * @return 参数名称列表
     */
    std::vector<std::string> listParams();
    
    /**
     * @brief 搜索参数 (支持通配符 *)
     * @param search_pattern 搜索模式
     * @return 匹配的参数名称列表
     */
    std::vector<std::string> searchParam(const std::string& search_pattern);
    
    /**
     * @brief 检查参数是否存在
     * @param param_name 参数名称
     * @return 是否存在
     */
    bool hasParam(const std::string& param_name);
    
    /**
     * @brief 获取单例实例
     */
    static ParameterClient& getInstance();
    
    /**
     * @brief 从参数服务器加载参数文件
     * @param filename 参数文件路径
     * @return 是否加载成功
     */
    bool loadParamFile(const std::string& filename);
    
    /**
     * @brief 保存参数到文件
     * @param filename 保存文件路径
     * @return 是否保存成功
     */
    bool saveParamFile(const std::string& filename);
    

    
private:
    std::string param_server_address_;
    int param_server_port_;
    std::shared_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> socket_;
    std::mutex socket_mutex_;
    
    // 发送请求并获取响应
    nlohmann::json sendRequest(const nlohmann::json& request);
};

} // namespace echo