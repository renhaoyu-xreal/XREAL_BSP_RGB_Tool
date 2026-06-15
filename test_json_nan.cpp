#include <iostream>
#include "build/_deps/json-src/single_include/nlohmann/json.hpp"
int main() {
    try {
        auto j = nlohmann::json::parse("{\"a\": NaN}");
        std::cout << j.dump() << std::endl;
    } catch(std::exception& e) {
        std::cout << "Parse error: " << e.what() << std::endl;
    }
}
