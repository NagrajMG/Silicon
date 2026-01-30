#include "logging/logger.hpp"
#include <fstream>
#include <cassert>
#include <string>

int main() {
    using silicon::logging::Logger;
    using silicon::logging::LogLevel;

    // setup
    auto& logger = Logger::instance();
    logger.set_log_path("runtime/logs/test.log");
    logger.set_min_level(LogLevel::Trace);

    logger.info("Test message {}", 42);

    // Check I
    std::ifstream file("runtime/logs/test.log");
    assert(file.is_open());

    //Check II
    auto begin = std::istreambuf_iterator<char>(file);
    auto end = std::istreambuf_iterator<char>();
    std::string content(begin, end);
    assert(content.find("Test message 42") != std::string::npos);

    return 0;
}
