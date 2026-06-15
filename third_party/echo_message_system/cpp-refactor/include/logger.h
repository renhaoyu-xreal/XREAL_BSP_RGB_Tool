#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <mutex>
#include <memory>
#include <chrono>
#include <iomanip>
#include <ctime>

/**
 * @namespace echo
 * @brief Echo消息系统命名空间
 */
namespace echo {

/**
 * @enum LogLevel
 * @brief 日志级别枚举
 */
enum class LogLevel {
    DEBUG = 0,   ///< 调试信息
    INFO = 1,    ///< 一般信息
    WARN = 2,    ///< 警告信息
    ERROR = 3,   ///< 错误信息
    FATAL = 4    ///< 致命错误
};

/**
 * @class Logger
 * @brief 轻量级日志管理器
 * @details 支持多日志级别、线程安全、可选文件输出
 */
class Logger {
public:
    /**
     * @brief 获取日志管理器单例
     */
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    /**
     * @brief 设置最小日志级别
     * @param level 日志级别
     */
    void setLogLevel(LogLevel level) {
        std::lock_guard<std::mutex> lock(mutex_);
        min_level_ = level;
    }

    /**
     * @brief 获取当前日志级别
     */
    LogLevel getLogLevel() const {
        return min_level_;
    }

    /**
     * @brief 启用文件日志
     * @param filename 日志文件路径
     */
    void enableFileLogging(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_stream_.open(filename, std::ios::app);
        if (file_stream_.is_open()) {
            file_logging_enabled_ = true;
        }
    }

    /**
     * @brief 禁用文件日志
     */
    void disableFileLogging() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_stream_.is_open()) {
            file_stream_.close();
        }
        file_logging_enabled_ = false;
    }

    /**
     * @brief 启用/禁用控制台输出
     * @param enabled 是否启用
     */
    void setConsoleOutput(bool enabled) {
        console_output_enabled_ = enabled;
    }

    /**
     * @brief 启用/禁用颜色输出
     * @param enabled 是否启用
     */
    void setColorOutput(bool enabled) {
        color_enabled_ = enabled;
    }

    /**
     * @brief 记录日志
     * @param level 日志级别
     * @param module 模块名称
     * @param message 日志消息
     */
    void log(LogLevel level, const std::string& module, const std::string& message) {
        if (level < min_level_) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string timestamp = getCurrentTimestamp();
        std::string level_str = getLevelString(level);
        std::string formatted_msg = formatMessage(timestamp, level_str, module, message);

        // 控制台输出
        if (console_output_enabled_) {
            if (color_enabled_) {
                std::cout << getColorCode(level) << formatted_msg << "\033[0m" << std::endl;
            } else {
                std::cout << formatted_msg << std::endl;
            }
        }

        // 文件输出
        if (file_logging_enabled_ && file_stream_.is_open()) {
            file_stream_ << formatted_msg << std::endl;
            file_stream_.flush();
        }
    }

    /**
     * @brief DEBUG级别日志
     */
    template<typename... Args>
    void debug(const std::string& module, Args&&... args) {
        log(LogLevel::DEBUG, module, concat(std::forward<Args>(args)...));
    }

    /**
     * @brief INFO级别日志
     */
    template<typename... Args>
    void info(const std::string& module, Args&&... args) {
        log(LogLevel::INFO, module, concat(std::forward<Args>(args)...));
    }

    /**
     * @brief WARN级别日志
     */
    template<typename... Args>
    void warn(const std::string& module, Args&&... args) {
        log(LogLevel::WARN, module, concat(std::forward<Args>(args)...));
    }

    /**
     * @brief ERROR级别日志
     */
    template<typename... Args>
    void error(const std::string& module, Args&&... args) {
        log(LogLevel::ERROR, module, concat(std::forward<Args>(args)...));
    }

    /**
     * @brief FATAL级别日志
     */
    template<typename... Args>
    void fatal(const std::string& module, Args&&... args) {
        log(LogLevel::FATAL, module, concat(std::forward<Args>(args)...));
    }

private:
    Logger() 
        : min_level_(LogLevel::INFO)
        , console_output_enabled_(true)
        , file_logging_enabled_(false)
        , color_enabled_(true) {}

    ~Logger() {
        if (file_stream_.is_open()) {
            file_stream_.close();
        }
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /**
     * @brief 获取当前时间戳字符串
     */
    std::string getCurrentTimestamp() const {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm tm_buf;
        localtime_r(&time_t_now, &tm_buf);

        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }

    /**
     * @brief 获取日志级别字符串
     */
    std::string getLevelString(LogLevel level) const {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::FATAL: return "FATAL";
            default: return "UNKNOWN";
        }
    }

    /**
     * @brief 获取日志级别颜色代码
     */
    std::string getColorCode(LogLevel level) const {
        switch (level) {
            case LogLevel::DEBUG: return "\033[36m";  // Cyan
            case LogLevel::INFO:  return "\033[32m";  // Green
            case LogLevel::WARN:  return "\033[33m";  // Yellow
            case LogLevel::ERROR: return "\033[31m";  // Red
            case LogLevel::FATAL: return "\033[35m";  // Magenta
            default: return "\033[0m";
        }
    }

    /**
     * @brief 格式化日志消息
     */
    std::string formatMessage(const std::string& timestamp, 
                              const std::string& level,
                              const std::string& module,
                              const std::string& message) const {
        std::ostringstream oss;
        oss << "[" << timestamp << "] [" << level << "] [" << module << "] " << message;
        return oss.str();
    }

    /**
     * @brief 连接多个参数为字符串
     */
    template<typename T>
    std::string concat(T&& arg) {
        std::ostringstream oss;
        oss << std::forward<T>(arg);
        return oss.str();
    }

    template<typename T, typename... Args>
    std::string concat(T&& first, Args&&... args) {
        std::ostringstream oss;
        oss << std::forward<T>(first);
        oss << concat(std::forward<Args>(args)...);
        return oss.str();
    }

    LogLevel min_level_;
    bool console_output_enabled_;
    bool file_logging_enabled_;
    bool color_enabled_;
    std::ofstream file_stream_;
    mutable std::mutex mutex_;
};

// 便捷宏定义
#define LOG_DEBUG(module, ...) echo::Logger::getInstance().debug(module, __VA_ARGS__)
#define LOG_INFO(module, ...)  echo::Logger::getInstance().info(module, __VA_ARGS__)
#define LOG_WARN(module, ...)  echo::Logger::getInstance().warn(module, __VA_ARGS__)
#define LOG_ERROR(module, ...) echo::Logger::getInstance().error(module, __VA_ARGS__)
#define LOG_FATAL(module, ...) echo::Logger::getInstance().fatal(module, __VA_ARGS__)

} // namespace echo
