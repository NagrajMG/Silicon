/*
================================================================================
SILICON – FILESYSTEM ENFORCER TECH GLOSSARY

This file implements a macOS user-space filesystem monitoring engine using:
- std::filesystem
- FSEvents (Apple API)
- libdispatch (GCD)
- multithreading (std::thread)
- structured logging
- snapshot-based diff detection
- event-driven + polling fallback design

--------------------------------------------------------------------------------
CORE CONCEPTS
--------------------------------------------------------------------------------

FilesystemEnforcer
  Main monitoring engine for filesystem policy enforcement.
  Runs in background thread. Observes volumes + protected paths.
  DOES NOT delete or unmount anything. Logs + warns only.

ScanStats
  Per-scan telemetry structure:
    mounts_seen
    new_mounts_detected
    path_events_detected
    notifications_emitted
    permission_errors

EventBackend
  Wrapper struct holding:
    FSEventStreamRef (Apple FSEvents stream)
    dispatch_queue_t (GCD queue)
  Used to manage macOS event stream safely.

--------------------------------------------------------------------------------
macOS-SPECIFIC APIs
--------------------------------------------------------------------------------

FSEvents
  Apple filesystem event API.
  Notifies when files/folders change.
  Event-driven monitoring (efficient).

dispatch_queue_t
  Grand Central Dispatch queue.
  Used to process FSEvents asynchronously.

CFArrayRef / CFStringRef
  CoreFoundation types (C-based macOS framework).

FSEventStreamCreate()
  Creates filesystem event stream.

FSEventStreamStart()
  Starts receiving events.

FSEventStreamStop()
  Stops stream.

kFSEventStreamEventFlagMustScanSubDirs
  Indicates dropped events; requires full rescan.

--------------------------------------------------------------------------------
std::filesystem
--------------------------------------------------------------------------------

std::filesystem::status(path)
  Returns metadata about file.

std::filesystem::last_write_time(path)
  Returns last modification timestamp.

std::filesystem::recursive_directory_iterator
  Walks entire directory tree.

std::error_code
  Thread-safe error reporting object.

--------------------------------------------------------------------------------
Concurrency & Threading
--------------------------------------------------------------------------------

std::atomic<bool>
  Lock-free thread-safe flags:
    running_
    stop_requested_
    has_pending_events_

--------------------------------------------------------------------------------
Design Patterns Used
--------------------------------------------------------------------------------

Event-driven + Polling Fallback
  If FSEvents works → react to changes.
  If FSEvents unavailable → full snapshot diff.

Snapshot Diffing
  Store previous filesystem snapshot.
  Compare to current snapshot.
  Detect:
    - created
    - modified
    - deleted

Rate Limiting
  Prevents notification spam.
  Uses timestamp comparisons.

Dependency Injection
  Interfaces:
    IFileSystem
    IClipboard
    NotificationSink
  Allows mocking in tests.

--------------------------------------------------------------------------------
Security-Oriented Concepts
--------------------------------------------------------------------------------

Protected Paths
  Sensitive directories defined in config.

Allowed Volumes
  Whitelisted external mounts.

Permission Errors
  EACCES / EPERM
  File exists but cannot access (SIP / macOS security).

Clipboard Hook
  Periodically clears clipboard if policy enabled.

sanitize_for_log()
  Removes control characters to prevent log injection.

--------------------------------------------------------------------------------
Error Handling
--------------------------------------------------------------------------------

EACCES
  Permission denied.

EPERM
  Operation not permitted (macOS SIP).

describe_error_code()
  Converts errno to readable string.

--------------------------------------------------------------------------------
Important Member Variables
--------------------------------------------------------------------------------

known_mounts_
  Tracks previous mounted volumes.

known_path_snapshot_
  Snapshot of previous filesystem state.

event_backend_
  Non-null when FSEvents active.

pending_event_paths_
  Paths collected from event callback.

volumes_root_
  "/Volumes" on macOS.

--------------------------------------------------------------------------------
Execution Flow Summary
--------------------------------------------------------------------------------

start()
  -> launches worker thread

scan_loop()
  -> perform_scan()
  -> wait (interval or event)

perform_scan()
  -> drain events
  -> monitor volumes
  -> monitor paths
  -> clipboard hook
  -> publish stats

stop()
  -> request shutdown
  -> join worker thread safely

================================================================================
IMPLEMENTATION STARTS HERE...
================================================================================
*/
#include "filesystem/filesystem_enforcer.hpp"

// Apple’s low-level concurrency/event-loop API
#include <dispatch/dispatch.h> // FSevents

#include <algorithm>
#include <cctype> // for individual char, tolower types
#include <cerrno> // Limited to standard C library errors, not thread safe
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <sstream>
#include <system_error> // Thread-safe and C++ error object

#include "logging/logger.hpp"

namespace silicon::filesystem {

namespace {
constexpr std::chrono::milliseconds kDefaultScanIntervalMs{2000};
constexpr std::chrono::milliseconds kMinScanIntervalMs{250}; // safety floor to avoid high level modifications
constexpr std::chrono::seconds kDefaultNotificationRateLimit{5};
constexpr std::chrono::seconds kMinNotificationRateLimit{1};
constexpr std::chrono::seconds kDefaultClipboardInterval{15};
constexpr std::chrono::seconds kMinClipboardInterval{5};
constexpr std::chrono::seconds kVolumeResyncInterval{60};
constexpr std::size_t kMaxPathEventsLoggedPerScan = 32;
constexpr CFTimeInterval kFSEventLatencySeconds = 0.25; // double representing seconds

bool is_permission_error(int error_code)  // Error Access (CAN SEE IT, CANT TOUCH IT)
  { return error_code == EACCES || error_code == EPERM; } //  Error Operation Not Permitted by SIP

std::string describe_error_code(int error_code) {
  if (error_code <= 0) {
    return "unknown";
  }
  // the OS maintains a static table of error strings
  // returns ppinter to the start of the string
  const char* text = std::strerror(error_code);
  if (text == nullptr) {
    return "unknown";
  }
  return std::string(text);
}

bool fill_fingerprint(const std::filesystem::path& path,
                      IFileSystem::PathFingerprint* out,
                      int* error_code_out) {
  if (error_code_out != nullptr) {
    *error_code_out = 0;
  }
  if (out == nullptr) {
    if (error_code_out != nullptr) {
      *error_code_out = EINVAL; // Invalid Argument
    }
    return false;
  }

  std::error_code ec;
  const auto status = std::filesystem::status(path, ec); //
  if (ec) {
    if (error_code_out != nullptr) {
      *error_code_out = ec.value();
    }
    return false;
  }

  out->is_directory = std::filesystem::is_directory(status); // Gives object that Stores information 
                                                              //about the type and permissions of a file.
  out->size_bytes = 0;

  if (std::filesystem::is_regular_file(status)) {
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
      if (error_code_out != nullptr) {
        *error_code_out = ec.value();
      }
      return false;
    }
    out->size_bytes = file_size;
  }

  const auto write_time = std::filesystem::last_write_time(path, ec);
  if (ec) {
    if (error_code_out != nullptr) {
      *error_code_out = ec.value();
    }
    return false;
  }
  out->modified_ticks = write_time.time_since_epoch().count(); // Jan 1, 1970
  return true;
}

}  // namespace

// "wrapper" pattern used to manage resources that don't natively follow C++ rules
struct FilesystemEnforcer::EventBackend {
  FSEventStreamRef stream{nullptr};
  dispatch_queue_t queue{nullptr};
};
// you can start, stop, and invalidate the stream from anywhere inside your FilesystemEnforcer class
void LoggingNotificationSink::notify(std::string_view message) {
  silicon::logging::Logger::instance().warn("NOTIFICATION: {}",
                                            FilesystemEnforcer::sanitize_for_log(message));
}

bool SystemClipboard::clear(std::string* error_message_out) {
  if (error_message_out != nullptr) {
    error_message_out->clear();
  }

  errno = 0; // ERROR NUMBER
  FILE* pipe = ::popen("/usr/bin/pbcopy", "w"); // Process open,pipe - a one-way data tunnel

  // pbcopy is the clipboard
  if (pipe == nullptr) {
    if (error_message_out != nullptr) {
      *error_message_out = std::strerror(errno);
    }
    return false;
  }

  const int close_status = ::pclose(pipe); // Closing the Pipe
  if (close_status != 0) {
    if (error_message_out != nullptr) {
      *error_message_out = std::format("pbcopy exited with status {}", close_status);
    }
    return false;
  }
  return true;
}

std::chrono::system_clock::time_point LocalFileSystem::now() const {
  return std::chrono::system_clock::now();
}

bool LocalFileSystem::list_directories(std::string_view root,
                                       std::vector<std::string>* directories_out,
                                       int* error_code_out,
                                       std::size_t* permission_errors_out) {
  if (directories_out == nullptr) {
    if (error_code_out != nullptr) {
      *error_code_out = EINVAL;
    }
    return false;
  }

  directories_out->clear();
  if (error_code_out != nullptr) {
    *error_code_out = 0;
  }
  if (permission_errors_out != nullptr) {
    *permission_errors_out = 0;
  }

  std::error_code ec;
  std::filesystem::directory_iterator it(
      std::filesystem::path(root), std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    if (error_code_out != nullptr) {
      *error_code_out = ec.value();
    }
    return false;
  }

  for (const auto& entry : it) {
    std::error_code is_dir_ec;
    const bool is_dir = entry.is_directory(is_dir_ec);
    if (is_dir_ec) {
      if (permission_errors_out != nullptr && is_permission_error(is_dir_ec.value())) {
        ++(*permission_errors_out);
      }
      continue;
    }
    if (is_dir) {
      directories_out->push_back(entry.path().generic_string());
    }
  }

  std::sort(directories_out->begin(), directories_out->end());
  // unqiue points to the new end of the vector
  directories_out->erase(std::unique(directories_out->begin(), directories_out->end()),
                         directories_out->end());
  return true;
}

bool LocalFileSystem::snapshot_tree(std::string_view root,
                                    Snapshot* snapshot_out,
                                    int* error_code_out,
                                    std::size_t* permission_errors_out) {
  if (snapshot_out == nullptr) {
    if (error_code_out != nullptr) {
      *error_code_out = EINVAL;
    }
    return false;
  }

  snapshot_out->clear();
  if (error_code_out != nullptr) {
    *error_code_out = 0;
  }
  if (permission_errors_out != nullptr) {
    *permission_errors_out = 0;
  }

  const std::filesystem::path root_path(root);
  std::error_code ec;
  const bool exists = std::filesystem::exists(root_path, ec);
  // true 
  if (ec) {
    if (error_code_out != nullptr) {
      *error_code_out = ec.value();
    }
    return false;
  }

  // false
  if (!exists) {
    /* A missing folder is a valid state, not a program error. */
    return true;
  }

  IFileSystem::PathFingerprint root_fingerprint{};
  int root_error = 0;
  if (!fill_fingerprint(root_path, &root_fingerprint, &root_error)) {
    if (permission_errors_out != nullptr && is_permission_error(root_error)) {
      ++(*permission_errors_out);
      return true;
    }
    if (error_code_out != nullptr) {
      *error_code_out = root_error;
    }
    return false;
  }
  snapshot_out->emplace(root_path.generic_string(), root_fingerprint);

  std::filesystem::recursive_directory_iterator it(
      root_path, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    if (error_code_out != nullptr) {
      *error_code_out = ec.value();
    }
    return false;
  }

  for (auto end = std::filesystem::recursive_directory_iterator{}; it != end; it.increment(ec)) {
    if (ec) {
      if (permission_errors_out != nullptr && is_permission_error(ec.value())) {
        ++(*permission_errors_out);
      }
      /* It sticks to the iterator in the directory iterator */
      ec.clear(); /* If I don't clear it, the iterator will just keep reporting the same error forever */
      continue;
    }

    IFileSystem::PathFingerprint fingerprint{};
    int fingerprint_error = 0;
    if (!fill_fingerprint(it->path(), &fingerprint, &fingerprint_error)) {
      if (permission_errors_out != nullptr && is_permission_error(fingerprint_error)) {
        ++(*permission_errors_out);
      }
      continue;
    }
    snapshot_out->emplace(it->path().generic_string(), fingerprint);
  }
  return true;
}

FilesystemEnforcer::FilesystemEnforcer(std::shared_ptr<silicon::config::ConfigManager> config,
                                       std::shared_ptr<IFileSystem> file_system,
                                       std::shared_ptr<NotificationSink> notification_sink,
                                       std::shared_ptr<IClipboard> clipboard,
                                       std::string volumes_root)
    : config_(std::move(config)), /* steel the pointer which is fast */
      file_system_(std::move(file_system)),
      notification_sink_(std::move(notification_sink)),
      clipboard_(std::move(clipboard)),
      volumes_root_(volumes_root.empty() ? "/Volumes" : std::move(volumes_root)) {
  auto& logger = silicon::logging::Logger::instance();

  if (config_ == nullptr) {
    logger.error("FilesystemEnforcer: Cannot initialize without ConfigManager");
    return;
  }

  if (file_system_ == nullptr) {
    file_system_ = std::make_shared<LocalFileSystem>();
  }
  if (notification_sink_ == nullptr) {
    notification_sink_ = std::make_shared<LoggingNotificationSink>();
  }
  if (clipboard_ == nullptr) {
    clipboard_ = std::make_shared<SystemClipboard>();
  }

  logger.info("FilesystemEnforcer initialized with scan interval: {} ms, volumes root: {}",
              resolve_scan_interval().count(),
              sanitize_for_log(volumes_root_));
}

FilesystemEnforcer::~FilesystemEnforcer() {
  auto& logger = silicon::logging::Logger::instance();
  if (!stop(std::chrono::milliseconds(1500))) {
    logger.warn(
        "FilesystemEnforcer: Graceful stop timed out in destructor; waiting for worker to exit");
    stop_requested_.store(true, std::memory_order_relaxed);
    wake_cv_.notify_all(); // wake if that thread was sleeping

    // stop and join -- moving the thread out of the protected scope before joining
    std::thread thread_to_join;
    {
      // cannot call .join() while holding the lifecycle_mutex_
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      thread_to_join = std::move(worker_);
    }
    if (thread_to_join.joinable()) {
      thread_to_join.join();
    }
  }
}

bool FilesystemEnforcer::start() {
  auto& logger = silicon::logging::Logger::instance();
  if (config_ == nullptr || file_system_ == nullptr) {
    logger.error("FilesystemEnforcer: start failed due to missing dependencies");
    return false;
  }

  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (worker_.joinable() || running_.load(std::memory_order_relaxed)) {
    logger.warn("FilesystemEnforcer: start requested while already running");
    return false;
  }

  // startup for the thread
  stop_requested_.store(false, std::memory_order_relaxed);
  worker_exited_ = false;
  running_.store(true, std::memory_order_relaxed);

  try {
    worker_ = std::thread(&FilesystemEnforcer::scan_loop, this);
  } catch (const std::exception& err) {
    running_.store(false, std::memory_order_relaxed);
    worker_exited_ = true;
    logger.error("FilesystemEnforcer: Failed to start worker thread: {}", err.what());
    return false;
  }

  std::ostringstream tid;
  tid << worker_.get_id();
  logger.info("FilesystemEnforcer started with thread ID: {}", tid.str());
  return true;
}

bool FilesystemEnforcer::stop(std::chrono::milliseconds timeout) {
  auto& logger = silicon::logging::Logger::instance();

  std::thread thread_to_join;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    // making it stop, joinable is false means we need to stop it gracefully
    // if thread isn't running, just clean state and return
    if (!worker_.joinable()) {
      running_.store(false, std::memory_order_relaxed);
      stop_requested_.store(true, std::memory_order_relaxed);
      worker_exited_ = true;
      return true;
    }

    /* if not joinable meaning, that was a dead process */
  }

  stop_requested_.store(true, std::memory_order_relaxed);
  wake_cv_.notify_all();

  bool stopped = false;
  {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    // CURRENT THREAD waits for WORKER THREAD to set worker_exited_ = true
    stopped = lifecycle_cv_.wait_for(lock, timeout, [this] { return worker_exited_; });
    if (stopped) {
      thread_to_join = std::move(worker_);
    }
  }

  if (!stopped) {
    logger.error("FilesystemEnforcer stop timeout after {} ms", timeout.count());
    return false;
  }

  if (thread_to_join.joinable()) {
    thread_to_join.join();
  }

  logger.info("FilesystemEnforcer stopped cleanly");
  return true;
}

FilesystemEnforcer::ScanStats FilesystemEnforcer::get_last_scan_stats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return last_scan_stats_;
}

void FilesystemEnforcer::run_single_scan_for_test() { perform_scan(); }

void FilesystemEnforcer::queue_path_event_for_test(std::string path) {
  enqueue_path_event(std::move(path));
}

std::string FilesystemEnforcer::sanitize_for_log(std::string_view value) {
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

void FilesystemEnforcer::scan_loop() {
  auto& logger = silicon::logging::Logger::instance();
  const bool events_enabled = start_event_stream();
  if (!events_enabled) {
    logger.warn(
        "FilesystemEnforcer: FSEvents unavailable; using polling fallback for path monitoring");
  }


  /*
    *
    * stop requested 
    * has pending events
    * time completed
    * 
    */

  try {
    while (!stop_requested_.load(std::memory_order_relaxed)) {
      perform_scan();

      const auto interval = resolve_scan_interval();
      std::unique_lock<std::mutex> wait_lock(wake_mutex_);
      wake_cv_.wait_for(wait_lock, interval, [this] {
        return stop_requested_.load(std::memory_order_relaxed) ||
               has_pending_events_.load(std::memory_order_relaxed);
      });
    }
  } catch (const std::exception& err) { // well defined cpp exceptions
    logger.error("FilesystemEnforcer: Worker loop aborted: {}", err.what());
  } catch (...) {
    logger.error("FilesystemEnforcer: Worker loop aborted due to unknown exception");
  }

  stop_event_stream();
  running_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    worker_exited_ = true;
  }
  lifecycle_cv_.notify_all();
}

void FilesystemEnforcer::perform_scan() {
  auto& logger = silicon::logging::Logger::instance();

  ScanStats stats{};
  stats.timestamp =
      (file_system_ != nullptr) ? file_system_->now() : std::chrono::system_clock::now();

  logger.debug("FilesystemEnforcer: Scan cycle started");

  if (config_ == nullptr || file_system_ == nullptr) {
    publish_scan_stats(stats);
    return;
  }

  std::vector<std::string> pending_paths;
  bool volume_event_pending = false;
  drain_pending_events(&pending_paths, &volume_event_pending);

  if (event_backend_ != nullptr) {
    // Full scan to establish baseline, 
    bool should_scan_volumes = !known_mounts_initialized_ || volume_event_pending;
    if (!should_scan_volumes) {
      if (!has_last_volume_resync_) {
        has_last_volume_resync_ = true;
        last_volume_resync_ = stats.timestamp;
      } else if ((stats.timestamp - last_volume_resync_) >= kVolumeResyncInterval) {
        should_scan_volumes = true;
      }
    }
    if (should_scan_volumes) {
      monitor_volumes(&stats);
      has_last_volume_resync_ = true;
      last_volume_resync_ = stats.timestamp;
    }
    monitor_paths_from_events(pending_paths, &stats);
  } else {
    monitor_volumes(&stats);
    if (!pending_paths.empty()) {
      monitor_paths_from_events(pending_paths, &stats);
    } else {
      monitor_paths_polling(&stats);
    }
  }

  run_clipboard_hook(stats.timestamp);

  publish_scan_stats(stats);
  logger.debug(
      "FilesystemEnforcer: Scan cycle complete - mounts={}, new_mounts={}, path_events={}, "
      "notifications={}, permission_errors={}",
      stats.mounts_seen,
      stats.new_mounts_detected,
      stats.path_events_detected,
      stats.notifications_emitted,
      stats.permission_errors);
}

void FilesystemEnforcer::monitor_volumes(ScanStats* stats) {
  if (stats == nullptr || file_system_ == nullptr) {
    return;
  }

  auto& logger = silicon::logging::Logger::instance();

  std::vector<std::string> mounts;
  int error_code = 0;
  std::size_t permission_errors = 0;
  const bool listed =
      file_system_->list_directories(volumes_root_, &mounts, &error_code, &permission_errors);
  stats->permission_errors += permission_errors;

  if (!listed) {
    if (is_permission_error(error_code)) {
      ++stats->permission_errors;
    }
    logger.error("FilesystemEnforcer: Failed to list mount root {}: {} (errno={})",
                 sanitize_for_log(volumes_root_),
                 describe_error_code(error_code),
                 error_code);
    return;
  }

  std::unordered_set<std::string> current_mounts;
  for (const auto& mount : mounts) {
    if (mount.empty()) {
      continue;
    }
    current_mounts.insert(mount);
  }
  stats->mounts_seen = current_mounts.size();

  if (!known_mounts_initialized_) {
    known_mounts_ = std::move(current_mounts);
    known_mounts_initialized_ = true;
    return;
  }

  const auto timestamp = std::chrono::floor<std::chrono::seconds>(stats->timestamp);
  const std::string timestamp_text = std::format("{:%Y-%m-%d %H:%M:%S}", timestamp);

  for (const auto& mount : current_mounts) {
    if (known_mounts_.contains(mount)) {
      continue;
    }
    if (is_mount_allowed(mount)) {
      logger.debug("FilesystemEnforcer: Allowed mount detected; suppressing alert: {}",
                   sanitize_for_log(mount));
      continue;
    }

    ++stats->new_mounts_detected;
    const std::string volume_name = sanitize_for_log(basename_from_path(mount));
    const std::string safe_mount = sanitize_for_log(mount);

    logger.warn(
        "FilesystemEnforcer: New external volume detected during exam mode; volume='{}' mount='{}' "
        "timestamp='{}' reason='New external volume detected during exam mode'",
        volume_name.empty() ? "<unknown>" : volume_name,
        safe_mount,
        timestamp_text);

    emit_notification_if_allowed(std::format("External volume detected: {} ({})",
                                             volume_name.empty() ? "<unknown>" : volume_name,
                                             safe_mount),
                                 stats);
  }

  known_mounts_ = std::move(current_mounts);
}

void FilesystemEnforcer::monitor_paths_from_events(const std::vector<std::string>& event_paths,
                                                   ScanStats* stats) {
  if (stats == nullptr || config_ == nullptr) {
    return;
  }

  auto& logger = silicon::logging::Logger::instance();
  const auto monitored_paths = load_monitored_paths();
  if (monitored_paths.empty() || event_paths.empty()) {
    return;
  }

  std::vector<std::string> ignore_patterns;
  try {
    ignore_patterns = config_->getSystemPathsToIgnore();
  } catch (const std::exception& err) {
    logger.error("FilesystemEnforcer: Failed to read ignore path list: {}", err.what());
    return;
  }

  std::unordered_set<std::string> unique_events;
  std::size_t logged_events = 0;
  std::size_t matched_events = 0;
  for (const auto& raw_path : event_paths) {
    if (raw_path.empty()) {
      continue;
    }
    if (!unique_events.insert(raw_path).second) {
      continue;
    }
    if (has_directory_prefix(raw_path, volumes_root_)) {
      continue;
    }
    if (should_ignore_path(raw_path, ignore_patterns)) {
      continue;
    }

    bool inside_monitored_root = false;
    for (const auto& root : monitored_paths) {
      if (has_directory_prefix(raw_path, root)) {
        inside_monitored_root = true;
        break;
      }
    }
    if (!inside_monitored_root) {
      continue;
    }

    ++matched_events;
    ++stats->path_events_detected;
    if (logged_events < kMaxPathEventsLoggedPerScan) {
      ++logged_events;
      logger.warn("FilesystemEnforcer: Protected path change detected [fsevent]: {}",
                  sanitize_for_log(raw_path));
    }

    emit_notification_if_allowed(std::format("Filesystem activity detected in monitored path: {}",
                                             sanitize_for_log(raw_path)),
                                 stats);
  }

  if (matched_events > kMaxPathEventsLoggedPerScan) {
    logger.warn("FilesystemEnforcer: {} additional path change events suppressed this scan cycle",
                matched_events - kMaxPathEventsLoggedPerScan);
  }
}

void FilesystemEnforcer::monitor_paths_polling(ScanStats* stats) {
  if (stats == nullptr || file_system_ == nullptr) {
    return;
  }

  auto& logger = silicon::logging::Logger::instance();

  const auto monitored_paths = load_monitored_paths();
  IFileSystem::Snapshot current_snapshot;

  for (const auto& root_path : monitored_paths) {
    if (stop_requested_.load(std::memory_order_relaxed)) {
      break;
    }

    IFileSystem::Snapshot root_snapshot;
    int error_code = 0;
    std::size_t permission_errors = 0;
    const bool snapshot_ok =
        file_system_->snapshot_tree(root_path, &root_snapshot, &error_code, &permission_errors);
    stats->permission_errors += permission_errors;

    if (!snapshot_ok) {
      if (is_permission_error(error_code)) {
        ++stats->permission_errors;
        logger.debug("FilesystemEnforcer: Permission denied while scanning monitored path {}",
                     sanitize_for_log(root_path));
      } else {
        logger.error("FilesystemEnforcer: Failed to scan monitored path {}: {} (errno={})",
                     sanitize_for_log(root_path),
                     describe_error_code(error_code),
                     error_code);
      }
      continue;
    }

    current_snapshot.insert(root_snapshot.begin(), root_snapshot.end());
  }

  if (!known_path_snapshot_initialized_) {
    known_path_snapshot_ = std::move(current_snapshot);
    known_path_snapshot_initialized_ = true;
    return;
  }

  struct PathEvent {
    std::string action;
    std::string path;
  };
  std::vector<PathEvent> events;

  for (const auto& [path, fingerprint] : current_snapshot) {
    const auto previous = known_path_snapshot_.find(path);
    if (previous == known_path_snapshot_.end()) {
      events.push_back({"created", path});
      continue;
    }
    if (!fingerprints_equal(previous->second, fingerprint)) {
      events.push_back({"modified", path});
    }
  }

  for (const auto& [path, _] : known_path_snapshot_) {
    if (!current_snapshot.contains(path)) {
      events.push_back({"deleted", path});
    }
  }

  std::size_t logged_events = 0;
  for (const auto& event : events) {
    ++stats->path_events_detected;

    if (logged_events < kMaxPathEventsLoggedPerScan) {
      ++logged_events;
      logger.warn("FilesystemEnforcer: Protected path change detected [{}]: {}",
                  sanitize_for_log(event.action),
                  sanitize_for_log(event.path));
    }

    emit_notification_if_allowed(std::format("Filesystem activity detected in monitored path: {}",
                                             sanitize_for_log(event.path)),
                                 stats);
  }

  if (events.size() > kMaxPathEventsLoggedPerScan) {
    logger.warn("FilesystemEnforcer: {} additional path change events suppressed this scan cycle",
                events.size() - kMaxPathEventsLoggedPerScan);
  }

  known_path_snapshot_ = std::move(current_snapshot);
}

bool FilesystemEnforcer::start_event_stream() {
  if (event_backend_ != nullptr) {
    return true;
  }

  auto& logger = silicon::logging::Logger::instance();

  std::vector<std::string> watch_roots = load_monitored_paths();
  watch_roots.push_back(volumes_root_);

  std::unordered_set<std::string> dedupe;
  std::vector<std::string> normalized_watch_roots;
  normalized_watch_roots.reserve(watch_roots.size());
  for (const auto& root : watch_roots) {
    if (root.empty()) {
      continue;
    }
    if (dedupe.insert(root).second) {
      normalized_watch_roots.push_back(root);
    }
  }

  if (normalized_watch_roots.empty()) {
    logger.warn("FilesystemEnforcer: No watch roots configured for FSEvents");
    return false;
  }

  CFMutableArrayRef cf_paths = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
  if (cf_paths == nullptr) {
    logger.error("FilesystemEnforcer: Failed to allocate FSEvents watch root array");
    return false;
  }

  for (const auto& root : normalized_watch_roots) {
    CFStringRef cf_root =
        CFStringCreateWithCString(kCFAllocatorDefault, root.c_str(), kCFStringEncodingUTF8);
    if (cf_root == nullptr) {
      logger.warn("FilesystemEnforcer: Skipping invalid FSEvents watch root {}",
                  sanitize_for_log(root));
      continue;
    }
    CFArrayAppendValue(cf_paths, cf_root);
    CFRelease(cf_root);
  }

  if (CFArrayGetCount(cf_paths) == 0) {
    CFRelease(cf_paths);
    logger.error("FilesystemEnforcer: FSEvents watch root list is empty after validation");
    return false;
  }

  auto backend = std::make_unique<EventBackend>();

  FSEventStreamContext context{};
  context.info = this;
  backend->stream =
      FSEventStreamCreate(kCFAllocatorDefault,
                          &FilesystemEnforcer::fsevents_callback,
                          &context,
                          cf_paths,
                          kFSEventStreamEventIdSinceNow,
                          kFSEventLatencySeconds,
                          kFSEventStreamCreateFlagUseCFTypes | kFSEventStreamCreateFlagFileEvents |
                              kFSEventStreamCreateFlagNoDefer);
  CFRelease(cf_paths);

  if (backend->stream == nullptr) {
    logger.error("FilesystemEnforcer: Failed to create FSEvents stream");
    return false;
  }

  backend->queue =
      dispatch_queue_create("com.silicon.agent.filesystem.fsevents", DISPATCH_QUEUE_SERIAL);
  if (backend->queue == nullptr) {
    logger.error("FilesystemEnforcer: Failed to create dispatch queue for FSEvents");
    FSEventStreamInvalidate(backend->stream);
    FSEventStreamRelease(backend->stream);
    return false;
  }

  FSEventStreamSetDispatchQueue(backend->stream, backend->queue);
  if (FSEventStreamStart(backend->stream) == false) {
    logger.error("FilesystemEnforcer: Failed to start FSEvents stream");
    FSEventStreamInvalidate(backend->stream);
    FSEventStreamRelease(backend->stream);
#if !defined(OS_OBJECT_USE_OBJC) || !OS_OBJECT_USE_OBJC
    dispatch_release(backend->queue);
#endif
    return false;
  }

  event_backend_ = std::move(backend);
  logger.info("FilesystemEnforcer: FSEvents stream started with {} watch roots",
              normalized_watch_roots.size());
  return true;
}

void FilesystemEnforcer::stop_event_stream() {
  if (event_backend_ == nullptr) {
    return;
  }

  if (event_backend_->stream != nullptr) {
    FSEventStreamStop(event_backend_->stream);
    FSEventStreamInvalidate(event_backend_->stream);
    FSEventStreamRelease(event_backend_->stream);
    event_backend_->stream = nullptr;
  }

  if (event_backend_->queue != nullptr) {
#if !defined(OS_OBJECT_USE_OBJC) || !OS_OBJECT_USE_OBJC
    dispatch_release(event_backend_->queue);
#endif
    event_backend_->queue = nullptr;
  }

  event_backend_.reset();

  std::lock_guard<std::mutex> lock(event_mutex_);
  pending_event_paths_.clear();
  volumes_event_pending_ = false;
  has_pending_events_.store(false, std::memory_order_relaxed);
}

void FilesystemEnforcer::drain_pending_events(std::vector<std::string>* event_paths_out,
                                              bool* volume_event_pending_out) {
  if (event_paths_out != nullptr) {
    event_paths_out->clear();
  }
  if (volume_event_pending_out != nullptr) {
    *volume_event_pending_out = false;
  }

  std::lock_guard<std::mutex> lock(event_mutex_);
  if (event_paths_out != nullptr) {
    event_paths_out->swap(pending_event_paths_);
  } else {
    pending_event_paths_.clear();
  }
  if (volume_event_pending_out != nullptr) {
    *volume_event_pending_out = volumes_event_pending_;
  }
  volumes_event_pending_ = false;
  has_pending_events_.store(false, std::memory_order_relaxed);
}

void FilesystemEnforcer::enqueue_path_event(std::string path) {
  if (path.empty()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    pending_event_paths_.push_back(path);
    if (has_directory_prefix(path, volumes_root_)) {
      volumes_event_pending_ = true;
    }
    has_pending_events_.store(true, std::memory_order_relaxed);
  }
  wake_cv_.notify_all();
}

void FilesystemEnforcer::fsevents_callback(ConstFSEventStreamRef stream_ref,
                                           void* client_call_back_info,
                                           size_t num_events,
                                           void* event_paths,
                                           const FSEventStreamEventFlags event_flags[],
                                           const FSEventStreamEventId event_ids[]) {
  (void)stream_ref;
  (void)event_ids;

  auto* self = static_cast<FilesystemEnforcer*>(client_call_back_info);
  if (self == nullptr || event_paths == nullptr || num_events == 0) {
    return;
  }

  CFArrayRef changed_paths = static_cast<CFArrayRef>(event_paths);
  const CFIndex path_count = CFArrayGetCount(changed_paths);
  const auto bounded_events =
      std::min<std::size_t>(num_events, static_cast<std::size_t>(std::max<CFIndex>(path_count, 0)));

  for (std::size_t i = 0; i < bounded_events; ++i) {
    auto* raw_path =
        static_cast<CFStringRef>(CFArrayGetValueAtIndex(changed_paths, static_cast<CFIndex>(i)));
    if (raw_path == nullptr) {
      continue;
    }

    const CFIndex max_size =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(raw_path), kCFStringEncodingUTF8) + 1;
    if (max_size <= 1) {
      continue;
    }

    std::string utf8(static_cast<std::size_t>(max_size), '\0');
    if (CFStringGetCString(raw_path, utf8.data(), max_size, kCFStringEncodingUTF8) == false) {
      continue;
    }
    utf8.resize(std::strlen(utf8.c_str()));
    self->enqueue_path_event(std::move(utf8));

    if (event_flags != nullptr && (event_flags[i] & (kFSEventStreamEventFlagMustScanSubDirs |
                                                     kFSEventStreamEventFlagUserDropped |
                                                     kFSEventStreamEventFlagKernelDropped)) != 0) {
      self->enqueue_path_event(self->volumes_root_);
    }
  }
}

void FilesystemEnforcer::run_clipboard_hook(std::chrono::system_clock::time_point now) {
  auto& logger = silicon::logging::Logger::instance();

  if (config_ == nullptr || clipboard_ == nullptr) {
    return;
  }

  bool clear_clipboard = false;
  bool debug_mode = true;
  try {
    clear_clipboard = config_->shouldClearClipboard();
    debug_mode = config_->isDebugMode();
  } catch (const std::exception& err) {
    logger.error("FilesystemEnforcer: Failed to read clipboard policy flags: {}", err.what());
    return;
  }

  if (!clear_clipboard) {
    has_next_clipboard_deadline_ = false;
    return;
  }

  if (!has_next_clipboard_deadline_) {
    has_next_clipboard_deadline_ = true;
    next_clipboard_deadline_ = now + resolve_clipboard_clear_interval();
    return;
  }

  if (now < next_clipboard_deadline_) {
    return;
  }

  next_clipboard_deadline_ = now + resolve_clipboard_clear_interval();

  if (clipboard_hook_disabled_) {
    return;
  }

  if (debug_mode) {
    logger.debug("FilesystemEnforcer: Debug mode active; clipboard clear skipped (dry-run)");
    return;
  }

  std::string error_message;
  if (!clipboard_->clear(&error_message)) {
    clipboard_hook_disabled_ = true;
    if (!clipboard_disable_logged_) {
      clipboard_disable_logged_ = true;
      logger.warn(
          "FilesystemEnforcer: Clipboard clear unavailable; disabling hook: {}",
          sanitize_for_log(error_message.empty() ? "unknown clipboard error" : error_message));
    }
    return;
  }

  logger.info("FilesystemEnforcer: Clipboard cleared by monitor policy");
}

std::chrono::milliseconds FilesystemEnforcer::resolve_scan_interval() const {
  if (config_ == nullptr) {
    return kDefaultScanIntervalMs;
  }

  try {
    const int configured = config_->getFilesystemScanIntervalMs();
    if (configured <= 0) {
      return kDefaultScanIntervalMs;
    }
    return std::max(kMinScanIntervalMs, std::chrono::milliseconds(configured));
  } catch (...) {
    return kDefaultScanIntervalMs;
  }
}

std::chrono::seconds FilesystemEnforcer::resolve_notification_rate_limit() const {
  if (config_ == nullptr) {
    return kDefaultNotificationRateLimit;
  }
  try {
    const int configured_seconds = config_->getNotificationRateLimitSeconds();
    if (configured_seconds <= 0) {
      return kDefaultNotificationRateLimit;
    }
    return std::max(kMinNotificationRateLimit, std::chrono::seconds(configured_seconds));
  } catch (...) {
    return kDefaultNotificationRateLimit;
  }
}

std::chrono::seconds FilesystemEnforcer::resolve_clipboard_clear_interval() const {
  if (config_ == nullptr) {
    return kDefaultClipboardInterval;
  }
  try {
    const int configured_seconds = config_->getClipboardClearIntervalSeconds();
    if (configured_seconds <= 0) {
      return kDefaultClipboardInterval;
    }
    return std::max(kMinClipboardInterval, std::chrono::seconds(configured_seconds));
  } catch (...) {
    return kDefaultClipboardInterval;
  }
}

void FilesystemEnforcer::emit_notification_if_allowed(std::string_view message, ScanStats* stats) {
  if (stats == nullptr || config_ == nullptr || notification_sink_ == nullptr) {
    return;
  }

  bool notifications_enabled = true;
  try {
    notifications_enabled = config_->areFilesystemNotificationsEnabled();
  } catch (...) {
    notifications_enabled = false;
  }
  if (!notifications_enabled) {
    return;
  }

  const auto now =
      (file_system_ != nullptr) ? file_system_->now() : std::chrono::system_clock::now();
  if (!should_emit_notification(now, resolve_notification_rate_limit())) {
    return;
  }

  notification_sink_->notify(sanitize_for_log(message));
  ++stats->notifications_emitted;
}

bool FilesystemEnforcer::should_emit_notification(std::chrono::system_clock::time_point now,
                                                  std::chrono::seconds minimum_interval) {
  if (!has_notification_time_) {
    has_notification_time_ = true;
    last_notification_time_ = now;
    return true;
  }

  if (now < last_notification_time_) {
    last_notification_time_ = now;
    return true;
  }

  if ((now - last_notification_time_) >= minimum_interval) {
    last_notification_time_ = now;
    return true;
  }
  return false;
}

std::vector<std::string> FilesystemEnforcer::load_monitored_paths() const {
  if (config_ == nullptr) {
    return {};
  }

  std::vector<std::string> monitored;
  std::unordered_set<std::string> seen;

  std::vector<std::string> protected_paths;
  std::vector<std::string> extra_paths;
  std::vector<std::string> ignored_paths;
  try {
    protected_paths = config_->getProtectedPaths();
    extra_paths = config_->getFilesystemMonitorPaths();
    ignored_paths = config_->getSystemPathsToIgnore();
  } catch (...) {
    return {};
  }

  auto append = [&](const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
      if (path.empty()) {
        continue;
      }
      if (should_ignore_path(path, ignored_paths)) {
        continue;
      }
      if (seen.insert(path).second) {
        monitored.push_back(path);
      }
    }
  };

  append(protected_paths);
  append(extra_paths);
  return monitored;
}

bool FilesystemEnforcer::should_ignore_path(std::string_view path,
                                            const std::vector<std::string>& ignore_patterns) const {
  for (const auto& pattern : ignore_patterns) {
    if (pattern.empty()) {
      continue;
    }
    if (has_directory_prefix(path, pattern)) {
      return true;
    }
  }
  return false;
}

bool FilesystemEnforcer::is_mount_allowed(std::string_view mount_path) const {
  if (config_ == nullptr) {
    return false;
  }

  std::vector<std::string> allowed_volumes;
  std::vector<std::string> allowed_mount_prefixes;
  try {
    allowed_volumes = config_->getAllowedVolumes();
    allowed_mount_prefixes = config_->getAllowedMountPrefixes();
  } catch (...) {
    return false;
  }

  const std::string mount_lower = lower_ascii_copy(mount_path);
  const std::string volume_name_lower = lower_ascii_copy(basename_from_path(mount_path));

  for (const auto& allowed_volume : allowed_volumes) {
    const auto allowed = lower_ascii_copy(allowed_volume);
    if (allowed.empty()) {
      continue;
    }
    if (allowed == volume_name_lower || allowed == mount_lower) {
      return true;
    }
  }

  for (const auto& allowed_prefix : allowed_mount_prefixes) {
    const auto prefix = lower_ascii_copy(allowed_prefix);
    if (prefix.empty()) {
      continue;
    }
    if (has_directory_prefix(mount_lower, prefix)) {
      return true;
    }
  }

  return false;
}

void FilesystemEnforcer::publish_scan_stats(const ScanStats& stats) {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  last_scan_stats_ = stats;
}

bool FilesystemEnforcer::has_directory_prefix(std::string_view path, std::string_view prefix) {
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

std::string FilesystemEnforcer::lower_ascii_copy(std::string_view value) {
  std::string output(value.begin(), value.end());
  std::transform(output.begin(), output.end(), output.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return output;
}

std::string FilesystemEnforcer::basename_from_path(std::string_view path) {
  if (path.empty()) {
    return {};
  }
  const std::size_t slash_pos = path.find_last_of('/');
  if (slash_pos == std::string_view::npos) {
    return std::string(path);
  }
  if (slash_pos + 1 >= path.size()) {
    return {};
  }
  return std::string(path.substr(slash_pos + 1));
}

bool FilesystemEnforcer::fingerprints_equal(const IFileSystem::PathFingerprint& left,
                                            const IFileSystem::PathFingerprint& right) {
  return left.is_directory == right.is_directory && left.size_bytes == right.size_bytes &&
         left.modified_ticks == right.modified_ticks;
}

}  // namespace silicon::filesystem
