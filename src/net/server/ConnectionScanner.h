#pragma once

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/app/App.h"

namespace ruvia::detail {

class ConnectionScanner final {
public:
    enum class Phase {
        kIdle,
        kReadingHeader,
        kReadingBody,
        kWebSocket,
        kWriting
    };

    struct Entry final {
        asio::ip::tcp::socket* socket{nullptr};
        Entry* prev{nullptr};
        Entry* next{nullptr};
        // Coarse timestamp source owned by the scanner; refreshed once per scan
        // tick so per-request touch()/setPhase() never hit the system clock.
        const std::int64_t* nowMs{nullptr};
        std::int64_t lastActiveMs{0};
        std::int64_t phaseStartedMs{0};
        Phase phase{Phase::kIdle};
        void* webSocketTarget{nullptr};
        bool (*webSocketTick)(void*, std::int64_t) noexcept{nullptr};

        void touch() noexcept;
        void setPhase(Phase nextPhase) noexcept;
    };

    class Guard final {
    public:
        Guard(ConnectionScanner* scanner, Entry& entry, asio::ip::tcp::socket& socket);
        ~Guard();

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        ConnectionScanner* scanner_;
        Entry* entry_;
    };

    ConnectionScanner(asio::any_io_executor executor, HttpServerOptions options);

    void start();
    void stop() noexcept;
    void setWorkerScanner(void* target, void (*scan)(void*) noexcept) noexcept;
    void registerEntry(Entry& entry, asio::ip::tcp::socket& socket) noexcept;
    void unregisterEntry(Entry& entry) noexcept;
    void closeAll() noexcept;

private:
    [[nodiscard]] std::chrono::milliseconds interval() const noexcept;
    [[nodiscard]] bool hasAnyTimeout() const noexcept;
    void schedule();
    void scan() noexcept;
    [[nodiscard]] bool isTimedOut(const Entry& entry, std::int64_t now) const noexcept;

    asio::steady_timer timer_;
    HttpServerOptions options_;
    std::int64_t cachedNowMs_{0};
    Entry sentinel_{};
    struct WorkerScanner final {
        void* target{nullptr};
        void (*scan)(void*) noexcept{nullptr};
    };
    std::array<WorkerScanner, 4> workerScanners_{};
    std::size_t workerScannerCount_{0};
    bool running_{false};
};

}  // namespace ruvia::detail
