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
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

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

#ifndef _WIN32
std::error_code exception_code() noexcept {
    try {
        throw;
    } catch (const std::system_error& error) {
        return error.code();
    } catch (const std::bad_alloc&) {
        return no_memory();
    } catch (...) {
        return io_error();
    }
}
#endif

}  // namespace

class http3_udp_socket::impl final {
public:
    impl(asio::io_context& network_io, udp_type::endpoint bind_endpoint)
        : io_(network_io),
          bind_endpoint_(std::move(bind_endpoint)),
          socket_(io_),
          owner_thread_(std::this_thread::get_id()) {
        if (!valid_bind_address(bind_endpoint_.address())) {
            throw std::invalid_argument("HTTP/3 server network UDP bind address is unsupported");
        }
        ipv6_socket_ = bind_endpoint_.address().is_v6();
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
            // port-unreachable must not fail the shared receive operation.
            BOOL report_port_unreachable = FALSE;
            DWORD returned_bytes = 0;
            if (::WSAIoctl(socket_.native_handle(), SIO_UDP_CONNRESET,
                    &report_port_unreachable, sizeof(report_port_unreachable),
                    nullptr, 0, &returned_bytes, nullptr, nullptr) == SOCKET_ERROR) {
                throw std::system_error(WSAGetLastError(), std::system_category(),
                    "configure HTTP/3 server network UDP peer errors");
            }
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
        if (!prepared_ || stopping_ || receive_.active_ || completion == nullptr ||
            bytes_value.empty() || bytes_value.size() > http3_udp_socket::datagram_buffer_size) {
            return false;
        }
        receive_.active_ = true;
        receive_.context_ = context_value;
        receive_.completion_ = completion;
        receive_.bytes_ = bytes_value;
#ifdef _WIN32
        const bool accepted = start_windows_receive();
#else
        const bool accepted = post_receive_attempt();
#endif
        if (!accepted) {
            receive_.active_ = false;
            receive_.context_ = nullptr;
            receive_.completion_ = nullptr;
            receive_.bytes_ = {};
            return false;
        }
        return true;
    }

    bool async_send(send_view view, void* context_value, send_completion completion) noexcept {
        require_owner_thread_noexcept();
        const std::size_t max_payload = ipv6_socket_ ? 65527U : 65507U;
        if (!prepared_ || stopping_ || send_.active_ || completion == nullptr ||
            !valid_source(view.source_, ipv6_socket_, bound_endpoint_) ||
            !valid_peer(view.peer_, ipv6_socket_) || view.bytes_.size() > max_payload) {
            return false;
        }
        send_.active_ = true;
        send_.context_ = context_value;
        send_.completion_ = completion;
        send_.view_ = view;
#ifdef _WIN32
        const bool accepted = start_windows_send();
#else
        const bool accepted = post_send_attempt();
#endif
        if (!accepted) {
            send_.active_ = false;
            send_.context_ = nullptr;
            send_.completion_ = nullptr;
            send_.view_ = {};
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
        cancel_windows_operation(receive_overlapped_);
        cancel_windows_operation(send_overlapped_);
#else
        (void)socket_.cancel(ignored);
#endif
        (void)socket_.close(ignored);
    }

    bool done() const noexcept {
        require_owner_thread_noexcept();
        return stopping_ && !socket_.is_open() && !receive_.active_ && !send_.active_ &&
               pending_handlers_ == 0 && callbacks_running_ == 0 && handler_storage_retired();
    }

private:
    [[nodiscard]] bool handler_storage_retired() const noexcept {
        const auto retired = [](const auto& storages) {
            return std::all_of(storages.begin(), storages.end(),
                [](const auto& storage) { return !storage.allocated_; });
        };
#ifndef _WIN32
        return retired(receive_posix_handler_storage_) && retired(send_posix_handler_storage_);
#else
        return retired(receive_handler_storage_) && retired(send_handler_storage_);
#endif
    }
    struct receive_slot_type final {
        std::span<std::byte> bytes_;
        alignas(std::max_align_t) std::array<std::byte, 256> control_{};
        sockaddr_storage peer_address_{};
        native_socket_length_type peer_address_length_{};
        bool active_{};
        void* context_{};
        receive_completion completion_{};
    };

    struct send_slot_type final {
        alignas(std::max_align_t) std::array<std::byte, 256> control_{};
        sockaddr_storage peer_address_{};
        native_socket_length_type peer_address_length_{};
        bool active_{};
        void* context_{};
        send_completion completion_{};
        send_view view_{};
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

    posix_handler_storage* acquire_posix_handler_storage(posix_direction direction) noexcept {
        auto& storages = direction == posix_direction::receive
                             ? receive_posix_handler_storage_
                             : send_posix_handler_storage_;
        for (auto& storage : storages) {
            if (!storage.allocated_) {
                return &storage;
            }
        }
        return nullptr;
    }
#endif

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

    void complete_receive(std::error_code error, std::size_t size = 0,
        udp_type::endpoint peer = {}, udp_type::endpoint local_destination = {}) noexcept {
        if (!receive_.active_) {
            std::terminate();
        }
        auto completion = receive_.completion_;
        void* const context_value = receive_.context_;
        const auto bytes_value = std::exchange(receive_.bytes_, {});
        receive_.active_ = false;
        receive_.completion_ = nullptr;
        receive_.context_ = nullptr;
        ++callbacks_running_;
        completion(context_value, error,
            receive_view{std::span<const std::byte>(bytes_value.data(), error ? 0 : size),
                std::move(peer), std::move(local_destination)});
        --callbacks_running_;
    }

    void complete_send(std::error_code error, std::size_t size = 0) noexcept {
        if (!send_.active_) {
            std::terminate();
        }
        auto completion = send_.completion_;
        void* const context_value = send_.context_;
        send_.active_ = false;
        send_.completion_ = nullptr;
        send_.context_ = nullptr;
        send_.view_ = {};
        ++callbacks_running_;
        completion(context_value, error, error ? 0 : size);
        --callbacks_running_;
    }

#ifndef _WIN32
    bool post_receive_attempt() noexcept {
        auto* const storage = acquire_posix_handler_storage(posix_direction::receive);
        if (storage == nullptr) {
            return false;
        }
        ++pending_handlers_;
        try {
            asio::post(io_, posix_io_handler{this, storage, posix_direction::receive});
            if (!storage->allocated_) {
                std::terminate();
            }
            return true;
        } catch (...) {
            if (storage->allocated_ || pending_handlers_ == 0) {
                std::terminate();
            }
            --pending_handlers_;
            return false;
        }
    }

    bool post_send_attempt() noexcept {
        auto* const storage = acquire_posix_handler_storage(posix_direction::send);
        if (storage == nullptr) {
            return false;
        }
        ++pending_handlers_;
        try {
            asio::post(io_, posix_io_handler{this, storage, posix_direction::send});
            if (!storage->allocated_) {
                std::terminate();
            }
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
        if (stopping_) {
            direction == posix_direction::receive ? complete_receive(aborted())
                                                  : complete_send(aborted());
        } else if (direction == posix_direction::receive) {
            attempt_receive();
        } else {
            attempt_send();
        }
        if (pending_handlers_ == 0) {
            std::terminate();
        }
        --pending_handlers_;
    }

    void handle_wait(posix_direction direction, const asio::error_code& error) noexcept {
        if (stopping_) {
            direction == posix_direction::receive ? complete_receive(aborted())
                                                  : complete_send(aborted());
        } else if (error) {
            direction == posix_direction::receive ? complete_receive(error)
                                                  : complete_send(error);
        } else if (direction == posix_direction::receive) {
            attempt_receive();
        } else {
            attempt_send();
        }
        if (pending_handlers_ == 0) {
            std::terminate();
        }
        --pending_handlers_;
    }

    void attempt_receive() noexcept {
        std::fill(receive_.control_.begin(), receive_.control_.end(), std::byte{});
        receive_.peer_address_ = {};
        receive_.peer_address_length_ =
            static_cast<native_socket_length_type>(sizeof(receive_.peer_address_));

        iovec input{};
        input.iov_base = receive_.bytes_.data();
        input.iov_len = receive_.bytes_.size();
        msghdr message{};
        message.msg_name = &receive_.peer_address_;
        message.msg_namelen = receive_.peer_address_length_;
        message.msg_iov = &input;
        message.msg_iovlen = 1;
        message.msg_control = receive_.control_.data();
        message.msg_controllen = receive_.control_.size();

        const ssize_t received_value = ::recvmsg(socket_.native_handle(), &message, MSG_DONTWAIT);
        if (received_value < 0) {
            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK) {
                wait_readable();
            } else {
                complete_receive(std::error_code(error, std::system_category()));
            }
            return;
        }

        udp_type::endpoint peer;
        udp_type::endpoint destination;
        if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
            !parse_native_endpoint(reinterpret_cast<const sockaddr*>(&receive_.peer_address_),
                message.msg_namelen, ipv6_socket_, peer) ||
            !parse_packet_info(message, destination)) {
            complete_receive(bad_message());
            return;
        }
        complete_receive({}, static_cast<std::size_t>(received_value), std::move(peer),
            std::move(destination));
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

    void attempt_send() noexcept {
        const auto peer = make_native_endpoint(send_.view_.peer_);
        send_.peer_address_ = peer.address_;
        send_.peer_address_length_ = peer.length_;

        std::fill(send_.control_.begin(), send_.control_.end(), std::byte{});
        iovec output{};
        std::byte empty_payload{};
        output.iov_base = send_.view_.bytes_.empty()
                              ? static_cast<void*>(&empty_payload)
                              : const_cast<std::byte*>(send_.view_.bytes_.data());
        output.iov_len = send_.view_.bytes_.size();
        msghdr message{};
        message.msg_name = &send_.peer_address_;
        message.msg_namelen = send_.peer_address_length_;
        message.msg_iov = &output;
        message.msg_iovlen = 1;
        message.msg_control = send_.control_.data();
        if (ipv6_socket_) {
            message.msg_controllen = CMSG_SPACE(sizeof(in6_pktinfo));
            cmsghdr* header_value = CMSG_FIRSTHDR(&message);
            if (header_value == nullptr) {
                complete_send(io_error());
                return;
            }
            header_value->cmsg_level = IPPROTO_IPV6;
            header_value->cmsg_type = IPV6_PKTINFO;
            header_value->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
            in6_pktinfo info{};
            const auto bytes_value = send_.view_.source_.address().to_v6().to_bytes();
            std::memcpy(&info.ipi6_addr, bytes_value.data(), bytes_value.size());
            std::memcpy(CMSG_DATA(header_value), &info, sizeof(info));
        } else {
            message.msg_controllen = CMSG_SPACE(sizeof(in_pktinfo));
            cmsghdr* header_value = CMSG_FIRSTHDR(&message);
            if (header_value == nullptr) {
                complete_send(io_error());
                return;
            }
            header_value->cmsg_level = IPPROTO_IP;
            header_value->cmsg_type = IP_PKTINFO;
            header_value->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
            in_pktinfo info{};
            const auto bytes_value = send_.view_.source_.address().to_v4().to_bytes();
            std::memcpy(&info.ipi_spec_dst, bytes_value.data(), bytes_value.size());
            std::memcpy(CMSG_DATA(header_value), &info, sizeof(info));
        }

        const ssize_t sent = ::sendmsg(socket_.native_handle(), &message, MSG_DONTWAIT);
        if (sent < 0) {
            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK) {
                wait_writable();
            } else {
                complete_send(std::error_code(error, std::system_category()));
            }
            return;
        }
        const auto size = static_cast<std::size_t>(sent);
        if (size != send_.view_.bytes_.size()) {
            complete_send(io_error());
            return;
        }
        complete_send({}, size);
    }

    void wait_readable() noexcept {
        auto* const storage = acquire_posix_handler_storage(posix_direction::receive);
        if (storage == nullptr) {
            complete_receive(no_memory());
            return;
        }
        ++pending_handlers_;
        try {
            socket_.async_wait(udp_type::socket::wait_read,
                posix_io_handler{this, storage, posix_direction::receive});
            if (!storage->allocated_) {
                std::terminate();
            }
        } catch (...) {
            if (storage->allocated_ || pending_handlers_ == 0) {
                std::terminate();
            }
            --pending_handlers_;
            complete_receive(exception_code());
        }
    }

    void wait_writable() noexcept {
        auto* const storage = acquire_posix_handler_storage(posix_direction::send);
        if (storage == nullptr) {
            complete_send(no_memory());
            return;
        }
        ++pending_handlers_;
        try {
            socket_.async_wait(udp_type::socket::wait_write,
                posix_io_handler{this, storage, posix_direction::send});
            if (!storage->allocated_) {
                std::terminate();
            }
        } catch (...) {
            if (storage->allocated_ || pending_handlers_ == 0) {
                std::terminate();
            }
            --pending_handlers_;
            complete_send(exception_code());
        }
    }
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

        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return allocator_type(storage_);
        }

        void operator()(const asio::error_code& error, std::size_t size) const noexcept {
            if (storage_->allocated_) {
                std::terminate();
            }
            ++owner_->callbacks_running_;
            if (direction_ == direction_type::receive) {
                owner_->finish_windows_receive(error, size);
            } else {
                owner_->finish_windows_send(error, size);
            }
            --owner_->callbacks_running_;
        }
    };

    static_assert(std::is_same_v<asio::associated_allocator_t<windows_completion_handler_type>,
        windows_completion_handler_type::allocator_type>);

    handler_storage_type* acquire_handler_storage(direction_type direction) noexcept {
        auto& storages = direction == direction_type::receive
                             ? receive_handler_storage_
                             : send_handler_storage_;
        for (auto& storage : storages) {
            if (!storage.allocated_) {
                storage.owner_ = this;
                return &storage;
            }
        }
        return nullptr;
    }

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

    bool start_windows_receive() noexcept {
        handler_storage_type* const storage = acquire_handler_storage(direction_type::receive);
        if (storage == nullptr) {
            return false;
        }
        bool submitted = false;
        try {
            windows_completion_handler_type handler{this, storage, direction_type::receive};
            asio::windows::overlapped_ptr operation(io_.get_executor(), handler);
            if (!storage->allocated_) {
                std::terminate();
            }
            receive_.peer_address_ = {};
            receive_.control_ = {};
            receive_buffer_descriptor_.buf = reinterpret_cast<char*>(receive_.bytes_.data());
            receive_buffer_descriptor_.len = static_cast<ULONG>(receive_.bytes_.size());
            receive_message_ = {};
            receive_message_.name = reinterpret_cast<sockaddr*>(&receive_.peer_address_);
            receive_message_.namelen = static_cast<INT>(sizeof(receive_.peer_address_));
            receive_message_.lpBuffers = &receive_buffer_descriptor_;
            receive_message_.dwBufferCount = 1;
            receive_message_.Control.buf = reinterpret_cast<char*>(receive_.control_.data());
            receive_message_.Control.len = static_cast<ULONG>(receive_.control_.size());
            DWORD received_value = 0;
            const int result_value = receive_message_function_(socket_.native_handle(), &receive_message_,
                &received_value, operation.get(), nullptr);
            if (result_value == 0) {
                receive_overlapped_ = operation.release();
                submitted = true;
            } else {
                const int error = WSAGetLastError();
                if (error == WSA_IO_PENDING) {
                    receive_overlapped_ = operation.release();
                    submitted = true;
                } else if (error == WSAEMSGSIZE) {
                    // Treat a synchronously truncated datagram like an IOCP packet-local
                    // error, keeping the normal overlapped callback/slot lifetime.
                    operation.complete(asio::error_code(error, std::system_category()), 0);
                    submitted = true;
                }
            }
        } catch (...) {
            if (storage->allocated_) {
                std::terminate();
            }
            return false;
        }
        if (!submitted && storage->allocated_) {
            std::terminate();
        }
        return submitted;
    }

    bool start_windows_send() noexcept {
        handler_storage_type* const storage = acquire_handler_storage(direction_type::send);
        if (storage == nullptr) {
            return false;
        }
        bool submitted = false;
        try {
            windows_completion_handler_type handler{this, storage, direction_type::send};
            asio::windows::overlapped_ptr operation(io_.get_executor(), handler);
            if (!storage->allocated_) {
                std::terminate();
            }
            const auto peer = make_native_endpoint(send_.view_.peer_);
            send_.peer_address_ = peer.address_;
            send_.peer_address_length_ = peer.length_;
            send_.control_ = {};
            send_buffer_descriptor_.buf = send_.view_.bytes_.empty()
                                              ? reinterpret_cast<char*>(&empty_send_byte_)
                                              : const_cast<char*>(reinterpret_cast<const char*>(
                                                    send_.view_.bytes_.data()));
            send_buffer_descriptor_.len = static_cast<ULONG>(send_.view_.bytes_.size());
            send_message_ = {};
            send_message_.name = reinterpret_cast<sockaddr*>(&send_.peer_address_);
            send_message_.namelen = static_cast<INT>(send_.peer_address_length_);
            send_message_.lpBuffers = &send_buffer_descriptor_;
            send_message_.dwBufferCount = 1;
            send_message_.Control.buf = reinterpret_cast<char*>(send_.control_.data());

            WSACMSGHDR* header_value = nullptr;
            if (ipv6_socket_) {
                send_message_.Control.len = static_cast<ULONG>(WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)));
                header_value = WSA_CMSG_FIRSTHDR(&send_message_);
                if (header_value == nullptr) {
                    return false;
                }
                header_value->cmsg_level = IPPROTO_IPV6;
                header_value->cmsg_type = IPV6_PKTINFO;
                header_value->cmsg_len = WSA_CMSG_LEN(sizeof(IN6_PKTINFO));
                IN6_PKTINFO info{};
                const auto bytes_value = send_.view_.source_.address().to_v6().to_bytes();
                std::memcpy(&info.ipi6_addr, bytes_value.data(), bytes_value.size());
                std::memcpy(WSA_CMSG_DATA(header_value), &info, sizeof(info));
            } else {
                send_message_.Control.len = static_cast<ULONG>(WSA_CMSG_SPACE(sizeof(IN_PKTINFO)));
                header_value = WSA_CMSG_FIRSTHDR(&send_message_);
                if (header_value == nullptr) {
                    return false;
                }
                header_value->cmsg_level = IPPROTO_IP;
                header_value->cmsg_type = IP_PKTINFO;
                header_value->cmsg_len = WSA_CMSG_LEN(sizeof(IN_PKTINFO));
                IN_PKTINFO info{};
                const auto bytes_value = send_.view_.source_.address().to_v4().to_bytes();
                std::memcpy(&info.ipi_addr, bytes_value.data(), bytes_value.size());
                std::memcpy(WSA_CMSG_DATA(header_value), &info, sizeof(info));
            }

            DWORD sent = 0;
            const int result_value = send_message_function_(socket_.native_handle(), &send_message_,
                0, &sent, operation.get(), nullptr);
            const bool accepted = result_value == 0 ||
                                  (result_value == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING);
            if (accepted) {
                send_overlapped_ = operation.release();
                submitted = true;
            }
        } catch (...) {
            if (storage->allocated_) {
                std::terminate();
            }
            return false;
        }
        if (!submitted && storage->allocated_) {
            std::terminate();
        }
        return submitted;
    }

    bool parse_windows_packet_info(udp_type::endpoint& destination) const noexcept {
        if ((receive_message_.dwFlags & MSG_CTRUNC) != 0) {
            return false;
        }
#ifdef MSG_TRUNC
        if ((receive_message_.dwFlags & MSG_TRUNC) != 0) {
            return false;
        }
#endif
        bool found = false;
        const auto* const control_start = reinterpret_cast<const std::byte*>(
            receive_message_.Control.buf);
        const std::size_t control_size = receive_message_.Control.len;
        for (WSACMSGHDR* header_value = WSA_CMSG_FIRSTHDR(
                 const_cast<WSAMSG*>(&receive_message_));
            header_value != nullptr; header_value = WSA_CMSG_NXTHDR(
                                         const_cast<WSAMSG*>(&receive_message_), header_value)) {
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

    void finish_windows_receive(const asio::error_code& error, std::size_t size) noexcept {
        receive_overlapped_ = nullptr;
        if (stopping_) {
            complete_receive(aborted());
            return;
        }
        if (error) {
            if (error.value() == WSAEMSGSIZE) {
                complete_receive(bad_message());
            } else {
                complete_receive(error);
            }
            return;
        }

        udp_type::endpoint peer;
        udp_type::endpoint destination;
        if (size > receive_.bytes_.size() ||
            !parse_native_endpoint(reinterpret_cast<const sockaddr*>(&receive_.peer_address_),
                static_cast<native_socket_length_type>(receive_message_.namelen), ipv6_socket_, peer) ||
            !parse_windows_packet_info(destination)) {
            complete_receive(bad_message());
            return;
        }
        complete_receive({}, size, std::move(peer), std::move(destination));
    }

    void finish_windows_send(const asio::error_code& error, std::size_t size) noexcept {
        send_overlapped_ = nullptr;
        if (stopping_) {
            complete_send(aborted());
        } else if (error) {
            complete_send(error);
        } else if (size != send_.view_.bytes_.size()) {
            complete_send(io_error());
        } else {
            complete_send({}, size);
        }
    }

    void cancel_windows_operation(OVERLAPPED* operation) noexcept {
        if (operation == nullptr || !socket_.is_open()) {
            return;
        }
        const HANDLE handle = reinterpret_cast<HANDLE>(
            static_cast<ULONG_PTR>(socket_.native_handle()));
        if (!::CancelIoEx(handle, operation)) {
            const DWORD error = ::GetLastError();
            // ERROR_NOT_FOUND means completion already won. In either case the
            // IOCP completion owns retirement; never synthesize a second result.
            if (error != ERROR_NOT_FOUND) {
                // Closing the socket below still cancels the operation. Keep the
                // exact OVERLAPPED and all message storage until its completion.
            }
        }
    }
#endif

    asio::io_context& io_;
    udp_type::endpoint bind_endpoint_;
    udp_type::endpoint bound_endpoint_;
    udp_type::socket socket_;
    std::thread::id owner_thread_;
    receive_slot_type receive_;
    send_slot_type send_;
    std::size_t pending_handlers_{};
    std::size_t callbacks_running_{};
    bool ipv6_socket_{};
    bool prepared_{};
    bool stopping_{};
#ifndef _WIN32
    std::array<posix_handler_storage, 2> receive_posix_handler_storage_{};
    std::array<posix_handler_storage, 2> send_posix_handler_storage_{};
#else
    LPFN_WSARECVMSG receive_message_function_{};
    LPFN_WSASENDMSG send_message_function_{};
    OVERLAPPED* receive_overlapped_{};
    OVERLAPPED* send_overlapped_{};
    std::array<handler_storage_type, 2> receive_handler_storage_{};
    std::array<handler_storage_type, 2> send_handler_storage_{};
    WSAMSG receive_message_{};
    WSAMSG send_message_{};
    WSABUF receive_buffer_descriptor_{};
    WSABUF send_buffer_descriptor_{};
    std::byte empty_send_byte_{};
#endif
};

http3_udp_socket::http3_udp_socket(asio::io_context& network_io,
    asio::ip::udp::endpoint bind_endpoint)
    : impl_(make_pmr_object<impl>(process_resource(), network_io, std::move(bind_endpoint))) {}

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
