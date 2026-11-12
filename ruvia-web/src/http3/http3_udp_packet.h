#pragma once

#if !defined(_WIN32)
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <system_error>
#include <utility>

#include <asio/error.hpp>
#include <asio/ip/udp.hpp>

namespace ruvia::detail {

inline void enable_http3_packet_info(asio::ip::udp::socket& socket) {
    const int enabled = 1;
    const bool v6 = socket.local_endpoint().protocol() == asio::ip::udp::v6();
    int result;
    if (v6) {
#if defined(IPV6_RECVPKTINFO)
        result = ::setsockopt(socket.native_handle(), IPPROTO_IPV6, IPV6_RECVPKTINFO, &enabled, sizeof(enabled));
#else
        result = ::setsockopt(socket.native_handle(), IPPROTO_IPV6, IPV6_PKTINFO, &enabled, sizeof(enabled));
#endif
    } else {
#if defined(IP_PKTINFO)
        result = ::setsockopt(socket.native_handle(), IPPROTO_IP, IP_PKTINFO, &enabled, sizeof(enabled));
#else
        result = ::setsockopt(socket.native_handle(), IPPROTO_IP, IP_RECVDSTADDR, &enabled, sizeof(enabled));
#endif
    }
    if (result != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to enable QUIC packet information");
    }
}

// Receives actual destination and ingress interface even for wildcard sockets.
// Socket is nonblocking; callers wait on the existing Asio reactor between bursts.
inline std::size_t receive_http3_packet(asio::ip::udp::socket& socket, std::span<std::byte> bytes,
    asio::ip::udp::endpoint& remote, asio::ip::udp::endpoint& local, std::uint32_t& interface_index,
    std::error_code& error) noexcept {
    alignas(cmsghdr) std::array<std::byte, 256> control{};
    iovec buffer{bytes.data(), bytes.size()};
    msghdr message{};
    message.msg_name = remote.data();
    message.msg_namelen = static_cast<socklen_t>(remote.capacity());
    message.msg_iov = &buffer;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    ssize_t received;
    do {
        received = ::recvmsg(socket.native_handle(), &message, 0);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        error = {errno, asio::system_category()};
        return 0;
    }
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
        error = asio::error::message_size;
        return 0;
    }
    if (message.msg_controllen > control.size()) {
        error = asio::error::fault;
        return 0;
    }
    remote.resize(message.msg_namelen);
    bool found = false;
    interface_index = 0;
    for (std::size_t offset = 0; message.msg_controllen - offset >= CMSG_LEN(0);) {
        auto* cmsg = reinterpret_cast<cmsghdr*>(control.data() + offset);
        const auto remaining = message.msg_controllen - offset;
        if (cmsg->cmsg_len < CMSG_LEN(0) || cmsg->cmsg_len > remaining) {
            error = asio::error::fault;
            return 0;
        }
#if defined(IP_PKTINFO)
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO && cmsg->cmsg_len >= CMSG_LEN(sizeof(in_pktinfo))) {
            in_pktinfo info{};
            std::memcpy(&info, CMSG_DATA(cmsg), sizeof(info));
            asio::ip::address_v4::bytes_type address{};
            std::memcpy(address.data(), &info.ipi_addr, address.size());
            local.address(asio::ip::address_v4(address));
            if (info.ipi_ifindex < 0) {
                error = asio::error::fault;
                return 0;
            }
            interface_index = static_cast<std::uint32_t>(info.ipi_ifindex);
            found = true;
        }
#else
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_RECVDSTADDR && cmsg->cmsg_len >= CMSG_LEN(sizeof(in_addr))) {
            asio::ip::address_v4::bytes_type address{};
            std::memcpy(address.data(), CMSG_DATA(cmsg), address.size());
            local.address(asio::ip::address_v4(address));
            found = true;
        }
#endif
        if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO && cmsg->cmsg_len >= CMSG_LEN(sizeof(in6_pktinfo))) {
            in6_pktinfo info{};
            std::memcpy(&info, CMSG_DATA(cmsg), sizeof(info));
            asio::ip::address_v6::bytes_type address{};
            std::memcpy(address.data(), &info.ipi6_addr, address.size());
            auto v6 = asio::ip::address_v6(address);
            interface_index = info.ipi6_ifindex;
            if (v6.is_link_local() || v6.is_multicast_link_local()) {
                v6.scope_id(interface_index);
            }
            local.address(v6);
            found = true;
        }
        // CMSG_SPACE keeps the native alignment without CMSG_NXTHDR's
        // signed pointer-difference comparison on musl.
        const auto advance = CMSG_SPACE(cmsg->cmsg_len - CMSG_LEN(0));
        if (advance > remaining) {
            break;
        }
        offset += advance;
    }
    if (!found) {
        error = asio::error::fault;
        return 0;
    }
    error.clear();
    return static_cast<std::size_t>(received);
}

inline std::error_code send_http3_packet(asio::ip::udp::socket& socket, const asio::ip::udp::endpoint& remote,
    std::span<const std::byte> bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    alignas(cmsghdr) std::array<std::byte, 256> control{};
    iovec buffer{const_cast<std::byte*>(bytes.data()), bytes.size()};
    msghdr message{};
    message.msg_name = const_cast<asio::ip::udp::endpoint::data_type*>(remote.data());
    message.msg_namelen = static_cast<socklen_t>(remote.size());
    message.msg_iov = &buffer;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    auto* cmsg = reinterpret_cast<cmsghdr*>(control.data());
    if (local.address().is_v4()) {
        if (!std::in_range<int>(interface_index)) {
            return asio::error::invalid_argument;
        }
        const auto address = local.address().to_v4().to_bytes();
        cmsg->cmsg_level = IPPROTO_IP;
#if defined(IP_PKTINFO)
        cmsg->cmsg_type = IP_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
        message.msg_controllen = CMSG_SPACE(sizeof(in_pktinfo));
        in_pktinfo info{};
        info.ipi_ifindex = static_cast<int>(interface_index);
        std::memcpy(&info.ipi_spec_dst, address.data(), address.size());
        std::memcpy(CMSG_DATA(cmsg), &info, sizeof(info));
#else
        cmsg->cmsg_type = IP_SENDSRCADDR;
        cmsg->cmsg_len = CMSG_LEN(sizeof(in_addr));
        message.msg_controllen = CMSG_SPACE(sizeof(in_addr));
        std::memcpy(CMSG_DATA(cmsg), address.data(), address.size());
#endif
    } else {
        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
        message.msg_controllen = CMSG_SPACE(sizeof(in6_pktinfo));
        in6_pktinfo info{};
        const auto address = local.address().to_v6().to_bytes();
        info.ipi6_ifindex = interface_index;
        std::memcpy(&info.ipi6_addr, address.data(), address.size());
        std::memcpy(CMSG_DATA(cmsg), &info, sizeof(info));
    }
    ssize_t sent;
    do {
        sent = ::sendmsg(socket.native_handle(), &message, 0);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        return {errno, asio::system_category()};
    }
    if (static_cast<std::size_t>(sent) != bytes.size()) {
        return asio::error::message_size;
    }
    return {};
}

}  // namespace ruvia::detail
#endif
