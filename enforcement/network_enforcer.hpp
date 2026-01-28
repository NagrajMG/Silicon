#pragma once
#include <string>
#include <unordered_set>

namespace logging { class Logger; }

// Enforces network restrictions during protected sessions
class NetworkEnforcer {
public:
    // Runtime configuration for network enforcement
    struct Config {
        bool enableHostsLockdown = false;          // Enable /etc/hosts lockdown (safe default: off)
        std::unordered_set<std::string> allowedDomains; // Whitelisted domains
        std::string hostsPath = "runtime/hosts.mock";   // Project-local hosts file
        std::string backupPath = "runtime/hosts.backup";  // Project-local backup
    };

    // Initialize network enforcer with logger and configuration
    NetworkEnforcer(logging::Logger& logger, Config cfg);

    // Apply network restrictions
    bool enable();

    // Restore original network state
    bool disable();

private:
    logging::Logger& logger_; // Central logging reference
    Config cfg_;              // Enforcement configuration

    // Read file contents into string
    static bool readFile(const std::string& path, std::string& out);

    // Write string contents to file
    static bool writeFile(const std::string& path, const std::string& content);
};
