#include "logging/logger.hpp"
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include<iostream>

namespace logging 
{

Logger::Logger(const std::string& filePath) : filePath_(filePath) {
    openIfNeeded();
}

void Logger::log(LogLevel level, const std::string& message) 
{
    std::lock_guard<std::mutex> lock(mutex_);

    openIfNeeded();
    // this will open a output file
    if (!stream_.is_open()) // if closed
    {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);

    std::tm localTime{};

#ifdef __APPLE__
    localtime_r(&nowTime, &localTime);
#else
    localTime = *std::localtime(&nowTime);
#endif

    stream_ << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S") << " [" << levelToString(level) << "] " << message << std::endl;
}

std::string Logger::defaultLogPath() {
    // APPLE keeps the base with $HOME 
    const std::string home = std::getenv("HOME");
    return home + "/Computing/cpp_projects/SILICON/logs/stdout.log";
}

void Logger::clearLogFile(const std::string& filePath) {
    std::ofstream ofs(filePath, std::ios::out | std::ios::trunc);
   return;
}

void Logger::openIfNeeded() {
    if (stream_.is_open()) {
        return;
    }

    std::filesystem::path Path = std::filesystem::path(filePath_);
    std::error_code ec;
    std::filesystem::create_directories(Path.parent_path(), ec);

    stream_.open(filePath_, std::ios::out | std::ios::app);
}

const char* Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        default:
            return "INFO";
    }
}

} // namespace logging
