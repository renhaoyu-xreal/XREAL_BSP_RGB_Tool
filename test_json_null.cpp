#include <iostream>
#include "build/_deps/json-src/single_include/nlohmann/json.hpp"
int main() {
    nlohmann::json j = {{"data", nullptr}};
    try {
        auto val = j.value("data", std::vector<double>{0,0,0});
    } catch(std::exception& e) {
        std::cout << e.what() << std::endl;
    }
}
