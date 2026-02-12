#include "config/config_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include "logging/logger.hpp"

namespace silicon::config {

namespace {

// Maximum config file size to prevent resource exhaustion
constexpr std::size_t kMaxConfigBytes = 1024 * 1024; // 1MiB

std::string default_config_path() {
  // This follows macOS reverse DNS naming convention
  const char* home = std::getenv("HOME"); 

  if (home) {
    std::filesystem::path user_support_path = std::filesystem::path(home) / "~/Library/Application Support/com.silicon.agent/policy.json";
    
    // Check config file exists
    std::error_code exists_ec;
    bool file_exists = std::filesystem::exists(user_support_path, exists_ec);
    
    if (exists_ec) {
      // Error checking if file exists
      // This could be permission denied, path too long, etc.
      using silicon::logging::Logger;
      auto& logger = Logger::instance();
      logger.warn("ConfigManager: Cannot check if config exists at {}: {}", user_support_path.generic_string(), exists_ec.message());
    } 
    else if (file_exists) {
      return user_support_path.generic_string();
    }

    // If we get here: file doesn't exist OR error occurred
    
    // Create directory structure
    auto support_dir = std::filesystem::path(home) / "~/Library/Application Support/com.silicon.agent/policy.json";

    std::error_code create_ec;
    bool dir_created = std::filesystem::create_directories(support_dir, create_ec);
    
    if (create_ec) {
      // Failed to create directory
      using silicon::logging::Logger;
      auto& logger = Logger::instance();
      logger.error("ConfigManager: Cannot create config directory {}: {}", support_dir.generic_string(), create_ec.message());
    } else {
      // Directory created or already exists
      if (!dir_created) {
        // Directory already existed
        using silicon::logging::Logger;
        auto& logger = Logger::instance();
        logger.debug("ConfigManager: Config directory already exists at {}", support_dir.generic_string());
      }
      return user_support_path.generic_string();
    }
  }
  
  // Fallback: Development/testing path
  const std::filesystem::path dev_path{ "runtime/config/policy.json" };
  using silicon::logging::Logger;
  auto& logger = Logger::instance();
  logger.info("ConfigManager: Using development config path: {}", dev_path.generic_string());

  return dev_path.generic_string();
}

// Helper to create a comma-separated string from vector items (for logging)
std::string join_strings(const std::vector<std::string>& items, std::size_t limit = 20) {
  if (items.empty()) {
    return "";
  }
  std::ostringstream out;
  std::size_t count = 0;

  for (const auto& item : items) {
    if (count > 0) {
      out << ", ";
    }
    out << item;
    count++;
    if (count >= limit) {
      if (items.size() > limit) {
        out << " ...";
      }
      break;
    }
  }
  return out.str();
}

// Remove duplicates while preserving original order
std::vector<std::string> dedupe_preserve_order(const std::vector<std::string>& input) {
  std::vector<std::string> output;
  output.reserve(input.size());
  std::unordered_set<std::string> seen;
  for (const auto& item : input) { 
    if (seen.insert(item).second) { // If item was NOT in the set before, return true
      output.push_back(item);
    }
  }
  return output;
}

// Ensure parent directory exists for config file
void ensure_config_directory(const std::string& config_path) {
  std::filesystem::path dir_path = std::filesystem::path(config_path).parent_path();
  if (!dir_path.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(dir_path, ec);
    // Failure is logged in constructor if occurs
  }
}

// Create default minimal config if file doesn't exist
bool create_default_config(const std::string& config_path) {
  nlohmann::json default_config = {
    {"allowed_processes", nlohmann::json::array()},
    {"blocked_processes", nlohmann::json::array()},
    {"allowed_domains", nlohmann::json::array()},
    {"blocked_domains", nlohmann::json::array()},
    {"protected_paths", {"/Users/copperhead07/Downloads"}},
    {"network_whitelist", nlohmann::json::array()},
    {"filesystem_monitor_paths", nlohmann::json::array()},
    {"allowed_volumes", nlohmann::json::array()},
    {"allowed_mount_prefixes", nlohmann::json::array()},
    {"debug_mode", true},
    {"heartbeat_interval_seconds", 120},
    {"process_scan_interval_seconds", 10},
    {"filesystem_scan_interval_ms", 2000},
    {"clear_clipboard", false},
    {"clipboard_clear_interval_seconds", 15},
    {"filesystem_notifications_enabled", true},
    {"notification_rate_limit_seconds", 5},
    {"system_paths_to_ignore", {
      "/System/Library/",
      "/usr/libexec/",
      "/usr/sbin/",
      "/sbin/",
      "/bin/",
      "/usr/bin/",
      "/usr/local/"
    }}
  };
  
  // Create the directory if it doesn't exist, then open the file for writing
  std::ofstream file(config_path);
  if (!file.is_open()) {
    return false; // Returns false if we don't have write permissions (common for /Library)
  }
  
  // Write the JSON with 2-space indentation so it's human-readable
  file << default_config.dump(2);
  return file.good();
}
} // namespace


// CONSTRUCTOR
// ConfigManager config("/path/to/policy.json");
ConfigManager::ConfigManager(const std::string& config_path)
: config_path_(config_path.empty() ? default_config_path() : config_path) {
  using silicon::logging::Logger;
  auto& logger = Logger::instance();

  // Ensure config directory exists
  ensure_config_directory(config_path_);
  
  // Normalize the path (expand ~, resolve symlinks, etc.)
  bool ok = false;
  const auto normalized = normalize_path(config_path_, &ok);

  if (ok && !normalized.empty()) {
    config_path_ = normalized;
  } 
  else {
    logger.warn("ConfigManager: Using raw config path {}", config_path_);
  }

  // Load the file existing
  if (!load()) {
    // If file doesn't exist or is invalid, create default
    logger.info("ConfigManager: Creating default config at {}", config_path_);
    if (create_default_config(config_path_)) {
      load(); // Load the default we just created
    } else {
      logger.error("ConfigManager: Failed to create default config");
      // Continue with empty/default configuration
      apply_defaults();
    }
  }
}

// PUBLIC METHODS
// Load configuration from file (public thread-safe wrapper)
bool ConfigManager::load() {
  std::unique_lock lock(mutex_);
  return load_locked();
}

// Reload configuration from disk (public thread-safe wrapper)
bool ConfigManager::reload() {
  std::unique_lock lock(mutex_);
  return load_locked();
}

// PRIVATE IMPLEMENTATION 

// MAIN THING RAW CODE :: load() and reload()
bool ConfigManager::load_locked() {
  using silicon::logging::Logger;
  auto& logger = Logger::instance();

  // Reset to defaults before loading new config
  apply_defaults();
  
  // Create empty JSON snapshot for raw config access
  config_snapshot_ = std::make_shared<nlohmann::json>(nlohmann::json::object());

  // Check if config file exists
  std::error_code ec;
  if (!std::filesystem::exists(config_path_, ec)) {
    logger.error("ConfigManager: Config file not found at {}", config_path_);
    return false;
  }

  // Check file size to prevent resource exhaustion
  const auto size = std::filesystem::file_size(config_path_, ec);
  if (ec) {
    logger.error("ConfigManager: Unable to read config file size at {}", config_path_);
    return false;
  }

  if (size > kMaxConfigBytes) {
    logger.error("ConfigManager: Config file exceeds max size ({} bytes)", size);
    return false;
  }

  // Open and read the file
  std::ifstream file(config_path_);
  if (!file.is_open()) {
    logger.error("ConfigManager: Failed to open config file at {}", config_path_);
    return false;
  }

  // Parse JSON content
  nlohmann::json json;
  try {
    file >> json;
  } catch (const std::exception& err) {
    logger.error("ConfigManager: Failed to parse JSON: {}", err.what());
    return false;
  }

  // Validate JSON structure
  if (!json.is_object()) {
    logger.error("ConfigManager: Config root must be a JSON object");
    return false;
  }

  // Parse individual config sections
  const bool parsed = parse_json_locked(json);
  if (!parsed) {
    logger.warn("ConfigManager: Config parsed with warnings; defaults applied where needed");
  }

  // Store snapshot for raw config access
  config_snapshot_ = std::make_shared<nlohmann::json>(json);
  
  // Log summary of what was loaded
  log_summary_locked();
  return true;
}

// Parse JSON structure and populate internal data structures
bool ConfigManager::parse_json_locked(const nlohmann::json& json) {
  using silicon::logging::Logger;
  auto& logger = Logger::instance();
  bool ok = true;

  // Lambda to read string arrays from JSON
  auto read_string_array = [&](const char* key, std::vector<std::string>& out) {
    if (!json.contains(key)) {
      return;
    }

    if (!json.at(key).is_array()) {
      logger.warn("ConfigManager: {} must be an array of strings", key);
      ok = false;
      return;
    }

    out.clear();
    for (const auto& entry : json.at(key)) {
      if (!entry.is_string()) {
        logger.warn("ConfigManager: {} contains non-string entries", key);
        ok = false;
        continue;
      }
      out.push_back(entry.get<std::string>());
    }
  };

  // Lambda to read integer values from JSON with strict minimum validation.
  auto read_int = [&](const char* key, int& target, int min_value) {
    if (!json.contains(key)) {
      return;
    }
    if (!json.at(key).is_number_integer()) {
      logger.warn("ConfigManager: {} must be an integer", key);
      ok = false;
      return;
    }
    const auto value = json.at(key).get<int>();
    if (value < min_value) {
      logger.warn("ConfigManager: {} must be >= {}", key, min_value);
      ok = false;
      return;
    }
    target = value;
  };

  // Lambda to read integer values and clamp low values to the minimum.
  auto read_int_clamped = [&](const char* key, int& target, int min_value) {
    if (!json.contains(key)) {
      return;
    }
    if (!json.at(key).is_number_integer()) {
      logger.warn("ConfigManager: {} must be an integer", key);
      ok = false;
      return;
    }
    const auto value = json.at(key).get<int>();
    if (value < min_value) {
      logger.warn("ConfigManager: {} below minimum {}; clamping", key, min_value);
      ok = false;
      target = min_value;
      return;
    }
    target = value;
  };

  auto read_bool = [&](const char* key, bool& target) {
    if (!json.contains(key)) {
      return;
    }
    if (!json.at(key).is_boolean()) {
      logger.warn("ConfigManager: {} must be a boolean", key);
      ok = false;
      return;
    }
    target = json.at(key).get<bool>();
  };

  // Temporary storage for parsed values
  std::vector<std::string> allowed_processes;
  std::vector<std::string> blocked_processes;
  std::vector<std::string> allowed_domains;
  std::vector<std::string> blocked_domains;
  std::vector<std::string> protected_paths;
  std::vector<std::string> network_whitelist;
  std::vector<std::string> filesystem_monitor_paths;
  std::vector<std::string> system_paths_to_ignore;
  std::vector<std::string> allowed_volumes;
  std::vector<std::string> allowed_mount_prefixes;

  // Read all config sections
  read_string_array("allowed_processes", allowed_processes);
  read_string_array("blocked_processes", blocked_processes);
  read_string_array("allowed_domains", allowed_domains);
  read_string_array("blocked_domains", blocked_domains);
  read_string_array("protected_paths", protected_paths);
  read_string_array("network_whitelist", network_whitelist);
  read_string_array("filesystem_monitor_paths", filesystem_monitor_paths);
  read_string_array("system_paths_to_ignore", system_paths_to_ignore);
  read_string_array("allowed_volumes", allowed_volumes);
  read_string_array("allowed_mount_prefixes", allowed_mount_prefixes);

  // Read integer config values
  read_bool("debug_mode", debug_mode_);
  read_bool("clear_clipboard", clear_clipboard_);
  read_bool("filesystem_notifications_enabled", filesystem_notifications_enabled_);
  read_int("heartbeat_interval_seconds", heartbeat_interval_seconds_, 1);
  read_int("process_scan_interval_seconds", process_scan_interval_seconds_, 1);
  read_int_clamped("filesystem_scan_interval_ms", filesystem_scan_interval_ms_, 250);
  read_int_clamped("clipboard_clear_interval_seconds", clipboard_clear_interval_seconds_, 5);
  read_int_clamped("notification_rate_limit_seconds", notification_rate_limit_seconds_, 1);

  // Store processes in sets for fast lookup
  allowed_processes_.clear();
  for (const auto& item : allowed_processes) {
    allowed_processes_.insert(to_lower_copy(item));
  }

  blocked_processes_.clear();
  for (const auto& item : blocked_processes) {
    blocked_processes_.insert(to_lower_copy(item));
  }

  // Store domains in sets for fast lookup
  allowed_domains_.clear();
  for (const auto& item : allowed_domains) {
    allowed_domains_.insert(to_lower_copy(item));
  }

  blocked_domains_.clear();
  for (const auto& item : blocked_domains) {
    blocked_domains_.insert(to_lower_copy(item));
  }

  // Store network whitelist
  network_whitelist_.clear();
  for (const auto& item : network_whitelist) {
    network_whitelist_.insert(item);
  }

  // Normalize and store file paths
  protected_paths_ = normalize_paths(protected_paths);
  filesystem_monitor_paths_ = normalize_paths(filesystem_monitor_paths);
  allowed_mount_prefixes_ = normalize_paths(allowed_mount_prefixes);
  allowed_volumes_ = dedupe_preserve_order(allowed_volumes);

  // Handle system paths to ignore
  if (!system_paths_to_ignore.empty()) {
    auto normalized_system_paths = normalize_paths(system_paths_to_ignore);
    system_paths_to_ignore_.insert(system_paths_to_ignore_.end(), normalized_system_paths.begin(), normalized_system_paths.end());
  }
  system_paths_to_ignore_ = dedupe_preserve_order(system_paths_to_ignore_);

  // Filter out any protected paths that fall under system ignore paths
  if (!system_paths_to_ignore_.empty()) {
    auto filter_paths = [&](std::vector<std::string>& paths) {
      std::vector<std::string> filtered;
      filtered.reserve(paths.size());
      for (const auto& path : paths) {
        bool skip = false;
        for (const auto& ignore : system_paths_to_ignore_) {
          if (has_prefix_path(path, ignore)) {
            skip = true;
            logger.warn("ConfigManager: Skipping protected path inside system ignore path {}", path);
            break;
          }
        }
        if (!skip) {
          filtered.push_back(path);
        }
      }
      paths = dedupe_preserve_order(filtered);
    };

    filter_paths(protected_paths_);
    filter_paths(filesystem_monitor_paths_);
  }

  return ok;
}

// Set default values for all config options
void ConfigManager::apply_defaults() {
  allowed_processes_.clear();
  blocked_processes_.clear();
  allowed_domains_.clear();
  blocked_domains_.clear();
  network_whitelist_.clear();

  protected_paths_.clear();
  filesystem_monitor_paths_.clear();
  allowed_volumes_.clear();
  allowed_mount_prefixes_.clear();

  heartbeat_interval_seconds_ = 120;      // Default: 120 seconds between heartbeats
  process_scan_interval_seconds_ = 10;    // Default: Scan processes every 10 seconds
  filesystem_scan_interval_ms_ = 2000;    // Default: filesystem polling every 2 seconds
  clipboard_clear_interval_seconds_ = 15; // Default: clear clipboard every 15 seconds
  notification_rate_limit_seconds_ = 5;   // Default: one filesystem notification per 5 seconds
  debug_mode_ = true;                     // Default: monitor-only development mode
  clear_clipboard_ = false;               // Default: do not clear clipboard automatically
  filesystem_notifications_enabled_ = true;

  // Default system paths to ignore
  system_paths_to_ignore_ = normalize_paths({
    "/System/Library/",
    "/usr/libexec/",
    "/usr/sbin/",
    "/sbin/",
    "/bin/",
    "/usr/bin/",
    "/usr/local/"
  });
}

// Log summary of loaded configuration for debugging
void ConfigManager::log_summary_locked() const {
  using silicon::logging::Logger;
  auto& logger = Logger::instance();

  logger.info("ConfigManager: Loaded config from {}", config_path_);
  logger.info("ConfigManager: Allowed processes: {}", allowed_processes_.size());
  logger.info("ConfigManager: Blocked processes: {}", blocked_processes_.size());
  logger.info("ConfigManager: Allowed domains: {}", allowed_domains_.size());
  logger.info("ConfigManager: Blocked domains: {}", blocked_domains_.size());
  logger.info("ConfigManager: Protected paths: {}", protected_paths_.size());
  logger.info("ConfigManager: Filesystem monitor paths: {}", filesystem_monitor_paths_.size());
  logger.info("ConfigManager: Allowed volumes: {}", allowed_volumes_.size());
  logger.info("ConfigManager: Allowed mount prefixes: {}", allowed_mount_prefixes_.size());
  logger.info("ConfigManager: System paths to ignore: {}", system_paths_to_ignore_.size());
  logger.info("ConfigManager: Heartbeat interval: {}s", heartbeat_interval_seconds_);
  logger.info("ConfigManager: Process scan interval: {}s", process_scan_interval_seconds_);
  logger.info("ConfigManager: Filesystem scan interval: {}ms", filesystem_scan_interval_ms_);
  logger.info("ConfigManager: Debug mode: {}", debug_mode_ ? "true" : "false");
  logger.info("ConfigManager: Filesystem notifications enabled: {}", filesystem_notifications_enabled_ ? "true" : "false");
  logger.info("ConfigManager: Filesystem notification rate limit: {}s", notification_rate_limit_seconds_);
  logger.info("ConfigManager: Clear clipboard: {}", clear_clipboard_ ? "true" : "false");
  logger.info("ConfigManager: Clipboard clear interval: {}s", clipboard_clear_interval_seconds_);

  // Debug-level logs with sample data
  if (!allowed_processes_.empty()) {
    std::vector<std::string> sample(allowed_processes_.begin(), allowed_processes_.end());
    logger.debug("ConfigManager: Allowed process list: {}", join_strings(sample));
  }

  if (!blocked_processes_.empty()) {
    std::vector<std::string> sample(blocked_processes_.begin(), blocked_processes_.end());
    logger.debug("ConfigManager: Blocked process list: {}", join_strings(sample));
  }

  if (!allowed_domains_.empty()) {
    std::vector<std::string> sample(allowed_domains_.begin(), allowed_domains_.end());
    logger.debug("ConfigManager: Allowed domain list: {}", join_strings(sample));
  }

  if (!blocked_domains_.empty()) {
    std::vector<std::string> sample(blocked_domains_.begin(), blocked_domains_.end());
    logger.debug("ConfigManager: Blocked domain list: {}", join_strings(sample));
  }

  if (!protected_paths_.empty()) {
    logger.debug("ConfigManager: Protected paths: {}", join_strings(protected_paths_));
  }

  if (!filesystem_monitor_paths_.empty()) {
    logger.debug("ConfigManager: Filesystem monitor paths: {}", join_strings(filesystem_monitor_paths_));
  }

  if (!allowed_volumes_.empty()) {
    logger.debug("ConfigManager: Allowed volumes: {}", join_strings(allowed_volumes_));
  }

  if (!allowed_mount_prefixes_.empty()) {
    logger.debug("ConfigManager: Allowed mount prefixes: {}", join_strings(allowed_mount_prefixes_));
  }
}

// QUERY METHODS
// Thread-safe methods for enforcement modules to query config

// Check if a process is allowed
// Usage: if (config.isProcessAllowed("zoom.us")) { ... }
bool ConfigManager::isProcessAllowed(const std::string& process_name) const {
  const auto normalized = to_lower_copy(process_name);
  std::string basename = normalized;
  const std::size_t slash_pos = normalized.find_last_of('/');
  if (slash_pos != std::string::npos && slash_pos + 1 < normalized.size()) {
    basename = normalized.substr(slash_pos + 1);
  }
  std::shared_lock lock(mutex_);

  // Blocked processes take precedence
  if (blocked_processes_.find(normalized) != blocked_processes_.end()) {
    return false;
  }
  if (!basename.empty() && blocked_processes_.find(basename) != blocked_processes_.end()) {
    return false;
  }

  // If no allowed processes specified, everything is allowed
  if (allowed_processes_.empty()) {
    return true;
  }

  // Check if process is in allowed list
  if (allowed_processes_.find(normalized) != allowed_processes_.end()) {
    return true;
  }
  if (!basename.empty() && allowed_processes_.find(basename) != allowed_processes_.end()) {
    return true;
  }
  return false;
}

// Check if a domain is allowed (supports wildcards like *.example.com)
// Usage: if (config.isDomainAllowed("exam.university.edu")) { ... }
bool ConfigManager::isDomainAllowed(const std::string& domain) const {
  const auto normalized = to_lower_copy(domain);
  std::shared_lock lock(mutex_);

  // Check blocked domains first
  for (const auto& pattern : blocked_domains_) {
    if (match_domain_pattern(normalized, pattern)) {
      return false;
    }
  }

  // If no allowed domains specified, everything is allowed
  if (allowed_domains_.empty()) {
    return true;
  }

  // Check allowed domains
  for (const auto& pattern : allowed_domains_) {
    if (match_domain_pattern(normalized, pattern)) {
      return true;
    }
  }

  // Domain not in allowed list
  return false;
}

// Get list of protected filesystem paths
std::vector<std::string> ConfigManager::getProtectedPaths() const {
  std::shared_lock lock(mutex_);
  return protected_paths_;
}

std::vector<std::string> ConfigManager::getFilesystemMonitorPaths() const {
  std::shared_lock lock(mutex_);
  return filesystem_monitor_paths_;
}

// Get list of system paths to ignore (for ProcessEnforcer safety
std::vector<std::string> ConfigManager::getSystemPathsToIgnore() const {
  std::shared_lock lock(mutex_);
  return system_paths_to_ignore_;
}

std::vector<std::string> ConfigManager::getAllowedVolumes() const {
  std::shared_lock lock(mutex_);
  return allowed_volumes_;
}

std::vector<std::string> ConfigManager::getAllowedMountPrefixes() const {
  std::shared_lock lock(mutex_);
  return allowed_mount_prefixes_;
}

// Get heartbeat interval in seconds
int ConfigManager::getHeartbeatInterval() const {
  std::shared_lock lock(mutex_);
  return heartbeat_interval_seconds_;
}

// Get process scan interval in seconds
int ConfigManager::getProcessScanInterval() const {
  std::shared_lock lock(mutex_);
  return process_scan_interval_seconds_;
}

int ConfigManager::getFilesystemScanIntervalMs() const {
  std::shared_lock lock(mutex_);
  return std::max(filesystem_scan_interval_ms_, 250);
}

bool ConfigManager::isDebugMode() const {
  std::shared_lock lock(mutex_);
  return debug_mode_;
}

bool ConfigManager::shouldClearClipboard() const {
  std::shared_lock lock(mutex_);
  return clear_clipboard_;
}

int ConfigManager::getClipboardClearIntervalSeconds() const {
  std::shared_lock lock(mutex_);
  return std::max(clipboard_clear_interval_seconds_, 5);
}

bool ConfigManager::areFilesystemNotificationsEnabled() const {
  std::shared_lock lock(mutex_);
  return filesystem_notifications_enabled_;
}

int ConfigManager::getNotificationRateLimitSeconds() const {
  std::shared_lock lock(mutex_);
  return std::max(notification_rate_limit_seconds_, 1);
}

// Get raw JSON config for debugging/inspection
const nlohmann::json& ConfigManager::getRawConfig() const {
  std::shared_lock lock(mutex_);
  // Use thread-local storage to avoid returning reference to temporary
  thread_local std::shared_ptr<const nlohmann::json> snapshot;
  snapshot = config_snapshot_;
  return *snapshot;
}

// HELPER METHODS
// Convert string to lowercase
std::string ConfigManager::to_lower_copy(std::string_view value) {
  std::string output(value.begin(), value.end());
  std::transform(output.begin(), output.end(), output.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return output;
}

// Match domain against pattern with wildcard support
// Patterns: "example.com", "*.example.com", "*" (matches everything)
bool ConfigManager::match_domain_pattern(std::string_view domain, std::string_view pattern) {
  if (pattern == "*") {
    return true;
  }
  // Handle wildcard patterns like "*.example.com"
  if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
    const std::string_view suffix = pattern.substr(2);
    if (domain.size() <= suffix.size()) {
      return false;
    }
    if (!domain.ends_with(suffix)) {
      return false;
    }
    // Ensure we match full subdomain segments (e.g., *.example.com matches a.example.com but not a.bexample.com)
    const auto diff = domain.size() - suffix.size();
    return domain[diff - 1] == '.';
  }
  // Exact match
  return domain == pattern;
}

// Normalize a filesystem path: expand ~, resolve symlinks, convert to absolute
// Returns empty string for invalid paths (contains ".." traversal attempts)
std::string ConfigManager::normalize_path(const std::string& path, bool* ok) {
  if (ok != nullptr) {
    *ok = false;
  }
  if (path.empty()) {
    return {};
  }

  // Expand home directory alias (~)
  std::string expanded = path;
  if (expanded == "~" || expanded.starts_with("~/")) {
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
      expanded = std::string(home) + expanded.substr(1);
    }
  }

  // Convert to absolute path
  std::filesystem::path fs_path(expanded);
  std::error_code ec;
  if (fs_path.is_relative()) {
    fs_path = std::filesystem::absolute(fs_path, ec);
    if (ec) {
      fs_path = std::filesystem::path(expanded).lexically_normal();
    }
  }

  // Resolve symlinks and canonicalize
  std::filesystem::path normalized = std::filesystem::weakly_canonical(fs_path, ec);
  if (ec) {
    normalized = fs_path.lexically_normal();
  }

  // Safety check: reject paths with ".." components
  for (const auto& part : normalized) {
    if (part == "..") {
      return {};
    }
  }

  if (ok != nullptr) {
    *ok = true;
  }
  return normalized.generic_string();
}

// Normalize multiple paths, filtering out invalid ones
std::vector<std::string> ConfigManager::normalize_paths(const std::vector<std::string>& paths) {
  using silicon::logging::Logger;
  auto& logger = Logger::instance();

  std::vector<std::string> normalized;
  normalized.reserve(paths.size());
  for (const auto& path : paths) {
    bool ok = false;
    auto normalized_path = normalize_path(path, &ok);
    if (!ok || normalized_path.empty()) {
      logger.warn("ConfigManager: Skipping invalid path {}", path);
      continue;
    }
    normalized.push_back(normalized_path);
  }
  return dedupe_preserve_order(normalized);
}

// Check if path starts with prefix (directory-aware comparison)
bool ConfigManager::has_prefix_path(std::string_view path, std::string_view prefix) {
  if (prefix.empty()) {
    return false;
  }
  if (!path.starts_with(prefix)) {
    return false;
  }
  if (path.size() == prefix.size()) {
    return true;
  }
  const char last = prefix.back();
  if (last == '/') {
    return true;
  }
  // Ensure we're matching full directory segments
  return path[prefix.size()] == '/';
}

}  // namespace silicon::config


/*
This ConfigManager acts as the centralized intelligence hub for SILICON, 
translating a static JSON policy into active security rules for app, network, 
and file monitoring.

It provides a thread-safe bridge that allows background enforcer
modules to instantly verify if a specific system action is authorized 
without slowing down the Mac.

By handling path normalization and automatic system exclusions, it ensures the 
agent remains robust against bypass attempts while preventing accidental 
interference with critical macOS functions.

*/