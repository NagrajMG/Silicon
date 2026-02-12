#pragma once

#include <chrono>
#include <memory>

#include "config/config_manager.hpp"
#include "filesystem/filesystem_enforcer.hpp"
#include "process/process_enforcer.hpp"

namespace silicon::agent {

class EnforcementCoordinator final {
 public:
  explicit EnforcementCoordinator(std::shared_ptr<silicon::config::ConfigManager> config);

  bool start();
  bool stop(std::chrono::milliseconds timeout = std::chrono::seconds(2));

  silicon::process::ProcessEnforcer::ScanStats get_last_process_stats() const;
  silicon::filesystem::FilesystemEnforcer::ScanStats get_last_filesystem_stats() const;

 private:
  silicon::process::ProcessEnforcer process_enforcer_;
  silicon::filesystem::FilesystemEnforcer filesystem_enforcer_;
};

}  // namespace silicon::agent
