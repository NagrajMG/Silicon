#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <libproc.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include "logging/logger.hpp"
#include "process/process_enforcer.hpp"

namespace fs = std::filesystem;

namespace {

class TempConfigFile 
{
 public:
  silicon::logging::Logger& logger = silicon::logging::Logger::instance();
  explicit TempConfigFile(const nlohmann::json& content) {
    // temporary directory
    temp_dir_ = fs::temp_directory_path() / fs::path("silicon_process_test_" + std::to_string(std::rand()));

    fs::create_directories(temp_dir_);
    // path for the config which should be default
    config_path_ = temp_dir_ / "policy.json";

    logger.debug("TempConfigFile: Creating sandbox at: {}" , config_path_.string());
    // take the path in for writing
    std::ofstream out(config_path_);
    // dump json
    out << content.dump(2);
    logger.debug("TempConfigFile: Policy JSON written successfully.");
  }

  ~TempConfigFile() {
    silicon::logging::Logger& logger = silicon::logging::Logger::instance();
    try {
      logger.debug("TempConfigFile: Cleaning up sandbox at: {}" , temp_dir_.string());
      fs::remove_all(temp_dir_);
    } catch (const std::exception& e) {
      logger.error("TempConfigFile: Cleanup failed: {}", e.what());
    }
  }

  std::string path() const {
    return config_path_.string();
  }

 private:
  fs::path temp_dir_{};
  fs::path config_path_{};
};

std::string current_process_path() {
  char buffer[PROC_PIDPATHINFO_MAXSIZE] = {0};
  const int ret = proc_pidpath(getpid(), buffer, sizeof(buffer));
  if (ret <= 0) {
    return {};
  }
  return std::string(buffer);
}

void test_start_stop_and_stats() {
  using silicon::config::ConfigManager;
  using silicon::process::ProcessEnforcer;

  // put in that temporary file
  TempConfigFile config_file(nlohmann::json{
      {"allowed_processes", nlohmann::json::array()},
      {"blocked_processes", nlohmann::json::array()},
      {"debug_mode", true},
      {"process_scan_interval_seconds", 1},
      {"system_paths_to_ignore", nlohmann::json::array(
            {"/System/Library/", "/usr/libexec/", "/usr/sbin/", "/sbin/", "/bin/", "/usr/bin/", "/usr/local/"})},
  });

  auto config = std::make_shared<ConfigManager>(config_file.path());
  ProcessEnforcer enforcer(config);

  // starting the program
  assert(enforcer.start());

  // again starting when it was already running
  assert(!enforcer.start()); // warning level

  std::this_thread::sleep_for(std::chrono::milliseconds(2200));

  const auto stats = enforcer.get_last_scan_stats();
  assert(stats.timestamp.time_since_epoch().count() > 0);

  assert(enforcer.stop(std::chrono::seconds(2)));
  assert(enforcer.stop(std::chrono::milliseconds(10)));
}

void test_blocked_process_detection() {
  using silicon::config::ConfigManager;
  using silicon::process::ProcessEnforcer;
  silicon::logging::Logger& logger = silicon::logging::Logger::instance();

  const std::string self_path = current_process_path();
  assert(!self_path.empty());

  TempConfigFile config_file(nlohmann::json{
      {"allowed_processes", nlohmann::json::array()},
      {"blocked_processes", nlohmann::json::array({self_path})}, // should return warning
      {"debug_mode", true},
      {"process_scan_interval_seconds", 1},
      {"system_paths_to_ignore", nlohmann::json::array(
            {"/System/Library/", "/usr/libexec/", "/usr/sbin/", "/sbin/", "/bin/", "/usr/bin/", "/usr/local/"})},
  });

  auto config = std::make_shared<ConfigManager>(config_file.path());
  ProcessEnforcer enforcer(config);
  assert(enforcer.start());

  bool found_violation = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4); 
  while (std::chrono::steady_clock::now() < deadline) {
    const auto stats = enforcer.get_last_scan_stats();
    if (stats.violations_detected > 0) {
      found_violation = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }

  const auto final_stats = enforcer.get_last_scan_stats();
  assert(enforcer.stop(std::chrono::seconds(2)));
  if (final_stats.total_processes_checked == 0 && final_stats.permission_errors > 0) {
    // Permission-restricted environments may not allow PID inspection.
    logger.warn("Process_enforcer_test : Permission-restricted environments inspection needed.");
    return;
  }
  assert(found_violation);
}

}  // namespace

int main() {
  using silicon::logging::LogLevel;
  using silicon::logging::Logger;

  auto& logger = Logger::instance();
  logger.set_log_path("runtime/logs/test_process_enforcer.log");
  logger.set_min_level(LogLevel::Debug);

  test_start_stop_and_stats();
  test_blocked_process_detection();
  return 0;
}
