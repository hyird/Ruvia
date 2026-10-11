// clang-format off
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cerrno>
#endif
// clang-format on

#include "http3/http3_udp_socket.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/associated_allocator.hpp>
#include <asio/error.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/post.hpp>

#include "ruvia/core/memory/process_resource.h"
#ifdef _WIN32
#include <asio/windows/overlapped_ptr.hpp>
#endif

namespace ruvia::detail {
namespace {
using udp_type = asio::ip::udp;
#ifdef _WIN32
using native_socket_length_type = int;
#else
using native_socket_length_type = socklen_t;
#endif

std::error_code bad_message() noexcept {
    return std::make_error_code(std::errc::bad_message);
}

std::error_code io_error() noexcept {
    return std::make_error_code(std::errc::io_error);
}

#ifndef _WIN32
std::error_code no_memory() noexcept {
    return std::make_error_code(std::errc::not_enough_memory);
}
#endif

std::error_code aborted() noexcept {
    return asio::error::make_error_code(asio::error::operation_aborted);
}

bool wildcard_address(const asio::ip::address& address) noexcept {
    return address.is_unspecified();
}

bool valid_concrete_address(const asio::ip::address& address) noexcept {
    if (address.is_v4()) {
        const auto ipv4 = address.to_v4();
        const auto bytes_value = ipv4.to_bytes();
        return !ipv4.is_unspecified() && !ipv4.is_multicast() &&
               !(bytes_value[0] == 255 && bytes_value[1] == 255 && bytes_value[2] == 255 && bytes_value[3] == 255);
    }
    if (!address.is_v6()) {
        return false;
    }
    const auto ipv6 = address.to_v6();
    return !ipv6.is_unspecified() && !ipv6.is_multicast() && !ipv6.is_v4_mapped() &&
           !ipv6.is_link_local() && ipv6.scope_id() == 0;
}

bool valid_bind_address(const asio::ip::address& address) noexcept {
    if (address.is_v4()) {
        const auto ipv4 = address.to_v4();
        const auto bytes_value = ipv4.to_bytes();
        return !ipv4.is_multicast() &&
               !(bytes_value[0] == 255 && bytes_value[1] == 255 && bytes_value[2] == 255 && bytes_value[3] == 255);
    }
    if (address.is_v6()) {
        const auto ipv6 = address.to_v6();
        return !ipv6.is_multicast() && !ipv6.is_v4_mapped() && !ipv6.is_link_local() &&
               ipv6.scope_id() == 0;
    }
    return false;
}

bool same_ip(const asio::ip::address& left, const asio::ip::address& right) noexcept {
    return left == right;
}

bool valid_peer(const udp_type::endpoint& endpoint, bool ipv6_socket) noexcept {
    return endpoint.port() != 0 && valid_concrete_address(endpoint.address()) &&
           endpoint.address().is_v6() == ipv6_socket;
}

bool valid_source(const udp_type::endpoint& endpoint, bool ipv6_socket,
    const udp_type::endpoint& bound) noexcept {
    if (endpoint.port() == 0 || endpoint.port() != bound.port() ||
        !valid_concrete_address(endpoint.address()) || endpoint.address().is_v6() != ipv6_socket) {
        return false;
    }
    return wildcard_address(bound.address()) || same_ip(endpoint.address(), bound.address());
}

struct native_endpoint final {
    sockaddr_storage address_{};
    native_socket_length_type length_{};
};

native_endpoint make_native_endpoint(const udp_type::endpoint& endpoint) noexcept {
    native_endpoint result;
    if (endpoint.address().is_v4()) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(endpoint.port());
        const auto bytes_value = endpoint.address().to_v4().to_bytes();
        std::memcpy(&address.sin_addr, bytes_value.data(), bytes_value.size());
        std::memcpy(&result.address_, &address, sizeof(address));
        result.length_ = static_cast<native_socket_length_type>(sizeof(address));
    } else {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(endpoint.port());
        const auto bytes_value = endpoint.address().to_v6().to_bytes();
        std::memcpy(&address.sin6_addr, bytes_value.data(), bytes_value.size());
        address.sin6_scope_id = endpoint.address().to_v6().scope_id();
        std::memcpy(&result.address_, &address, sizeof(address));
        result.length_ = static_cast<native_socket_length_type>(sizeof(address));
    }
    return result;
}

bool parse_native_endpoint(const sockaddr* address, native_socket_length_type length, bool ipv6_socket,
    udp_type::endpoint& result_value) noexcept {
    if (address == nullptr) {
        return false;
    }
    if (!ipv6_socket && address->sa_family == AF_INET &&
        length >= static_cast<native_socket_length_type>(sizeof(sockaddr_in))) {
        const auto& ipv4 = *reinterpret_cast<const sockaddr_in*>(address);
        asio::ip::address_v4::bytes_type bytes_value{};
        std::memcpy(bytes_value.data(), &ipv4.sin_addr, bytes_value.size());
        result_value = udp_type::endpoint(asio::ip::address_v4(bytes_value), ntohs(ipv4.sin_port));
        return valid_peer(result_value, false);
    }
    if (ipv6_socket && address->sa_family == AF_INET6 &&
        length >= static_cast<native_socket_length_type>(sizeof(sockaddr_in6))) {
        const auto& ipv6 = *reinterpret_cast<const sockaddr_in6*>(address);
        asio::ip::address_v6::bytes_type bytes_value{};
        std::memcpy(bytes_value.data(), &ipv6.sin6_addr, bytes_value.size());
        result_value = udp_type::endpoint(asio::ip::address_v6(bytes_value, ipv6.sin6_scope_id),
            ntohs(ipv6.sin6_port));
        return valid_peer(result_value, true);
    }
    return false;
}

// A receive buffer longer than any UDP payload only wastes the caller's slot.
constexpr std::size_t max_receive_buffer_bytes = 65536;

#ifndef _WIN32
// Datagrams one readiness or flush round may complete per slot before the
// socket yields its loop turn and re-arms.
constexpr std::size_t batch_rounds_per_slot = 4;
#endif

std::size_t checked_operation_count(std::size_t count) {
    if (count == 0 || count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("HTTP/3 UDP operation limits must be positive 32-bit counts");
    }
    return count;
}

#ifdef _WIN32
// An overlapped completion reports the Win32 translation of its NTSTATUS rather
// than the Winsock code a synchronous call returns. Map per-datagram ICMP and
// truncation results to their portable codes so callers classify both alike.
std::error_code portable_completion_error(const std::error_code& error) noexcept {
    if (error.category() != std::system_category()) {
        return error;
    }
    switch (error.value()) {
        case ERROR_NETNAME_DELETED:
            return asio::error::make_error_code(asio::error::connection_reset);
        case ERROR_PORT_UNREACHABLE:
        case ERROR_CONNECTION_REFUSED:
            return asio::error::make_error_code(asio::error::connection_refused);
        case ERROR_HOST_UNREACHABLE:
            return asio::error::make_error_code(asio::error::host_unreachable);
        case ERROR_NETWORK_UNREACHABLE:
            return asio::error::make_error_code(asio::error::network_unreachable);
        case ERROR_MORE_DATA:
            return asio::error::make_error_code(asio::error::message_size);
        default:
            return error;
    }
}
#endif

}  // namespace

class http3_udp_socket::impl final {
public:
    impl(asio::io_context& network_io, udp_type::endpoint bind_endpoint, operation_limits limits)
        : io_(network_io),
          bind_endpoint_(std::move(bind_endpoint)),
          socket_(io_),
          owner_thread_(std::this_thread::get_id()),
          receive_slots_(checked_operation_count(limits.receive_), process_resource()),
          send_slots_(checked_operation_count(limits.send_), process_resource()),
          free_receive_(receive_slots_.size()),
          free_send_(send_slots_.size())
#ifndef _WIN32
          ,
          receive_queue_(receive_slots_.size()),
          send_queue_(send_slots_.size())
#if defined(__linux__)
          ,
          receive_messages_(receive_slots_.size(), process_resource()),
          receive_vectors_(receive_slots_.size(), process_resource()),
          send_messages_(send_slots_.size(), process_resource()),
          send_vectors_(send_slots_.size(), process_resource())
#endif
#endif
    {
        if (!valid_bind_address(bind_endpoint_.address())) {
            throw std::invalid_argument("HTTP/3 server network UDP bind address is unsupported");
        }
        ipv6_socket_ = bind_endpoint_.address().is_v6();
        for (std::size_t index = 0; index < receive_slots_.size(); ++index) {
            free_receive_.push(static_cast<std::uint32_t>(index));
#ifdef _WIN32
            receive_slots_[index].handler_storage_.owner_ = this;
#endif
        }
        for (std::size_t index = 0; index < send_slots_.size(); ++index) {
            free_send_.push(static_cast<std::uint32_t>(index));
#ifdef _WIN32
            send_slots_[index].handler_storage_.owner_ = this;
#endif
        }
    }

    ~impl() {
        require_owner_thread_noexcept();
        if (!stopping_) {
            request_stop();
        }
        if (!done()) {
            std::terminate();
        }
    }

    void prepare() {
        require_owner_thread();
        if (prepared_ || stopping_) {
            throw std::logic_error("HTTP/3 server network UDP socket cannot be prepared in this state");
        }

        asio::error_code error;
        socket_.open(ipv6_socket_ ? udp_type::v6() : udp_type::v4(), error);
        if (error) {
            throw std::system_error(error, "open HTTP/3 server network UDP socket");
        }

        try {
            if (ipv6_socket_) {
                socket_.set_option(asio::ip::v6_only(true), error);
                if (error) {
                    throw std::system_error(error, "set HTTP/3 server network IPv6-only mode");
                }
                set_packet_info_option(IPPROTO_IPV6,
#ifdef _WIN32
                    IPV6_PKTINFO
#else
                    IPV6_RECVPKTINFO
#endif
                );
            } else {
                set_packet_info_option(IPPROTO_IP, IP_PKTINFO);
            }
#ifdef _WIN32
            load_message_extensions();
            // This socket serves independent QUIC peers. A closed peer's ICMP
            // port-unreachable or a TTL-expired ICMP must not fail the shared
            // receive operation.
            disable_peer_error_report(SIO_UDP_CONNRESET);
            disable_peer_error_report(SIO_UDP_NETRESET);
#endif
            socket_.bind(bind_endpoint_, error);
            if (error) {
                throw std::system_error(error, "bind HTTP/3 server network UDP socket");
            }
            bound_endpoint_ = socket_.local_endpoint(error);
            if (error) {
                throw std::system_error(error, "read HTTP/3 server network UDP bound port");
            }
            prepared_ = true;
        } catch (...) {
            asio::error_code ignored;
            (void)socket_.close(ignored);
            throw;
        }
    }

    std::uint16_t bound_port() const noexcept {
        return prepared_ ? bound_endpoint_.port() : 0;
    }

    bool async_receive(std::span<std::byte> bytes_value, void* context_value, receive_completion completion) noexcept {
        require_owner_thread_noexcept();
        if (!prepared_ || stopping_ || free_receive_.empty() || completion == nullptr ||
            bytes_value.empty() || bytes_value.size() > max_receive_buffer_bytes) {
            return false;
        }
        const auto index = free_receive_.pop();
        auto& slot = receive_slots_[index];
        slot.active_ = true;
        slot.context_ = context_value;
        slot.completion_ = completion;
        slot.bytes_ = bytes_value;
#ifdef _WIN32
        const bool accepted = start_windows_receive(index);
#else
        const bool accepted = queue_posix_receive(index);
#endif
        if (!accepted) {
            slot.active_ = false;
            slot.context_ = nullptr;
            slot.completion_ = nullptr;
            slot.bytes_ = {};
            free_receive_.push(index);
            return false;
        }
        return true;
    }

    bool async_send(send_view view, void* context_value, send_completion completion) noexcept {
        require_owner_thread_noexcept();
        const std::size_t max_payload = ipv6_socket_ ? 65527U : 65507U;
        if (!prepared_ || stopping_ || free_send_.empty() || completion == nullptr ||
            !valid_source(view.source_, ipv6_socket_, bound_endpoint_) ||
            !valid_peer(view.peer_, ipv6_socket_) || view.bytes_.size() > max_payload) {
            return false;
        }
        const auto index = free_send_.pop();
        auto& slot = send_slots_[index];
        slot.active_ = true;
        slot.context_ = context_value;
        slot.completion_ = completion;
        slot.view_ = std::move(view);
#ifdef _WIN32
        const bool accepted = start_windows_send(index);
#else
        const bool accepted = queue_posix_send(index);
#endif
        if (!accepted) {
            slot.active_ = false;
            slot.context_ = nullptr;
            slot.completion_ = nullptr;
            slot.view_ = {};
            free_send_.push(index);
            return false;
        }
        return true;
    }

    void request_stop() noexcept {
        require_owner_thread_noexcept();
        if (stopping_) {
            return;
        }
        stopping_ = true;
        asio::error_code ignored;
#ifdef _WIN32
        for (auto& slot : receive_slots_) {
            cancel_windows_operation(slot.overlapped_);
        }
        for (auto& slot : send_slots_) {
            cancel_windows_operation(slot.overlapped_);
        }
#else
        (void)socket_.cancel(ignored);
#endif
        (void)socket_.close(ignored);
    }

    bool done() const noexcept {
        require_owner_thread_noexcept();
        return stopping_ && !socket_.is_open() && free_receive_.size() == receive_slots_.size() &&
               free_send_.size() == send_slots_.size() && pending_handlers_ == 0 &&
               callbacks_running_ == 0 && handler_storage_retired();
    }

private:
    // Fixed FIFO of slot indices. Its capacity is the slot count and every
    // index sits in at most one ring of a kind, so a push never overflows.
    class index_ring final {
    public:
        explicit index_ring(std::size_t capacity)
            : indices_(capacity, process_resource()) {}

        [[nodiscard]] bool empty() const noexcept {
            return count_ == 0;
        }
        [[nodiscard]] std::size_t size() const noexcept {
            return count_;
        }
        [[nodiscard]] std::uint32_t operator[](std::size_t offset) const noexcept {
            return indices_[(head_ + offset) % indices_.size()];
        }
        void push(std::uint32_t index) noexcept {
            if (count_ == indices_.size()) {
                std::terminate();
            }
            indices_[(head_ + count_) % indices_.size()] = index;
            ++count_;
        }
        [[nodiscard]] std::uint32_t pop() noexcept {
            if (count_ == 0) {
                std::terminate();
            }
            const auto index = indices_[head_];
            head_ = (head_ + 1) % indices_.size();
            --count_;
            return index;
        }
        void drop_back() noexcept {
            if (count_ == 0) {
                std::terminate();
            }
            --count_;
        }

    private:
        std::pmr::vector<std::uint32_t> indices_;
        std::size_t head_{};
        std::size_t count_{};
    };

#ifndef _WIN32
    struct posix_handler_storage final {
        alignas(std::max_align_t) std::array<std::byte, 1024> bytes_{};
        bool allocated_{};
    };

    template <class t_type>
    class posix_handler_allocator {
    public:
        using value_type = t_type;

        explicit posix_handler_allocator(posix_handler_storage* storage) noexcept
            : storage_(storage) {}

        template <class u_type>
        posix_handler_allocator(const posix_handler_allocator<u_type>& other) noexcept
            : storage_(other.storage()) {}

        [[nodiscard]] t_type* allocate(std::size_t count) {
            if (storage_ == nullptr || count != 1 || storage_->allocated_ ||
                sizeof(t_type) > storage_->bytes_.size() ||
                alignof(t_type) > alignof(std::max_align_t)) {
                throw std::bad_alloc();
            }
            storage_->allocated_ = true;
            return reinterpret_cast<t_type*>(storage_->bytes_.data());
        }

        void deallocate(t_type* pointer, std::size_t) noexcept {
            if (storage_ == nullptr || pointer != reinterpret_cast<t_type*>(storage_->bytes_.data()) || !storage_->allocated_) {
                std::terminate();
            }
            storage_->allocated_ = false;
        }

        [[nodiscard]] posix_handler_storage* storage() const noexcept {
            return storage_;
        }

        template <class u_type>
        friend class posix_handler_allocator;

        template <class u_type>
        friend bool operator==(const posix_handler_allocator& left,
            const posix_handler_allocator<u_type>& right) noexcept {
            return left.storage_ == right.storage();
        }

        template <class u_type>
        friend bool operator!=(const posix_handler_allocator& left,
            const posix_handler_allocator<u_type>& right) noexcept {
            return !(left == right);
        }

    private:
        posix_handler_storage* storage_{};
    };

    enum class posix_direction : std::uint8_t { receive,
        send };

    // One readiness wait per direction plus one deferred send flush serve every
    // admitted operation; each wake drains every queued slot it can.
    struct posix_io_handler final {
        using allocator_type = posix_handler_allocator<posix_io_handler>;

        impl* owner_{};
        posix_handler_storage* storage_{};
        posix_direction direction_{};

        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return allocator_type(storage_);
        }

        void operator()() const noexcept {
            if (storage_->allocated_) {
                std::terminate();
            }
            owner_->handle_posted(direction_);
        }

        void operator()(const asio::error_code& error) const noexcept {
            if (storage_->allocated_) {
                std::terminate();
            }
            owner_->handle_wait(direction_, error);
        }
    };

    static_assert(std::is_same_v<asio::associated_allocator_t<posix_io_handler>,
        posix_io_handler::allocator_type>);
#else
    struct handler_storage_type final {
        impl* owner_{};
        alignas(std::max_align_t) std::array<std::byte, 1024> bytes_{};
        bool allocated_{};
    };

    template <class t_type>
    class fixed_handler_allocator_type {
    public:
        using value_type = t_type;

        explicit fixed_handler_allocator_type(handler_storage_type* storage) noexcept
            : storage_(storage) {}

        template <class u_type>
        fixed_handler_allocator_type(const fixed_handler_allocator_type<u_type>& other) noexcept
            : storage_(other.storage()) {}

        [[nodiscard]] t_type* allocate(std::size_t count) {
            if (storage_ == nullptr || count != 1 || storage_->allocated_ ||
                sizeof(t_type) > storage_->bytes_.size() ||
                alignof(t_type) > alignof(std::max_align_t)) {
                throw std::bad_alloc();
            }
            storage_->allocated_ = true;
            ++storage_->owner_->pending_handlers_;
            return reinterpret_cast<t_type*>(storage_->bytes_.data());
        }

        void deallocate(t_type* pointer, std::size_t) noexcept {
            if (storage_ == nullptr || pointer != reinterpret_cast<t_type*>(storage_->bytes_.data()) || !storage_->allocated_) {
                std::terminate();
            }
            storage_->allocated_ = false;
            if (storage_->owner_->pending_handlers_ == 0) {
                std::terminate();
            }
            --storage_->owner_->pending_handlers_;
        }

        [[nodiscard]] handler_storage_type* storage() const noexcept {
            return storage_;
        }

        template <class u_type>
        friend class fixed_handler_allocator_type;

        template <class u_type>
        friend bool operator==(const fixed_handler_allocator_type& left,
            const fixed_handler_allocator_type<u_type>& right) noexcept {
            return left.storage_ == right.storage();
        }

        template <class u_type>
        friend bool operator!=(const fixed_handler_allocator_type& left,
            const fixed_handler_allocator_type<u_type>& right) noexcept {
            return !(left == right);
        }

    private:
        handler_storage_type* storage_{};
    };

    enum class direction_type : std::uint8_t { receive,
        send };

    struct windows_completion_handler_type final {
        using allocator_type = fixed_handler_allocator_type<windows_completion_handler_type>;

        impl* owner_{};
        handler_storage_type* storage_{};
        direction_type direction_{};
        std::uint32_t index_{};

        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return allocator_type(storage_);
        }

        void operator()(const asio::error_code& error, std::size_t size) const noexcept {
            if (storage_->allocated_) {
                std::terminate();
            }
            ++owner_->callbacks_running_;
            if (direction_ == direction_type::receive) {
                owner_->finish_windows_receive(index_, error, size);
            } else {
                owner_->finish_windows_send(index_, error, size);
            }
            --owner_->callbacks_running_;
        }
    };

    static_assert(std::is_same_v<asio::associated_allocator_t<windows_completion_handler_type>,
        windows_completion_handler_type::allocator_type>);
#endif

    struct receive_slot_type final {
        std::span<std::byte> bytes_;
        alignas(std::max_align_t) std::array<std::byte, 256> control_{};
        sockaddr_storage peer_address_{};
        bool active_{};
        void* context_{};
        receive_completion completion_{};
#ifdef _WIN32
        handler_storage_type handler_storage_{};
        OVERLAPPED* overlapped_{};
        WSAMSG message_{};
        WSABUF buffer_{};
#endif
    };

    struct send_slot_type final {
        alignas(std::max_align_t) std::array<std::byte, 256> control_{};
        sockaddr_storage peer_address_{};
        native_socket_length_type peer_address_length_{};
        bool active_{};
        void* context_{};
        send_completion completion_{};
        send_view view_{};
#ifdef _WIN32
        handler_storage_type handler_storage_{};
        OVERLAPPED* overlapped_{};
        WSAMSG message_{};
        WSABUF buffer_{};
#endif
    };

    [[nodiscard]] bool handler_storage_retired() const noexcept {
#ifndef _WIN32
        return !receive_handler_storage_.allocated_ && !send_handler_storage_.allocated_;
#else
        const auto retired = [](const auto& slots) {
            return std::all_of(slots.begin(), slots.end(),
                [](const auto& slot) { return !slot.handler_storage_.allocated_; });
        };
        return retired(receive_slots_) && retired(send_slots_);
#endif
    }

    void require_owner_thread() const {
        if (std::this_thread::get_id() != owner_thread_) {
            throw std::logic_error("HTTP/3 server network UDP socket used outside its owner thread");
        }
    }

    void require_owner_thread_noexcept() const noexcept {
        if (std::this_thread::get_id() != owner_thread_) {
            std::terminate();
        }
    }

    void set_packet_info_option(int level, int option) {
#ifdef _WIN32
        const int enabled = 1;
        if (::setsockopt(socket_.native_handle(), level, option,
                reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == SOCKET_ERROR) {
            throw std::system_error(WSAGetLastError(), std::system_category(),
                "enable HTTP/3 server network UDP pktinfo");
        }
#else
        const int enabled = 1;
        if (::setsockopt(socket_.native_handle(), level, option, &enabled, sizeof(enabled)) != 0) {
            throw std::system_error(errno, std::system_category(),
                "enable HTTP/3 server network UDP pktinfo");
        }
#endif
    }

    // The slot returns to the free ring before its callback, which may re-arm it.
    void complete_receive(std::uint32_t index, std::error_code error, std::size_t size = 0,
        udp_type::endpoint peer = {}, udp_type::endpoint local_destination = {}) noexcept {
        auto& slot = receive_slots_[index];
        if (!slot.active_) {
            std::terminate();
        }
        auto completion = slot.completion_;
        void* const context_value = slot.context_;
        const auto bytes_value = std::exchange(slot.bytes_, {});
        slot.active_ = false;
        slot.completion_ = nullptr;
        slot.context_ = nullptr;
        free_receive_.push(index);
        ++callbacks_running_;
        completion(context_value, error,
            receive_view{std::span<const std::byte>(bytes_value.data(), error ? 0 : size),
                std::move(peer), std::move(local_destination)});
        --callbacks_running_;
    }

    void complete_send(std::uint32_t index, std::error_code error, std::size_t size = 0) noexcept {
        auto& slot = send_slots_[index];
        if (!slot.active_) {
            std::terminate();
        }
        auto completion = slot.completion_;
        void* const context_value = slot.context_;
        slot.active_ = false;
        slot.completion_ = nullptr;
        slot.context_ = nullptr;
        slot.view_ = {};
        free_send_.push(index);
        ++callbacks_running_;
        completion(context_value, error, error ? 0 : size);
        --callbacks_running_;
    }

#ifndef _WIN32
    bool queue_posix_receive(std::uint32_t index) noexcept {
        receive_queue_.push(index);
        if (receive_waiting_ || receive_draining_ || start_receive_wait()) {
            return true;
        }
        receive_queue_.drop_back();
        return false;
    }

    bool queue_posix_send(std::uint32_t index) noexcept {
        send_queue_.push(index);
        if (send_scheduled_ || send_flushing_ || post_send_flush()) {
            return true;
        }
        send_queue_.drop_back();
        return false;
    }

    bool start_receive_wait() noexcept {
        auto* const storage = &receive_handler_storage_;
        ++pending_handlers_;
        try {
            socket_.async_wait(udp_type::socket::wait_read,
                posix_io_handler{this, storage, posix_direction::receive});
            if (!storage->allocated_) {
                std::terminate();
            }
            receive_waiting_ = true;
            return true;
        } catch (...) {
            if (storage->allocated_ || pending_handlers_ == 0) {
                std::terminate();
            }
            --pending_handlers_;
            return false;
        }
    }

    bool start_send_wait() noexcept {
        auto* const storage = &send_handler_storage_;
        ++pending_handlers_;
        try {
            socket_.async_wait(udp_type::socket::wait_write,
                posix_io_handler{this, storage, posix_direction::send});
            if (!storage->allocated_) {
                std::terminate();
            }
            send_scheduled_ = true;
            return true;
        } catch (...) {
            if (storage->allocated_ || pending_handlers_ == 0) {
                std::terminate();
            }
            --pending_handlers_;
            return false;
        }
    }

    // Sends queued before the flush runs leave together; callbacks never run
    // inside async_send.
    bool post_send_flush() noexcept {
        auto* const storage = &send_handler_storage_;
        ++pending_handlers_;
        try {
            asio::post(io_, posix_io_handler{this, storage, posix_direction::send});
            if (!storage->allocated_) {
                std::terminate();
            }
            send_scheduled_ = true;
            return true;
        } catch (...) {
            if (storage->allocated_ || pending_handlers_ == 0) {
                std::terminate();
            }
            --pending_handlers_;
            return false;
        }
    }

    void handle_posted(posix_direction direction) noexcept {
        if (direction != posix_direction::send) {
            std::terminate();
        }
        send_scheduled_ = false;
        if (stopping_) {
            abort_sends();
        } else {
            flush_sends();
        }
        if (pending_handlers_ == 0) {
            std::terminate();
        }
        --pending_handlers_;
    }

    void handle_wait(posix_direction direction, const asio::error_code& error) noexcept {
        if (direction == posix_direction::receive) {
            receive_waiting_ = false;
            if (stopping_) {
                abort_receives();
            } else if (error) {
                fail_receives(error);
            } else {
                drain_receives();
            }
        } else {
            send_scheduled_ = false;
            if (stopping_) {
                abort_sends();
            } else if (error) {
                fail_sends(error);
            } else {
                flush_sends();
            }
        }
        if (pending_handlers_ == 0) {
            std::terminate();
        }
        --pending_handlers_;
    }

    // Stopping rejects re-arming, so these loops terminate.
    void abort_receives() noexcept {
        while (!receive_queue_.empty()) {
            complete_receive(receive_queue_.pop(), aborted());
        }
    }

    void abort_sends() noexcept {
        while (!send_queue_.empty()) {
            complete_send(send_queue_.pop(), aborted());
        }
    }

    void fail_receives(std::error_code error) noexcept {
        receive_draining_ = true;
        for (auto count = receive_queue_.size(); count != 0; --count) {
            complete_receive(receive_queue_.pop(), error);
        }
        finish_receive_round();
    }

    void fail_sends(std::error_code error) noexcept {
        send_flushing_ = true;
        for (auto count = send_queue_.size(); count != 0; --count) {
            complete_send(send_queue_.pop(), error);
        }
        finish_send_round(false);
    }

    // Receives re-armed by callbacks during a round wait for the next readiness.
    void finish_receive_round() noexcept {
        receive_draining_ = false;
        if (stopping_) {
            abort_receives();
            return;
        }
        if (!receive_queue_.empty() && !start_receive_wait()) {
            for (auto count = receive_queue_.size(); count != 0; --count) {
                complete_receive(receive_queue_.pop(), no_memory());
            }
        }
    }

    // Sends admitted by callbacks during a round leave with the next flush.
    void finish_send_round(bool wait_writable) noexcept {
        send_flushing_ = false;
        if (stopping_) {
            abort_sends();
            return;
        }
        if (!send_queue_.empty() && !(wait_writable ? start_send_wait() : post_send_flush())) {
            for (auto count = send_queue_.size(); count != 0; --count) {
                complete_send(send_queue_.pop(), no_memory());
            }
        }
    }

    void prepare_receive_message(receive_slot_type& slot, msghdr& message, iovec& vector) noexcept {
        slot.peer_address_ = {};
        vector.iov_base = slot.bytes_.data();
        vector.iov_len = slot.bytes_.size();
        message = {};
        message.msg_name = &slot.peer_address_;
        message.msg_namelen = static_cast<native_socket_length_type>(sizeof(slot.peer_address_));
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = slot.control_.data();
        message.msg_controllen = slot.control_.size();
    }

    // Metadata is parsed before completion returns the slot to its caller.
    void finish_posix_receive(std::uint32_t index, const msghdr& message, std::size_t size) noexcept {
        udp_type::endpoint peer;
        udp_type::endpoint destination;
        if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
            !parse_native_endpoint(static_cast<const sockaddr*>(message.msg_name),
                message.msg_namelen, ipv6_socket_, peer) ||
            !parse_packet_info(message, destination)) {
            complete_receive(index, bad_message());
            return;
        }
        complete_receive(index, {}, size, std::move(peer), std::move(destination));
    }

    void drain_receives() noexcept {
        receive_draining_ = true;
        auto budget = receive_slots_.size() * batch_rounds_per_slot;
        while (!stopping_ && !receive_queue_.empty() && budget != 0) {
#if defined(__linux__)
            const auto count = std::min({receive_queue_.size(), budget, receive_messages_.size()});
            for (std::size_t offset = 0; offset < count; ++offset) {
                prepare_receive_message(receive_slots_[receive_queue_[offset]],
                    receive_messages_[offset].msg_hdr, receive_vectors_[offset]);
                receive_messages_[offset].msg_len = 0;
            }
            const int received_value = ::recvmmsg(socket_.native_handle(), receive_messages_.data(),
                static_cast<unsigned int>(count), MSG_DONTWAIT, nullptr);
            if (received_value < 0) {
                const int error = errno;
                if (error == EAGAIN || error == EWOULDBLOCK) {
                    break;
                }
                if (error != EINTR) {
                    complete_receive(receive_queue_.pop(), std::error_code(error, std::system_category()));
                }
                --budget;
                continue;
            }
            // Callbacks only append re-armed slots, so the head still matches
            // the prepared messages in order.
            const auto received_count = static_cast<std::size_t>(received_value);
            for (std::size_t offset = 0; offset < received_count; ++offset) {
                finish_posix_receive(receive_queue_.pop(), receive_messages_[offset].msg_hdr,
                    receive_messages_[offset].msg_len);
            }
            budget -= received_count;
            if (received_count < count) {
                break;
            }
#else
            msghdr message{};
            iovec vector{};
            prepare_receive_message(receive_slots_[receive_queue_[0]], message, vector);
            const ssize_t received_value = ::recvmsg(socket_.native_handle(), &message, MSG_DONTWAIT);
            --budget;
            if (received_value < 0) {
                const int error = errno;
                if (error == EAGAIN || error == EWOULDBLOCK) {
                    break;
                }
                if (error != EINTR) {
                    complete_receive(receive_queue_.pop(), std::error_code(error, std::system_category()));
                }
                continue;
            }
            finish_posix_receive(receive_queue_.pop(), message, static_cast<std::size_t>(received_value));
#endif
        }
        finish_receive_round();
    }

    bool parse_packet_info(const msghdr& message, udp_type::endpoint& destination) const noexcept {
        bool found = false;
        const auto* const control_start = static_cast<const std::byte*>(message.msg_control);
        const std::size_t control_size = message.msg_controllen;
        for (const cmsghdr* header_value = CMSG_FIRSTHDR(const_cast<msghdr*>(&message));
            header_value != nullptr; header_value = CMSG_NXTHDR(const_cast<msghdr*>(&message),
                                         const_cast<cmsghdr*>(header_value))) {
            const auto* const header_bytes = reinterpret_cast<const std::byte*>(header_value);
            if (header_bytes < control_start ||
                static_cast<std::size_t>(header_bytes - control_start) > control_size ||
                header_value->cmsg_len < CMSG_LEN(0) ||
                header_value->cmsg_len > control_size -
                                             static_cast<std::size_t>(header_bytes - control_start)) {
                return false;
            }

            if (!ipv6_socket_ && header_value->cmsg_level == IPPROTO_IP &&
                header_value->cmsg_type == IP_PKTINFO) {
                if (found || header_value->cmsg_len < CMSG_LEN(sizeof(in_pktinfo))) {
                    return false;
                }
                in_pktinfo info{};
                std::memcpy(&info, CMSG_DATA(const_cast<cmsghdr*>(header_value)), sizeof(info));
                asio::ip::address_v4::bytes_type bytes_value{};
                // ipi_addr is the packet's destination. ipi_spec_dst is a local
                // source-selection hint and is not correct for received-packet metadata.
                std::memcpy(bytes_value.data(), &info.ipi_addr, bytes_value.size());
                const asio::ip::address address{asio::ip::address_v4(bytes_value)};
                if (!valid_concrete_address(address)) {
                    return false;
                }
                destination = udp_type::endpoint(address, bound_endpoint_.port());
                found = true;
            } else if (ipv6_socket_ && header_value->cmsg_level == IPPROTO_IPV6 &&
                       header_value->cmsg_type == IPV6_PKTINFO) {
                if (found || header_value->cmsg_len < CMSG_LEN(sizeof(in6_pktinfo))) {
                    return false;
                }
                in6_pktinfo info{};
                std::memcpy(&info, CMSG_DATA(const_cast<cmsghdr*>(header_value)), sizeof(info));
                asio::ip::address_v6::bytes_type bytes_value{};
                std::memcpy(bytes_value.data(), &info.ipi6_addr, bytes_value.size());
                // The interface index is deliberately not converted to a scope
                // id; link-local addresses are rejected by valid_concrete_address.
                const asio::ip::address address{asio::ip::address_v6(bytes_value)};
                if (!valid_concrete_address(address)) {
                    return false;
                }
                destination = udp_type::endpoint(address, bound_endpoint_.port());
                found = true;
            }
        }
        return found;
    }

    void prepare_send_message(send_slot_type& slot, msghdr& message, iovec& vector) noexcept {
        const auto peer = make_native_endpoint(slot.view_.peer_);
        slot.peer_address_ = peer.address_;
        slot.peer_address_length_ = peer.length_;
        slot.control_ = {};
        vector.iov_base = slot.view_.bytes_.empty()
                              ? static_cast<void*>(&empty_send_byte_)
                              : const_cast<std::byte*>(slot.view_.bytes_.data());
        vector.iov_len = slot.view_.bytes_.size();
        message = {};
        message.msg_name = &slot.peer_address_;
        message.msg_namelen = slot.peer_address_length_;
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = slot.control_.data();
        // The 256-byte control area always holds one packet-info header.
        if (ipv6_socket_) {
            message.msg_controllen = CMSG_SPACE(sizeof(in6_pktinfo));
            cmsghdr* const header_value = CMSG_FIRSTHDR(&message);
            header_value->cmsg_level = IPPROTO_IPV6;
            header_value->cmsg_type = IPV6_PKTINFO;
            header_value->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
            in6_pktinfo info{};
            const auto bytes_value = slot.view_.source_.address().to_v6().to_bytes();
            std::memcpy(&info.ipi6_addr, bytes_value.data(), bytes_value.size());
            std::memcpy(CMSG_DATA(header_value), &info, sizeof(info));
        } else {
            message.msg_controllen = CMSG_SPACE(sizeof(in_pktinfo));
            cmsghdr* const header_value = CMSG_FIRSTHDR(&message);
            header_value->cmsg_level = IPPROTO_IP;
            header_value->cmsg_type = IP_PKTINFO;
            header_value->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
            in_pktinfo info{};
            const auto bytes_value = slot.view_.source_.address().to_v4().to_bytes();
            std::memcpy(&info.ipi_spec_dst, bytes_value.data(), bytes_value.size());
            std::memcpy(CMSG_DATA(header_value), &info, sizeof(info));
        }
    }

    void flush_sends() noexcept {
        send_flushing_ = true;
        auto budget = send_slots_.size() * batch_rounds_per_slot;
        bool wait_writable = false;
        while (!stopping_ && !send_queue_.empty() && budget != 0) {
#if defined(__linux__)
            const auto count = std::min({send_queue_.size(), budget, send_messages_.size()});
            for (std::size_t offset = 0; offset < count; ++offset) {
                prepare_send_message(send_slots_[send_queue_[offset]], send_messages_[offset].msg_hdr,
                    send_vectors_[offset]);
                send_messages_[offset].msg_len = 0;
            }
            const int sent_value = ::sendmmsg(socket_.native_handle(), send_messages_.data(),
                static_cast<unsigned int>(count), MSG_DONTWAIT);
            if (sent_value < 0) {
                const int error = errno;
                if (error == EAGAIN || error == EWOULDBLOCK) {
                    wait_writable = true;
                    break;
                }
                if (error != EINTR) {
                    complete_send(send_queue_.pop(), std::error_code(error, std::system_category()));
                }
                --budget;
                continue;
            }
            // A short count leaves the failing message at the head; the next
            // call reports its error.
            const auto sent_count = static_cast<std::size_t>(sent_value);
            for (std::size_t offset = 0; offset < sent_count; ++offset) {
                const auto index = send_queue_.pop();
                const std::size_t size = send_messages_[offset].msg_len;
                complete_send(index, size == send_slots_[index].view_.bytes_.size() ? std::error_code{} : io_error(), size);
            }
            budget -= sent_count;
#else
            msghdr message{};
            iovec vector{};
            const auto index = send_queue_[0];
            prepare_send_message(send_slots_[index], message, vector);
            const ssize_t sent_value = ::sendmsg(socket_.native_handle(), &message, MSG_DONTWAIT);
            --budget;
            if (sent_value < 0) {
                const int error = errno;
                if (error == EAGAIN || error == EWOULDBLOCK) {
                    wait_writable = true;
                    break;
                }
                if (error != EINTR) {
                    complete_send(send_queue_.pop(), std::error_code(error, std::system_category()));
                }
                continue;
            }
            const auto size = static_cast<std::size_t>(sent_value);
            (void)send_queue_.pop();
            complete_send(index, size == send_slots_[index].view_.bytes_.size() ? std::error_code{} : io_error(), size);
#endif
        }
        finish_send_round(wait_writable);
    }
#else
    void load_message_extensions() {
        GUID receive_id = WSAID_WSARECVMSG;
        GUID send_id = WSAID_WSASENDMSG;
        DWORD returned = 0;
        if (::WSAIoctl(socket_.native_handle(), SIO_GET_EXTENSION_FUNCTION_POINTER,
                &receive_id, sizeof(receive_id), &receive_message_function_,
                sizeof(receive_message_function_),
                &returned, nullptr, nullptr) == SOCKET_ERROR) {
            throw std::system_error(WSAGetLastError(), std::system_category(),
                "load WSARecvMsg for HTTP/3 server network");
        }
        returned = 0;
        if (::WSAIoctl(socket_.native_handle(), SIO_GET_EXTENSION_FUNCTION_POINTER,
                &send_id, sizeof(send_id), &send_message_function_,
                sizeof(send_message_function_),
                &returned, nullptr, nullptr) == SOCKET_ERROR) {
            throw std::system_error(WSAGetLastError(), std::system_category(),
                "load WSASendMsg for HTTP/3 server network");
        }
    }

    void disable_peer_error_report(DWORD control) {
        BOOL report = FALSE;
        DWORD returned = 0;
        if (::WSAIoctl(socket_.native_handle(), control, &report, sizeof(report),
                nullptr, 0, &returned, nullptr, nullptr) == SOCKET_ERROR) {
            throw std::system_error(WSAGetLastError(), std::system_category(),
                "configure HTTP/3 server network UDP peer errors");
        }
    }

    bool start_windows_receive(std::uint32_t index) noexcept {
        auto& slot = receive_slots_[index];
        handler_storage_type* const storage = &slot.handler_storage_;
        try {
            windows_completion_handler_type handler{this, storage, direction_type::receive, index};
            asio::windows::overlapped_ptr operation(io_.get_executor(), handler);
            if (!storage->allocated_) {
                std::terminate();
            }
            slot.peer_address_ = {};
            slot.buffer_.buf = reinterpret_cast<char*>(slot.bytes_.data());
            slot.buffer_.len = static_cast<ULONG>(slot.bytes_.size());
            slot.message_ = {};
            slot.message_.name = reinterpret_cast<sockaddr*>(&slot.peer_address_);
            slot.message_.namelen = static_cast<INT>(sizeof(slot.peer_address_));
            slot.message_.lpBuffers = &slot.buffer_;
            slot.message_.dwBufferCount = 1;
            slot.message_.Control.buf = reinterpret_cast<char*>(slot.control_.data());
            slot.message_.Control.len = static_cast<ULONG>(slot.control_.size());
            DWORD received_value = 0;
            const int result_value = receive_message_function_(socket_.native_handle(), &slot.message_,
                &received_value, operation.get(), nullptr);
            const int error = result_value == 0 ? 0 : WSAGetLastError();
            if (result_value == 0 || error == WSA_IO_PENDING) {
                slot.overlapped_ = operation.release();
            } else {
                // Like send, a synchronous failure (a truncated datagram or a
                // reported per-peer ICMP error) completes through the callback so
                // its owner can classify packet-local errors.
                operation.complete(asio::error_code(error, std::system_category()), 0);
            }
            return true;
        } catch (...) {
            if (storage->allocated_) {
                std::terminate();
            }
            return false;
        }
    }

    bool start_windows_send(std::uint32_t index) noexcept {
        auto& slot = send_slots_[index];
        handler_storage_type* const storage = &slot.handler_storage_;
        try {
            windows_completion_handler_type handler{this, storage, direction_type::send, index};
            asio::windows::overlapped_ptr operation(io_.get_executor(), handler);
            if (!storage->allocated_) {
                std::terminate();
            }
            const auto peer = make_native_endpoint(slot.view_.peer_);
            slot.peer_address_ = peer.address_;
            slot.peer_address_length_ = peer.length_;
            slot.control_ = {};
            slot.buffer_.buf = slot.view_.bytes_.empty()
                                   ? reinterpret_cast<char*>(&empty_send_byte_)
                                   : const_cast<char*>(reinterpret_cast<const char*>(
                                         slot.view_.bytes_.data()));
            slot.buffer_.len = static_cast<ULONG>(slot.view_.bytes_.size());
            slot.message_ = {};
            slot.message_.name = reinterpret_cast<sockaddr*>(&slot.peer_address_);
            slot.message_.namelen = static_cast<INT>(slot.peer_address_length_);
            slot.message_.lpBuffers = &slot.buffer_;
            slot.message_.dwBufferCount = 1;
            slot.message_.Control.buf = reinterpret_cast<char*>(slot.control_.data());

            WSACMSGHDR* header_value = nullptr;
            if (ipv6_socket_) {
                slot.message_.Control.len = static_cast<ULONG>(WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)));
                header_value = WSA_CMSG_FIRSTHDR(&slot.message_);
                if (header_value == nullptr) {
                    return false;
                }
                header_value->cmsg_level = IPPROTO_IPV6;
                header_value->cmsg_type = IPV6_PKTINFO;
                header_value->cmsg_len = WSA_CMSG_LEN(sizeof(IN6_PKTINFO));
                IN6_PKTINFO info{};
                const auto bytes_value = slot.view_.source_.address().to_v6().to_bytes();
                std::memcpy(&info.ipi6_addr, bytes_value.data(), bytes_value.size());
                std::memcpy(WSA_CMSG_DATA(header_value), &info, sizeof(info));
            } else {
                slot.message_.Control.len = static_cast<ULONG>(WSA_CMSG_SPACE(sizeof(IN_PKTINFO)));
                header_value = WSA_CMSG_FIRSTHDR(&slot.message_);
                if (header_value == nullptr) {
                    return false;
                }
                header_value->cmsg_level = IPPROTO_IP;
                header_value->cmsg_type = IP_PKTINFO;
                header_value->cmsg_len = WSA_CMSG_LEN(sizeof(IN_PKTINFO));
                IN_PKTINFO info{};
                const auto bytes_value = slot.view_.source_.address().to_v4().to_bytes();
                std::memcpy(&info.ipi_addr, bytes_value.data(), bytes_value.size());
                std::memcpy(WSA_CMSG_DATA(header_value), &info, sizeof(info));
            }

            DWORD sent = 0;
            const int result_value = send_message_function_(socket_.native_handle(), &slot.message_,
                0, &sent, operation.get(), nullptr);
            const int error = result_value == 0 ? 0 : WSAGetLastError();
            if (result_value == 0 || error == WSA_IO_PENDING) {
                slot.overlapped_ = operation.release();
            } else {
                // Like POSIX, a synchronous send failure completes through the
                // callback so its owner can classify destination-local errors.
                operation.complete(asio::error_code(error, std::system_category()), 0);
            }
            return true;
        } catch (...) {
            if (storage->allocated_) {
                std::terminate();
            }
            return false;
        }
    }

    bool parse_windows_packet_info(const WSAMSG& message, udp_type::endpoint& destination) const noexcept {
        if ((message.dwFlags & MSG_CTRUNC) != 0) {
            return false;
        }
#ifdef MSG_TRUNC
        if ((message.dwFlags & MSG_TRUNC) != 0) {
            return false;
        }
#endif
        bool found = false;
        const auto* const control_start = reinterpret_cast<const std::byte*>(message.Control.buf);
        const std::size_t control_size = message.Control.len;
        for (WSACMSGHDR* header_value = WSA_CMSG_FIRSTHDR(const_cast<WSAMSG*>(&message));
            header_value != nullptr; header_value = WSA_CMSG_NXTHDR(
                                         const_cast<WSAMSG*>(&message), header_value)) {
            const auto* const header_bytes = reinterpret_cast<const std::byte*>(header_value);
            if (header_bytes < control_start ||
                static_cast<std::size_t>(header_bytes - control_start) > control_size ||
                header_value->cmsg_len < WSA_CMSG_LEN(0) ||
                header_value->cmsg_len > control_size -
                                             static_cast<std::size_t>(header_bytes - control_start)) {
                return false;
            }
            if (!ipv6_socket_ && header_value->cmsg_level == IPPROTO_IP &&
                header_value->cmsg_type == IP_PKTINFO) {
                if (found || header_value->cmsg_len < WSA_CMSG_LEN(sizeof(IN_PKTINFO))) {
                    return false;
                }
                IN_PKTINFO info{};
                std::memcpy(&info, WSA_CMSG_DATA(header_value), sizeof(info));
                asio::ip::address_v4::bytes_type bytes_value{};
                std::memcpy(bytes_value.data(), &info.ipi_addr, bytes_value.size());
                const asio::ip::address address{asio::ip::address_v4(bytes_value)};
                if (!valid_concrete_address(address)) {
                    return false;
                }
                destination = udp_type::endpoint(address, bound_endpoint_.port());
                found = true;
            } else if (ipv6_socket_ && header_value->cmsg_level == IPPROTO_IPV6 &&
                       header_value->cmsg_type == IPV6_PKTINFO) {
                if (found || header_value->cmsg_len < WSA_CMSG_LEN(sizeof(IN6_PKTINFO))) {
                    return false;
                }
                IN6_PKTINFO info{};
                std::memcpy(&info, WSA_CMSG_DATA(header_value), sizeof(info));
                asio::ip::address_v6::bytes_type bytes_value{};
                std::memcpy(bytes_value.data(), &info.ipi6_addr, bytes_value.size());
                const asio::ip::address address{asio::ip::address_v6(bytes_value)};
                if (!valid_concrete_address(address)) {
                    return false;
                }
                destination = udp_type::endpoint(address, bound_endpoint_.port());
                found = true;
            }
        }
        return found;
    }

    void finish_windows_receive(std::uint32_t index, const asio::error_code& error, std::size_t size) noexcept {
        auto& slot = receive_slots_[index];
        slot.overlapped_ = nullptr;
        if (stopping_) {
            complete_receive(index, aborted());
            return;
        }
        if (error) {
            const auto portable = portable_completion_error(error);
            complete_receive(index, portable == asio::error::message_size ? bad_message() : portable);
            return;
        }

        udp_type::endpoint peer;
        udp_type::endpoint destination;
        if (size > slot.bytes_.size() ||
            !parse_native_endpoint(reinterpret_cast<const sockaddr*>(&slot.peer_address_),
                static_cast<native_socket_length_type>(slot.message_.namelen), ipv6_socket_, peer) ||
            !parse_windows_packet_info(slot.message_, destination)) {
            complete_receive(index, bad_message());
            return;
        }
        complete_receive(index, {}, size, std::move(peer), std::move(destination));
    }

    void finish_windows_send(std::uint32_t index, const asio::error_code& error, std::size_t size) noexcept {
        auto& slot = send_slots_[index];
        slot.overlapped_ = nullptr;
        if (stopping_) {
            complete_send(index, aborted());
        } else if (error) {
            complete_send(index, portable_completion_error(error));
        } else if (size != slot.view_.bytes_.size()) {
            complete_send(index, io_error());
        } else {
            complete_send(index, {}, size);
        }
    }

    void cancel_windows_operation(OVERLAPPED* operation) noexcept {
        if (operation == nullptr || !socket_.is_open()) {
            return;
        }
        const HANDLE handle = reinterpret_cast<HANDLE>(
            static_cast<ULONG_PTR>(socket_.native_handle()));
        // ERROR_NOT_FOUND means completion already won. Either way the IOCP
        // completion owns retirement, and closing the socket still cancels the
        // operation; its OVERLAPPED and message storage stay until then.
        (void)::CancelIoEx(handle, operation);
    }
#endif

    asio::io_context& io_;
    udp_type::endpoint bind_endpoint_;
    udp_type::endpoint bound_endpoint_;
    udp_type::socket socket_;
    std::thread::id owner_thread_;
    // Sized once at construction; slot addresses stay stable for native I/O.
    std::pmr::vector<receive_slot_type> receive_slots_;
    std::pmr::vector<send_slot_type> send_slots_;
    index_ring free_receive_;
    index_ring free_send_;
#ifndef _WIN32
    index_ring receive_queue_;
    index_ring send_queue_;
#if defined(__linux__)
    std::pmr::vector<mmsghdr> receive_messages_;
    std::pmr::vector<iovec> receive_vectors_;
    std::pmr::vector<mmsghdr> send_messages_;
    std::pmr::vector<iovec> send_vectors_;
#endif
    posix_handler_storage receive_handler_storage_{};
    posix_handler_storage send_handler_storage_{};
    bool receive_waiting_{};
    bool receive_draining_{};
    // A send flush is posted or a writable wait is armed.
    bool send_scheduled_{};
    bool send_flushing_{};
#else
    LPFN_WSARECVMSG receive_message_function_{};
    LPFN_WSASENDMSG send_message_function_{};
#endif
    std::size_t pending_handlers_{};
    std::size_t callbacks_running_{};
    bool ipv6_socket_{};
    bool prepared_{};
    bool stopping_{};
    std::byte empty_send_byte_{};
};

http3_udp_socket::http3_udp_socket(asio::io_context& network_io,
    asio::ip::udp::endpoint bind_endpoint, operation_limits limits)
    : impl_(make_pmr_object<impl>(process_resource(), network_io, std::move(bind_endpoint), limits)) {}

http3_udp_socket::~http3_udp_socket() = default;

void http3_udp_socket::prepare() {
    impl_->prepare();
}

std::uint16_t http3_udp_socket::bound_port() const noexcept {
    return impl_->bound_port();
}

bool http3_udp_socket::async_receive(std::span<std::byte> bytes_value, void* context_value,
    receive_completion completion) noexcept {
    return impl_->async_receive(bytes_value, context_value, completion);
}

bool http3_udp_socket::async_send(send_view view, void* context_value,
    send_completion completion) noexcept {
    return impl_->async_send(std::move(view), context_value, completion);
}

void http3_udp_socket::request_stop() noexcept {
    impl_->request_stop();
}

bool http3_udp_socket::done() const noexcept {
    return impl_->done();
}

}  // namespace ruvia::detail
