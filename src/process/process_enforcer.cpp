#include "process/process_enforcer.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <format>
#include <sstream>
#include <vector>

#include <libproc.h>
#include <sys/sysctl.h>

#include "logging/logger.hpp"

namespace silicon::process {

namespace {

constexpr std::chrono::milliseconds kDefaultScanIntervalMs{2000};
constexpr std::chrono::milliseconds kMinScanIntervalMs{250};
constexpr int kSysctlRetryCount = 3;

/**
 * @brief Fallback PID enumeration using libproc when sysctl fails
 * @param error_code_out Optional pointer to receive error code on failure
 * @return Vector of process IDs, empty on failure
 */
std::vector<int> enumerate_with_libproc(int* error_code_out) {
  if (error_code_out != nullptr) {
    *error_code_out = 0;
  }

  const int count_hint = ::proc_listallpids(nullptr, 0); // void *buffer, int buffersize
  // The number of running processes is returned
  if (count_hint <= 0) {
    if (error_code_out != nullptr) {
      *error_code_out = (errno != 0) ? errno : EPERM; // default: Operation not permitted
    }
    return {};
  }
   
  // buffer counts in size_t
  std::vector<pid_t> pid_buffer(static_cast<std::size_t>(count_hint)); // Creates: [slot0][slot1][slot2]...[slot count_hint]
  // Filled : No. of processes
  const int filled = ::proc_listallpids(
      pid_buffer.data(), // Pointer to the raw memory inside the vector
      static_cast<int>(pid_buffer.size() * sizeof(pid_t)));  // Total buffer size in bytes

  if (filled <= 0) { 
    if (error_code_out != nullptr) {
      *error_code_out = (errno != 0) ? errno : EPERM;
    }
    return {};
  }

  std::vector<int> pids;
  pids.reserve(static_cast<std::size_t>(filled));
  // STORING THE PIDS IN INT
  for (int i = 0; i < filled; ++i) {
    if (pid_buffer[static_cast<std::size_t>(i)] > 0) {
      pids.push_back(static_cast<int>(pid_buffer[static_cast<std::size_t>(i)]));
    }
  }

  std::sort(pids.begin(), pids.end());
  pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
  return pids;
}

}  // namespace

/**
 * @brief Constructs a ProcessEnforcer with shared configuration
 * @param config Shared pointer to ConfigManager instance
 */
ProcessEnforcer::ProcessEnforcer(std::shared_ptr<silicon::config::ConfigManager> config)
    : config_(std::move(config)) {
  using silicon::logging::Logger;
  auto& logger = Logger::instance();

  /* 
   * this is about the shared pointer
   * CONFIG : Allows multiple owners to manage the lifetime of a single object on the heap
   * 2 pointers --> the reference count in the control block is incremented, one to the actual object
   */

  if (config_ == nullptr) {
    logger.error("ProcessEnforcer: Cannot initialize without ConfigManager");
    return;
  }

  logger.info("ProcessEnforcer initialized with scan interval: {} ms", resolve_scan_interval().count());
}

/**
 * @brief Destructor ensures graceful thread shutdown
 */
ProcessEnforcer::~ProcessEnforcer() {
  using silicon::logging::Logger;
  auto& logger = Logger::instance();

  // Try to stop the thread gracefully
  // but if it takes longer than 1.5 seconds
  // do emergency shutdown

  if (!stop(std::chrono::milliseconds(1500))) {
    logger.warn("ProcessEnforcer: Graceful stop timed out in destructor; waiting for worker to exit");
    stop_requested_.store(true, std::memory_order_relaxed); // please stop now
    wake_cv_.notify_all(); // Wakes up any waiting thread

    std::thread thread_to_join; // empty termination letter
    // only one acess the worker at a time
    {
      // only one can have it : lifecycle_mutex_
      std::lock_guard<std::mutex> lock(lifecycle_mutex_); // protects access to the worker_ thread
      thread_to_join = std::move(worker_); // worker is moved so that, it cannot be join with a lock
    }

    // safely join the thread
    if (thread_to_join.joinable()) {
      thread_to_join.join();
    }
  }
}

/**
 * @brief Starts the background monitoring thread
 * @return true if started successfully, false if already running or config invalid
 */

bool ProcessEnforcer::start() {
  using silicon::logging::Logger;
  auto& logger = Logger::instance();

  if (config_ == nullptr) {
    logger.error("ProcessEnforcer: start failed because ConfigManager is null");
    return false;
  }

  std::lock_guard<std::mutex> lock(lifecycle_mutex_);

  /*
   *
   *  Release : Make sure everything I wrote is visible BEFORE I set this flag (Writers)
   *  Acquire : Whatever is done before should be visible, who sees the loading (Readers)
   * 
   */

  if (worker_.joinable() || running_.load(std::memory_order_relaxed)) {
    // just load what state you saw right now, no ordering
    logger.warn("ProcessEnforcer: start requested while already running");
    return false;
  }

  stop_requested_.store(false, std::memory_order_relaxed);
  worker_exited_ = false;
  running_.store(true, std::memory_order_relaxed);

  try {
    worker_ = std::thread(&ProcessEnforcer::scan_loop, this); // scan for this object
  } catch (const std::exception& err) {
    running_.store(false, std::memory_order_relaxed);
    worker_exited_ = true;
    logger.error("ProcessEnforcer: Failed to start worker thread: {}", err.what());
    return false;
  }

  std::ostringstream tid;
  tid << worker_.get_id();
  logger.info("ProcessEnforcer started with thread ID: {}", tid.str());
  return true;
}

/**
 * @brief Stops the background monitoring thread gracefully
 * @param timeout Maximum time to wait for thread shutdown
 * @return true if stopped cleanly, false on timeout or error
 */
bool ProcessEnforcer::stop(std::chrono::milliseconds timeout) {
  using silicon::logging::Logger;
  auto& logger = Logger::instance();

  std::thread thread_to_join;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!worker_.joinable()) {
      running_.store(false, std::memory_order_relaxed);
      stop_requested_.store(true, std::memory_order_relaxed);
      worker_exited_ = true;
      return true;
    }
  }

  stop_requested_.store(true, std::memory_order_relaxed);
  wake_cv_.notify_all();

  bool stopped = false; // acknowledgement from the thread
  {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_); // protects access to the thread's lifecycle state
    stopped = lifecycle_cv_.wait_for(lock, timeout, [this] { return worker_exited_; }); // wait here, until i get the response
    if (stopped) {
      thread_to_join = std::move(worker_);
    }
  }

  if (!stopped) {
    logger.error("ProcessEnforcer stop timeout after {} ms", timeout.count());
    return false;
  }

  if (thread_to_join.joinable()) {
    thread_to_join.join();
  }

  logger.info("ProcessEnforcer stopped cleanly");
  return true;
}

/**
 * @brief Retrieves statistics from the most recent scan
 * @return ScanStats structure containing scan metrics
 */
ProcessEnforcer::ScanStats ProcessEnforcer::get_last_scan_stats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return last_scan_stats_;
}

/**
 * @brief Main scanning loop executed in background thread
 */
void ProcessEnforcer::scan_loop() {
  using silicon::logging::Logger;
  auto& logger = Logger::instance();

  try {
    while (!stop_requested_.load(std::memory_order_relaxed)) {
      perform_scan();

      const auto interval = resolve_scan_interval();
      std::unique_lock<std::mutex> wait_lock(wake_mutex_); // locks the sleep wake system
      wake_cv_.wait_for(wait_lock, interval, [this] {
        return stop_requested_.load(std::memory_order_relaxed);
      });
    }
  } catch (const std::exception& err) {
    logger.error("ProcessEnforcer: Worker loop aborted: {}", err.what());
  } catch (...) {
    logger.error("ProcessEnforcer: Worker loop aborted due to unknown exception");
  }

  running_.store(false, std::memory_order_relaxed);
  // safely lock and try the worker off
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    worker_exited_ = true;
  }
  lifecycle_cv_.notify_all(); // tells that the process stopped to all stuff who are waiting for the worker to reply
}

/**
 * @brief Performs a single scanning iteration
 */
void ProcessEnforcer::perform_scan() {
  using silicon::logging::Logger;
  auto& logger = Logger::instance();

  ScanStats stats{};
  stats.timestamp = std::chrono::system_clock::now();

  if (config_ == nullptr) {
    publish_scan_stats(stats);
    return;
  }

  std::vector<std::string> system_ignore_patterns;
  try {
    system_ignore_patterns = config_->getSystemPathsToIgnore();
  } catch (const std::exception& err) {
    logger.error("ProcessEnforcer: Failed to read system ignore patterns: {}", err.what());
  }

  // (List ALL Processes)
  int enumerate_error = 0;
  const auto pids = enumerate_all_pids(&enumerate_error);
  if (pids.empty() && enumerate_error != 0) {
    if (enumerate_error == EACCES || enumerate_error == EPERM) {
      ++stats.permission_errors;
    }
    logger.error("ProcessEnforcer scan failed: process enumeration error code {}", enumerate_error);
    publish_scan_stats(stats);
    return;
  }

  for (const int pid : pids) {
    if (stop_requested_.load(std::memory_order_relaxed)) {
      break; // If someone told me to stop scanning, quit NOW
    }

    ProcessSnapshot snapshot{};
    bool permission_denied = false;
    if (!read_process_snapshot(pid, &snapshot, &permission_denied)) {
      if (permission_denied) {
        ++stats.permission_errors;
        logger.debug("ProcessEnforcer: Error inspecting PID {}: permission denied", pid);
      }
      continue;
    }

    if (snapshot.executable_path.empty() || snapshot.executable_path.front() != '/') {
      continue;
    }

    if (should_ignore_process(snapshot.executable_path, snapshot.executable_name, system_ignore_patterns)) {
      // logger.debug("ProcessEnforcer: Skipping system process: {} (PID: {})",
      //              sanitize_for_log(snapshot.executable_path),
      //              snapshot.pid);
      continue;
    }

    ++stats.total_processes_checked;

    bool allowed = true;
    try {
      allowed = config_->isProcessAllowed(snapshot.executable_path);
    } catch (const std::exception& err) {
      logger.error("ProcessEnforcer: Policy check failed for PID {}: {}", snapshot.pid, err.what());
      continue;
    }

    if (!allowed) {
      ++stats.violations_detected;
      const std::string safe_path = sanitize_for_log(snapshot.executable_path);
      const std::string violation_message =
          std::format("ProcessEnforcer: Blocked process detected: {} (PID: {})", safe_path, snapshot.pid);

      logger.warn("{}", violation_message);
      if (should_show_notifications()) {
        show_user_notification(violation_message);
      }
    }
  }

  publish_scan_stats(stats);
  logger.info("ProcessEnforcer: Scan completed - checked {} processes, {} violations, {} permission errors",
               stats.total_processes_checked,
               stats.violations_detected,
               stats.permission_errors);
}

std::vector<int> ProcessEnforcer::enumerate_all_pids(int* error_code_out) {
  if (error_code_out != nullptr) {
    *error_code_out = 0;
  }

  int sysctl_error = 0;
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
  
  /* 
  * 
  *  CTL_KERN: "Talk to the kernel"
  *  KERN_PROC: "About processes"
  *  KERN_PROC_ALL: "Give me ALL processes"
  *  0: "No special flags"
  * 
  */

  for (int attempt = 0; attempt < kSysctlRetryCount; ++attempt) {
    // STEP 1: Get required size
    size_t required_bytes = 0;

    // Hey macOS, how many bytes to store ALL process info
    if (::sysctl(mib, 4, nullptr, &required_bytes, nullptr, 0) == -1) {
      sysctl_error = errno;
      break;
    }

    // Kernel gave garbage data
    if (required_bytes == 0 || required_bytes % sizeof(kinfo_proc) != 0) {
      // Sanity check: should be multiple of kinfo_proc size
      return {};
    }

    // Correct capacity calculation
    if (required_bytes % sizeof(kinfo_proc) != 0) {
      // Kernel bug or race condition
      if (error_code_out) *error_code_out = EINVAL;
      return {};
    }

    const std::size_t capacity = required_bytes / sizeof(kinfo_proc);
    std::vector<kinfo_proc> process_entries(capacity);

    // STEP 2: Get actual data
    size_t actual_bytes = required_bytes;  // Start with max possible
    if (::sysctl(mib, 4, process_entries.data(), &actual_bytes, nullptr, 0) == -1) {
      if (errno == ENOMEM) {
        sysctl_error = errno;
        continue;  // Retry
      }
      sysctl_error = errno;
      break;
    }

    // Use ACTUAL bytes returned, not allocated capacity
    if (actual_bytes == 0 || actual_bytes % sizeof(kinfo_proc) != 0) {
      return {};  // Invalid data
    }

    const std::size_t actual_count = actual_bytes / sizeof(kinfo_proc);
    
    // FIX: Iterate only over ACTUAL entries
    std::vector<int> pids;
    pids.reserve(actual_count);
    for (std::size_t i = 0; i < actual_count && i < process_entries.size(); ++i) {
      const int pid = process_entries[i].kp_proc.p_pid;
      if (pid > 0) {
        pids.push_back(pid);
      }
    }

    std::sort(pids.begin(), pids.end());
    pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    return pids;
  }

  int fallback_error = 0;
  auto fallback_pids = enumerate_with_libproc(&fallback_error);
  if (!fallback_pids.empty()) {
    if (error_code_out != nullptr) {
      *error_code_out = 0;
    }
    return fallback_pids;
  }

  if (error_code_out != nullptr) {
    if (fallback_error != 0) {
      *error_code_out = fallback_error;
    } else if (sysctl_error != 0) {
      *error_code_out = sysctl_error;
    } else {
      *error_code_out = ENOMEM;
    }
  }
  return {};
}

/**
 * @brief Reads process information for a given PID
 * @param pid Process ID to inspect
 * @param snapshot_out Pointer to ProcessSnapshot to populate
 * @param permission_denied_out Optional pointer to receive permission status
 * @return true if snapshot was successfully read, false otherwise
 */
bool ProcessEnforcer::read_process_snapshot(int pid, ProcessSnapshot* snapshot_out, bool* permission_denied_out) {
  if (permission_denied_out != nullptr) {
    *permission_denied_out = false;
  }
  if (snapshot_out == nullptr || pid <= 0) {
    return false;
  }

  // Start with clean slate, store the PID we're checking
  snapshot_out->pid = pid;
  snapshot_out->executable_path.clear();
  snapshot_out->executable_name.clear();

  // Where is the executable file for process PID?
  char path_buffer[PROC_PIDPATHINFO_MAXSIZE] = {0};
  errno = 0;
  const int path_length = ::proc_pidpath(pid, path_buffer, sizeof(path_buffer));
  if (path_length > 0) {
    snapshot_out->executable_path.assign(path_buffer);
  } else if (permission_denied_out != nullptr && (errno == EACCES || errno == EPERM)) {
    *permission_denied_out = true;
  }

  char name_buffer[PROC_PIDPATHINFO_MAXSIZE] = {0};
  const int name_length = ::proc_name(pid, name_buffer, sizeof(name_buffer));
  if (name_length > 0) {
    snapshot_out->executable_name.assign(name_buffer);
  } else if (!snapshot_out->executable_path.empty()) {
    snapshot_out->executable_name = basename_from_path(snapshot_out->executable_path);
  }

  if (snapshot_out->executable_path.empty()) {
    return false;
  }

  if (snapshot_out->executable_name.empty()) {
    snapshot_out->executable_name = basename_from_path(snapshot_out->executable_path);
  }

  return !snapshot_out->executable_name.empty() || !snapshot_out->executable_path.empty();
}

/**
 * @brief Determines scan interval from configuration
 * @return Scan interval in milliseconds
 */
std::chrono::milliseconds ProcessEnforcer::resolve_scan_interval() const {
  if (config_ == nullptr) {
    return kDefaultScanIntervalMs;
  }

  int raw_interval = 0;
  try {
    raw_interval = config_->getProcessScanInterval();
  } catch (...) {
    return kDefaultScanIntervalMs;
  }

  if (raw_interval <= 0) {
    return kDefaultScanIntervalMs;
  }

  std::chrono::milliseconds interval =
      (raw_interval >= 250) ? std::chrono::milliseconds(raw_interval)
                            : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds(raw_interval));

  if (interval < kMinScanIntervalMs) {
    interval = kMinScanIntervalMs;
  }
  return interval;
}

/**
 * @brief Checks if a process should be ignored based on system patterns
 * @param path Full executable path
 * @param name Process name
 * @param ignore_patterns Vector of system ignore patterns
 * @return true if process should be ignored, false otherwise
 */
bool ProcessEnforcer::should_ignore_process(std::string_view path,
                                            std::string_view name,
                                            const std::vector<std::string>& ignore_patterns) const {
  const std::string normalized_path = lower_ascii_copy(path);
  std::string normalized_name = lower_ascii_copy(name);
  if (normalized_name.empty()) {
    normalized_name = lower_ascii_copy(basename_from_path(path));
  }

  for (const auto& raw_pattern : ignore_patterns) {
    if (raw_pattern.empty()) {
      continue;
    }

    const std::string pattern = lower_ascii_copy(raw_pattern);
    if (pattern.empty()) {
      continue;
    }

    if (pattern.front() == '/') {
      if (has_directory_prefix(normalized_path, pattern)) {
        return true;
      }
      continue;
    }

    if (!normalized_name.empty() && normalized_name == pattern) {
      return true;
    }
  }

  return false;
}

/**
 * @brief Checks if a path starts with a directory prefix
 * @param path Path to check
 * @param prefix Directory prefix to match
 * @return true if path starts with prefix and is a directory boundary
 */
bool ProcessEnforcer::has_directory_prefix(std::string_view path, std::string_view prefix) {
  if (prefix.empty() || path.size() < prefix.size()) {
    return false;
  }
  if (!path.starts_with(prefix)) {
    return false;
  }
  if (path.size() == prefix.size()) {
    return true;
  }
  if (prefix.back() == '/') {
    return true;
  }
  return path[prefix.size()] == '/';
}

/**
 * @brief Converts string to lowercase ASCII copy
 * @param value Input string
 * @return Lowercase copy of input
 */
std::string ProcessEnforcer::lower_ascii_copy(std::string_view value) {
  std::string output(value.begin(), value.end());
  std::transform(output.begin(), output.end(), output.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return output;
}

/**
 * @brief Extracts basename from full path
 * @param path Full filesystem path
 * @return Basename component of path
 */
std::string ProcessEnforcer::basename_from_path(std::string_view path) {
  if (path.empty()) {
    return {};
  }
  const std::size_t pos = path.find_last_of('/');
  if (pos == std::string_view::npos) {
    return std::string(path);
  }
  if (pos + 1 >= path.size()) {
    return {};
  }
  return std::string(path.substr(pos + 1));
}

/**
 * @brief Sanitizes string for safe logging
 * @param value Input string potentially containing special characters
 * @return Sanitized string safe for log output
 */
std::string ProcessEnforcer::sanitize_for_log(std::string_view value) {
  std::string output;
  output.reserve(value.size());
  for (const unsigned char c : value) {
    if (c == '\n' || c == '\r' || c == '\t') {
      output.push_back(' ');
      continue;
    }
    if (std::isprint(c) != 0) {
      output.push_back(static_cast<char>(c));
      continue;
    }
    output.push_back('?');
  }
  return output;
}

/**
 * @brief Determines if user notifications should be shown
 * @return true if in production mode (not debug), false otherwise
 */
bool ProcessEnforcer::should_show_notifications() const {
  if (config_ == nullptr) {
    return false;
  }
  try {
    return !config_->isDebugMode();
  } catch (...) {
    return false;
  }
}

/**
 * @brief Placeholder for user notification mechanism
 * @param message Notification message to display
 */
void ProcessEnforcer::show_user_notification(std::string_view message) const {
  silicon::logging::Logger::instance().info(
      "ProcessEnforcer: Notification placeholder invoked: {}",
      sanitize_for_log(message));
}

/**
 * @brief Updates scan statistics with thread safety
 * @param stats New scan statistics to publish
 */
void ProcessEnforcer::publish_scan_stats(const ScanStats& stats) {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  last_scan_stats_ = stats;
}

}  // namespace silicon::process