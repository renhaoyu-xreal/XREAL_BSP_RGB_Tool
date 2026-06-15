#pragma once

#include <zmq.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace echo {

/**
 * @brief 字符串工具类 - 提供字符串处理和模式匹配功能
 */
class StringUtils {
public:
    /**
     * @brief 增强的通配符模式匹配
     * @param str 要匹配的字符串
     * @param pattern 匹配模式，支持 *（匹配0或多个字符）和 ?（匹配1个字符）
     * @return 是否匹配成功
     */
    static bool matchPattern(const std::string& str, const std::string& pattern);
    
    /**
     * @brief 将字符串分割为向量
     * @param str 要分割的字符串
     * @param delimiter 分隔符
     * @return 分割后的字符串向量
     */
    static std::vector<std::string> split(const std::string& str, char delimiter);
    
    /**
     * @brief 移除字符串两端的空白字符
     * @param str 要处理的字符串
     * @return 处理后的字符串
     */
    static std::string trim(const std::string& str);
};

/**
 * @brief 上下文管理器 - 管理全局 ZeroMQ 上下文
 */
class ContextManager {
public:
    /**
     * @brief 获取全局上下文实例
     * @return ZeroMQ 上下文指针
     */
    static std::shared_ptr<zmq::context_t> getGlobalContext();
    
private:
    ContextManager();
    ~ContextManager();
    
    static std::shared_ptr<zmq::context_t> global_context_;
    static std::mutex context_mutex_;
};

} // namespace echo