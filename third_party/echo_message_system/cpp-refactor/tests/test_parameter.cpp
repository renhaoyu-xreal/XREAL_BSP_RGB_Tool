#include "parameter_server.h"
#include "logger.h"
#include <thread>
#include <chrono>
#include <cassert>

using json = nlohmann::json;

void testBasicOperations() {
    LOG_INFO("Test", "=== Test Basic Operations ===");
    
    echo::ParameterClient client("127.0.0.1", 5591);
    
    // 测试设置参数
    LOG_INFO("Test", "Testing setParam...");
    assert(client.setParam("/robot/name", json("my_robot")));
    assert(client.setParam("/robot/speed", json(1.5)));
    assert(client.setParam("/robot/enabled", json(true)));
    LOG_INFO("Test", "✓ setParam passed");
    
    // 测试获取参数
    LOG_INFO("Test", "Testing getParam...");
    json value;
    assert(client.getParam("/robot/name", value));
    assert(value.get<std::string>() == "my_robot");
    
    assert(client.getParam("/robot/speed", value));
    assert(std::abs(value.get<double>() - 1.5) < 0.001);
    
    assert(client.getParam("/robot/enabled", value));
    assert(value.get<bool>() == true);
    LOG_INFO("Test", "✓ getParam passed");
    
    // 测试模板版本的getParam
    LOG_INFO("Test", "Testing getParam<T>...");
    std::string name = client.getParam<std::string>("/robot/name");
    assert(name == "my_robot");
    double speed = client.getParam<double>("/robot/speed");
    assert(std::abs(speed - 1.5) < 0.001);
    LOG_INFO("Test", "✓ getParam<T> passed");
}

void testHasParam() {
    LOG_INFO("Test", "=== Test hasParam ===");
    
    echo::ParameterClient client("127.0.0.1", 5591);
    
    assert(client.hasParam("/robot/name") == true);
    assert(client.hasParam("/nonexistent/param") == false);
    
    LOG_INFO("Test", "✓ hasParam passed");
}

void testListParams() {
    LOG_INFO("Test", "=== Test listParams ===");
    
    echo::ParameterClient client("127.0.0.1", 5591);
    
    auto params = client.listParams();
    LOG_INFO("Test", "Found ", params.size(), " parameters");
    
    for (const auto& param : params) {
        LOG_INFO("Test", "  - ", param);
    }
    
    assert(params.size() > 0);
    assert(std::find(params.begin(), params.end(), "/robot/name") != params.end());
    
    LOG_INFO("Test", "✓ listParams passed");
}

void testSearchParam() {
    LOG_INFO("Test", "=== Test searchParam ===");
    
    echo::ParameterClient client("127.0.0.1", 5591);
    
    // 设置一些带前缀的参数
    client.setParam("/camera/width", json(1920));
    client.setParam("/camera/height", json(1080));
    client.setParam("/camera/fps", json(30));
    
    // 搜索所有camera参数
    auto results = client.searchParam("/camera/*");
    LOG_INFO("Test", "Search '/camera/*' found ", results.size(), " parameters");
    
    for (const auto& param : results) {
        LOG_INFO("Test", "  - ", param);
    }
    
    assert(results.size() >= 3);
    
    LOG_INFO("Test", "✓ searchParam passed");
}

void testDeleteParam() {
    LOG_INFO("Test", "=== Test deleteParam ===");
    
    echo::ParameterClient client("127.0.0.1", 5591);
    
    // 设置一个参数用于删除测试
    client.setParam("/temp/test_param", json("test_value"));
    assert(client.hasParam("/temp/test_param") == true);
    
    // 删除参数
    assert(client.deleteParam("/temp/test_param") == true);
    assert(client.hasParam("/temp/test_param") == false);
    
    LOG_INFO("Test", "✓ deleteParam passed");
}

void testComplexTypes() {
    LOG_INFO("Test", "=== Test Complex Types ===");
    
    echo::ParameterClient client("127.0.0.1", 5591);
    
    // 数组
    json arr = json::array({1, 2, 3, 4, 5});
    client.setParam("/array/values", arr);
    json retrieved_arr;
    client.getParam("/array/values", retrieved_arr);
    assert(retrieved_arr == arr);
    LOG_INFO("Test", "✓ Array parameter passed");
    
    // 对象/字典
    json obj = json::object({
        {"x", 10},
        {"y", 20},
        {"z", 30}
    });
    client.setParam("/position/coordinates", obj);
    json retrieved_obj;
    client.getParam("/position/coordinates", retrieved_obj);
    assert(retrieved_obj["x"] == 10);
    assert(retrieved_obj["y"] == 20);
    assert(retrieved_obj["z"] == 30);
    LOG_INFO("Test", "✓ Object parameter passed");
    
    // 嵌套结构
    json nested = json{
        {"robot", {
            {"name", "robot_1"},
            {"position", {1.0, 2.0, 3.0}},
            {"sensors", {
                {"imu", true},
                {"camera", true}
            }}
        }}
    };
    client.setParam("/config/nested", nested);
    json retrieved_nested;
    client.getParam("/config/nested", retrieved_nested);
    assert(retrieved_nested["robot"]["name"] == "robot_1");
    LOG_INFO("Test", "✓ Nested structure passed");
}

void testFileOperations() {
    LOG_INFO("Test", "=== Test File Operations ===");
    
    echo::ParameterClient client("127.0.0.1", 5591);
    
    // 清除之前的参数
    auto params = client.listParams();
    for (const auto& param : params) {
        client.deleteParam(param);
    }
    
    // 设置一些参数
    client.setParam("/test/param1", json("value1"));
    client.setParam("/test/param2", json(42));
    client.setParam("/test/param3", json(3.14));
    
    // 保存到文件
    assert(client.saveParamFile("/tmp/params.json") == true);
    LOG_INFO("Test", "✓ Saved parameters to /tmp/params.json");
    
    // 清除所有参数
    params = client.listParams();
    for (const auto& param : params) {
        client.deleteParam(param);
    }
    assert(client.listParams().empty());
    
    // 从文件加载
    assert(client.loadParamFile("/tmp/params.json") == true);
    LOG_INFO("Test", "✓ Loaded parameters from /tmp/params.json");
    
    // 验证参数
    assert(client.hasParam("/test/param1") == true);
    assert(client.hasParam("/test/param2") == true);
    assert(client.hasParam("/test/param3") == true);
    
    std::string val1 = client.getParam<std::string>("/test/param1");
    int val2 = client.getParam<int>("/test/param2");
    double val3 = client.getParam<double>("/test/param3");
    
    assert(val1 == "value1");
    assert(val2 == 42);
    assert(std::abs(val3 - 3.14) < 0.01);
    
    LOG_INFO("Test", "✓ File operations passed");
}

void testErrorHandling() {
    LOG_INFO("Test", "=== Test Error Handling ===");
    
    echo::ParameterClient client("127.0.0.1", 5591);
    
    // 获取不存在的参数
    json value;
    assert(client.getParam("/nonexistent/param", value) == false);
    
    // 删除不存在的参数
    assert(client.deleteParam("/nonexistent/param") == false);
    
    LOG_INFO("Test", "✓ Error handling passed");
}

int main() {
    echo::Logger::getInstance().setLogLevel(echo::LogLevel::INFO);
    
    LOG_INFO("Main", "Waiting for Parameter Server to start...");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    try {
        testBasicOperations();
        testHasParam();
        testListParams();
        testSearchParam();
        testDeleteParam();
        testComplexTypes();
        testFileOperations();
        testErrorHandling();
        
        LOG_INFO("Main", "");
        LOG_INFO("Main", "====================================");
        LOG_INFO("Main", "✓ All tests passed successfully!");
        LOG_INFO("Main", "====================================");
        
        return 0;
    } catch (const std::exception& e) {
        LOG_ERROR("Main", "Test failed with exception: ", e.what());
        return 1;
    }
}
