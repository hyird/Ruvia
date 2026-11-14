#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/completion_condition.hpp>
#include <asio/detail/socket_ops.hpp>
#include <asio/error.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>

#include "blocking_pool.h"
#include "borrowed_text.h"
#include "connection_scanner.h"
#include "event_loop.h"
#include "http1_server_response.h"
#include "http_error.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response.h"
#include "http_server.h"
#include "io_operation.h"
#include "server_observation.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "transport_stream_end.h"
#include "worker_ref.h"
#include "worker_signal.h"

#if defined(_WIN32)
#include <winerror.h>
#endif
#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif
#if defined(_WIN32)
#include <mswsock.h>

#include <asio/windows/overlapped_ptr.hpp>
#endif

namespace ruvia::detail {

inline constexpr std::size_t http_body_buffer_bytes = std::size_t{8} * 1024;
// The smallest capacity a buffered known-length body grows to while it is read
// straight into its own storage. It matches the WebSocket read window: a peer
// that sends only headers can make the declared length allocate at most this
// much before any body bytes arrive; beyond it capacity doubles from arrivals.
inline constexpr std::size_t known_length_min_read_bytes = std::size_t{64} * 1024;

// http_error_info borrows its fields. The caller's http_protocol_error lvalue
// must outlive any suspension of the error handler that reads this view.
[[nodiscard]] inline http_error_info copy_http_protocol_error_info(const http_protocol_error& error RUVIA_LIFETIMEBOUND) noexcept {
    return http_error_info({.status_ = error.status(), .code_ = error.code(), .message_ = std::string_view(error.what())});
}
http_error_info copy_http_protocol_error_info(const http_protocol_error&&) = delete;

// Owner-thread phase of one server worker:
// - running: admits connections and requests;
// - stopped: the core runtime stopped; every connection is closed now.
enum class http_server_worker_state : std::uint8_t {
    fresh,
    running,
    stopped,
};

// True only while the worker admits new connections and requests.
[[nodiscard]] inline bool http_server_worker_running(http_server_worker_state state_value) noexcept {
    return state_value == http_server_worker_state::running;
}

// One worker's admitted-connection gauge. Only the owning worker counts; any
// thread reads it relaxed (stats(), and the Windows acceptor choosing the
// least-loaded worker).
class connection_count final {
public:
    explicit connection_count(connection_count* parent = nullptr) noexcept :
        parent_(parent) {}
    connection_count(const connection_count&) = delete;
    connection_count& operator=(const connection_count&) = delete;

    [[nodiscard]] std::size_t active() const noexcept {
        return active_.load(std::memory_order_relaxed);
    }

    // Worker thread: one admitted connection.
    void acquire() noexcept {
        active_.fetch_add(1, std::memory_order_relaxed);
        if (parent_) {
            parent_->acquire();
        }
    }

    // Worker thread: one closed connection.
    void release() noexcept {
        const auto previous = active_.fetch_sub(1, std::memory_order_relaxed);
        if (previous == 0) {
            std::terminate();
        }
        if (parent_) {
            parent_->release();
        }
        if (previous == 1) {
            std::vector<std::weak_ptr<wait_state>> ready;
            {
                std::lock_guard lock(mutex_);
                ready.swap(waiters_);
            }
            for (auto& weak : ready) {
                if (auto state = weak.lock()) {
                    state->closed_.store(true, std::memory_order_release);
                    detail::worker_ref_access::defer_or_terminate(state->worker_, [state] {
                        state->changed_.notify();
                    });
                }
            }
        }
    }

    // The caller retains the gauge; registrations never borrow its coroutine.
    static task<void> wait(std::shared_ptr<connection_count> count, worker_ref caller, operation_options options, stop_token host_stop) {
        if (!caller.is_current()) {
            throw std::logic_error("connection wait must run on its caller worker");
        }
        stop_group stops(options.stop_token_, host_stop);
        options.stop_token_ = stops.token();
        auto state = std::make_shared<wait_state>(caller);
        operation_interrupt_source source;
        operation_interrupt interrupt(source, caller, options, state.get(), [](void* target) noexcept {
            static_cast<wait_state*>(target)->changed_.notify();
        }, "wait_connections_closed");
        {
            std::lock_guard lock(count->mutex_);
            if (count->active() != 0) {
                count->waiters_.push_back(state);
            } else {
                state->closed_.store(true, std::memory_order_relaxed);
            }
        }
        // Never lock() under the gauge mutex: a temporary strong reference
        // could become the last owner and destroy a wait state while locked.
        struct subscription final {
            connection_count& count_;
            std::weak_ptr<wait_state> state_;
            ~subscription() {
                std::lock_guard lock(count_.mutex_);
                std::erase_if(count_.waiters_, [this](const auto& weak) {
                    return weak.expired() || (!weak.owner_before(state_) && !state_.owner_before(weak));
                });
            }
        } registration{*count, state};
        interrupt.throw_if_interrupted();
        while (!state->closed_.load(std::memory_order_acquire)) {
            co_await state->changed_.wait();
            interrupt.throw_if_interrupted();
        }
        interrupt.throw_if_interrupted();
    }

private:
    std::atomic<std::size_t> active_{};
    struct wait_state final {
        explicit wait_state(worker_ref worker) :
            worker_(std::move(worker)),
            changed_(worker_) {}
        worker_ref worker_;
        worker_signal changed_;
        std::atomic<bool> closed_{false};
    };
    connection_count* parent_;
    std::mutex mutex_;
    std::vector<std::weak_ptr<wait_state>> waiters_;
};

// One admitted connection's place in its worker's connection_count, as a
// linear value. Every transport's connection owns exactly one: a TCP session
// through accepted_connection_lease, a QUIC connection directly.
class connection_slot final {
public:
    explicit connection_slot(connection_count& count) noexcept :
        count_(&count) {
        count_->acquire();
    }

    connection_slot(const connection_slot&) = delete;
    connection_slot& operator=(const connection_slot&) = delete;

    connection_slot(connection_slot&& other) noexcept :
        count_(std::exchange(other.count_, nullptr)) {}

    connection_slot& operator=(connection_slot&&) = delete;

    ~connection_slot() {
        if (count_ != nullptr) {
            count_->release();
        }
    }

private:
    connection_count* count_;
};

// Merge endpoint gauges across workers and successive listener instances.
inline void append_listener_stats(http_server_stats& total, const http_server_stats& worker) {
    for (const auto& listener : worker.listeners_) {
        auto found = std::find_if(total.listeners_.begin(), total.listeners_.end(), [&](const auto& existing) {
            return existing.address_ == listener.address_ && existing.port_ == listener.port_;
        });
        if (found == total.listeners_.end()) {
            total.listeners_.push_back(listener);
        } else {
            found->active_connections_ += listener.active_connections_;
        }
    }
}

// Process-wide monotonically increasing IDs cannot alias across workers,
// hosts, or restarts. Exhaustion terminates rather than recycling a stale ID.
class connection_id_sequence final {
public:
    connection_id_sequence() noexcept = default;

    connection_id_sequence(const connection_id_sequence&) = delete;
    connection_id_sequence& operator=(const connection_id_sequence&) = delete;

    [[nodiscard]] std::uint64_t next() noexcept {
        const auto id = next_.fetch_add(1, std::memory_order_relaxed);
        if (id == 0) {
            std::terminate();
        }
        return id;
    }

private:
    inline static std::atomic<std::uint64_t> next_{1};
};

enum class accept_error_action : std::uint8_t { retry,
    report_and_retry,
    fail };

// Peer disappearance and readiness races are normal accept outcomes. Resource
// and network failures leave the listener usable but must remain observable.
[[nodiscard]] inline accept_error_action classify_accept_error(const std::error_code& error) noexcept {
    if (error == asio::error::connection_aborted || error == asio::error::connection_reset || error == asio::error::eof || error == asio::error::try_again || error == asio::error::would_block || error == asio::error::interrupted
#if defined(_WIN32)
        || (error.category() == asio::system_category() && error.value() == ERROR_NETNAME_DELETED)
#endif
    ) {
        return accept_error_action::retry;
    }
    if (error == asio::error::no_descriptors || error == asio::error::no_buffer_space || error == asio::error::no_memory || error == std::errc::too_many_files_open_in_system || error == std::errc::protocol_error || error == asio::error::network_down || error == asio::error::network_unreachable || error == asio::error::host_unreachable || error == asio::error::no_protocol_option || error == asio::error::operation_not_supported || error == asio::error::no_permission || error == asio::error::timed_out) {
        return accept_error_action::report_and_retry;
    }
#if defined(EHOSTDOWN) && defined(ENONET)
    if (error.category() == asio::system_category() && (error.value() == EHOSTDOWN || error.value() == ENONET)) {
        return accept_error_action::report_and_retry;
    }
#endif
    return accept_error_action::fail;
}

// Intrusive, worker-affine registry. No lock is held while invoking user code
// or releasing state. IDs are retired before close callbacks can reenter.
class connection_registry final {
public:
    [[nodiscard]] bool close_connection(std::uint64_t id) noexcept;
    void close_listener(const void* listener) noexcept;

private:
    friend class connection_lifecycle;
    connection_lifecycle* head_{};
};

class connection_lifecycle final {
public:
    connection_lifecycle(connection_registry& registry, const void* listener, connection_event event, connection_open_callback_ref opened, connection_close_callback_ref closed, std::uint32_t interface_index = 0, connection_transport transport = {}) :
        registry_(&registry),
        listener_(listener),
        event_(std::move(event)),
        closed_(closed),
        transport_(transport),
        bound_(transport.valid()),
        interface_index_(interface_index) {
        next_ = registry.head_;
        if (next_ != nullptr) {
            next_->prev_ = this;
        }
        registry.head_ = this;
        format_local();
        try {
            if (opened) {
                state_ = opened(event_);
            }
        } catch (...) {
            if (prev_ != nullptr) {
                prev_->next_ = next_;
            } else {
                registry_->head_ = next_;
            }
            if (next_ != nullptr) {
                next_->prev_ = prev_;
            }
            registry_ = nullptr;
            if (bound_) {
                transport_.close();
            }
            info_.reason_ = connection_close_reason::handler_error;
            if (closed_) {
                closed_(event_, state_, info_);
            }
            throw;
        }
    }
    connection_lifecycle(const connection_lifecycle&) = delete;
    connection_lifecycle& operator=(const connection_lifecycle&) = delete;
    connection_lifecycle& operator=(connection_lifecycle&&) = delete;
    connection_lifecycle(connection_lifecycle&& other) noexcept :
        registry_(std::exchange(other.registry_, nullptr)),
        listener_(other.listener_),
        prev_(other.prev_),
        next_(other.next_),
        event_(other.event_),
        closed_(other.closed_),
        state_(std::move(other.state_)),
        info_(other.info_),
        nearest_hop_(other.nearest_hop_),
        hop_restricted_(other.hop_restricted_),
        transport_(other.transport_),
        aborted_(other.aborted_),
        bound_(other.bound_),
        interface_index_(other.interface_index_),
        local_text_(other.local_text_),
        local_text_size_(other.local_text_size_) {
        if (registry_ != nullptr) {
            if (prev_ != nullptr) {
                prev_->next_ = this;
            } else {
                registry_->head_ = this;
            }
            if (next_ != nullptr) {
                next_->prev_ = this;
            }
        }
    }
    ~connection_lifecycle() {
        if (registry_ == nullptr) {
            return;
        }
        if (prev_ != nullptr) {
            prev_->next_ = next_;
        } else {
            registry_->head_ = next_;
        }
        if (next_ != nullptr) {
            next_->prev_ = prev_;
        }
        registry_ = nullptr;
        const auto closed = closed_;
        const auto event = event_;
        const auto info = info_;
        const auto state = std::move(state_);
        if (closed) {
            closed(event, state, info);
        }
    }
    [[nodiscard]] std::uint64_t connection_id() const noexcept {
        return event_.connection_id();
    }
    [[nodiscard]] const std::shared_ptr<void>& state() const noexcept {
        return state_;
    }
    [[nodiscard]] std::string_view local_address() const noexcept {
        return std::string_view(local_text_.data(), local_text_size_);
    }
    [[nodiscard]] const ip_endpoint& local_endpoint() const noexcept {
        return event_.local();
    }
    [[nodiscard]] const ip_endpoint& remote_endpoint() const noexcept {
        return event_.remote();
    }
    // On a PROXY protocol listener the event's remote is self-reported by the
    // socket peer; forwarding trust must also hold for that nearest hop.
    void restrict_forwarding_trust(std::optional<ip_address> nearest_hop) noexcept {
        nearest_hop_ = nearest_hop;
        hop_restricted_ = true;
    }
    [[nodiscard]] bool forwarding_trust_restricted() const noexcept {
        return hop_restricted_;
    }
    [[nodiscard]] const std::optional<ip_address>& nearest_hop() const noexcept {
        return nearest_hop_;
    }
    void set_reason(connection_close_reason reason) noexcept {
        // Keep the initiating cause when teardown subsequently cancels I/O.
        if (info_.reason_ == connection_close_reason::normal || (info_.reason_ == connection_close_reason::peer_eof && reason != connection_close_reason::normal)) {
            info_.reason_ = reason;
        }
    }
    void note_read(std::size_t bytes) noexcept {
        add_bytes(info_.bytes_in_, bytes);
    }
    void note_write(std::size_t bytes) noexcept {
        add_bytes(info_.bytes_out_, bytes);
    }
    void note_request() noexcept {
        add_bytes(info_.requests_, 1);
    }
    void note_error(const std::error_code& error) noexcept {
        if (!error || error == asio::error::would_block || error == asio::error::try_again || error == asio::error::interrupted) {
            return;
        }
        if (is_stream_read_end(error)) {
            set_reason(connection_close_reason::peer_eof);
        } else if (error == asio::error::timed_out || error == std::errc::timed_out) {
            set_reason(connection_close_reason::timeout);
        } else if (error == asio::error::operation_aborted || error == std::errc::operation_canceled || error == asio::error::bad_descriptor) {
            set_reason(connection_close_reason::cancelled);
        } else if (error == std::errc::protocol_error || error.category() == asio::error::get_ssl_category()) {
            set_reason(connection_close_reason::protocol_error);
        } else {
            set_reason(connection_close_reason::transport_error);
        }
    }
    void note_exception(const std::exception_ptr& failure) noexcept {
        if (failure == nullptr) {
            return;
        }
        try {
            std::rethrow_exception(failure);
        } catch (const inbound_transport_error& error) {
            note_error(error.code());
        } catch (...) {
            set_reason(connection_close_reason::handler_error);
        }
    }
    [[nodiscard]] std::uint32_t interface_index() const noexcept {
        return interface_index_;
    }
    [[nodiscard]] std::uint16_t local_port() const noexcept {
        return event_.local().port();
    }
    void bind_transport(connection_transport transport) noexcept {
        transport_ = transport;
        bound_ = true;
        if (aborted_) {
            transport_.close();
        }
    }
    void abort() noexcept {
        set_reason(connection_close_reason::aborted);
        aborted_ = true;
        if (bound_) {
            transport_.close();
        }
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }

private:
    friend class connection_registry;
    static void add_bytes(std::uint64_t& value, std::size_t bytes) noexcept {
        value = saturating_add(value, static_cast<std::uint64_t>(bytes));
    }
    void format_local() noexcept {
        local_text_size_ = static_cast<std::uint8_t>(event_.local().address().format_to(local_text_).size());
    }
    connection_registry* registry_;
    const void* listener_;
    connection_lifecycle* prev_{};
    connection_lifecycle* next_{};
    connection_event event_;
    connection_close_callback_ref closed_;
    std::shared_ptr<void> state_;
    connection_close_info info_;
    std::optional<ip_address> nearest_hop_;
    bool hop_restricted_{};
    connection_transport transport_;
    bool aborted_{};
    bool bound_{};
    std::uint32_t interface_index_{};
    std::array<char, ip_address::max_text_size> local_text_{};
    std::uint8_t local_text_size_{};
};

inline bool connection_registry::close_connection(std::uint64_t id) noexcept {
    if (id == 0) {
        return false;
    }
    for (auto* node = head_; node != nullptr; node = node->next_) {
        if (node->connection_id() == id) {
            node->abort();
            return true;
        }
    }
    return false;
}

inline void connection_registry::close_listener(const void* listener) noexcept {
    for (auto* node = head_; node != nullptr; node = node->next_) {
        if (node->listener_ == listener) {
            node->abort();
        }
    }
}

}  // namespace ruvia::detail
