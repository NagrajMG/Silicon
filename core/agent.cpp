#include "logging/logger.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include<iostream>

namespace 
{
std::atomic_bool g_running{true};

void handleSignal(int) {
    std::cout<<"\n=========================\n";
    std::cout << "   Gentle termination\n";
    std::cout<<"=========================\n";
    g_running.store(false);
}
} 

int main() {

    const std::string log_location = logging::Logger::defaultLogPath();
    
    // “If SIGINT or SIGTERM arrives, call handleSignal() instead of killing me.”
    std::signal(SIGINT, handleSignal); // control + C
    std::signal(SIGTERM, handleSignal); // kill
    std::signal(SIGQUIT, handleSignal); // control + \

    // LOGGING-->NAMESPACE, LOGGEr-->CLASS
    logging::Logger logger(log_location);

    // Clear the logs
    logger.clearLogFile(log_location);

    // Start the process
    logger.log(logging::LogLevel::Info, "Silicon System started");

    // Risk free, stops automatically after 1 minute
    auto start = std::chrono::steady_clock::now();

    while (g_running.load()) {
        logger.log(logging::LogLevel::Info, "Dhak Dhak..");
        std::this_thread::sleep_for(std::chrono::seconds(5));

        auto now = std::chrono::steady_clock::now();
        if (now - start >= std::chrono::minutes(1)) {
            break;
        }
    }
    
    if (g_running.load())
    {
        logger.log(logging::LogLevel::Info, "Silicon System Timed-out, Ahh! My MacBook saved from crashing!");
    }
    else
    {
        logger.log(logging::LogLevel::Info, "Silicon System Stopped, You did that!");   
    }

    return 0;
}
