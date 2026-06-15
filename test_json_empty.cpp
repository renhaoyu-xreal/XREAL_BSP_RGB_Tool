#include <iostream>
#include <cmath>
#include "build/_deps/json-src/single_include/nlohmann/json.hpp"
int main() {
    nlohmann::json rawImuData = nlohmann::json::parse("{\"hasGyro\": true, \"gyro\": []}");
    
    auto gyro = rawImuData.value("gyro", std::vector<double>{0, 0, 0});
    try {
        nlohmann::json message = {{"data", {gyro[0], gyro[1], gyro[2], 0.0, 0.0, 0.0}}};
        auto dataArray = message.value("data", std::vector<double>{});
    } catch(std::exception& e) {
        std::cout << "Inner Error: " << e.what() << std::endl;
    }
}
