#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace silicon::logging {

enum class LogLevel : unsigned char {
  Trace = 0,
  Debug,
  Info,
  Warn,
  Error,
  Critical
};

// Final : No inheritence possible
class Logger final {
 public:

  static Logger& instance() {
    static Logger instance;
    return instance; // Returns the alias of the logger
  }

  // Deleted the copy, move constructor and assignment
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;
  Logger(Logger&&) = delete;
  Logger& operator=(Logger&&) = delete;

  void set_log_path(std::string_view path) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_.assign(path.begin(), path.end());
    reopen_stream_locked();
  }

  void set_min_level(LogLevel level) noexcept {
    min_level_.store(level, std::memory_order_relaxed);
  }

  template <typename... Args>
  void log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    if (!is_enabled(level)) {
      return;
    }

    std::string message;
    try {
      // Perfect forwarding — passes arguments to std::format without unnecessary copies
      message = std::format(fmt, std::forward<Args>(args)...);
    } 
    catch (const std::format_error& err) {
      message = std::format("format_error: {}", err.what());
    }

    write_line(level, message);
  }

  void log_message(LogLevel level, std::string_view message) {
    if (!is_enabled(level)) {
      return;
    }

    write_line(level, message);
  }

  template <typename... Args>
  void trace(std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::Trace, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void debug(std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::Debug, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void info(std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::Info, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void warn(std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::Warn, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void error(std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::Error, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void critical(std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::Critical, fmt, std::forward<Args>(args)...);
  }

 private:
 // Constructor
  Logger() { 
    reopen_stream_locked(); 
  }

  // Destructor
  ~Logger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_.is_open()) {
      stream_.flush();
    }
  }

  static constexpr int level_rank(LogLevel level) noexcept {
    return static_cast<int>(level);
  }

  bool is_enabled(LogLevel level) const noexcept {
    return level_rank(level) >= level_rank(min_level_.load(std::memory_order_relaxed));
  }

  void reopen_stream_locked() {
    stream_.close();
    stream_.clear();

    try {
      std::filesystem::path file_path(path_);
      if (file_path.has_parent_path()) {
        std::filesystem::create_directories(file_path.parent_path());
      }
    } 
    catch (const std::exception&) {
      
    }

    // if there was no directory, creating in current directory else, create the directory
    stream_.open(path_, std::ios::out | std::ios::app);
  }

  static std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = floor<seconds>(now);
    const auto micros = duration_cast<microseconds>(now - secs).count();

    try {
      const auto base = std::format("{:%Y-%m-%d %H:%M:%S}", secs);
      return std::format("{}.{:06}", base, micros);
    } 
    catch (const std::format_error&) {
      return "0000-00-00 00:00:00.000000";
    }
  }

  static std::string_view level_label(LogLevel level) {
    switch (level) {
      case LogLevel::Trace:
        return "TRACE";
      case LogLevel::Debug:
        return "DEBUG";
      case LogLevel::Info:
        return "INFO";
      case LogLevel::Warn:
        return "WARN";
      case LogLevel::Error:
        return "ERROR";
      case LogLevel::Critical:
        return "CRITICAL";
      default:
        return "UNKNOWN";
    }
  }

  void write_line(LogLevel level, std::string_view message) {
    const auto ts = timestamp();
    std::lock_guard<std::mutex> lock(mutex_);

    if (!stream_.is_open()) {
      reopen_stream_locked();
    }

    if (!stream_.is_open()) {
      return;
    }

    stream_ << ts << " [" << level_label(level) << "] " << message << '\n';
    stream_.flush(); // buffered data in the stream is written in the file
  }

  std::mutex mutex_{};
  std::ofstream stream_{};
  std::string path_{"runtime/logs/silicon_dev.log"};
  std::atomic<LogLevel> min_level_{LogLevel::Trace}; // changing log levels safely across threads without locking
};
}

/*
====================================
      Logger Features
====================================

1. Thread-Safe Logging — Uses mutex to serialize file writes across threads.
2. Meyers’ Singleton — Guarantees exactly one global logger instance.
3. Log Level Filtering — Filters messages based on atomic minimum severity.
4. Compile-Time Format Safety — Enforces correct format strings via std::format_string.
5. Perfect Forwarding — Avoids unnecessary argument copies during formatting.
6. Automatic Directory Creation — Creates missing log directories before file open.
7. Immediate Durability — flush() forces buffered logs to be written instantly.
8. Microsecond Timestamps — Logs precise time using std::chrono.
9. Append Mode — Preserves previous logs by writing in append mode.
10. Exception Safe Formatting — Handles std::format errors without crashing.
11. RAII Cleanup — Flushes stream safely in destructor.
12. Final Class Design — Prevents inheritance misuse.
13. Concurrency-Aware Structure — Formats outside lock, writes inside lock.
14. Atomic Configuration — Allows lock-free runtime log level changes.
15. Minimal External Dependencies — Uses only standard C++ library.

*/