#include "parameter_server.h"
#include "logger.h"
#include <csignal>
#include <thread>
#include <chrono>

static bool running = true;

void signalHandler(int sig) {
    running = false;
}

int main() {
    echo::Logger::getInstance().setLogLevel(echo::LogLevel::INFO);
    
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    LOG_INFO("Main", "Starting Echo Parameter Server on port 5591...");
    
    echo::ParameterServer param_server(5591);
    
    // 在单独的线程中运行
    std::thread param_server_thread([&param_server]() {
        param_server.start();
    });
    
    // 给参数服务器一些时间启动
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 尝试注册到Master
    param_server.registerToMaster("127.0.0.1", 5590);
    
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    LOG_INFO("Main", "Stopping Parameter Server...");
    param_server.stop();
    
    if (param_server_thread.joinable()) {
        param_server_thread.join();
    }
    
    LOG_INFO("Main", "Parameter Server stopped");
    
    return 0;
}
