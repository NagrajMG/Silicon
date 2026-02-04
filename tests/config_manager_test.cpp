#include <iostream>
#include <fstream>
#include <filesystem>
#include <cassert>
#include "src/config/config_manager.hpp"
#include "logging/logger.hpp"

namespace fs = std::filesystem;

// Test helper to create temporary config files
class TempConfigFile {
public:
    TempConfigFile(const std::string& content) {
        temp_dir = "silicon_test";
        fs::create_directories(temp_dir);
        config_path = temp_dir / "test_config.json";
        
        std::ofstream file(config_path);
        file << content;
    }
    
    ~TempConfigFile() {
        try {
            fs::remove_all(temp_dir);
        } catch (...) {
        }
    }
    
    std::string path() const { return config_path.string(); }
    
private:
    fs::path temp_dir;
    fs::path config_path;
};

void test_basic_loading() {
    std::cout << "Test: Basic Config Loading\n";
    
    TempConfigFile config(
R"({
        "allowed_processes": ["zoom.us", "Safari"],
        "blocked_processes": ["Discord", "Slack"],
        "allowed_domains": ["exam.example.com"],
        "heartbeat_interval_seconds": 30,
        "process_scan_interval_seconds": 5
    })");
    
    silicon::config::ConfigManager manager(config.path());
    
    // Testing files, domain, ints
    assert(manager.isProcessAllowed("zoom.us") == true);
    assert(manager.isProcessAllowed("safari") == true);
    assert(manager.isProcessAllowed("Discord") == false);
    assert(manager.isProcessAllowed("UnknownApp") == false);
    
    assert(manager.isDomainAllowed("exam.example.com") == true);
    assert(manager.isDomainAllowed("cheat.example.com") == false);
    
    assert(manager.getHeartbeatInterval() == 30);
    assert(manager.getProcessScanInterval() == 5);
    
    std::cout << "Config loading passed\n";
}

void test_wildcard_domains() {
    std::cout << "Test: Wildcard Domain Matching\n";
    
    TempConfigFile config(R"({
        "allowed_domains": ["*.example.com", "specific.com"],
        "blocked_domains": ["*.blocked.com"]
    })");
    
    silicon::config::ConfigManager manager(config.path());
    
    // Test allowed wildcards
    assert(manager.isDomainAllowed("api.example.com") == true);
    assert(manager.isDomainAllowed("sub.api.example.com") == true);
    assert(manager.isDomainAllowed("example.com") == false); 
    assert(manager.isDomainAllowed("specific.com") == true);
    
    // Test blocked wildcards
    assert(manager.isDomainAllowed("cheat.blocked.com") == false);
    assert(manager.isDomainAllowed("api.blocked.com") == false);
    
    std::cout << "Wildcard domain matching passed\n";
}

void test_path_normalization() {
    std::cout << "Test: Path Normalization\n";
    
    TempConfigFile config(R"({
        "protected_paths": ["~/Documents", "/tmp/test", "../parent"],
        "system_paths_to_ignore": ["/usr/bin/"]
    })");
    
    silicon::config::ConfigManager manager(config.path());
    
    auto paths = manager.getProtectedPaths();
    auto ignore_paths = manager.getSystemPathsToIgnore();
    
    // Check that paths were normalized
    for (const auto& path : paths) {
        assert(!path.empty());
        assert(path.find("..") == std::string::npos);  // No parent directory references
        assert(path.find("~") == std::string::npos);   // Home expanded
    }
    
    // Check system paths
    assert(!ignore_paths.empty());
    assert(std::find(ignore_paths.begin(), ignore_paths.end(), "/usr/bin") != ignore_paths.end());
    
    std::cout << "Path normalization passed\n";
}

void test_reload_functionality() {
    std::cout << "Test: Config Reload\n";
    
    // Create initial config
    std::string temp_dir = "silicon_reload_test";
    fs::create_directories(temp_dir);
    std::string config_path = temp_dir + "/reload_config.json";
    
    {
        std::ofstream file(config_path);
        file << R"({"allowed_processes": ["App1"], "heartbeat_interval_seconds": 10})";
    }
    
    silicon::config::ConfigManager manager(config_path);
    assert(manager.isProcessAllowed("App1") == true);
    assert(manager.getHeartbeatInterval() == 10);
    
    // Update config file
    {
        std::ofstream file(config_path);
        file << R"({"allowed_processes": ["App2"], "heartbeat_interval_seconds": 20})";
    }
    
    // Reload and verify changes
    assert(manager.reload() == true);
    assert(manager.isProcessAllowed("App1") == false);
    assert(manager.isProcessAllowed("App2") == true);
    assert(manager.getHeartbeatInterval() == 20);
    
    // Cleanup
    fs::remove_all(temp_dir);
    std::cout << "Config reload passed\n";
}

void test_missing_config() {
    std::cout << "Test: Missing Config File\n";
    
    // Non-existent path should create default
    std::string fake_path = "silicon_nonexistent/config.json";
    silicon::config::ConfigManager manager(fake_path);
    
    // Should have defaults loaded
    assert(manager.getHeartbeatInterval() == 120);  // Default value
    assert(manager.getProcessScanInterval() == 10); // Default value
    
    // Should have system paths
    auto ignore_paths = manager.getSystemPathsToIgnore();
    assert(!ignore_paths.empty());
    
    std::cout << "Missing config handling passed\n";
}

void test_empty_and_malformed_config() {
    std::cout << "Test: Empty and Malformed Config\n";
    
    // Test empty config
    {
        TempConfigFile empty_config("{}");
        silicon::config::ConfigManager manager(empty_config.path());
        
        // Should use defaults
        assert(manager.getHeartbeatInterval() == 120);
        auto ignore_paths = manager.getSystemPathsToIgnore();
        assert(!ignore_paths.empty());
    }
    
    // Test malformed JSON
    {
        TempConfigFile bad_config("{ invalid json }");
        silicon::config::ConfigManager manager(bad_config.path());
        
        // Should fall back to defaults
        assert(manager.getHeartbeatInterval() == 120);
    }
    
    std::cout << "Empty/malformed config handling passed\n";
}

void test_process_allow_logic() {
    std::cout << "Test: Process Allow/Block Logic\n";
    
    TempConfigFile config(R"({
        "allowed_processes": ["Zoom", "Browser"],
        "blocked_processes": ["CheatTool"]
    })");
    
    silicon::config::ConfigManager manager(config.path());
    
    assert(manager.isProcessAllowed("Zoom") == true);
    assert(manager.isProcessAllowed("CheatTool") == false);
    assert(manager.isProcessAllowed("Browser") == true);
    assert(manager.isProcessAllowed("browser") == true);
    assert(manager.isProcessAllowed("BROWSER") == true);

    {
        TempConfigFile config2(R"({
            "blocked_processes": ["BadApp"]
        })");
        
        silicon::config::ConfigManager manager2(config2.path());

        assert(manager2.isProcessAllowed("GoodApp") == true);
        assert(manager2.isProcessAllowed("BadApp") == false);
    }
    
    std::cout << "Process logic passed\n";
}

int main() {
    std::cout << "=== SILICON Config Manager Tests ===\n\n";
    
    auto& logger = silicon::logging::Logger::instance();
    
    try {
        test_basic_loading();
        test_wildcard_domains();
        test_path_normalization();
        test_reload_functionality();
        test_missing_config();
        test_empty_and_malformed_config();
        test_process_allow_logic();
        
        std::cout << "\n=== All tests passed! ===\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n Test failed: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "\n Unknown test failure\n";
        return 1;
    }
}