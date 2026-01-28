#include "enforcement/filesystem_enforcer.hpp"
#include "logging/logger.hpp"
#include <system_error>

// Initialize filesystem enforcer
FilesystemEnforcer::FilesystemEnforcer(logging::Logger& logger, Config cfg)
: logger_(logger), cfg_(std::move(cfg)) {}

// Normalize path to canonical form
std::string FilesystemEnforcer::normalize(const std::string& p) {
    return std::filesystem::weakly_canonical(
        std::filesystem::path(p)
    ).string();
}

// Take snapshot of protected paths
std::unordered_map<std::string, FilesystemEnforcer::Entry>
FilesystemEnforcer::snapshot() const {
    std::unordered_map<std::string, Entry> snap;

    // Iterate configured base paths
    for (const auto& baseRaw : cfg_.protectedPaths) {
        std::error_code ec;
        auto base = std::filesystem::path(baseRaw);

        // Skip non-existent paths
        if (!std::filesystem::exists(base, ec)) continue;

        // Recursively traverse directory tree
        for (auto it = std::filesystem::recursive_directory_iterator(base, ec);
             it != std::filesystem::recursive_directory_iterator();
             it.increment(ec)) {

            if (ec) continue; // Skip traversal errors

            Entry e;
            e.isDir = it->is_directory(ec);
            if (!e.isDir) {
                e.size = it->file_size(ec); // File size
            }
            e.mtime = it->last_write_time(ec); // Modification time

            // Store entry by full path
            auto key = it->path().string();
            snap[key] = e;
        }
    }
    return snap;
}

std::string FilesystemEnforcer::redactPath(const std::string& p) const {
    static const std::filesystem::path home =
        std::filesystem::path(std::getenv("HOME"));

    std::filesystem::path path(p);

    if (!home.empty() && path.string().starts_with(home.string())) {
        return std::string("<HOME>") +
               path.string().substr(home.string().size());
    }
    return p;
}

// Compare snapshots and detect changes
void FilesystemEnforcer::scan() {
    auto cur = snapshot(); // Current snapshot

    // Detect created or modified files
    for (const auto& [path, e] : cur) {
        auto it = last_.find(path);
        if (it == last_.end()) {
            // New file or directory
            logger_.log(logging::LogLevel::Warn,
                "[VIOLATION][filesystem] event=created path=" + redactPath(path));
        } else {
            const auto& prev = it->second;
            // Metadata change detected
            if (e.mtime != prev.mtime || e.size != prev.size) {
                logger_.log(logging::LogLevel::Warn,
                    "[VIOLATION][filesystem] event=modified path=" + redactPath(path));
            }
        }
    }

    // Detect deleted files
    for (const auto& [path, _] : last_) {
        if (cur.find(path) == cur.end()) {
            logger_.log(logging::LogLevel::Warn,
                "[VIOLATION][filesystem] event=deleted path=" + redactPath(path));
        }
    }

    // Update baseline snapshot
    last_ = std::move(cur);
}
