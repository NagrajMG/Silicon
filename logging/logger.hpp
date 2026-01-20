#pragma once

#include <fstream>
#include <mutex>
#include <string>

namespace logging {

enum class LogLevel {
    Info,
    Warn,
    Error
};

class Logger {
public:
    explicit Logger(const std::string& filePath);

    void log(LogLevel level, const std::string& message);

    static std::string defaultLogPath();

    void clearLogFile(const std::string& filePath);

private:
    void openIfNeeded();
    static const char* levelToString(LogLevel level);

    std::string filePath_;
    std::ofstream stream_;
    std::mutex mutex_;
};

} // namespace logging
