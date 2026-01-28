#include "core/session_manager.hpp"

void SessionManager::startSession() { 
    active_.store(true); 
}

void SessionManager::stopSession()  { 
    active_.store(false); 
}

bool SessionManager::isActive() const { 
    return active_.load(); 
}
