#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <asio/any_io_executor.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

namespace ruvia::detail {

struct ConnectionScannerOptions final {
    std::chrono::milliseconds scanInterval{std::chrono::seconds(1)};
    std::int64_t idleTimeoutMs{0};
    std::int64_t headerTimeoutMs{0};
    std::int64_t bodyTimeoutMs{0};
    std::int64_t writeTimeoutMs{0};
};

class ConnectionScanner final {
public:
    enum class Phase {
        kIdle,
        kReadingHeader,
        kReadingBody,
        kWebSocket,
        kWriting
    };

    class Entry final {
    public:
        using WebSocketTick = bool (*)(void*, std::int64_t) noexcept;

        void touch() noexcept;
        void setPhase(Phase nextPhase) noexcept;
        [[nodiscard]] std::int64_t lastActiveMs() const noexcept;
        void setWebSocketHeartbeat(void* target, WebSocketTick tick) noexcept;
        void clearWebSocketHeartbeat(void* target) noexcept;

    private:
        friend class ConnectionScanner;

        [[nodiscard]] bool linked() const noexcept;
        [[nodiscard]] bool tickWebSocket(std::int64_t now) noexcept;

        asio::ip::tcp::socket* socket_{nullptr};
        Entry* prev_{nullptr};
        Entry* next_{nullptr};
        // Coarse timestamp source owned by the scanner; refreshed once per scan
        // tick so per-request touch()/setPhase() never hit the system clock.
        const std::int64_t* nowMs_{nullptr};
        std::int64_t lastActiveMs_{0};
        std::int64_t phaseStartedMs_{0};
        Phase phase_{Phase::kIdle};
        void* webSocketTarget_{nullptr};
        WebSocketTick webSocketTick_{nullptr};
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

    ConnectionScanner(asio::any_io_executor executor, ConnectionScannerOptions options);

    void start();
    void stop() noexcept;
    void setWorkerScanner(void* target, void (*scan)(void*) noexcept) noexcept;
    void registerEntry(Entry& entry, asio::ip::tcp::socket& socket) noexcept;
    void unregisterEntry(Entry& entry) noexcept;
    void closeAll() noexcept;

private:
    [[nodiscard]] bool hasAnyTimeout() const noexcept;
    void schedule();
    void scan() noexcept;
    [[nodiscard]] bool isTimedOut(const Entry& entry, std::int64_t now) const noexcept;

    asio::steady_timer timer_;
    ConnectionScannerOptions options_;
    std::int64_t cachedNowMs_{0};
    Entry sentinel_{};
    struct WorkerScanner final {
        void* target{nullptr};
        void (*scan)(void*) noexcept{nullptr};
    };
    std::array<WorkerScanner, 5> workerScanners_{};
    std::size_t workerScannerCount_{0};
    bool running_{false};
};

}  // namespace ruvia::detail
