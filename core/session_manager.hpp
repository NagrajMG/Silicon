#pragma once
#include <atomic>

class SessionManager {
public:
    void startSession();
    void stopSession();
    bool isActive() const;

private:
    std::atomic_bool active_{false};
};
