#include "core/scheduler.hpp"
#include <thread>

void Scheduler::addTask(std::function<void()> task, std::chrono::milliseconds interval) {
    Task t;
    t.fn = std::move(task);
    t.interval = interval;
    t.next = Clock::now() + interval;
    tasks_.push_back(std::move(t));
}

void Scheduler::run(const std::function<bool()>& shouldStop) {
    while (!shouldStop()) {
        auto now = Clock::now();

        for (auto& t : tasks_) {
            if (now >= t.next) {
                t.fn();
                t.next = now + t.interval;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
