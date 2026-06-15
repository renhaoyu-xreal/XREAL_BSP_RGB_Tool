#include <iostream>
#include <cmath>
#include "build/_deps/json-src/single_include/nlohmann/json.hpp"
int main() {
    double nan_val = std::nan("");
    nlohmann::json j = {{"data", {nan_val, 1.0, 2.0}}};
    std::cout << j.dump() << std::endl;
}
