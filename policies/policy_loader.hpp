#pragma once 
#include <string> 
#include <unordered_set> 
#include <vector>

// Loads policy
class PolicyLoader {
public:
    // baseDir: project root used to resolve policy file paths
    explicit PolicyLoader(std::string baseDir);  // maintains f(args) 

    // Returns the whitelist of allowed process names
    std::unordered_set<std::string> loadAllowedProcesses() const;

    // Returns filesystem paths protected during a session
    std::vector<std::string> loadProtectedPaths() const;

    // Returns the whitelist of allowed network domains
    std::unordered_set<std::string> loadAllowedDomains() const;

private:
    // Base directory for resolving policy files
    std::string baseDir_;

    // Reads a text file into a vector of lines
    static std::vector<std::string> readLines(const std::string& path);

    // Trims leading and trailing whitespace
    static std::string trim(const std::string& s);
};
