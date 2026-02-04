#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>

namespace silicon::config {

class ConfigManager final {
 public:
  explicit ConfigManager(const std::string& config_path);

  bool load();
  bool reload();

  bool isProcessAllowed(const std::string& process_name) const;
  bool isDomainAllowed(const std::string& domain) const;

  std::vector<std::string> getProtectedPaths() const;
  std::vector<std::string> getSystemPathsToIgnore() const;

  int getHeartbeatInterval() const;
  int getProcessScanInterval() const;

  const nlohmann::json& getRawConfig() const;

  // TODO: startFileWatch() - watch config file for changes
  // TODO: updateFromCloud(const std::string& json)
  // TODO: validateSchema(const nlohmann::json& json)
  // TODO: config versioning support
  // TODO: support multiple config formats (JSON/YAML/plist)

 private:
  bool load_locked();
  bool parse_json_locked(const nlohmann::json& json);
  void apply_defaults();
  void log_summary_locked() const;

  static std::string to_lower_copy(std::string_view value);
  static bool match_domain_pattern(std::string_view domain, std::string_view pattern);
  static std::string normalize_path(const std::string& path, bool* ok);
  static std::vector<std::string> normalize_paths(const std::vector<std::string>& paths);
  static bool has_prefix_path(std::string_view path, std::string_view prefix);

  std::string config_path_;

  mutable std::shared_mutex mutex_{};

  std::unordered_set<std::string> allowed_processes_;
  std::unordered_set<std::string> blocked_processes_;
  std::unordered_set<std::string> allowed_domains_;
  std::unordered_set<std::string> blocked_domains_;
  std::unordered_set<std::string> network_whitelist_;

  std::vector<std::string> protected_paths_;
  std::vector<std::string> filesystem_monitor_paths_;
  std::vector<std::string> system_paths_to_ignore_;

  int heartbeat_interval_seconds_{60};
  int process_scan_interval_seconds_{2};

  std::shared_ptr<const nlohmann::json> config_snapshot_;
};

}  // namespace silicon::config
