#include "agent/Agent.hpp"
#include "logging/logger.hpp"

#include <atomic>
#include <thread>
#include <chrono>
#include <fstream>
#include <cassert>
#include <string>

int main() {
    using silicon::agent::Agent;
    using silicon::logging::Logger;
    using silicon::logging::LogLevel;

    std::atomic<bool> shutdown_requested_{false};
    std::atomic<int> last_signal_{0};

    auto& logger = Logger::instance();
    logger.set_log_path("runtime/logs/test_agent.log");
    logger.set_min_level(LogLevel::Debug);

    Agent agent(std::chrono::seconds(1));

    // Run agent in background thread
    std::thread thr([&] {
        agent.run(shutdown_requested_, &last_signal_);
    });

    // running for a few seconds
    std::this_thread::sleep_for(std::chrono::seconds(3));

    shutdown_requested_.store(true);
    thr.join();

    // Check I
    std::ifstream file("runtime/logs/test_agent.log");
    assert(file.is_open());

    auto begin = std::istreambuf_iterator<char>(file);
    auto end = std::istreambuf_iterator<char>();
    std::string content(begin, end);

    // Checks II
    assert(content.find("Agent state: STARTING") != std::string::npos);
    assert(content.find("Agent state: RUNNING") != std::string::npos);
    assert(content.find("Agent state: SHUTTING_DOWN") != std::string::npos);
    assert(content.find("Heartbeat") != std::string::npos);

    return 0;
}
