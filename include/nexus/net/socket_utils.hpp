#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace nexus::net {

/**
 * @brief RAII wrapper around a POSIX file descriptor.
 */
class ScopedFd {
public:
    ScopedFd() noexcept = default;
    explicit ScopedFd(int fd) noexcept : fd_(fd) {}
    ~ScopedFd() noexcept { Reset(); }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : fd_(other.Release()) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    int Get() const noexcept { return fd_; }
    bool Valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return Valid(); }

    int Release() noexcept {
        int tmp = fd_;
        fd_ = -1;
        return tmp;
    }

    void Reset(int new_fd = -1) noexcept;

private:
    int fd_ = -1;
};

class SocketUtils {
public:
    static bool SetNonBlocking(int fd);
    static bool SetReuseAddr(int fd);
    static bool SetTcpNoDelay(int fd);

    /**
     * @brief Creates, binds, and listens on a non-blocking TCP server socket.
     *        If port == 0, an ephemeral port is chosen and written to `out_bound_port`.
     */
    static ScopedFd CreateTcpServer(std::string_view bind_ip, uint16_t port,
                                    uint16_t* out_bound_port = nullptr);

    /**
     * @brief Connects a blocking or non-blocking TCP client socket to host:port.
     */
    static ScopedFd ConnectTcp(std::string_view host, uint16_t port, int timeout_ms = 3000);

    /**
     * @brief Reliably writes all bytes in `data` to `fd`, handling partial writes and EAGAIN.
     */
    static bool SendAll(int fd, std::span<const uint8_t> data, int timeout_ms = 2000);
    static bool SendAll(int fd, std::string_view text, int timeout_ms = 2000) {
        return SendAll(fd, std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(text.data()), text.size()), timeout_ms);
    }
};

}  // namespace nexus::net
