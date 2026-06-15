#include "parameter_server.h"
#include "logger.h"
#include <iostream>
#include <thread>
#include <chrono>

using json = nlohmann::json;

void printHeader(const std::string& title) {
    std::cout << "\n" << std::string(50, '=') << std::endl;
    std::cout << title << std::endl;
    std::cout << std::string(50, '=') << std::endl;
}

void example1_basicOperations() {
    printHeader("Example 1: Basic Parameter Operations");
    
    echo::ParameterClient client;
    
    // 设置参数
    std::cout << "\n1. Setting parameters..." << std::endl;
    client.setParam("/robot/name", json("robot_1"));
    client.setParam("/robot/max_speed", json(2.5));
    client.setParam("/robot/enabled", json(true));
    std::cout << "✓ Parameters set" << std::endl;
    
    // 获取参数
    std::cout << "\n2. Getting parameters..." << std::endl;
    std::string name = client.getParam<std::string>("/robot/name");
    double speed = client.getParam<double>("/robot/max_speed");
    bool enabled = client.getParam<bool>("/robot/enabled");
    
    std::cout << "  Robot Name: " << name << std::endl;
    std::cout << "  Max Speed: " << speed << " m/s" << std::endl;
    std::cout << "  Enabled: " << (enabled ? "yes" : "no") << std::endl;
}

void example2_complexTypes() {
    printHeader("Example 2: Complex Data Types");
    
    echo::ParameterClient client;
    
    // 数组
    std::cout << "\n1. Array parameters..." << std::endl;
    json position = json::array({10.0, 20.0, 30.0});
    client.setParam("/robot/position", position);
    auto retrieved = client.getParam<std::vector<double>>("/robot/position");
    std::cout << "  Position: (" << retrieved[0] << ", " << retrieved[1] 
              << ", " << retrieved[2] << ")" << std::endl;
    
    // 对象
    std::cout << "\n2. Object parameters..." << std::endl;
    json config = json::object({
        {"version", "1.0"},
        {"author", "robotics_team"},
        {"date", "2026-01-13"}
    });
    client.setParam("/robot/config", config);
    json cfg = client.getParam<json>("/robot/config");
    std::cout << "  Version: " << cfg["version"] << std::endl;
    std::cout << "  Author: " << cfg["author"] << std::endl;
    std::cout << "  Date: " << cfg["date"] << std::endl;
    
    // 嵌套结构
    std::cout << "\n3. Nested structures..." << std::endl;
    json nested = json{
        {"motors", json::object({
            {"left", json::object({{"speed", 100}})},
            {"right", json::object({{"speed", 100}})}
        })},
        {"sensors", json::object({
            {"imu", true},
            {"camera", true}
        })}
    };
    client.setParam("/robot/state", nested);
    auto state = client.getParam<json>("/robot/state");
    std::cout << "  Motor left speed: " << state["motors"]["left"]["speed"] << std::endl;
    std::cout << "  Motor right speed: " << state["motors"]["right"]["speed"] << std::endl;
    std::cout << "  IMU enabled: " << state["sensors"]["imu"] << std::endl;
}

void example3_search() {
    printHeader("Example 3: Parameter Search");
    
    echo::ParameterClient client;
    
    // 设置多个分组参数
    std::cout << "\n1. Setting grouped parameters..." << std::endl;
    client.setParam("/camera/width", json(1920));
    client.setParam("/camera/height", json(1080));
    client.setParam("/camera/fps", json(30));
    client.setParam("/camera/format", json("RGB"));
    std::cout << "✓ Camera parameters set" << std::endl;
    
    // 搜索
    std::cout << "\n2. Searching for /camera/* parameters..." << std::endl;
    auto results = client.searchParam("/camera/*");
    for (const auto& param : results) {
        json value;
        if (client.getParam(param, value)) {
            std::cout << "  " << param << " = " << value.dump() << std::endl;
        }
    }
    
    // 列出所有参数
    std::cout << "\n3. Listing all parameters..." << std::endl;
    auto all_params = client.listParams();
    std::cout << "  Total parameters: " << all_params.size() << std::endl;
    std::cout << "  Parameters:" << std::endl;
    for (const auto& param : all_params) {
        std::cout << "    - " << param << std::endl;
    }
}

void example4_fileOperations() {
    printHeader("Example 4: File Operations");
    
    echo::ParameterClient client;
    
    // 清除之前的参数
    auto all_params = client.listParams();
    for (const auto& param : all_params) {
        client.deleteParam(param);
    }
    
    // 设置一些参数
    std::cout << "\n1. Setting parameters for file save..." << std::endl;
    client.setParam("/app/name", json("MyRobot"));
    client.setParam("/app/version", json("1.0"));
    client.setParam("/app/author", json("developer"));
    client.setParam("/settings/debug", json(true));
    client.setParam("/settings/log_level", json("INFO"));
    std::cout << "✓ Parameters set" << std::endl;
    
    // 保存到文件
    std::cout << "\n2. Saving parameters to file..." << std::endl;
    std::string filename = "/tmp/robot_config_example.json";
    if (client.saveParamFile(filename)) {
        std::cout << "✓ Parameters saved to: " << filename << std::endl;
        std::cout << "  File content:" << std::endl;
        
        // 读取文件内容
        std::ifstream file(filename);
        std::string line;
        while (std::getline(file, line)) {
            std::cout << "    " << line << std::endl;
        }
    }
    
    // 清除参数
    std::cout << "\n3. Clearing all parameters..." << std::endl;
    auto params = client.listParams();
    for (const auto& param : params) {
        client.deleteParam(param);
    }
    std::cout << "✓ All parameters deleted" << std::endl;
    std::cout << "  Remaining parameters: " << client.listParams().size() << std::endl;
    
    // 从文件加载
    std::cout << "\n4. Loading parameters from file..." << std::endl;
    if (client.loadParamFile(filename)) {
        std::cout << "✓ Parameters loaded from: " << filename << std::endl;
        std::cout << "  Loaded parameters:" << std::endl;
        auto loaded = client.listParams();
        for (const auto& param : loaded) {
            json value;
            if (client.getParam(param, value)) {
                std::cout << "    " << param << " = " << value.dump() << std::endl;
            }
        }
    }
}

void example5_errorHandling() {
    printHeader("Example 5: Error Handling");
    
    echo::ParameterClient client;
    
    std::cout << "\n1. Attempting to get non-existent parameter..." << std::endl;
    json value;
    if (client.getParam("/non/existent/param", value)) {
        std::cout << "✓ Parameter found" << std::endl;
    } else {
        std::cout << "✗ Parameter not found (expected)" << std::endl;
    }
    
    std::cout << "\n2. Checking parameter existence..." << std::endl;
    if (client.hasParam("/non/existent/param")) {
        std::cout << "✓ Parameter exists" << std::endl;
    } else {
        std::cout << "✗ Parameter does not exist (expected)" << std::endl;
    }
    
    std::cout << "\n3. Setting and then deleting a parameter..." << std::endl;
    client.setParam("/temp/test", json("test_value"));
    if (client.hasParam("/temp/test")) {
        std::cout << "✓ Parameter created" << std::endl;
    }
    
    client.deleteParam("/temp/test");
    if (!client.hasParam("/temp/test")) {
        std::cout << "✓ Parameter deleted" << std::endl;
    }
}

int main() {
    echo::Logger::getInstance().setLogLevel(echo::LogLevel::WARN);
    
    std::cout << "\n╔════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Parameter Server - Usage Examples                ║" << std::endl;
    std::cout << "║  Waiting for Parameter Server on port 5591...    ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════╝" << std::endl;
    
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    try {
        example1_basicOperations();
        example2_complexTypes();
        example3_search();
        example4_fileOperations();
        example5_errorHandling();
        
        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "✓ All examples completed successfully!" << std::endl;
        std::cout << std::string(50, '=') << std::endl << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n✗ Error: " << e.what() << std::endl;
        return 1;
    }
}
