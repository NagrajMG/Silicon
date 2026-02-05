#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "config/config_manager.hpp"

namespace silicon::process {

/**
 * @brief Passive process policy enforcer for monitor-only exam mode.
 *
 * The enforcer periodically scans running processes on macOS, evaluates each
 * process against ConfigManager policy, skips configured system processes, and
 * logs policy violations without terminating processes.
 */

class ProcessEnforcer final {
 public:
  /**
   * @brief Per-scan summary information for observability and tests.
   */
  struct ScanStats {
    std::chrono::system_clock::time_point timestamp{};
    std::size_t total_processes_checked{0};
    std::size_t violations_detected{0};
    std::size_t permission_errors{0};
  };

  /**
   * @brief Construct a monitor-only process enforcer.
   * @param config Shared policy source used for process checks and runtime settings.
   */
  explicit ProcessEnforcer(std::shared_ptr<silicon::config::ConfigManager> config);

  /**
   * @brief Destructor that attempts a graceful shutdown of the background thread.
   */
  ~ProcessEnforcer();

  /**
   * @brief Start periodic background scanning.
   * @return True when the worker is started, false if already running or invalid.
   */
  bool start();

  /**
   * @brief Request stop and wait for the worker thread to exit.
   * @param timeout Maximum time to wait for clean shutdown before returning false.
   * @return True if stopped cleanly or not running, false on timeout.
   */
  bool stop(std::chrono::milliseconds timeout = std::chrono::milliseconds(500));

  /**
   * @brief Get the most recent scan summary.
   * @return Thread-safe snapshot of the last collected scan stats.
   */
  ScanStats get_last_scan_stats() const;

 private:
  /**
   * @brief Minimal process data needed for policy checks and logging.
   */
  struct ProcessSnapshot {
    int pid{0};
    std::string executable_path{};
    std::string executable_name{};
  };

  /**
   * @brief Main worker loop that runs periodic scans until stop is requested.
   * @return No return value; updates lifecycle state and signals stop waiters.
   */
  void scan_loop();

  /**
   * @brief Run one full scan across all visible processes.
   * @return No return value; emits logs and updates last scan stats.
   */
  void perform_scan();

  /**
   * @brief Enumerate active PIDs via macOS sysctl.
   * @param error_code_out Optional output receiving errno-style code on failure.
   * @return Vector of positive process IDs; empty on failure or no processes.
   */
  static std::vector<int> enumerate_all_pids(int* error_code_out);

  /**
   * @brief Read process path/name metadata using macOS libproc APIs.
   * @param pid Process ID to inspect.
   * @param snapshot_out Output structure on success.
   * @param permission_denied_out Set true when access is denied for this PID.
   * @return True if process metadata was read, false otherwise.
   */
  static bool read_process_snapshot(int pid, ProcessSnapshot* snapshot_out, bool* permission_denied_out);

  /**
   * @brief Resolve scan interval with backward compatibility for sec/ms configs.
   * @return Scan sleep interval in milliseconds.
   */
  std::chrono::milliseconds resolve_scan_interval() const;

  /**
   * @brief Decide whether a process should be skipped as a system process.
   * @param path Absolute executable path when available.
   * @param name Executable basename when available.
   * @param ignore_patterns Configured ignore prefixes/names.
   * @return True when the process should be skipped.
   */
  bool should_ignore_process(std::string_view path,
                             std::string_view name,
                             const std::vector<std::string>& ignore_patterns) const;

  /**
   * @brief Directory-aware prefix comparison for path ignore matching.
   * @param path Candidate executable path.
   * @param prefix Ignore prefix path from configuration.
   * @return True if path falls inside the ignored prefix.
   */
  static bool has_directory_prefix(std::string_view path, std::string_view prefix);

  /**
   * @brief Return lowercase ASCII copy for case-insensitive process matching.
   * @param value Input string view.
   * @return Lowercased copy of the input.
   */
  static std::string lower_ascii_copy(std::string_view value);

  /**
   * @brief Extract the trailing executable name from an absolute path.
   * @param path Absolute or relative executable path.
   * @return Basename component, or empty string when unavailable.
   */
  static std::string basename_from_path(std::string_view path);

  /**
   * @brief Sanitize strings before logging to avoid log injection noise.
   * @param value Arbitrary text that may contain control characters.
   * @return Sanitized printable string safe for structured log lines.
   */
  static std::string sanitize_for_log(std::string_view value);

  /**
   * @brief Determine whether violation notifications should be emitted to users.
   * @return True in non-debug mode, false in debug mode.
   */
  bool should_show_notifications() const;

  /**
   * @brief Placeholder for future OS-native user notifications.
   * @param message Human-readable violation message.
   * @return No return value.
   */
  void show_user_notification(std::string_view message) const;

  /**
   * @brief Publish scan stats atomically via a mutex-protected assignment.
   * @param stats Newly collected scan summary.
   * @return No return value.
   */
  void publish_scan_stats(const ScanStats& stats);

  std::shared_ptr<silicon::config::ConfigManager> config_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};

  mutable std::mutex lifecycle_mutex_{};
  std::condition_variable lifecycle_cv_{};
  std::thread worker_{};
  bool worker_exited_{true};

  mutable std::mutex wake_mutex_{};
  std::condition_variable wake_cv_{};

  mutable std::mutex stats_mutex_{};
  ScanStats last_scan_stats_{};
};

}  // namespace silicon::process
