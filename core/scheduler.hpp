#pragma once
#include <chrono>
#include <functional>
#include <vector>

class Scheduler {
public:
    using Clock = std::chrono::steady_clock;

    // function template for type-safe and generality for functor, function pointer and normal function
    void addTask(std::function<void()> task, std::chrono::milliseconds interval);
    void run(const std::function<bool()> &shouldStop);

private:

    struct Task {
        std::function<void()> fn;
        std::chrono::milliseconds interval;
        Clock::time_point next;
    };

    std::vector<Task> tasks_;
};
