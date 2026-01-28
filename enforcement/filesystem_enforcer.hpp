#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>

namespace logging { class Logger; }

// Detects unauthorized filesystem changes via snapshot diffing
class FilesystemEnforcer {
public:
    // Runtime configuration
    struct Config {
        std::vector<std::string> protectedPaths; // Paths under protection
    };

    // Initialize filesystem enforcer
    FilesystemEnforcer(logging::Logger& logger, Config cfg);
    std::string redactPath(const std::string& p) const;

    // Scan filesystem and detect changes
    void scan(); // snapshot + diff

private:
    logging::Logger& logger_; // Central logger
    Config cfg_;              // Enforcement configuration

    // Metadata captured per file/directory
    struct Entry {
        std::uintmax_t size = 0;                    // File size
        std::filesystem::file_time_type mtime{};    // Last modification time
        bool isDir = false;                         // Directory flag
    };

    std::unordered_map<std::string, Entry> last_;   // Previous snapshot
    std::unordered_map<std::string, Entry> snapshot() const; // Current snapshot
    static std::string normalize(const std::string& p); // Canonical path form
};
