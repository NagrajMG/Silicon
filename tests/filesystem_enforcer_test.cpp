/*
===============================================================================
TEST SUITE: FilesystemEnforcer 
===============================================================================

The tests use dependency injection (FakeFileSystem, RecordingNotificationSink,
RecordingClipboard) to avoid touching the real OS.

-------------------------------------------------------------------------------
WHAT IS BEING TESTED OVERALL?
-------------------------------------------------------------------------------

1. External volume detection logic
2. Protected path monitoring logic
3. System path ignore rules
4. Notification rate limiting
5. Configuration safety clamping
6. Clipboard enforcement logic (debug vs production)
7. Log sanitization safety
8. Snapshot-based file change detection
9. Event-driven + polling fallback behavior
10. Statistics tracking correctness

===============================================================================
*/

#include "filesystem/filesystem_enforcer.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "config/config_manager.hpp"
#include "logging/logger.hpp"

namespace fs = std::filesystem;

namespace {

class TempConfigFile final {
 public:
  explicit TempConfigFile(const nlohmann::json& content) {
    temp_dir_ =
        fs::temp_directory_path() / fs::path("silicon_fs_test_" + std::to_string(std::rand()));
    fs::create_directories(temp_dir_);
    config_path_ = temp_dir_ / "policy.json";

    std::ofstream out(config_path_);
    out << content.dump(2);
  }

  ~TempConfigFile() {
    try {
      fs::remove_all(temp_dir_);
    } catch (...) {
    }
  }

  std::string path() const { return config_path_.string(); }

 private:
  fs::path temp_dir_{};
  fs::path config_path_{};
};

class FakeFileSystem final : public silicon::filesystem::IFileSystem {
 public:
  std::chrono::system_clock::time_point now_value = std::chrono::system_clock::time_point{};
  std::vector<std::string> mounts{};
  std::unordered_map<std::string, Snapshot> snapshots_by_root{};
  int list_error_code{0};
  std::size_t list_permission_errors{0};
  std::unordered_map<std::string, int> snapshot_error_codes{};
  std::unordered_map<std::string, std::size_t> snapshot_permission_errors{};

  std::chrono::system_clock::time_point now() const override { return now_value; }

  bool list_directories(std::string_view,
                        std::vector<std::string>* directories_out,
                        int* error_code_out,
                        std::size_t* permission_errors_out) override {
    if (directories_out == nullptr) {
      if (error_code_out != nullptr) {
        *error_code_out = EINVAL;
      }
      return false;
    }
    directories_out->clear();
    if (error_code_out != nullptr) {
      *error_code_out = list_error_code;
    }
    if (permission_errors_out != nullptr) {
      *permission_errors_out = list_permission_errors;
    }
    if (list_error_code != 0) {
      return false;
    }
    *directories_out = mounts;
    return true;
  }

  bool snapshot_tree(std::string_view root,
                     Snapshot* snapshot_out,
                     int* error_code_out,
                     std::size_t* permission_errors_out) override {
    if (snapshot_out == nullptr) {
      if (error_code_out != nullptr) {
        *error_code_out = EINVAL;
      }
      return false;
    }

    snapshot_out->clear();
    const std::string root_path(root);
    const auto error_it = snapshot_error_codes.find(root_path);
    const int code = (error_it != snapshot_error_codes.end()) ? error_it->second : 0;

    if (error_code_out != nullptr) {
      *error_code_out = code;
    }

    const auto permission_it = snapshot_permission_errors.find(root_path);
    if (permission_errors_out != nullptr) {
      *permission_errors_out =
          (permission_it != snapshot_permission_errors.end()) ? permission_it->second : 0;
    }

    if (code != 0) {
      return false;
    }

    const auto snapshot_it = snapshots_by_root.find(root_path);
    if (snapshot_it != snapshots_by_root.end()) {
      *snapshot_out = snapshot_it->second;
    }
    return true;
  }
};

class RecordingNotificationSink final : public silicon::filesystem::NotificationSink {
 public:
  void notify(std::string_view message) override { messages.push_back(std::string(message)); }

  std::vector<std::string> messages{};
};

class RecordingClipboard final : public silicon::filesystem::IClipboard {
 public:
  bool fail_clear{false};
  int clear_calls{0};

  bool clear(std::string* error_message_out) override {
    ++clear_calls;
    if (fail_clear) {
      if (error_message_out != nullptr) {
        *error_message_out = "clipboard unavailable";
      }
      return false;
    }
    if (error_message_out != nullptr) {
      error_message_out->clear();
    }
    return true;
  }
};

silicon::filesystem::IFileSystem::PathFingerprint file_fingerprint(std::uintmax_t size,
                                                                   std::int64_t tick) {
  silicon::filesystem::IFileSystem::PathFingerprint fp{};
  fp.is_directory = false;
  fp.size_bytes = size;
  fp.modified_ticks = tick;
  return fp;
}

silicon::filesystem::IFileSystem::PathFingerprint dir_fingerprint(std::int64_t tick) {
  silicon::filesystem::IFileSystem::PathFingerprint fp{};
  fp.is_directory = true;
  fp.size_bytes = 0;
  fp.modified_ticks = tick;
  return fp;
}

/*
-------------------------------------------------------------------------------
TEST: test_volume_detection_and_monitor_only_defaults()
-------------------------------------------------------------------------------

  First scan establishes baseline known_mounts_
  No alerts during baseline
  When a new volume appears:
      - new_mounts_detected increments
      - notifications_emitted increments
      - sink receives exactly 1 message
  Clipboard is NOT triggered
  Stats correctly reflect mount count

WHY THIS MATTERS:

Ensures:
- New external volumes are detected
- System runs in monitor-only mode
- No deletion/unmounting happens
- No side effects during baseline

-------------------------------------------------------------------------------
*/
void test_volume_detection_and_monitor_only_defaults() {
  TempConfigFile config_file(nlohmann::json{
      {"debug_mode", true},
      {"filesystem_notifications_enabled", true},
      {"notification_rate_limit_seconds", 1},
      {"filesystem_scan_interval_ms", 500},
      {"protected_paths", nlohmann::json::array()},
      {"filesystem_monitor_paths", nlohmann::json::array()},
      {"system_paths_to_ignore", nlohmann::json::array()},
  });

  auto config = std::make_shared<silicon::config::ConfigManager>(config_file.path());
  auto fake_fs = std::make_shared<FakeFileSystem>();
  auto sink = std::make_shared<RecordingNotificationSink>();
  auto clipboard = std::make_shared<RecordingClipboard>();

  fake_fs->now_value = std::chrono::system_clock::from_time_t(1'700'000'000);
  fake_fs->mounts = {"/Volumes/A", "/Volumes/B"};

  silicon::filesystem::FilesystemEnforcer enforcer(config, fake_fs, sink, clipboard, "/Volumes");
  enforcer.run_single_scan_for_test();

  auto stats = enforcer.get_last_scan_stats();
  assert(stats.new_mounts_detected == 0);
  assert(stats.notifications_emitted == 0);

  fake_fs->now_value += std::chrono::seconds(2);
  fake_fs->mounts = {"/Volumes/A", "/Volumes/B", "/Volumes/C"};
  enforcer.run_single_scan_for_test();

  stats = enforcer.get_last_scan_stats();
  assert(stats.mounts_seen == 3);
  assert(stats.new_mounts_detected == 1);
  assert(stats.notifications_emitted == 1);
  assert(sink->messages.size() == 1);
  assert(clipboard->clear_calls == 0);
}

/*
-------------------------------------------------------------------------------
TEST: test_notification_rate_limit()
-------------------------------------------------------------------------------

WHAT IT TESTS:

  Multiple new mounts detected rapidly
  Only ONE notification allowed within rate limit window
  After interval expires -- notifications allowed again

VALIDATES:

- resolve_notification_rate_limit()
- should_emit_notification()
- Time-based throttling
- Prevention of notification spam

-------------------------------------------------------------------------------
*/

void test_notification_rate_limit() {
  TempConfigFile config_file(nlohmann::json{
      {"debug_mode", false},
      {"filesystem_notifications_enabled", true},
      {"notification_rate_limit_seconds", 5},
      {"filesystem_scan_interval_ms", 500},
      {"protected_paths", nlohmann::json::array()},
      {"filesystem_monitor_paths", nlohmann::json::array()},
      {"system_paths_to_ignore", nlohmann::json::array()},
  });

  auto config = std::make_shared<silicon::config::ConfigManager>(config_file.path());
  auto fake_fs = std::make_shared<FakeFileSystem>();
  auto sink = std::make_shared<RecordingNotificationSink>();
  auto clipboard = std::make_shared<RecordingClipboard>();

  fake_fs->now_value = std::chrono::system_clock::from_time_t(1'700'100'000);
  fake_fs->mounts = {"/Volumes/A"};

  silicon::filesystem::FilesystemEnforcer enforcer(config, fake_fs, sink, clipboard, "/Volumes");
  enforcer.run_single_scan_for_test();

  fake_fs->now_value += std::chrono::seconds(1);
  fake_fs->mounts = {"/Volumes/A", "/Volumes/B", "/Volumes/C"};
  enforcer.run_single_scan_for_test();
  auto stats = enforcer.get_last_scan_stats();
  assert(stats.new_mounts_detected == 2);
  assert(stats.notifications_emitted == 1);

  fake_fs->now_value += std::chrono::seconds(2);
  fake_fs->mounts = {"/Volumes/A", "/Volumes/B", "/Volumes/C", "/Volumes/D"};
  enforcer.run_single_scan_for_test();
  stats = enforcer.get_last_scan_stats();
  assert(stats.new_mounts_detected == 1);
  assert(stats.notifications_emitted == 0);
  assert(sink->messages.size() == 1);

  fake_fs->now_value += std::chrono::seconds(10);
  fake_fs->mounts = {"/Volumes/A", "/Volumes/B", "/Volumes/C", "/Volumes/D", "/Volumes/E"};
  enforcer.run_single_scan_for_test();
  stats = enforcer.get_last_scan_stats();
  assert(stats.new_mounts_detected == 1);
  assert(stats.notifications_emitted == 1);
  assert(sink->messages.size() == 2);
}

/*
-------------------------------------------------------------------------------
TEST: test_ignored_system_paths_do_not_trigger_events()
-------------------------------------------------------------------------------

WHAT IT TESTS:

• System paths listed in system_paths_to_ignore
• Event injected inside ignored system directory
• path_events_detected remains 0

VALIDATES:

- should_ignore_path()
- has_directory_prefix()
- Ignore pattern matching logic

WHY IMPORTANT:

Prevents false positives from macOS internal directories.
Critical for stable proctoring agent behavior.

-------------------------------------------------------------------------------
*/

void test_ignored_system_paths_do_not_trigger_events() {
  TempConfigFile config_file(nlohmann::json{
      {"debug_mode", true},
      {"protected_paths", nlohmann::json::array({"/System/Library/ExamHidden"})},
      {"filesystem_monitor_paths", nlohmann::json::array()},
      {"system_paths_to_ignore", nlohmann::json::array({"/System/Library/"})},
  });

  auto config = std::make_shared<silicon::config::ConfigManager>(config_file.path());
  assert(config->getProtectedPaths().empty());

  auto fake_fs = std::make_shared<FakeFileSystem>();
  auto sink = std::make_shared<RecordingNotificationSink>();
  auto clipboard = std::make_shared<RecordingClipboard>();

  fake_fs->now_value = std::chrono::system_clock::from_time_t(1'700'200'000);
  fake_fs->mounts = {"/Volumes/A"};

  silicon::filesystem::FilesystemEnforcer enforcer(config, fake_fs, sink, clipboard, "/Volumes");
  enforcer.run_single_scan_for_test();

  fake_fs->now_value += std::chrono::seconds(1);
  enforcer.queue_path_event_for_test("/System/Library/ExamHidden/file.txt");
  enforcer.run_single_scan_for_test();

  const auto stats = enforcer.get_last_scan_stats();
  assert(stats.path_events_detected == 0);
}

/*
-------------------------------------------------------------------------------
TEST: test_protected_paths_trigger_events()
-------------------------------------------------------------------------------

WHAT IT TESTS:

• Path inside protected_paths changes
• Event is detected
• path_events_detected increments
• Notifications obey config flag

VALIDATES:

- monitor_paths_from_events()
- Prefix matching logic
- Snapshot baseline behavior
- Config flag integration

-------------------------------------------------------------------------------
*/

void test_protected_paths_trigger_events() {
  TempConfigFile config_file(nlohmann::json{
      {"debug_mode", true},
      {"filesystem_notifications_enabled", false},
      {"protected_paths", nlohmann::json::array({"/tmp/silicon_protected"})},
      {"filesystem_monitor_paths", nlohmann::json::array()},
      {"system_paths_to_ignore", nlohmann::json::array({"/System/Library/"})},
  });

  auto config = std::make_shared<silicon::config::ConfigManager>(config_file.path());
  const auto protected_paths = config->getProtectedPaths();
  assert(protected_paths.size() == 1);
  const std::string root = protected_paths.front();

  auto fake_fs = std::make_shared<FakeFileSystem>();
  auto sink = std::make_shared<RecordingNotificationSink>();
  auto clipboard = std::make_shared<RecordingClipboard>();

  fake_fs->now_value = std::chrono::system_clock::from_time_t(1'700'300'000);
  fake_fs->mounts = {"/Volumes/A"};
  fake_fs->snapshots_by_root[root] = {
      {root, dir_fingerprint(1)},
      {root + "/notes.txt", file_fingerprint(10, 10)},
  };

  silicon::filesystem::FilesystemEnforcer enforcer(config, fake_fs, sink, clipboard, "/Volumes");
  enforcer.run_single_scan_for_test();

  fake_fs->now_value += std::chrono::seconds(2);
  enforcer.queue_path_event_for_test(root + "/notes.txt");
  enforcer.queue_path_event_for_test(root + "/new.txt");
  enforcer.run_single_scan_for_test();

  const auto stats = enforcer.get_last_scan_stats();
  assert(stats.path_events_detected >= 2);
  assert(stats.notifications_emitted == 0);
}

/*
-------------------------------------------------------------------------------
TEST: test_config_interval_clamps()
-------------------------------------------------------------------------------

WHAT IT TESTS:

• Too-small scan interval → clamped to 250ms minimum
• Too-small clipboard interval → clamped to 5 seconds
• Zero notification interval → clamped to 1 second

VALIDATES:

- ConfigManager defensive logic
- Safety floors
- Prevents CPU abuse or spam

-------------------------------------------------------------------------------
*/

void test_config_interval_clamps() {
  TempConfigFile config_file(nlohmann::json{
      {"filesystem_scan_interval_ms", 10},
      {"clipboard_clear_interval_seconds", 1},
      {"notification_rate_limit_seconds", 0},
  });

  silicon::config::ConfigManager config(config_file.path());
  assert(config.getFilesystemScanIntervalMs() == 250);
  assert(config.getClipboardClearIntervalSeconds() == 5);
  assert(config.getNotificationRateLimitSeconds() == 1);
}

/*
-------------------------------------------------------------------------------
TEST: test_sanitize_for_log()
-------------------------------------------------------------------------------

WHAT IT TESTS:

• Removes newline, carriage return, tab characters
• Ensures output becomes single-line safe text

VALIDATES:

- sanitize_for_log()

-------------------------------------------------------------------------------
*/

void test_sanitize_for_log() {
  const std::string raw = "line1\nline2\twith\rcontrol";
  const std::string sanitized = silicon::filesystem::FilesystemEnforcer::sanitize_for_log(raw);
  assert(sanitized == "line1 line2 with control");
}

/*
-------------------------------------------------------------------------------
TEST: test_clipboard_dry_run_in_debug_mode()
-------------------------------------------------------------------------------

WHAT IT TESTS:

• clear_clipboard = true in config
• debug_mode = true
• Clipboard clear should NOT execute
• clear_calls remains 0

VALIDATES:

- run_clipboard_hook()
- Debug mode guard
- Dry-run safety behavior

-------------------------------------------------------------------------------
*/
void test_clipboard_dry_run_in_debug_mode() {
  TempConfigFile config_file(nlohmann::json{
      {"debug_mode", true},
      {"clear_clipboard", true},
      {"clipboard_clear_interval_seconds", 5},
      {"filesystem_notifications_enabled", false},
      {"protected_paths", nlohmann::json::array()},
      {"filesystem_monitor_paths", nlohmann::json::array()},
      {"system_paths_to_ignore", nlohmann::json::array()},
  });

  auto config = std::make_shared<silicon::config::ConfigManager>(config_file.path());
  auto fake_fs = std::make_shared<FakeFileSystem>();
  auto sink = std::make_shared<RecordingNotificationSink>();
  auto clipboard = std::make_shared<RecordingClipboard>();

  fake_fs->now_value = std::chrono::system_clock::from_time_t(1'700'400'000);
  fake_fs->mounts = {"/Volumes/A"};

  silicon::filesystem::FilesystemEnforcer enforcer(config, fake_fs, sink, clipboard, "/Volumes");
  enforcer.run_single_scan_for_test();

  fake_fs->now_value += std::chrono::seconds(6);
  enforcer.run_single_scan_for_test();

  assert(clipboard->clear_calls == 0);
}

}  // namespace

/*
===============================================================================
FAKE COMPONENTS USED IN TESTING
===============================================================================

FakeFileSystem:
  - Simulates mount listing
  - Simulates snapshot tree state
  - Controls time deterministically
  - Injects permission errors

RecordingNotificationSink:
  - Captures notification messages
  - Allows assertion of count + content

RecordingClipboard:
  - Tracks clear() calls
  - Can simulate failure
  - Ensures no real clipboard interaction

TempConfigFile:
  - Creates temporary JSON policy
  - Automatically cleans up
  - Isolates each test case

===============================================================================
*/

int main() {
  auto& logger = silicon::logging::Logger::instance();
  logger.set_log_path("runtime/logs/test_filesystem_enforcer.log");
  logger.set_min_level(silicon::logging::LogLevel::Debug);

  test_volume_detection_and_monitor_only_defaults();
  test_notification_rate_limit();
  test_ignored_system_paths_do_not_trigger_events();
  test_protected_paths_trigger_events();
  test_config_interval_clamps();
  test_sanitize_for_log();
  test_clipboard_dry_run_in_debug_mode();

  return 0;
}
