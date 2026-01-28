#include "enforcement/network_enforcer.hpp"
#include "logging/logger.hpp"
#include <fstream>

// Initialize network enforcer with logger and configuration
NetworkEnforcer::NetworkEnforcer(logging::Logger& logger, Config cfg)
: logger_(logger), cfg_(std::move(cfg)) {}

// Read entire file into a string
bool NetworkEnforcer::readFile(const std::string& path, std::string& out) {
    std::ifstream ifs(path);
    if (!ifs) return false;
    out.assign((std::istreambuf_iterator<char>(ifs)),
               std::istreambuf_iterator<char>());
    return true;
}

// Write content to file (truncate existing)
bool NetworkEnforcer::writeFile(const std::string& path,
                                const std::string& content) {
    std::ofstream ofs(path, std::ios::out | std::ios::trunc);
    if (!ofs) return false;
    ofs << content;
    return true;
}

// Enable network restriction (Phase 2 demo)
bool NetworkEnforcer::enable() {
    if (!cfg_.enableHostsLockdown) {
        logger_.log(logging::LogLevel::Info,
                    "[network] lockdown disabled (log_only)");
        return true;
    }

    std::string original;
    if (!readFile(cfg_.hostsPath, original)) {
        logger_.log(logging::LogLevel::Error,
                    "[network] failed to read hosts file");
        return false;
    }

    // Backup original hosts file
    if (!writeFile(cfg_.backupPath, original)) {
        logger_.log(logging::LogLevel::Error,
                    "[network] failed to write backup");
        return false;
    }

    // Build minimal restricted hosts file (Phase 2)
    std::string newHosts;
    
    newHosts += "## Silicon network restriction (Phase 2 demo)\n";
    newHosts += "127.0.0.1 localhost\n";
    newHosts += "::1 localhost\n\n";
    newHosts += "## Network access restricted during protected session\n";
    newHosts += "## (No real domain enforcement in Phase 2)\n";

    // Write restricted hosts file
    if (!writeFile(cfg_.hostsPath, newHosts)) {
        logger_.log(logging::LogLevel::Error,
                    "[network] failed to write restricted hosts");
        return false;
    }

    logger_.log(logging::LogLevel::Warn,
                "[network] network restriction enabled (demo)");
    return true;
}

// Disable network restriction and restore state
bool NetworkEnforcer::disable() {
    if (!cfg_.enableHostsLockdown) return true;

    std::string backup;
    if (!readFile(cfg_.backupPath, backup)) {
        logger_.log(logging::LogLevel::Error,
                    "[network] failed to read backup");
        return false;
    }

    if (!writeFile(cfg_.hostsPath, backup)) {
        logger_.log(logging::LogLevel::Error,
                    "[network] failed to restore hosts");
        return false;
    }

    logger_.log(logging::LogLevel::Info,
                "[network] network restriction removed");
    return true;
}
