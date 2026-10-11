#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "nexus/net/socket_utils.hpp"

namespace nexus::net {

/**
 * @brief Linux epoll Reactor EventLoop supporting non-blocking I/O multiplexing
 *        and cross-thread task execution via eventfd wakeup.
 */
class EventLoop {
public:
    using EventCallback = std::function<void(uint32_t events)>;
    using Task = std::function<void()>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void Loop();
    void Quit();

    bool AddOrUpdateFd(int fd, uint32_t events, EventCallback cb);
    void RemoveFd(int fd);

    void QueueInLoop(Task task);

private:
    void Wakeup();
    void HandleWakeupRead();
    void ExecutePendingTasks();

    ScopedFd epoll_fd_;
    ScopedFd wakeup_fd_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};

    std::unordered_map<int, EventCallback> callbacks_;
    std::mutex task_mutex_;
    std::vector<Task> pending_tasks_;
};

}  // namespace nexus::net
