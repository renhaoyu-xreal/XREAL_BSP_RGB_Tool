#include "context.h"
#include <mutex>
#include <vector>
#include <algorithm>
#include <cctype>

namespace echo {

// ==================== StringUtils Implementation ====================

bool StringUtils::matchPattern(const std::string& str, const std::string& pattern) {
    size_t str_len = str.length();
    size_t pat_len = pattern.length();
    
    // 创建动态规划表
    std::vector<std::vector<bool>> dp(str_len + 1, std::vector<bool>(pat_len + 1, false));
    dp[0][0] = true; // 空字符串匹配空模式
    
    // 处理模式中的前导 *
    for (size_t j = 1; j <= pat_len; j++) {
        if (pattern[j - 1] == '*') {
            dp[0][j] = dp[0][j - 1];
        } else {
            break;
        }
    }
    
    // 填充DP表
    for (size_t i = 1; i <= str_len; i++) {
        for (size_t j = 1; j <= pat_len; j++) {
            if (pattern[j - 1] == '*') {
                // * 匹配0个或多个字符
                dp[i][j] = dp[i][j - 1] || dp[i - 1][j];
            } else if (pattern[j - 1] == '?' || pattern[j - 1] == str[i - 1]) {
                // ? 匹配1个字符，或字符完全匹配
                dp[i][j] = dp[i - 1][j - 1];
            }
        }
    }
    
    return dp[str_len][pat_len];
}

std::vector<std::string> StringUtils::split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    
    for (char c : str) {
        if (c == delimiter) {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else {
            token += c;
        }
    }
    
    if (!token.empty()) {
        tokens.push_back(token);
    }
    
    return tokens;
}

std::string StringUtils::trim(const std::string& str) {
    if (str.empty()) return str;
    
    size_t start = 0;
    size_t end = str.length() - 1;
    
    // 找到第一个非空白字符
    while (start <= end && std::isspace(static_cast<unsigned char>(str[start]))) {
        start++;
    }
    
    if (start > end) return "";
    
    // 找到最后一个非空白字符
    while (end >= start && std::isspace(static_cast<unsigned char>(str[end]))) {
        end--;
    }
    
    return str.substr(start, end - start + 1);
}

// ==================== ContextManager Implementation ====================

std::shared_ptr<zmq::context_t> ContextManager::global_context_ = nullptr;
std::mutex ContextManager::context_mutex_;

ContextManager::ContextManager() {}

ContextManager::~ContextManager() {}

std::shared_ptr<zmq::context_t> ContextManager::getGlobalContext() {
    std::lock_guard<std::mutex> lock(context_mutex_);
    
    if (!global_context_) {
        global_context_ = std::make_shared<zmq::context_t>(1);
    }
    
    return global_context_;
}

} // namespace echo