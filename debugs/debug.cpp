#include "debugs/debug.hpp"

#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include<iostream>

namespace {
std::mutex dbg_mutex;

std::string debug_log_path() {
    const char* home = std::getenv("HOME");
    std::string base = home ? home : ".";
    std::cout << base << "\n";
    return base + "/Computing/cpp_projects/SILICON/debugs/output.txt";
}
}

void debug_log(const std::string& file,
               const std::string& function,
               const std::string& message)
{
    std::lock_guard<std::mutex> lock(dbg_mutex);

    std::filesystem::path p(file);
    std::string filename = p.filename().string();

    std::ofstream ofs(debug_log_path(), std::ios::out | std::ios::app);
    if (!ofs.is_open()) return;

    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&tt, &tm);

    ofs << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << " [DEBUG] "
        << "file=" << filename
        << " func=" << function
        << " msg=" << message
        << "\n";
}
