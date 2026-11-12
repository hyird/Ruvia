#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include <asio/ip/udp.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_handle.h"

#include "http3/http3_connection_id.h"
#include "server/inherited_listener_sockets.h"

namespace ruvia::detail {

class http3_endpoint;

#if defined(_WIN32)
class windows_acceptor;
#endif

// One datagram handed to a worker that did not receive it. It owns its bytes
// because it crosses threads.
struct http3_handoff_datagram final {
    std::pmr::vector<std::byte> bytes_;
    asio::ip::udp::endpoint remote_;
    std::uint32_t ecn_{0};
};

// Slot identities outlive their worker-local endpoint. Another worker can
// still own a connection issued under that slot's secret after this one has
// released its last connection and socket.
struct http3_endpoint_registration final {
    http3_endpoint* endpoint_;
    asio::ip::udp::endpoint local_;
    std::shared_ptr<const http3_secret> secret_;
    std::size_t listener_index_;
};

// Datagrams in flight from one producer to one worker, product limit. A
// datagram past it is dropped, as UDP may drop it anyway; QUIC loss recovery
// retransmits what matters.
inline constexpr std::size_t http3_max_datagrams_in_flight = 256;

// One handed-off datagram's place in its in-flight bound: reserved before the
// datagram is posted, released when the posted job ran or was abandoned, on
// whichever thread that is.
class http3_in_flight_token final {
public:
    // Empty when the bound is full.
    [[nodiscard]] static http3_in_flight_token reserve(std::atomic<std::size_t>& count) noexcept {
        if (count.fetch_add(1, std::memory_order_relaxed) >= http3_max_datagrams_in_flight) {
            count.fetch_sub(1, std::memory_order_relaxed);
            return http3_in_flight_token(nullptr);
        }
        return http3_in_flight_token(&count);
    }
    http3_in_flight_token(http3_in_flight_token&& other) noexcept
        : count_(std::exchange(other.count_, nullptr)) {}
    http3_in_flight_token(const http3_in_flight_token&) = delete;
    http3_in_flight_token& operator=(const http3_in_flight_token&) = delete;
    http3_in_flight_token& operator=(http3_in_flight_token&&) = delete;
    ~http3_in_flight_token() {
        if (count_ != nullptr) {
            count_->fetch_sub(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return count_ != nullptr;
    }

private:
    explicit http3_in_flight_token(std::atomic<std::size_t>* count) noexcept
        : count_(count) {}

    std::atomic<std::size_t>* count_;
};

// The HTTP/3 rendezvous of every worker of one application run. A datagram
// for a connection a worker owns (see http3_connection_id.h) is handed to
// that worker here when another thread received it: on POSIX another worker
// whose SO_REUSEPORT socket the kernel chose, on Windows the acceptor that
// receives every datagram. It travels on the owner's bounded worker queue,
// at most http3_max_datagrams_in_flight per owner at a time, and is dropped
// when either bound is full or the owner no longer serves.
class http3_worker_directory final {
public:
    using receive_type = void (*)(void* target, std::size_t listener_index, http3_handoff_datagram& datagram) noexcept;

    explicit http3_worker_directory(std::size_t worker_count);
    http3_worker_directory(const http3_worker_directory&) = delete;
    ~http3_worker_directory();
    http3_worker_directory& operator=(const http3_worker_directory&) = delete;

    [[nodiscard]] std::size_t worker_count() const noexcept {
        return slots_.size();
    }

    // Construction thread, before any worker serves -- for a directory
    // application::add_listener() creates, before any worker uses it.
    void bind_worker(std::size_t index, const worker_handle& worker_value);
    // The owning worker's thread: opens and closes its receiving side.
    void attach(std::size_t index, void* target, receive_type receive) noexcept;
    void detach(std::size_t index) noexcept;

    // Worker-local endpoint registrations share packet routing across removed
    // and re-added listeners bound to the same UDP endpoint.
    void register_endpoint(std::size_t worker_index, http3_endpoint& endpoint,
        const asio::ip::udp::endpoint& local, std::shared_ptr<const http3_secret> secret,
        std::size_t listener_index);
    void unregister_endpoint(std::size_t worker_index, http3_endpoint& endpoint) noexcept;
    [[nodiscard]] std::span<const http3_endpoint_registration> endpoints(std::size_t worker_index) const noexcept {
        return slots_[worker_index]->endpoints_;
    }

#if !defined(_WIN32)
    // The first call for `listener_index` binds every worker's SO_REUSEPORT
    // socket in worker index order 0..N-1 -- or takes `inherited`'s UDP
    // sockets, in that order already, when it holds them -- and attaches
    // SO_ATTACH_REUSEPORT_CBPF on Linux; each call assigns `worker_index`'s
    // socket to `socket`.
    void open_listener_socket(std::size_t worker_index, std::size_t listener_index,
        const asio::ip::udp::endpoint& configured_endpoint, const http3_secret& secret,
        inherited_listener_sockets* inherited, asio::ip::udp::socket& socket);
    // Closes the sockets of `listener_index` no worker claimed, for a listener
    // whose addition was rolled back.
    void close_unclaimed_listener_sockets(std::size_t listener_index) noexcept;
#endif
    // Any thread. False when the datagram was dropped.
    [[nodiscard]] bool hand_off(std::size_t owner, std::size_t listener_index,
        std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
        std::uint32_t ecn = 0) noexcept;

private:
    struct slot final {
        worker_handle worker_;
        // Read and written only on the owning worker's thread.
        void* target_{nullptr};
        receive_type receive_{nullptr};
        std::atomic<std::size_t> in_flight_{0};
        std::pmr::vector<http3_endpoint_registration> endpoints_{process_resource()};
    };

    std::pmr::vector<std::unique_ptr<slot, pmr_object_deleter<slot>>> slots_;
    std::mutex bind_mutex_;
    std::pmr::vector<std::pmr::vector<int>> listener_sockets_;
};

// Where one worker's HTTP/3 endpoints sit among the application's workers:
// the directory (null without an HTTP/3 listener) and this worker's index in
// it, which the connection IDs it issues carry.
struct http3_worker_placement final {
    http3_worker_directory* directory_;
    std::size_t worker_index_;
#if defined(_WIN32)
    // Sends this worker's datagrams from the listener's one UDP socket.
    windows_acceptor* acceptor_;
#endif
};

}  // namespace ruvia::detail
