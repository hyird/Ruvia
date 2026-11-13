#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/detail/socket_ops.hpp>
#include <asio/error.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/http/ip_address.h"
#include "ruvia/web/http_client_types.h"
#include "ruvia/web/server_config.h"

#include "ratelimit/rate_limiter.h"
#include "server/connection_scanner.h"
#include "server/inbound_transport_error.h"

namespace ruvia::detail {

[[nodiscard]] inline ip_address to_ip_address(const asio::ip::address& address) noexcept {
    if (address.is_v4()) {
        return ip_address::ipv4(address.to_v4().to_bytes());
    }
    const auto v6 = address.to_v6();
    return ip_address::ipv6(v6.to_bytes(), static_cast<std::uint32_t>(v6.scope_id()));
}

[[nodiscard]] inline asio::ip::address to_asio_address(const ip_address& address) noexcept {
    if (address.family() == ip_address_family::ipv4) {
        asio::ip::address_v4::bytes_type bytes{};
        std::copy(address.bytes().begin(), address.bytes().end(), bytes.begin());
        return asio::ip::address_v4(bytes);
    }
    asio::ip::address_v6::bytes_type bytes{};
    std::copy(address.bytes().begin(), address.bytes().end(), bytes.begin());
    return asio::ip::address_v6(bytes, address.scope_id());
}

// The peer address a connection's services name, shared by TCP and QUIC
// sessions: dotted IPv4, also for an IPv4-mapped IPv6 peer, otherwise the
// RFC 5952 IPv6 text.
inline void assign_peer_address(std::string& output, const asio::ip::address& address) {
    asio::ip::address_v4 v4;
    if (address.is_v4()) {
        v4 = address.to_v4();
    } else {
        const auto v6 = address.to_v6();
        if (!v6.is_v4_mapped()) {
            const auto bytes = v6.to_bytes();
            std::array<char, 64> ipv6_buffer;
            asio::error_code ec;
            const char* formatted = asio::detail::socket_ops::inet_ntop(
                ASIO_OS_DEF(AF_INET6), bytes.data(), ipv6_buffer.data(),
                ipv6_buffer.size(), v6.scope_id(), ec);
            if (formatted == nullptr) {
                asio::detail::throw_error(ec);
            }
            output.assign(formatted, std::strlen(formatted));
            return;
        }
        v4 = asio::ip::make_address_v4(asio::ip::v4_mapped, v6);
    }
    const auto v4_bytes = v4.to_bytes();
    std::array<char, 15> buffer;
    const auto formatted = format_ipv4_octets(v4_bytes.data(), buffer);
    output.assign(formatted.data(), formatted.size());
}

}  // namespace ruvia::detail

#if defined(_WIN32)
#include <winerror.h>
#endif

namespace ruvia::detail {

enum class accept_error_action : std::uint8_t { retry,
    report_and_retry,
    fail };

// Peer disappearance and readiness races are normal accept outcomes. Resource
// and network failures leave the listener usable but must remain observable.
[[nodiscard]] inline accept_error_action classify_accept_error(const std::error_code& error) noexcept {
    if (error == asio::error::connection_aborted || error == asio::error::connection_reset ||
        error == asio::error::eof || error == asio::error::try_again ||
        error == asio::error::would_block || error == asio::error::interrupted
#if defined(_WIN32)
        || (error.category() == asio::system_category() && error.value() == ERROR_NETNAME_DELETED)
#endif
    ) {
        return accept_error_action::retry;
    }
    if (error == asio::error::no_descriptors || error == asio::error::no_buffer_space ||
        error == asio::error::no_memory || error == std::errc::too_many_files_open_in_system ||
        error == std::errc::protocol_error || error == asio::error::network_down ||
        error == asio::error::network_unreachable || error == asio::error::host_unreachable ||
        error == asio::error::no_protocol_option || error == asio::error::operation_not_supported ||
        error == asio::error::no_permission || error == asio::error::timed_out) {
        return accept_error_action::report_and_retry;
    }
#if defined(EHOSTDOWN) && defined(ENONET)
    if (error.category() == asio::system_category() && (error.value() == EHOSTDOWN || error.value() == ENONET)) {
        return accept_error_action::report_and_retry;
    }
#endif
    return accept_error_action::fail;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

inline void validate_client_tcp_config(const client_tcp_config& config) {
    switch (config.no_delay_) {
        case tcp_no_delay_policy::system_default:
        case tcp_no_delay_policy::enable:
            break;
        default:
            throw std::invalid_argument("TCP no-delay policy is invalid");
    }
    switch (config.keep_alive_) {
        case tcp_keep_alive_policy::system_default:
        case tcp_keep_alive_policy::enable:
            break;
        default:
            throw std::invalid_argument("TCP keepalive policy is invalid");
    }
}

// Returns the error of the first enabled policy that cannot be applied.
[[nodiscard]] inline std::error_code apply_tcp_socket_policies(
    asio::ip::tcp::socket& socket, const client_tcp_config& config) noexcept {
    std::error_code error;
    if (config.no_delay_ == tcp_no_delay_policy::enable) {
        socket.set_option(asio::ip::tcp::no_delay(true), error);
        if (error) {
            return error;
        }
    }
    if (config.keep_alive_ == tcp_keep_alive_policy::enable) {
        socket.set_option(asio::socket_base::keep_alive(true), error);
    }
    return error;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

class connection_lifecycle;

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
    connection_lifecycle(connection_registry& registry, const void* listener, connection_event event,
        connection_open_callback_ref opened, connection_close_callback_ref closed, std::uint32_t interface_index = 0,
        connection_transport transport = {})
        : registry_(&registry),
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
    connection_lifecycle(connection_lifecycle&& other) noexcept
        : registry_(std::exchange(other.registry_, nullptr)),
          listener_(other.listener_),
          prev_(other.prev_),
          next_(other.next_),
          event_(other.event_),
          closed_(other.closed_),
          state_(std::move(other.state_)),
          info_(other.info_),
          transport_(other.transport_),
          aborted_(other.aborted_),
          bound_(other.bound_),
          interface_index_(other.interface_index_),
          local_text_(other.local_text_) {
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
        return local_text_.data();
    }
    [[nodiscard]] const ip_endpoint& local_endpoint() const noexcept {
        return event_.local();
    }
    [[nodiscard]] const ip_endpoint& remote_endpoint() const noexcept {
        return event_.remote();
    }
    void set_reason(connection_close_reason reason) noexcept {
        // Keep the initiating cause when teardown subsequently cancels I/O.
        if (info_.reason_ == connection_close_reason::normal ||
            (info_.reason_ == connection_close_reason::peer_eof && reason != connection_close_reason::normal)) {
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
        if (!error || error == asio::error::would_block || error == asio::error::try_again ||
            error == asio::error::interrupted) {
            return;
        }
        if (is_stream_read_end(error)) {
            set_reason(connection_close_reason::peer_eof);
        } else if (error == asio::error::timed_out || error == std::errc::timed_out) {
            set_reason(connection_close_reason::timeout);
        } else if (error == asio::error::operation_aborted || error == std::errc::operation_canceled ||
                   error == asio::error::bad_descriptor) {
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
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        value += (std::min)(maximum - value, static_cast<std::uint64_t>(bytes));
    }
    void format_local() noexcept {
        const auto address = to_asio_address(event_.local().address());
        asio::error_code error;
        if (address.is_v4()) {
            const auto bytes = address.to_v4().to_bytes();
            (void)asio::detail::socket_ops::inet_ntop(ASIO_OS_DEF(AF_INET), bytes.data(),
                local_text_.data(), local_text_.size(), 0, error);
        } else {
            const auto v6 = address.to_v6();
            const auto bytes = v6.to_bytes();
            (void)asio::detail::socket_ops::inet_ntop(ASIO_OS_DEF(AF_INET6), bytes.data(),
                local_text_.data(), local_text_.size(), v6.scope_id(), error);
        }
    }
    connection_registry* registry_;
    const void* listener_;
    connection_lifecycle* prev_{};
    connection_lifecycle* next_{};
    connection_event event_;
    connection_close_callback_ref closed_;
    std::shared_ptr<void> state_;
    connection_close_info info_;
    connection_transport transport_;
    bool aborted_{};
    bool bound_{};
    std::uint32_t interface_index_{};
    std::array<char, 64> local_text_{};
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
