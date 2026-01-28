#include "policies/policy_loader.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

PolicyLoader::PolicyLoader(std::string baseDir) : baseDir_(std::move(baseDir)) {}

static std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::vector<std::string> PolicyLoader::readLines(const std::string& path) {
    std::ifstream ifs(path);
    std::vector<std::string> out;
    std::string line;
    while (std::getline(ifs, line)) out.push_back(line);
    return out;
}

std::string PolicyLoader::trim(const std::string& s) {
    auto isspace2 = [](unsigned char c){ return std::isspace(c); };
    size_t b = 0, e = s.size();
    while (b < e && isspace2((unsigned char)s[b])) b++;
    while (e > b && isspace2((unsigned char)s[e-1])) e--;
    return s.substr(b, e - b);
}

std::unordered_set<std::string> PolicyLoader::loadAllowedProcesses() const {
    auto path = joinPath(baseDir_, "policies/allowed_processes.txt");
    auto lines = readLines(path);
    std::unordered_set<std::string> s;
    for (auto& l : lines) {
        auto t = trim(l);
        if (t.empty() || t[0] == '#') continue;
        s.insert(t);
    }
    return s;
}

std::vector<std::string> PolicyLoader::loadProtectedPaths() const {
    auto path = joinPath(baseDir_, "policies/protected_paths.txt");
    auto lines = readLines(path);
    std::vector<std::string> v;
    for (auto& l : lines) {
        auto t = trim(l);
        if (t.empty() || t[0] == '#') continue;
        v.push_back(t);
    }
    return v;
}

std::unordered_set<std::string> PolicyLoader::loadAllowedDomains() const {
    auto path = joinPath(baseDir_, "policies/allowed_domains.txt");
    auto lines = readLines(path);
    std::unordered_set<std::string> s;
    for (auto& l : lines) {
        auto t = trim(l);
        if (t.empty() || t[0] == '#') continue;
        s.insert(t);
    }
    return s;
}
