#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace echo {

// Base64 编码/解码函数
inline std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char* base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    std::string result;
    int i = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];
    
    for (size_t n = 0; n < data.size(); n++) {
        char_array_3[i++] = data[n];
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            
            for (i = 0; i < 4; i++) result += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    
    if (i > 0) {
        for (int j = i; j < 3; j++) char_array_3[j] = '\0';
        
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        
        for (int j = 0; j <= i; j++) result += base64_chars[char_array_4[j]];
        while (i++ < 3) result += '=';
    }
    
    return result;
}

inline std::vector<uint8_t> base64_decode(const std::string& encoded_string) {
    static const std::string base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    std::vector<uint8_t> result;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;
    
    int val = 0, valb = -6;
    for (unsigned char c : encoded_string) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            result.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    
    return result;
}

/**
 * @brief 消息基类
 */
class Message {
public:
    using Ptr = std::shared_ptr<Message>;
    
    explicit Message(const std::string& topic) : topic_(topic) {}
    virtual ~Message() = default;
    
    virtual std::string getType() const = 0;
    virtual nlohmann::json toJson() const = 0;
    
    const std::string& getTopic() const { return topic_; }
    
protected:
    std::string topic_;
};

/**
 * @brief IMU 数据消息
 */
class ImuMessage : public Message {
public:
    ImuMessage(const std::string& topic, uint64_t timestamp_ns, int type,
               double d1, double d2, double d3, double d4, double d5, double d6)
        : Message(topic), timestamp_ns_(timestamp_ns), data_type_(type) {
        data_[0] = d1; data_[1] = d2; data_[2] = d3;
        data_[3] = d4; data_[4] = d5; data_[5] = d6;
    }
    
    std::string getType() const override { return "ImuMessage"; }
    
    nlohmann::json toJson() const override {
        return {
            {"type", getType()},
            {"topic", topic_},
            {"timestamp_ns", timestamp_ns_},
            {"data_type", data_type_},
            {"data", {data_[0], data_[1], data_[2], data_[3], data_[4], data_[5]}}
        };
    }
    
    static Ptr fromJson(const nlohmann::json& j) {
        auto data = j["data"];
        return std::make_shared<ImuMessage>(
            j["topic"], j["timestamp_ns"], j["data_type"],
            data[0], data[1], data[2], data[3], data[4], data[5]
        );
    }
    
    uint64_t getTimestamp() const { return timestamp_ns_; }
    const double* getData() const { return data_; }
    
private:
    uint64_t timestamp_ns_;
    int data_type_;
    double data_[6];
};

/**
 * @brief 延迟数据消息
 */
class LatencyMessage : public Message {
public:
    LatencyMessage(const std::string& topic, uint64_t timestamp_ns, int type,
                   double d1, double d2, double d3, double d4, double d5, double d6)
        : Message(topic), timestamp_ns_(timestamp_ns), data_type_(type) {
        data_[0] = d1; data_[1] = d2; data_[2] = d3;
        data_[3] = d4; data_[4] = d5; data_[5] = d6;
    }
    
    std::string getType() const override { return "LatencyMessage"; }
    
    nlohmann::json toJson() const override {
        return {
            {"type", getType()},
            {"topic", topic_},
            {"timestamp_ns", timestamp_ns_},
            {"data_type", data_type_},
            {"data", {data_[0], data_[1], data_[2], data_[3], data_[4], data_[5]}}
        };
    }
    
    static Ptr fromJson(const nlohmann::json& j) {
        auto data = j["data"];
        return std::make_shared<LatencyMessage>(
            j["topic"], j["timestamp_ns"], j["data_type"],
            data[0], data[1], data[2], data[3], data[4], data[5]
        );
    }
    
    uint64_t getTimestamp() const { return timestamp_ns_; }
    const double* getData() const { return data_; }
    
private:
    uint64_t timestamp_ns_;
    int data_type_;
    double data_[6];
};

/**
 * @brief 图像数据消息
 */
class ImageMessage : public Message {
public:
    ImageMessage(const std::string& topic, const std::vector<uint8_t>& data, 
                 const std::string& filename = "")
        : Message(topic), image_data_(data), filename_(filename) {}
    
    std::string getType() const override { return "ImageMessage"; }
    
    nlohmann::json toJson() const override {
        return {
            {"type", getType()},
            {"topic", topic_},
            {"filename", filename_},
            {"data", base64_encode(image_data_)}
        };
    }
    
    static Ptr fromJson(const nlohmann::json& j) {
        std::vector<uint8_t> data = base64_decode(j["data"]);
        return std::make_shared<ImageMessage>(j["topic"], data, j.value("filename", ""));
    }
    
    const std::vector<uint8_t>& getData() const { return image_data_; }
    const std::string& getFilename() const { return filename_; }
    
private:
    std::vector<uint8_t> image_data_;
    std::string filename_;
};

/**
 * @brief 二进制数据消息
 */
class BinaryMessage : public Message {
public:
    BinaryMessage(const std::string& topic, const std::vector<uint8_t>& data)
        : Message(topic), binary_data_(data) {}
    
    std::string getType() const override { return "BinaryMessage"; }
    
    nlohmann::json toJson() const override {
        return {
            {"type", getType()},
            {"topic", topic_},
            {"data", base64_encode(binary_data_)}
        };
    }
    
    static Ptr fromJson(const nlohmann::json& j) {
        std::vector<uint8_t> data = base64_decode(j["data"]);
        return std::make_shared<BinaryMessage>(j["topic"], data);
    }
    
    const std::vector<uint8_t>& getData() const { return binary_data_; }
    
private:
    std::vector<uint8_t> binary_data_;
};

/**
 * @brief 消息工厂 - 从 JSON 反序列化
 */
class MessageFactory {
public:
    static Message::Ptr fromJson(const nlohmann::json& j) {
        std::string type = j["type"];
        if (type == "ImuMessage") return ImuMessage::fromJson(j);
        if (type == "LatencyMessage") return LatencyMessage::fromJson(j);
        if (type == "ImageMessage") return ImageMessage::fromJson(j);
        if (type == "BinaryMessage") return BinaryMessage::fromJson(j);
        return nullptr;
    }
};

} // namespace echo
