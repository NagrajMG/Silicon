#include "enforcement/process_enforcer.hpp"
#include "logging/logger.hpp"
#include <signal.h>
#include <libproc.h> // MacOS system library that exposes kernel process information to user-space programs.
#include <vector>
#include <cstring>


// vector allocates → kernel writes → vector resized
std::vector<pid_t> ProcessEnforcer::listPids() {
    std::vector<pid_t> pids(4096);

    // Asking the kernel to write all running process IDs into a buffer and returns how many bytes were written.
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));

    if (bytes <= 0) return {};             

    int count = bytes / sizeof(pid_t);       // Number of valid PIDs
    pids.resize(count);
    return pids;
}

// Resolve a human-readable name for a given PID
std::string ProcessEnforcer::processName(pid_t pid) {

    char namebuf[PROC_PIDPATHINFO_MAXSIZE];    // maximum buffer size (in bytes) for the name
    std::memset(namebuf, 0, sizeof(namebuf));  // memset(pointer, value, size);

    // Attempt short process name resolution
    char shortName[PROC_PIDPATHINFO_MAXSIZE];
    std::memset(shortName, 0, sizeof(shortName));
    int n = proc_name(pid, shortName, sizeof(shortName));
    if (n > 0) return std::string(shortName);

    // Fallback to full executable path
    int ret = proc_pidpath(pid, namebuf, sizeof(namebuf));
    if (ret > 0) return std::string(namebuf);

    return {};                               
}

// Log and optionally terminate an unauthorized process
void ProcessEnforcer::handleUnauthorized(pid_t pid, const std::string& name) {
    logger_.log(logging::LogLevel::Warn,
                "[VIOLATION][process] name=" + name +
                " pid=" + std::to_string(pid) +
                (cfg_.killUnauthorized ? " action=terminate" : " action=log_only"));

    if (!cfg_.killUnauthorized) return;      // Detection-only mode

    ::kill(pid, SIGTERM);                    // Request graceful shutdown

    if (cfg_.escalateToSigKill) {
        ::kill(pid, SIGKILL);                // Force termination
    }
}

// Scan active processes and enforce allowlist policy
void ProcessEnforcer::enforce() {
    auto now = std::chrono::steady_clock::now();

    // Warm-up phase
    if (now - start_ < cfg_.warmup) return;

    for (pid_t pid : listPids()) {
        if (pid <= 1) continue;

        auto name = processName(pid);
        if (name.empty() || name == "SILICON") continue;

        // Ignore Apple system processes
        if (name.starts_with("/System/") || name.starts_with("/usr/")) {
            continue;
        }

        if (cfg_.allowlist.count(name)) continue;

        // Track repeats
        int& count = seen_[name];
        count++;

        if (count < cfg_.repeatThreshold) continue;

        handleUnauthorized(pid, name);
    }
}

/*
Arguments for the proc_listpids functions

Return Pids
No filter
pointer to contiguous memory ----- Write the results here
Maximum memory the kernel is allowed to use
*/
