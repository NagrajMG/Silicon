#pragma once
#include <unordered_set>
#include <string>
#include <vector>
#include <sys/types.h>

namespace logging { class Logger; }

#include <unordered_map>
#include <chrono>

class ProcessEnforcer {
public:
    struct Config {
        bool killUnauthorized = false;
        bool escalateToSigKill = false;
        std::unordered_set<std::string> allowlist;

        int repeatThreshold = 5;                  // trigger after N repeats
        std::chrono::seconds warmup{60};          // ignore startup noise
    };

    ProcessEnforcer(logging::Logger& logger, Config cfg)
        : logger_(logger), cfg_(std::move(cfg)),
          start_(std::chrono::steady_clock::now()) {}

    void enforce();

private:
    logging::Logger& logger_;
    Config cfg_;

    std::unordered_map<std::string, int> seen_;
    std::chrono::steady_clock::time_point start_;

    static std::vector<pid_t> listPids();
    static std::string processName(pid_t pid);
    void handleUnauthorized(pid_t pid, const std::string& name);
};
