#include "agent/Agent.hpp"

#include <atomic>
#include <csignal>
#include <chrono>
#include "logging/logger.hpp"

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

int main() {
  using silicon::logging::Logger;
  using silicon::logging::LogLevel;
  using silicon::agent::Agent;

  auto& logger = Logger::instance();
  logger.set_log_path("runtime/logs/silicon_agent.log");

#ifndef NDEBUG
  logger.set_min_level(LogLevel::Debug);
#else
  logger.set_min_level(LogLevel::Info);
#endif

  install_signal_handlers();

#ifndef NDEBUG
  const std::chrono::seconds heartbeat_interval{10};
#else
  const std::chrono::seconds heartbeat_interval{20};
#endif

  Agent agent(heartbeat_interval);
  agent.run(g_shutdown_requested_, &g_last_signal_);

  logger.info("Agent shutdown complete!");
  return 0;
}
