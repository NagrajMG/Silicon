#pragma once
#include <CoreServices/CoreServices.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "config/config_manager.hpp"

namespace silicon::filesystem {

/*
 *
 * virtual fucntions be like "I don’t care how this job is done. I only care that it can be done"
 * If the function is not virtual, the compiler is rigid. It only looks at the Type on the pointer,
 * not the actual object it’s holding.
 *
 */

// For decoupling, plug and play behaviour
class NotificationSink {  // Abstract class here, The interface
 public:
  virtual ~NotificationSink() = default;
  virtual void notify(std::string_view message) = 0;  // Pure Virtual Function
};

class LoggingNotificationSink final : public NotificationSink {
 public:
  void notify(std::string_view message) override;
};

// format for interface
class IClipboard {
 public:
  virtual ~IClipboard() = default;
  virtual bool clear(std::string* error_message_out) = 0;
};

// final -- devirtualization help compiler optimization -- fast current object function lookup
class SystemClipboard final : public IClipboard {
 public:
  bool clear(std::string* error_message_out) override;
};

// ID CARD TEMPLATE
class IFileSystem {
 public:
  struct PathFingerprint {
    bool is_directory{false};
    std::uintmax_t size_bytes{0};
    std::int64_t modified_ticks{0};  // When was it last modified?
  };

  using Snapshot = std::unordered_map<std::string, PathFingerprint>;

  virtual ~IFileSystem() = default;

  virtual std::chrono::system_clock::time_point now() const = 0;

  virtual bool list_directories(std::string_view root,
                                std::vector<std::string>* directories_out,
                                int* error_code_out,
                                std::size_t* permission_errors_out) = 0;

  // Walk through this folder and remember everything you see
  virtual bool snapshot_tree(std::string_view root,
                             Snapshot* snapshot_out,
                             int* error_code_out,
                             std::size_t* permission_errors_out) = 0;
};

class LocalFileSystem final : public IFileSystem {
 public:
  std::chrono::system_clock::time_point now() const override;

  bool list_directories(std::string_view root,
                        std::vector<std::string>* directories_out,
                        int* error_code_out,
                        std::size_t* permission_errors_out) override;

  bool snapshot_tree(std::string_view root,
                     Snapshot* snapshot_out,
                     int* error_code_out,
                     std::size_t* permission_errors_out) override;
};

class FilesystemEnforcer final {
 public:
  // for debugging + monitoring
  struct ScanStats {
    std::chrono::system_clock::time_point timestamp{};
    std::size_t mounts_seen{0};
    std::size_t new_mounts_detected{0};
    std::size_t path_events_detected{0};
    std::size_t notifications_emitted{0};
    std::size_t permission_errors{0};
  };

  explicit FilesystemEnforcer(std::shared_ptr<silicon::config::ConfigManager> config,
                              std::shared_ptr<IFileSystem> file_system = nullptr,
                              std::shared_ptr<NotificationSink> notification_sink = nullptr,
                              std::shared_ptr<IClipboard> clipboard = nullptr,
                              std::string volumes_root = "/Volumes");

  ~FilesystemEnforcer();

  bool start();
  bool stop(std::chrono::milliseconds timeout = std::chrono::milliseconds(500));

  ScanStats get_last_scan_stats() const;

  // Test hooks.
  void run_single_scan_for_test();
  void queue_path_event_for_test(std::string path);

  static std::string sanitize_for_log(std::string_view value);

 private:
  void scan_loop();
  void perform_scan();
  void monitor_volumes(ScanStats* stats);
  void monitor_paths_polling(ScanStats* stats); // slow backup scan
  void monitor_paths_from_events(const std::vector<std::string>& event_paths, ScanStats* stats); // fast, OS will tell
  void run_clipboard_hook(std::chrono::system_clock::time_point now);

  // Gets the data from the config file
  std::chrono::milliseconds resolve_scan_interval() const;
  std::chrono::seconds resolve_notification_rate_limit() const;
  std::chrono::seconds resolve_clipboard_clear_interval() const;

  void emit_notification_if_allowed(std::string_view message, ScanStats* stats);
  bool should_emit_notification(std::chrono::system_clock::time_point now,
                                std::chrono::seconds minimum_interval);

  std::vector<std::string> load_monitored_paths() const;
  bool should_ignore_path(std::string_view path,
                          const std::vector<std::string>& ignore_patterns) const;
  bool is_mount_allowed(std::string_view mount_path) const;

  bool start_event_stream();
  void stop_event_stream();
  void drain_pending_events(std::vector<std::string>* event_paths_out,
                            bool* volume_event_pending_out);
  void enqueue_path_event(std::string path);
  static void fsevents_callback(ConstFSEventStreamRef stream_ref,
                                void* client_call_back_info,
                                size_t num_events,
                                void* event_paths,
                                const FSEventStreamEventFlags event_flags[],
                                const FSEventStreamEventId event_ids[]);

  void publish_scan_stats(const ScanStats& stats);

  static bool has_directory_prefix(std::string_view path, std::string_view prefix);
  static std::string lower_ascii_copy(std::string_view value);
  static std::string basename_from_path(std::string_view path);
  static bool fingerprints_equal(const IFileSystem::PathFingerprint& left,
                                 const IFileSystem::PathFingerprint& right);

  std::shared_ptr<silicon::config::ConfigManager> config_{};
  std::shared_ptr<IFileSystem> file_system_{};
  std::shared_ptr<NotificationSink> notification_sink_{};
  std::shared_ptr<IClipboard> clipboard_{};
  std::string volumes_root_{"/Volumes"};

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

  struct EventBackend;
  std::unique_ptr<EventBackend> event_backend_{};
  mutable std::mutex event_mutex_{};
  std::vector<std::string> pending_event_paths_{};
  bool volumes_event_pending_{false};
  std::atomic<bool> has_pending_events_{false};

  bool known_mounts_initialized_{false};
  std::unordered_set<std::string> known_mounts_{};

  bool known_path_snapshot_initialized_{false};
  IFileSystem::Snapshot known_path_snapshot_{};

  bool has_last_volume_resync_{false};
  std::chrono::system_clock::time_point last_volume_resync_{};

  bool has_notification_time_{false};
  std::chrono::system_clock::time_point last_notification_time_{};

  bool has_next_clipboard_deadline_{false};
  std::chrono::system_clock::time_point next_clipboard_deadline_{};
  bool clipboard_hook_disabled_{false};
  bool clipboard_disable_logged_{false};
};

}  // namespace silicon::filesystem
