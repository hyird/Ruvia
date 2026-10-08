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

#include "http3/Http3UdpSocket.h"

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

#include "ruvia/core/memory/ProcessResource.h"
#ifdef _WIN32
#include <asio/windows/overlapped_ptr.hpp>
#endif

namespace ruvia::detail {
namespace {
using Udp = asio::ip::udp;
#ifdef _WIN32
using NativeSocketLength = int;
#else
using NativeSocketLength = socklen_t;
#endif

std::error_code badMessage() noexcept {
    return std::make_error_code(std::errc::bad_message);
}

std::error_code ioError() noexcept {
    return std::make_error_code(std::errc::io_error);
}

#ifndef _WIN32
std::error_code noMemory() noexcept {
    return std::make_error_code(std::errc::not_enough_memory);
}
#endif

std::error_code aborted() noexcept {
    return asio::error::make_error_code(asio::error::operation_aborted);
}

bool wildcardAddress(const asio::ip::address& address) noexcept {
    return address.is_unspecified();
}

bool validConcreteAddress(const asio::ip::address& address) noexcept {
    if (address.is_v4()) {
        const auto ipv4 = address.to_v4();
        const auto bytes = ipv4.to_bytes();
        return !ipv4.is_unspecified() && !ipv4.is_multicast() &&
               !(bytes[0] == 255 && bytes[1] == 255 && bytes[2] == 255 && bytes[3] == 255);
    }
    if (!address.is_v6()) {
        return false;
    }
    const auto ipv6 = address.to_v6();
    return !ipv6.is_unspecified() && !ipv6.is_multicast() && !ipv6.is_v4_mapped() &&
           !ipv6.is_link_local() && ipv6.scope_id() == 0;
}

bool validBindAddress(const asio::ip::address& address) noexcept {
    if (address.is_v4()) {
        const auto ipv4 = address.to_v4();
        const auto bytes = ipv4.to_bytes();
        return !ipv4.is_multicast() &&
               !(bytes[0] == 255 && bytes[1] == 255 && bytes[2] == 255 && bytes[3] == 255);
    }
    if (address.is_v6()) {
        const auto ipv6 = address.to_v6();
        return !ipv6.is_multicast() && !ipv6.is_v4_mapped() && !ipv6.is_link_local() &&
               ipv6.scope_id() == 0;
    }
    return false;
}

bool sameIp(const asio::ip::address& left, const asio::ip::address& right) noexcept {
    return left == right;
}

bool validPeer(const Udp::endpoint& endpoint, bool ipv6Socket) noexcept {
    return endpoint.port() != 0 && validConcreteAddress(endpoint.address()) &&
           endpoint.address().is_v6() == ipv6Socket;
}

bool validSource(const Udp::endpoint& endpoint, bool ipv6Socket,
    const Udp::endpoint& bound) noexcept {
    if (endpoint.port() == 0 || endpoint.port() != bound.port() ||
        !validConcreteAddress(endpoint.address()) || endpoint.address().is_v6() != ipv6Socket) {
        return false;
    }
    return wildcardAddress(bound.address()) || sameIp(endpoint.address(), bound.address());
}

struct NativeEndpoint final {
    sockaddr_storage address{};
    NativeSocketLength length{};
};

NativeEndpoint makeNativeEndpoint(const Udp::endpoint& endpoint) noexcept {
    NativeEndpoint result;
    if (endpoint.address().is_v4()) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(endpoint.port());
        const auto bytes = endpoint.address().to_v4().to_bytes();
        std::memcpy(&address.sin_addr, bytes.data(), bytes.size());
        std::memcpy(&result.address, &address, sizeof(address));
        result.length = static_cast<NativeSocketLength>(sizeof(address));
    } else {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(endpoint.port());
        const auto bytes = endpoint.address().to_v6().to_bytes();
        std::memcpy(&address.sin6_addr, bytes.data(), bytes.size());
        address.sin6_scope_id = endpoint.address().to_v6().scope_id();
        std::memcpy(&result.address, &address, sizeof(address));
        result.length = static_cast<NativeSocketLength>(sizeof(address));
    }
    return result;
}

bool parseNativeEndpoint(const sockaddr* address, NativeSocketLength length, bool ipv6Socket,
    Udp::endpoint& result) noexcept {
    if (address == nullptr) {
        return false;
    }
    if (!ipv6Socket && address->sa_family == AF_INET &&
        length >= static_cast<NativeSocketLength>(sizeof(sockaddr_in))) {
        const auto& ipv4 = *reinterpret_cast<const sockaddr_in*>(address);
        asio::ip::address_v4::bytes_type bytes{};
        std::memcpy(bytes.data(), &ipv4.sin_addr, bytes.size());
        result = Udp::endpoint(asio::ip::address_v4(bytes), ntohs(ipv4.sin_port));
        return validPeer(result, false);
    }
    if (ipv6Socket && address->sa_family == AF_INET6 &&
        length >= static_cast<NativeSocketLength>(sizeof(sockaddr_in6))) {
        const auto& ipv6 = *reinterpret_cast<const sockaddr_in6*>(address);
        asio::ip::address_v6::bytes_type bytes{};
        std::memcpy(bytes.data(), &ipv6.sin6_addr, bytes.size());
        result = Udp::endpoint(asio::ip::address_v6(bytes, ipv6.sin6_scope_id),
            ntohs(ipv6.sin6_port));
        return validPeer(result, true);
    }
    return false;
}

#ifndef _WIN32
std::error_code exceptionCode() noexcept {
    try {
        throw;
    } catch (const std::system_error& error) {
        return error.code();
    } catch (const std::bad_alloc&) {
        return noMemory();
    } catch (...) {
        return ioError();
    }
}
#endif

}  // namespace

class http3_udp_socket::impl final {
public:
    impl(asio::io_context& networkIo, Udp::endpoint bindEndpoint)
        : io_(networkIo),
          bindEndpoint_(std::move(bindEndpoint)),
          socket_(io_),
          ownerThread_(std::this_thread::get_id()) {
        if (!validBindAddress(bindEndpoint_.address())) {
            throw std::invalid_argument("HTTP/3 server network UDP bind address is unsupported");
        }
        ipv6Socket_ = bindEndpoint_.address().is_v6();
    }

    ~impl() {
        requireOwnerThreadNoexcept();
        if (!stopping_) {
            request_stop();
        }
        if (!done()) {
            std::terminate();
        }
    }

    void prepare() {
        requireOwnerThread();
        if (prepared_ || stopping_) {
            throw std::logic_error("HTTP/3 server network UDP socket cannot be prepared in this state");
        }

        asio::error_code error;
        socket_.open(ipv6Socket_ ? Udp::v6() : Udp::v4(), error);
        if (error) {
            throw std::system_error(error, "open HTTP/3 server network UDP socket");
        }

        try {
            if (ipv6Socket_) {
                socket_.set_option(asio::ip::v6_only(true), error);
                if (error) {
                    throw std::system_error(error, "set HTTP/3 server network IPv6-only mode");
                }
                setPacketInfoOption(IPPROTO_IPV6,
#ifdef _WIN32
                    IPV6_PKTINFO
#else
                    IPV6_RECVPKTINFO
#endif
                );
            } else {
                setPacketInfoOption(IPPROTO_IP, IP_PKTINFO);
            }
#ifdef _WIN32
            loadMessageExtensions();
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
            socket_.bind(bindEndpoint_, error);
            if (error) {
                throw std::system_error(error, "bind HTTP/3 server network UDP socket");
            }
            boundEndpoint_ = socket_.local_endpoint(error);
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
        return prepared_ ? boundEndpoint_.port() : 0;
    }

    bool async_receive(std::span<std::byte> bytes, void* context, receive_completion completion) noexcept {
        requireOwnerThreadNoexcept();
        if (!prepared_ || stopping_ || receive_.active || completion == nullptr ||
            bytes.empty() || bytes.size() > http3_udp_socket::datagram_buffer_size) {
            return false;
        }
        receive_.active = true;
        receive_.context = context;
        receive_.completion = completion;
        receive_.bytes = bytes;
#ifdef _WIN32
        const bool accepted = startWindowsReceive();
#else
        const bool accepted = postReceiveAttempt();
#endif
        if (!accepted) {
            receive_.active = false;
            receive_.context = nullptr;
            receive_.completion = nullptr;
            receive_.bytes = {};
            return false;
        }
        return true;
    }

    bool async_send(send_view view, void* context, send_completion completion) noexcept {
        requireOwnerThreadNoexcept();
        const std::size_t maxPayload = ipv6Socket_ ? 65527U : 65507U;
        if (!prepared_ || stopping_ || send_.active || completion == nullptr ||
            !validSource(view.source, ipv6Socket_, boundEndpoint_) ||
            !validPeer(view.peer, ipv6Socket_) || view.bytes.size() > maxPayload) {
            return false;
        }
        send_.active = true;
        send_.context = context;
        send_.completion = completion;
        send_.view = view;
#ifdef _WIN32
        const bool accepted = startWindowsSend();
#else
        const bool accepted = postSendAttempt();
#endif
        if (!accepted) {
            send_.active = false;
            send_.context = nullptr;
            send_.completion = nullptr;
            send_.view = {};
            return false;
        }
        return true;
    }

    void request_stop() noexcept {
        requireOwnerThreadNoexcept();
        if (stopping_) {
            return;
        }
        stopping_ = true;
        asio::error_code ignored;
#ifdef _WIN32
        cancelWindowsOperation(receiveOverlapped_);
        cancelWindowsOperation(sendOverlapped_);
#else
        (void)socket_.cancel(ignored);
#endif
        (void)socket_.close(ignored);
    }

    bool done() const noexcept {
        requireOwnerThreadNoexcept();
        return stopping_ && !socket_.is_open() && !receive_.active && !send_.active &&
               pendingHandlers_ == 0 && callbacksRunning_ == 0 && handlerStorageRetired();
    }

private:
    [[nodiscard]] bool handlerStorageRetired() const noexcept {
        const auto retired = [](const auto& storages) {
            return std::all_of(storages.begin(), storages.end(),
                [](const auto& storage) { return !storage.allocated; });
        };
#ifndef _WIN32
        return retired(receivePosixHandlerStorage_) && retired(sendPosixHandlerStorage_);
#else
        return retired(receiveHandlerStorage_) && retired(sendHandlerStorage_);
#endif
    }
    struct ReceiveSlot final {
        std::span<std::byte> bytes;
        alignas(std::max_align_t) std::array<std::byte, 256> control{};
        sockaddr_storage peerAddress{};
        NativeSocketLength peerAddressLength{};
        bool active{};
        void* context{};
        receive_completion completion{};
    };

    struct SendSlot final {
        alignas(std::max_align_t) std::array<std::byte, 256> control{};
        sockaddr_storage peerAddress{};
        NativeSocketLength peerAddressLength{};
        bool active{};
        void* context{};
        send_completion completion{};
        send_view view{};
    };

#ifndef _WIN32
    struct PosixHandlerStorage final {
        alignas(std::max_align_t) std::array<std::byte, 1024> bytes{};
        bool allocated{};
    };

    template <class T>
    class PosixHandlerAllocator {
    public:
        using value_type = T;

        explicit PosixHandlerAllocator(PosixHandlerStorage* storage) noexcept
            : storage_(storage) {}

        template <class U>
        PosixHandlerAllocator(const PosixHandlerAllocator<U>& other) noexcept
            : storage_(other.storage()) {}

        [[nodiscard]] T* allocate(std::size_t count) {
            if (storage_ == nullptr || count != 1 || storage_->allocated ||
                sizeof(T) > storage_->bytes.size() ||
                alignof(T) > alignof(std::max_align_t)) {
                throw std::bad_alloc();
            }
            storage_->allocated = true;
            return reinterpret_cast<T*>(storage_->bytes.data());
        }

        void deallocate(T* pointer, std::size_t) noexcept {
            if (storage_ == nullptr || pointer != reinterpret_cast<T*>(storage_->bytes.data()) || !storage_->allocated) {
                std::terminate();
            }
            storage_->allocated = false;
        }

        [[nodiscard]] PosixHandlerStorage* storage() const noexcept {
            return storage_;
        }

        template <class U>
        friend class PosixHandlerAllocator;

        template <class U>
        friend bool operator==(const PosixHandlerAllocator& left,
            const PosixHandlerAllocator<U>& right) noexcept {
            return left.storage_ == right.storage();
        }

        template <class U>
        friend bool operator!=(const PosixHandlerAllocator& left,
            const PosixHandlerAllocator<U>& right) noexcept {
            return !(left == right);
        }

    private:
        PosixHandlerStorage* storage_{};
    };

    enum class PosixDirection : std::uint8_t { kReceive,
        kSend };

    struct PosixIoHandler final {
        using allocator_type = PosixHandlerAllocator<PosixIoHandler>;

        impl* owner{};
        PosixHandlerStorage* storage{};
        PosixDirection direction{};

        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return allocator_type(storage);
        }

        void operator()() const noexcept {
            if (storage->allocated) {
                std::terminate();
            }
            owner->handlePosted(direction);
        }

        void operator()(const asio::error_code& error) const noexcept {
            if (storage->allocated) {
                std::terminate();
            }
            owner->handleWait(direction, error);
        }
    };

    static_assert(std::is_same_v<asio::associated_allocator_t<PosixIoHandler>,
        PosixIoHandler::allocator_type>);

    PosixHandlerStorage* acquirePosixHandlerStorage(PosixDirection direction) noexcept {
        auto& storages = direction == PosixDirection::kReceive
                             ? receivePosixHandlerStorage_
                             : sendPosixHandlerStorage_;
        for (auto& storage : storages) {
            if (!storage.allocated) {
                return &storage;
            }
        }
        return nullptr;
    }
#endif

    void requireOwnerThread() const {
        if (std::this_thread::get_id() != ownerThread_) {
            throw std::logic_error("HTTP/3 server network UDP socket used outside its owner thread");
        }
    }

    void requireOwnerThreadNoexcept() const noexcept {
        if (std::this_thread::get_id() != ownerThread_) {
            std::terminate();
        }
    }

    void setPacketInfoOption(int level, int option) {
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

    void completeReceive(std::error_code error, std::size_t size = 0,
        Udp::endpoint peer = {}, Udp::endpoint local_destination = {}) noexcept {
        if (!receive_.active) {
            std::terminate();
        }
        auto completion = receive_.completion;
        void* const context = receive_.context;
        const auto bytes = std::exchange(receive_.bytes, {});
        receive_.active = false;
        receive_.completion = nullptr;
        receive_.context = nullptr;
        ++callbacksRunning_;
        completion(context, error,
            receive_view{std::span<const std::byte>(bytes.data(), error ? 0 : size),
                std::move(peer), std::move(local_destination)});
        --callbacksRunning_;
    }

    void completeSend(std::error_code error, std::size_t size = 0) noexcept {
        if (!send_.active) {
            std::terminate();
        }
        auto completion = send_.completion;
        void* const context = send_.context;
        send_.active = false;
        send_.completion = nullptr;
        send_.context = nullptr;
        send_.view = {};
        ++callbacksRunning_;
        completion(context, error, error ? 0 : size);
        --callbacksRunning_;
    }

#ifndef _WIN32
    bool postReceiveAttempt() noexcept {
        auto* const storage = acquirePosixHandlerStorage(PosixDirection::kReceive);
        if (storage == nullptr) {
            return false;
        }
        ++pendingHandlers_;
        try {
            asio::post(io_, PosixIoHandler{this, storage, PosixDirection::kReceive});
            if (!storage->allocated) {
                std::terminate();
            }
            return true;
        } catch (...) {
            if (storage->allocated || pendingHandlers_ == 0) {
                std::terminate();
            }
            --pendingHandlers_;
            return false;
        }
    }

    bool postSendAttempt() noexcept {
        auto* const storage = acquirePosixHandlerStorage(PosixDirection::kSend);
        if (storage == nullptr) {
            return false;
        }
        ++pendingHandlers_;
        try {
            asio::post(io_, PosixIoHandler{this, storage, PosixDirection::kSend});
            if (!storage->allocated) {
                std::terminate();
            }
            return true;
        } catch (...) {
            if (storage->allocated || pendingHandlers_ == 0) {
                std::terminate();
            }
            --pendingHandlers_;
            return false;
        }
    }

    void handlePosted(PosixDirection direction) noexcept {
        if (stopping_) {
            direction == PosixDirection::kReceive ? completeReceive(aborted())
                                                  : completeSend(aborted());
        } else if (direction == PosixDirection::kReceive) {
            attemptReceive();
        } else {
            attemptSend();
        }
        if (pendingHandlers_ == 0) {
            std::terminate();
        }
        --pendingHandlers_;
    }

    void handleWait(PosixDirection direction, const asio::error_code& error) noexcept {
        if (stopping_) {
            direction == PosixDirection::kReceive ? completeReceive(aborted())
                                                  : completeSend(aborted());
        } else if (error) {
            direction == PosixDirection::kReceive ? completeReceive(error)
                                                  : completeSend(error);
        } else if (direction == PosixDirection::kReceive) {
            attemptReceive();
        } else {
            attemptSend();
        }
        if (pendingHandlers_ == 0) {
            std::terminate();
        }
        --pendingHandlers_;
    }

    void attemptReceive() noexcept {
        std::fill(receive_.control.begin(), receive_.control.end(), std::byte{});
        receive_.peerAddress = {};
        receive_.peerAddressLength =
            static_cast<NativeSocketLength>(sizeof(receive_.peerAddress));

        iovec input{};
        input.iov_base = receive_.bytes.data();
        input.iov_len = receive_.bytes.size();
        msghdr message{};
        message.msg_name = &receive_.peerAddress;
        message.msg_namelen = receive_.peerAddressLength;
        message.msg_iov = &input;
        message.msg_iovlen = 1;
        message.msg_control = receive_.control.data();
        message.msg_controllen = receive_.control.size();

        const ssize_t received = ::recvmsg(socket_.native_handle(), &message, MSG_DONTWAIT);
        if (received < 0) {
            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK) {
                waitReadable();
            } else {
                completeReceive(std::error_code(error, std::system_category()));
            }
            return;
        }

        Udp::endpoint peer;
        Udp::endpoint destination;
        if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
            !parseNativeEndpoint(reinterpret_cast<const sockaddr*>(&receive_.peerAddress),
                message.msg_namelen, ipv6Socket_, peer) ||
            !parsePacketInfo(message, destination)) {
            completeReceive(badMessage());
            return;
        }
        completeReceive({}, static_cast<std::size_t>(received), std::move(peer),
            std::move(destination));
    }

    bool parsePacketInfo(const msghdr& message, Udp::endpoint& destination) const noexcept {
        bool found = false;
        const auto* const controlStart = static_cast<const std::byte*>(message.msg_control);
        const std::size_t controlSize = message.msg_controllen;
        for (const cmsghdr* header = CMSG_FIRSTHDR(const_cast<msghdr*>(&message));
            header != nullptr; header = CMSG_NXTHDR(const_cast<msghdr*>(&message),
                                   const_cast<cmsghdr*>(header))) {
            const auto* const headerBytes = reinterpret_cast<const std::byte*>(header);
            if (headerBytes < controlStart ||
                static_cast<std::size_t>(headerBytes - controlStart) > controlSize ||
                header->cmsg_len < CMSG_LEN(0) ||
                header->cmsg_len > controlSize -
                                       static_cast<std::size_t>(headerBytes - controlStart)) {
                return false;
            }

            if (!ipv6Socket_ && header->cmsg_level == IPPROTO_IP &&
                header->cmsg_type == IP_PKTINFO) {
                if (found || header->cmsg_len < CMSG_LEN(sizeof(in_pktinfo))) {
                    return false;
                }
                in_pktinfo info{};
                std::memcpy(&info, CMSG_DATA(const_cast<cmsghdr*>(header)), sizeof(info));
                asio::ip::address_v4::bytes_type bytes{};
                // ipi_addr is the packet's destination. ipi_spec_dst is a local
                // source-selection hint and is not correct for received-packet metadata.
                std::memcpy(bytes.data(), &info.ipi_addr, bytes.size());
                const asio::ip::address address{asio::ip::address_v4(bytes)};
                if (!validConcreteAddress(address)) {
                    return false;
                }
                destination = Udp::endpoint(address, boundEndpoint_.port());
                found = true;
            } else if (ipv6Socket_ && header->cmsg_level == IPPROTO_IPV6 &&
                       header->cmsg_type == IPV6_PKTINFO) {
                if (found || header->cmsg_len < CMSG_LEN(sizeof(in6_pktinfo))) {
                    return false;
                }
                in6_pktinfo info{};
                std::memcpy(&info, CMSG_DATA(const_cast<cmsghdr*>(header)), sizeof(info));
                asio::ip::address_v6::bytes_type bytes{};
                std::memcpy(bytes.data(), &info.ipi6_addr, bytes.size());
                // The interface index is deliberately not converted to a scope
                // id; link-local addresses are rejected by validConcreteAddress.
                const asio::ip::address address{asio::ip::address_v6(bytes)};
                if (!validConcreteAddress(address)) {
                    return false;
                }
                destination = Udp::endpoint(address, boundEndpoint_.port());
                found = true;
            }
        }
        return found;
    }

    void attemptSend() noexcept {
        const auto peer = makeNativeEndpoint(send_.view.peer);
        send_.peerAddress = peer.address;
        send_.peerAddressLength = peer.length;

        std::fill(send_.control.begin(), send_.control.end(), std::byte{});
        iovec output{};
        std::byte emptyPayload{};
        output.iov_base = send_.view.bytes.empty()
                              ? static_cast<void*>(&emptyPayload)
                              : const_cast<std::byte*>(send_.view.bytes.data());
        output.iov_len = send_.view.bytes.size();
        msghdr message{};
        message.msg_name = &send_.peerAddress;
        message.msg_namelen = send_.peerAddressLength;
        message.msg_iov = &output;
        message.msg_iovlen = 1;
        message.msg_control = send_.control.data();
        if (ipv6Socket_) {
            message.msg_controllen = CMSG_SPACE(sizeof(in6_pktinfo));
            cmsghdr* header = CMSG_FIRSTHDR(&message);
            if (header == nullptr) {
                completeSend(ioError());
                return;
            }
            header->cmsg_level = IPPROTO_IPV6;
            header->cmsg_type = IPV6_PKTINFO;
            header->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
            in6_pktinfo info{};
            const auto bytes = send_.view.source.address().to_v6().to_bytes();
            std::memcpy(&info.ipi6_addr, bytes.data(), bytes.size());
            std::memcpy(CMSG_DATA(header), &info, sizeof(info));
        } else {
            message.msg_controllen = CMSG_SPACE(sizeof(in_pktinfo));
            cmsghdr* header = CMSG_FIRSTHDR(&message);
            if (header == nullptr) {
                completeSend(ioError());
                return;
            }
            header->cmsg_level = IPPROTO_IP;
            header->cmsg_type = IP_PKTINFO;
            header->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
            in_pktinfo info{};
            const auto bytes = send_.view.source.address().to_v4().to_bytes();
            std::memcpy(&info.ipi_spec_dst, bytes.data(), bytes.size());
            std::memcpy(CMSG_DATA(header), &info, sizeof(info));
        }

        const ssize_t sent = ::sendmsg(socket_.native_handle(), &message, MSG_DONTWAIT);
        if (sent < 0) {
            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK) {
                waitWritable();
            } else {
                completeSend(std::error_code(error, std::system_category()));
            }
            return;
        }
        const auto size = static_cast<std::size_t>(sent);
        if (size != send_.view.bytes.size()) {
            completeSend(ioError());
            return;
        }
        completeSend({}, size);
    }

    void waitReadable() noexcept {
        auto* const storage = acquirePosixHandlerStorage(PosixDirection::kReceive);
        if (storage == nullptr) {
            completeReceive(noMemory());
            return;
        }
        ++pendingHandlers_;
        try {
            socket_.async_wait(Udp::socket::wait_read,
                PosixIoHandler{this, storage, PosixDirection::kReceive});
            if (!storage->allocated) {
                std::terminate();
            }
        } catch (...) {
            if (storage->allocated || pendingHandlers_ == 0) {
                std::terminate();
            }
            --pendingHandlers_;
            completeReceive(exceptionCode());
        }
    }

    void waitWritable() noexcept {
        auto* const storage = acquirePosixHandlerStorage(PosixDirection::kSend);
        if (storage == nullptr) {
            completeSend(noMemory());
            return;
        }
        ++pendingHandlers_;
        try {
            socket_.async_wait(Udp::socket::wait_write,
                PosixIoHandler{this, storage, PosixDirection::kSend});
            if (!storage->allocated) {
                std::terminate();
            }
        } catch (...) {
            if (storage->allocated || pendingHandlers_ == 0) {
                std::terminate();
            }
            --pendingHandlers_;
            completeSend(exceptionCode());
        }
    }
#else
    struct HandlerStorage final {
        impl* owner{};
        alignas(std::max_align_t) std::array<std::byte, 1024> bytes{};
        bool allocated{};
    };

    template <class T>
    class FixedHandlerAllocator {
    public:
        using value_type = T;

        explicit FixedHandlerAllocator(HandlerStorage* storage) noexcept
            : storage_(storage) {}

        template <class U>
        FixedHandlerAllocator(const FixedHandlerAllocator<U>& other) noexcept
            : storage_(other.storage()) {}

        [[nodiscard]] T* allocate(std::size_t count) {
            if (storage_ == nullptr || count != 1 || storage_->allocated ||
                sizeof(T) > storage_->bytes.size() ||
                alignof(T) > alignof(std::max_align_t)) {
                throw std::bad_alloc();
            }
            storage_->allocated = true;
            ++storage_->owner->pendingHandlers_;
            return reinterpret_cast<T*>(storage_->bytes.data());
        }

        void deallocate(T* pointer, std::size_t) noexcept {
            if (storage_ == nullptr || pointer != reinterpret_cast<T*>(storage_->bytes.data()) || !storage_->allocated) {
                std::terminate();
            }
            storage_->allocated = false;
            if (storage_->owner->pendingHandlers_ == 0) {
                std::terminate();
            }
            --storage_->owner->pendingHandlers_;
        }

        [[nodiscard]] HandlerStorage* storage() const noexcept {
            return storage_;
        }

        template <class U>
        friend class FixedHandlerAllocator;

        template <class U>
        friend bool operator==(const FixedHandlerAllocator& left,
            const FixedHandlerAllocator<U>& right) noexcept {
            return left.storage_ == right.storage();
        }

        template <class U>
        friend bool operator!=(const FixedHandlerAllocator& left,
            const FixedHandlerAllocator<U>& right) noexcept {
            return !(left == right);
        }

    private:
        HandlerStorage* storage_{};
    };

    enum class Direction : std::uint8_t { kReceive,
        kSend };

    struct WindowsCompletionHandler final {
        using allocator_type = FixedHandlerAllocator<WindowsCompletionHandler>;

        impl* owner{};
        HandlerStorage* storage{};
        Direction direction{};

        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return allocator_type(storage);
        }

        void operator()(const asio::error_code& error, std::size_t size) const noexcept {
            if (storage->allocated) {
                std::terminate();
            }
            ++owner->callbacksRunning_;
            if (direction == Direction::kReceive) {
                owner->finishWindowsReceive(error, size);
            } else {
                owner->finishWindowsSend(error, size);
            }
            --owner->callbacksRunning_;
        }
    };

    static_assert(std::is_same_v<asio::associated_allocator_t<WindowsCompletionHandler>,
        WindowsCompletionHandler::allocator_type>);

    HandlerStorage* acquireHandlerStorage(Direction direction) noexcept {
        auto& storages = direction == Direction::kReceive
                             ? receiveHandlerStorage_
                             : sendHandlerStorage_;
        for (auto& storage : storages) {
            if (!storage.allocated) {
                storage.owner = this;
                return &storage;
            }
        }
        return nullptr;
    }

    void loadMessageExtensions() {
        GUID receiveId = WSAID_WSARECVMSG;
        GUID sendId = WSAID_WSASENDMSG;
        DWORD returned = 0;
        if (::WSAIoctl(socket_.native_handle(), SIO_GET_EXTENSION_FUNCTION_POINTER,
                &receiveId, sizeof(receiveId), &receiveMessageFunction_,
                sizeof(receiveMessageFunction_),
                &returned, nullptr, nullptr) == SOCKET_ERROR) {
            throw std::system_error(WSAGetLastError(), std::system_category(),
                "load WSARecvMsg for HTTP/3 server network");
        }
        returned = 0;
        if (::WSAIoctl(socket_.native_handle(), SIO_GET_EXTENSION_FUNCTION_POINTER,
                &sendId, sizeof(sendId), &sendMessageFunction_,
                sizeof(sendMessageFunction_),
                &returned, nullptr, nullptr) == SOCKET_ERROR) {
            throw std::system_error(WSAGetLastError(), std::system_category(),
                "load WSASendMsg for HTTP/3 server network");
        }
    }

    bool startWindowsReceive() noexcept {
        HandlerStorage* const storage = acquireHandlerStorage(Direction::kReceive);
        if (storage == nullptr) {
            return false;
        }
        bool submitted = false;
        try {
            WindowsCompletionHandler handler{this, storage, Direction::kReceive};
            asio::windows::overlapped_ptr operation(io_.get_executor(), handler);
            if (!storage->allocated) {
                std::terminate();
            }
            receive_.peerAddress = {};
            receive_.control = {};
            receiveBufferDescriptor_.buf = reinterpret_cast<char*>(receive_.bytes.data());
            receiveBufferDescriptor_.len = static_cast<ULONG>(receive_.bytes.size());
            receiveMessage_ = {};
            receiveMessage_.name = reinterpret_cast<sockaddr*>(&receive_.peerAddress);
            receiveMessage_.namelen = static_cast<INT>(sizeof(receive_.peerAddress));
            receiveMessage_.lpBuffers = &receiveBufferDescriptor_;
            receiveMessage_.dwBufferCount = 1;
            receiveMessage_.Control.buf = reinterpret_cast<char*>(receive_.control.data());
            receiveMessage_.Control.len = static_cast<ULONG>(receive_.control.size());
            DWORD received = 0;
            const int result = receiveMessageFunction_(socket_.native_handle(), &receiveMessage_,
                &received, operation.get(), nullptr);
            if (result == 0) {
                receiveOverlapped_ = operation.release();
                submitted = true;
            } else {
                const int error = WSAGetLastError();
                if (error == WSA_IO_PENDING) {
                    receiveOverlapped_ = operation.release();
                    submitted = true;
                } else if (error == WSAEMSGSIZE) {
                    // Treat a synchronously truncated datagram like an IOCP packet-local
                    // error, keeping the normal overlapped callback/slot lifetime.
                    operation.complete(asio::error_code(error, std::system_category()), 0);
                    submitted = true;
                }
            }
        } catch (...) {
            if (storage->allocated) {
                std::terminate();
            }
            return false;
        }
        if (!submitted && storage->allocated) {
            std::terminate();
        }
        return submitted;
    }

    bool startWindowsSend() noexcept {
        HandlerStorage* const storage = acquireHandlerStorage(Direction::kSend);
        if (storage == nullptr) {
            return false;
        }
        bool submitted = false;
        try {
            WindowsCompletionHandler handler{this, storage, Direction::kSend};
            asio::windows::overlapped_ptr operation(io_.get_executor(), handler);
            if (!storage->allocated) {
                std::terminate();
            }
            const auto peer = makeNativeEndpoint(send_.view.peer);
            send_.peerAddress = peer.address;
            send_.peerAddressLength = peer.length;
            send_.control = {};
            sendBufferDescriptor_.buf = send_.view.bytes.empty()
                                            ? reinterpret_cast<char*>(&emptySendByte_)
                                            : const_cast<char*>(reinterpret_cast<const char*>(
                                                  send_.view.bytes.data()));
            sendBufferDescriptor_.len = static_cast<ULONG>(send_.view.bytes.size());
            sendMessage_ = {};
            sendMessage_.name = reinterpret_cast<sockaddr*>(&send_.peerAddress);
            sendMessage_.namelen = static_cast<INT>(send_.peerAddressLength);
            sendMessage_.lpBuffers = &sendBufferDescriptor_;
            sendMessage_.dwBufferCount = 1;
            sendMessage_.Control.buf = reinterpret_cast<char*>(send_.control.data());

            WSACMSGHDR* header = nullptr;
            if (ipv6Socket_) {
                sendMessage_.Control.len = static_cast<ULONG>(WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)));
                header = WSA_CMSG_FIRSTHDR(&sendMessage_);
                if (header == nullptr) {
                    return false;
                }
                header->cmsg_level = IPPROTO_IPV6;
                header->cmsg_type = IPV6_PKTINFO;
                header->cmsg_len = WSA_CMSG_LEN(sizeof(IN6_PKTINFO));
                IN6_PKTINFO info{};
                const auto bytes = send_.view.source.address().to_v6().to_bytes();
                std::memcpy(&info.ipi6_addr, bytes.data(), bytes.size());
                std::memcpy(WSA_CMSG_DATA(header), &info, sizeof(info));
            } else {
                sendMessage_.Control.len = static_cast<ULONG>(WSA_CMSG_SPACE(sizeof(IN_PKTINFO)));
                header = WSA_CMSG_FIRSTHDR(&sendMessage_);
                if (header == nullptr) {
                    return false;
                }
                header->cmsg_level = IPPROTO_IP;
                header->cmsg_type = IP_PKTINFO;
                header->cmsg_len = WSA_CMSG_LEN(sizeof(IN_PKTINFO));
                IN_PKTINFO info{};
                const auto bytes = send_.view.source.address().to_v4().to_bytes();
                std::memcpy(&info.ipi_addr, bytes.data(), bytes.size());
                std::memcpy(WSA_CMSG_DATA(header), &info, sizeof(info));
            }

            DWORD sent = 0;
            const int result = sendMessageFunction_(socket_.native_handle(), &sendMessage_,
                0, &sent, operation.get(), nullptr);
            const bool accepted = result == 0 ||
                                  (result == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING);
            if (accepted) {
                sendOverlapped_ = operation.release();
                submitted = true;
            }
        } catch (...) {
            if (storage->allocated) {
                std::terminate();
            }
            return false;
        }
        if (!submitted && storage->allocated) {
            std::terminate();
        }
        return submitted;
    }

    bool parseWindowsPacketInfo(Udp::endpoint& destination) const noexcept {
        if ((receiveMessage_.dwFlags & MSG_CTRUNC) != 0) {
            return false;
        }
#ifdef MSG_TRUNC
        if ((receiveMessage_.dwFlags & MSG_TRUNC) != 0) {
            return false;
        }
#endif
        bool found = false;
        const auto* const controlStart = reinterpret_cast<const std::byte*>(
            receiveMessage_.Control.buf);
        const std::size_t controlSize = receiveMessage_.Control.len;
        for (WSACMSGHDR* header = WSA_CMSG_FIRSTHDR(
                 const_cast<WSAMSG*>(&receiveMessage_));
            header != nullptr; header = WSA_CMSG_NXTHDR(
                                   const_cast<WSAMSG*>(&receiveMessage_), header)) {
            const auto* const headerBytes = reinterpret_cast<const std::byte*>(header);
            if (headerBytes < controlStart ||
                static_cast<std::size_t>(headerBytes - controlStart) > controlSize ||
                header->cmsg_len < WSA_CMSG_LEN(0) ||
                header->cmsg_len > controlSize -
                                       static_cast<std::size_t>(headerBytes - controlStart)) {
                return false;
            }
            if (!ipv6Socket_ && header->cmsg_level == IPPROTO_IP &&
                header->cmsg_type == IP_PKTINFO) {
                if (found || header->cmsg_len < WSA_CMSG_LEN(sizeof(IN_PKTINFO))) {
                    return false;
                }
                IN_PKTINFO info{};
                std::memcpy(&info, WSA_CMSG_DATA(header), sizeof(info));
                asio::ip::address_v4::bytes_type bytes{};
                std::memcpy(bytes.data(), &info.ipi_addr, bytes.size());
                const asio::ip::address address{asio::ip::address_v4(bytes)};
                if (!validConcreteAddress(address)) {
                    return false;
                }
                destination = Udp::endpoint(address, boundEndpoint_.port());
                found = true;
            } else if (ipv6Socket_ && header->cmsg_level == IPPROTO_IPV6 &&
                       header->cmsg_type == IPV6_PKTINFO) {
                if (found || header->cmsg_len < WSA_CMSG_LEN(sizeof(IN6_PKTINFO))) {
                    return false;
                }
                IN6_PKTINFO info{};
                std::memcpy(&info, WSA_CMSG_DATA(header), sizeof(info));
                asio::ip::address_v6::bytes_type bytes{};
                std::memcpy(bytes.data(), &info.ipi6_addr, bytes.size());
                const asio::ip::address address{asio::ip::address_v6(bytes)};
                if (!validConcreteAddress(address)) {
                    return false;
                }
                destination = Udp::endpoint(address, boundEndpoint_.port());
                found = true;
            }
        }
        return found;
    }

    void finishWindowsReceive(const asio::error_code& error, std::size_t size) noexcept {
        receiveOverlapped_ = nullptr;
        if (stopping_) {
            completeReceive(aborted());
            return;
        }
        if (error) {
            if (error.value() == WSAEMSGSIZE) {
                completeReceive(badMessage());
            } else {
                completeReceive(error);
            }
            return;
        }

        Udp::endpoint peer;
        Udp::endpoint destination;
        if (size > receive_.bytes.size() ||
            !parseNativeEndpoint(reinterpret_cast<const sockaddr*>(&receive_.peerAddress),
                static_cast<NativeSocketLength>(receiveMessage_.namelen), ipv6Socket_, peer) ||
            !parseWindowsPacketInfo(destination)) {
            completeReceive(badMessage());
            return;
        }
        completeReceive({}, size, std::move(peer), std::move(destination));
    }

    void finishWindowsSend(const asio::error_code& error, std::size_t size) noexcept {
        sendOverlapped_ = nullptr;
        if (stopping_) {
            completeSend(aborted());
        } else if (error) {
            completeSend(error);
        } else if (size != send_.view.bytes.size()) {
            completeSend(ioError());
        } else {
            completeSend({}, size);
        }
    }

    void cancelWindowsOperation(OVERLAPPED* operation) noexcept {
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
    Udp::endpoint bindEndpoint_;
    Udp::endpoint boundEndpoint_;
    Udp::socket socket_;
    std::thread::id ownerThread_;
    ReceiveSlot receive_;
    SendSlot send_;
    std::size_t pendingHandlers_{};
    std::size_t callbacksRunning_{};
    bool ipv6Socket_{};
    bool prepared_{};
    bool stopping_{};
#ifndef _WIN32
    std::array<PosixHandlerStorage, 2> receivePosixHandlerStorage_{};
    std::array<PosixHandlerStorage, 2> sendPosixHandlerStorage_{};
#else
    LPFN_WSARECVMSG receiveMessageFunction_{};
    LPFN_WSASENDMSG sendMessageFunction_{};
    OVERLAPPED* receiveOverlapped_{};
    OVERLAPPED* sendOverlapped_{};
    std::array<HandlerStorage, 2> receiveHandlerStorage_{};
    std::array<HandlerStorage, 2> sendHandlerStorage_{};
    WSAMSG receiveMessage_{};
    WSAMSG sendMessage_{};
    WSABUF receiveBufferDescriptor_{};
    WSABUF sendBufferDescriptor_{};
    std::byte emptySendByte_{};
#endif
};

http3_udp_socket::http3_udp_socket(asio::io_context& networkIo,
    asio::ip::udp::endpoint bindEndpoint)
    : impl_(makePmrObject<impl>(processResource(), networkIo, std::move(bindEndpoint))) {}

http3_udp_socket::~http3_udp_socket() = default;

void http3_udp_socket::prepare() {
    impl_->prepare();
}

std::uint16_t http3_udp_socket::bound_port() const noexcept {
    return impl_->bound_port();
}

bool http3_udp_socket::async_receive(std::span<std::byte> bytes, void* context,
    receive_completion completion) noexcept {
    return impl_->async_receive(bytes, context, completion);
}

bool http3_udp_socket::async_send(send_view view, void* context,
    send_completion completion) noexcept {
    return impl_->async_send(std::move(view), context, completion);
}

void http3_udp_socket::request_stop() noexcept {
    impl_->request_stop();
}

bool http3_udp_socket::done() const noexcept {
    return impl_->done();
}

}  // namespace ruvia::detail
