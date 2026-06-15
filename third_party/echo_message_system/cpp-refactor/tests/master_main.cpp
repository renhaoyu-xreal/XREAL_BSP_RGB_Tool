#include "master.h"
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
    
    LOG_INFO("Main", "Starting Echo Master on port 5590...");
    
    echo::Master master(5590);
    
    // 在单独的线程中运行，以便可以响应信号
    std::thread master_thread([&master]() {
        master.start();
    });
    
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    LOG_INFO("Main", "Stopping Master...");
    master.stop();
    
    if (master_thread.joinable()) {
        master_thread.join();
    }
    
    LOG_INFO("Main", "Master stopped");
    
    return 0;
}
