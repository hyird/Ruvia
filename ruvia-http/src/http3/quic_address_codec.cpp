#include "http3/quic_address_codec.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace ruvia::detail {
namespace {

std::uint16_t to_network_port(std::uint16_t port) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(port);
    } else {
        return port;
    }
}

std::uint16_t from_network_port(std::uint16_t port) noexcept {
    return to_network_port(port);
}

ngtcp2_socklen address_length(const quic_address& address) {
    switch (address.family) {
        case quic_address_family::ipv4:
            return static_cast<ngtcp2_socklen>(sizeof(ngtcp2_sockaddr_in));
        case quic_address_family::ipv6:
            return static_cast<ngtcp2_socklen>(sizeof(ngtcp2_sockaddr_in6));
    }
    throw std::invalid_argument("unsupported QUIC address family");
}

void encode_address(ngtcp2_sockaddr_union& storage, const quic_address& address) {
    switch (address.family) {
        case quic_address_family::ipv4:
            storage.in = {};
            storage.in.sin_family = NGTCP2_AF_INET;
            storage.in.sin_port = to_network_port(address.port);
            std::memcpy(&storage.in.sin_addr, address.bytes.data(), sizeof(storage.in.sin_addr));
            return;
        case quic_address_family::ipv6:
            storage.in6 = {};
            storage.in6.sin6_family = NGTCP2_AF_INET6;
            storage.in6.sin6_port = to_network_port(address.port);
            std::memcpy(storage.in6.sin6_addr.s6_addr, address.bytes.data(), address.bytes.size());
            storage.in6.sin6_scope_id = address.scope_id;
            return;
    }
    throw std::invalid_argument("unsupported QUIC address family");
}

}  // namespace

ngtcp2_addr encode_quic_address(ngtcp2_sockaddr_union& storage, const quic_address& address) {
    const auto length = address_length(address);
    encode_address(storage, address);
    return {.addr = &storage.sa, .addrlen = length};
}

quic_address decode_quic_address(const ngtcp2_addr& address) {
    if (address.addr == nullptr) {
        throw std::invalid_argument("ngtcp2 address pointer is null");
    }

    if (address.addrlen < sizeof(address.addr->sa_family)) {
        throw std::invalid_argument("ngtcp2 address is too short to contain a family");
    }

    decltype(address.addr->sa_family) family{};
    std::memcpy(&family, address.addr, sizeof(family));
    quic_address result{};
    if (family == NGTCP2_AF_INET) {
        if (address.addrlen != sizeof(ngtcp2_sockaddr_in)) {
            throw std::invalid_argument("invalid IPv4 ngtcp2 address length");
        }
        ngtcp2_sockaddr_in native{};
        std::memcpy(&native, address.addr, sizeof(native));
        result.family = quic_address_family::ipv4;
        result.port = from_network_port(native.sin_port);
        std::memcpy(result.bytes.data(), &native.sin_addr, sizeof(native.sin_addr));
        return result;
    }
    if (family == NGTCP2_AF_INET6) {
        if (address.addrlen != sizeof(ngtcp2_sockaddr_in6)) {
            throw std::invalid_argument("invalid IPv6 ngtcp2 address length");
        }
        ngtcp2_sockaddr_in6 native{};
        std::memcpy(&native, address.addr, sizeof(native));
        result.family = quic_address_family::ipv6;
        result.port = from_network_port(native.sin6_port);
        std::memcpy(result.bytes.data(), native.sin6_addr.s6_addr, result.bytes.size());
        result.scope_id = native.sin6_scope_id;
        return result;
    }
    throw std::invalid_argument("unsupported ngtcp2 address family");
}

void fill_quic_path(ngtcp2_path_storage& path, const quic_address& local,
    const quic_address& peer) {
    ngtcp2_sockaddr_union local_storage{};
    ngtcp2_sockaddr_union peer_storage{};
    const auto local_length = address_length(local);
    const auto peer_length = address_length(peer);
    encode_address(local_storage, local);
    encode_address(peer_storage, peer);
    ngtcp2_path_storage_init(&path, &local_storage.sa, local_length,
        &peer_storage.sa, peer_length, nullptr);
}

}  // namespace ruvia::detail
