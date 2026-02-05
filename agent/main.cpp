#include "agent/Agent.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>

#include "config/config_manager.hpp"
#include "logging/logger.hpp"
#include "process/process_enforcer.hpp"

namespace {

std::atomic<bool> g_shutdown_requested_{false};
std::atomic<int> g_last_signal_{0};

void handle_signal(int signal) noexcept {
  g_last_signal_.store(signal, std::memory_order_relaxed);
  g_shutdown_requested_.store(true, std::memory_order_relaxed);
}

void install_signal_handlers() noexcept {

  struct sigaction action {};
  action.sa_handler = handle_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;

  // For this process, if SIGINT arrives, use this action object which has handle_signal which will handle this situation
  sigaction(SIGINT, &action, nullptr);

  // For this process, if SIGTERM arrives, use this action object which has handle_signal which will handle this situation
  sigaction(SIGTERM, &action, nullptr);
}

} // ending

int main(int argc, char** argv) {
  using silicon::logging::Logger;
  using silicon::logging::LogLevel;
  using silicon::agent::Agent;
  using silicon::config::ConfigManager;
  using silicon::process::ProcessEnforcer;

  auto& logger = Logger::instance();
  logger.set_log_path("runtime/logs/silicon_agent.log");

#ifndef NDEBUG
  logger.set_min_level(LogLevel::Debug);
#else
  logger.set_min_level(LogLevel::Info);
#endif

  const std::string config_path = (argc > 1 && argv[1] != nullptr) ? argv[1] : "";
  auto config_manager = std::make_shared<ConfigManager>(config_path);

  install_signal_handlers();

  const int heartbeat_seconds = std::max(1, config_manager->getHeartbeatInterval());
  const std::chrono::seconds heartbeat_interval{heartbeat_seconds};

  Agent agent(heartbeat_interval);
  ProcessEnforcer process_enforcer(config_manager);

  if (!process_enforcer.start()) {
    logger.error("ProcessEnforcer failed to start; continuing agent without process monitoring");
  }

  agent.run(g_shutdown_requested_, &g_last_signal_);

  if (!process_enforcer.stop(std::chrono::seconds(2))) {
    logger.error("ProcessEnforcer did not stop within timeout");
  }

  logger.info("Agent shutdown complete!");
  return 0;
}
