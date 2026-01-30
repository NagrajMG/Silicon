#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include "logging/logger.hpp"

namespace silicon::agent {

enum class LifecycleState : std::uint8_t {
  Starting = 0,
  Running,
  ShuttingDown
};

class Agent final {
 public:

 // Constructor
  explicit Agent(std::chrono::seconds heartbeat_interval) noexcept 
  : heartbeat_interval_(heartbeat_interval) {}

  void run(std::atomic<bool>& shutdown_requested_, std::atomic<int>* last_signal_ = nullptr) noexcept {

    using silicon::logging::Logger;
    auto& logger = Logger::instance();

    set_state(LifecycleState::Starting);
    logger.info("Agent state: STARTING");

    set_state(LifecycleState::Running);
    logger.info("Agent state: RUNNING");

    auto next_heartbeat = std::chrono::steady_clock::now();

    // only atomicity guranteed
    while (!shutdown_requested_.load(std::memory_order_relaxed)) {
      const auto now = std::chrono::steady_clock::now();

      if (now >= next_heartbeat) {
        logger.info("Heartbeat: I am alive");
        next_heartbeat = now + heartbeat_interval_;
      }

      const auto remaining = next_heartbeat - now;

      if (remaining > std::chrono::milliseconds(0)) {
        const auto max_sleep = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(1));
        const auto sleep_for = std::min(remaining, max_sleep);
        std::this_thread::sleep_for(sleep_for);
      } 
      else {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }

    set_state(LifecycleState::ShuttingDown);
    
    if (last_signal_ != nullptr) {
      const int sig = last_signal_->load(std::memory_order_relaxed);

      if (sig != 0) {
        logger.warn("Shutdown requested by signal {}", sig);
      }
    }
    logger.info("Agent state: SHUTTING_DOWN");
  }

  LifecycleState state() const noexcept {
    return state_.load(std::memory_order_relaxed);
  }


 private:
  void set_state(LifecycleState state) noexcept {
    state_.store(state, std::memory_order_relaxed);
  }

  std::atomic<LifecycleState> state_{LifecycleState::Starting};
  std::chrono::seconds heartbeat_interval_{60};
};

}  // namespace silicon::agent
