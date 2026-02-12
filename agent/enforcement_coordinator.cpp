#include "agent/enforcement_coordinator.hpp"

#include "logging/logger.hpp"

namespace silicon::agent {

EnforcementCoordinator::EnforcementCoordinator(
    std::shared_ptr<silicon::config::ConfigManager> config)
    : process_enforcer_(config), filesystem_enforcer_(std::move(config)) {}

bool EnforcementCoordinator::start() {
  auto& logger = silicon::logging::Logger::instance();

  bool process_started = process_enforcer_.start();
  if (!process_started) {
    logger.error("EnforcementCoordinator: ProcessEnforcer failed to start");
  }

  bool filesystem_started = filesystem_enforcer_.start();
  if (!filesystem_started) {
    logger.error("EnforcementCoordinator: FilesystemEnforcer failed to start");
  }

  return process_started && filesystem_started;
}

bool EnforcementCoordinator::stop(std::chrono::milliseconds timeout) {
  auto& logger = silicon::logging::Logger::instance();

  bool filesystem_stopped = filesystem_enforcer_.stop(timeout);
  if (!filesystem_stopped) {
    logger.error("EnforcementCoordinator: FilesystemEnforcer stop timed out");
  }

  bool process_stopped = process_enforcer_.stop(timeout);
  if (!process_stopped) {
    logger.error("EnforcementCoordinator: ProcessEnforcer stop timed out");
  }

  return filesystem_stopped && process_stopped;
}

silicon::process::ProcessEnforcer::ScanStats EnforcementCoordinator::get_last_process_stats()
    const {
  return process_enforcer_.get_last_scan_stats();
}

silicon::filesystem::FilesystemEnforcer::ScanStats
EnforcementCoordinator::get_last_filesystem_stats() const {
  return filesystem_enforcer_.get_last_scan_stats();
}

}  // namespace silicon::agent
