#include "logging/logger.hpp"
#include "core/session_manager.hpp"
#include "core/scheduler.hpp"
#include "policies/policy_loader.hpp"
#include "enforcement/process_enforcer.hpp"
#include "enforcement/filesystem_enforcer.hpp"
#include "enforcement/network_enforcer.hpp"
#include "debugs/debug.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>


namespace {
std::atomic_bool g_running{true};

void handleSignal(int) {
    g_running.store(false);
}
} // namespace

int main() {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGQUIT, handleSignal);

    // Logger
    logging::Logger logger(logging::Logger::defaultLogPath());
    logger.log(logging::LogLevel::Info, "SILICON Phase2 starting");

    // Load policies
    PolicyLoader loader("/Users/copperhead07/Computing/cpp_projects/SILICON");
    auto allowedProcs = loader.loadAllowedProcesses();
    auto protectedPaths = loader.loadProtectedPaths();
    auto allowedDomains = loader.loadAllowedDomains();


    // Session
    SessionManager session;
    session.startSession(); // Phase2: auto-start
    logger.log(logging::LogLevel::Info, "Protected session started");

    // Enforcers (SAFE defaults)
    ProcessEnforcer::Config pcfg;
    pcfg.allowlist = std::move(allowedProcs);
    pcfg.killUnauthorized = false;       
    pcfg.escalateToSigKill = false;

    FilesystemEnforcer::Config fcfg;
    fcfg.protectedPaths = std::move(protectedPaths);

    NetworkEnforcer::Config ncfg;
    ncfg.allowedDomains = std::move(allowedDomains);
    ncfg.enableHostsLockdown = false;    // <-- requires sudo

    ProcessEnforcer processEnforcer(logger, pcfg);
    FilesystemEnforcer fsEnforcer(logger, fcfg);
    NetworkEnforcer netEnforcer(logger, ncfg);

    // Enable network lockdown
    netEnforcer.enable();

    Scheduler sched;

    sched.addTask([&]{
        if (session.isActive()) processEnforcer.enforce();
    }, std::chrono::milliseconds(3000));

    sched.addTask([&]{
        if (session.isActive()) fsEnforcer.scan();
    }, std::chrono::milliseconds(5000));

    // Stop after 2 minutes
    auto start = Scheduler::Clock::now();
    auto shouldStop = [&]{
        if (!g_running.load()) return true;
        return (Scheduler::Clock::now() - start) >= std::chrono::minutes(1);
    };

    sched.run(shouldStop);
    session.stopSession();
    netEnforcer.disable();
    logger.log(logging::LogLevel::Info, "Protected session ended");
    logger.log(logging::LogLevel::Info, "SILICON Phase2 stopped");

    DEBUG("Working file as expected, put too many system process runningas background.");
    
    return 0;
}
