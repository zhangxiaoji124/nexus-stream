#include "nexus/net/event_loop.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <utility>

namespace nexus::net {

EventLoop::EventLoop()
    : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)),
      wakeup_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (epoll_fd_ && wakeup_fd_) {
        AddOrUpdateFd(wakeup_fd_.Get(), EPOLLIN, [this](uint32_t) { HandleWakeupRead(); });
    }
}

EventLoop::~EventLoop() {
    Quit();
}

void EventLoop::Loop() {
    running_.store(true);
    quit_.store(false);

    std::vector<epoll_event> active_events(64);
    while (!quit_.load()) {
        int num_events = ::epoll_wait(
            epoll_fd_.Get(),
            active_events.data(),
            static_cast<int>(active_events.size()),
            100 /* timeout ms */);

        if (num_events > 0) {
            for (int i = 0; i < num_events; ++i) {
                int fd = active_events[i].data.fd;
                uint32_t revents = active_events[i].events;
                auto it = callbacks_.find(fd);
                if (it != callbacks_.end() && it->second) {
                    // Copy callback in case handler removes itself
                    auto cb = it->second;
                    cb(revents);
                }
            }
            if (static_cast<size_t>(num_events) == active_events.size()) {
                active_events.resize(active_events.size() * 2);
            }
        }
        ExecutePendingTasks();
    }
    ExecutePendingTasks();
    running_.store(false);
}

void EventLoop::Quit() {
    quit_.store(true);
    Wakeup();
}

bool EventLoop::AddOrUpdateFd(int fd, uint32_t events, EventCallback cb) {
    if (!epoll_fd_ || fd < 0) return false;
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    bool exists = (callbacks_.find(fd) != callbacks_.end());
    int op = exists ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (::epoll_ctl(epoll_fd_.Get(), op, fd, &ev) < 0) {
        int fallback_op = exists ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
        if (::epoll_ctl(epoll_fd_.Get(), fallback_op, fd, &ev) < 0) {
            return false;
        }
    }
    callbacks_[fd] = std::move(cb);
    return true;
}

void EventLoop::RemoveFd(int fd) {
    if (!epoll_fd_ || fd < 0) return;
    ::epoll_ctl(epoll_fd_.Get(), EPOLL_CTL_DEL, fd, nullptr);
    callbacks_.erase(fd);
}

void EventLoop::QueueInLoop(Task task) {
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        pending_tasks_.push_back(std::move(task));
    }
    Wakeup();
}

void EventLoop::Wakeup() {
    if (!wakeup_fd_) return;
    uint64_t one = 1;
    ssize_t n = ::write(wakeup_fd_.Get(), &one, sizeof(one));
    (void)n;
}

void EventLoop::HandleWakeupRead() {
    uint64_t one = 0;
    ssize_t n = ::read(wakeup_fd_.Get(), &one, sizeof(one));
    (void)n;
}

void EventLoop::ExecutePendingTasks() {
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        tasks.swap(pending_tasks_);
    }
    for (auto& task : tasks) {
        if (task) task();
    }
}

}  // namespace nexus::net
