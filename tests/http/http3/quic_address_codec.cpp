#include "http3/quic_address_codec.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "test_harness.h"

namespace {

ruvia::quic_address ipv4(std::uint8_t last, std::uint16_t port) {
    ruvia::quic_address address{};
    address.family_ = ruvia::quic_address_family::ipv4;
    address.bytes_[0] = std::byte{192};
    address.bytes_[1] = std::byte{0};
    address.bytes_[2] = std::byte{2};
    address.bytes_[3] = static_cast<std::byte>(last);
    address.port_ = port;
    return address;
}

ruvia::quic_address ipv6(std::uint8_t last, std::uint16_t port, std::uint32_t scope) {
    ruvia::quic_address address{};
    address.family_ = ruvia::quic_address_family::ipv6;
    address.bytes_[0] = std::byte{0x20};
    address.bytes_[1] = std::byte{0x01};
    address.bytes_[15] = static_cast<std::byte>(last);
    address.port_ = port;
    address.scope_id_ = scope;
    return address;
}

}  // namespace

RUVIA_TEST(quic_address_codec_encodes_and_decodes_ipv4) {
    const auto expected = ipv4(7, 4433);
    ngtcp2_sockaddr_union storage{};
    const auto native = ruvia::detail::encode_quic_address(storage, expected);

    RUVIA_CHECK_EQ(native.addr, &storage.sa);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(native).family_, expected.family_);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(native).bytes_, expected.bytes_);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(native).port_, expected.port_);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(native).scope_id_, 0U);
}

RUVIA_TEST(quic_address_codec_encodes_and_decodes_ipv6_with_host_scope) {
    const auto expected = ipv6(9, 8443, 0x12345678);
    ngtcp2_sockaddr_union storage{};
    const auto native = ruvia::detail::encode_quic_address(storage, expected);
    const auto decoded = ruvia::detail::decode_quic_address(native);

    RUVIA_CHECK_EQ(native.addr, &storage.sa);
    RUVIA_CHECK_EQ(decoded.family_, expected.family_);
    RUVIA_CHECK_EQ(decoded.bytes_, expected.bytes_);
    RUVIA_CHECK_EQ(decoded.port_, expected.port_);
    RUVIA_CHECK_EQ(decoded.scope_id_, expected.scope_id_);
}

RUVIA_TEST(quic_address_codec_fills_stable_owned_paths) {
    auto local = ipv4(1, 4000);
    auto peer = ipv4(2, 4433);
    ngtcp2_path_storage path{};
    ruvia::detail::fill_quic_path(path, local, peer);

    RUVIA_CHECK_EQ(path.path.local.addr, &path.local_addrbuf.sa);
    RUVIA_CHECK_EQ(path.path.remote.addr, &path.remote_addrbuf.sa);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(path.path.local).bytes_, local.bytes_);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(path.path.remote).bytes_, peer.bytes_);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(path.path.local).port_, local.port_);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(path.path.remote).port_, peer.port_);

    local = ipv4(8, 5000);
    peer = ipv4(9, 6000);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(path.path.local).bytes_[3], std::byte{1});
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(path.path.local).port_, 4000);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(path.path.remote).bytes_[3], std::byte{2});
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(path.path.remote).port_, 4433);

    ngtcp2_path_storage same_path{};
    ruvia::detail::fill_quic_path(same_path, ipv4(1, 4000), ipv4(2, 4433));
    RUVIA_CHECK(ngtcp2_path_eq(&path.path, &same_path.path));

    ngtcp2_path_storage changed_port{};
    ruvia::detail::fill_quic_path(changed_port, ipv4(1, 4001), ipv4(2, 4433));
    RUVIA_CHECK(!ngtcp2_path_eq(&path.path, &changed_port.path));

    ngtcp2_path_storage changed_ip{};
    ruvia::detail::fill_quic_path(changed_ip, ipv4(1, 4000), ipv4(3, 4433));
    RUVIA_CHECK(!ngtcp2_path_eq(&path.path, &changed_ip.path));
}

RUVIA_TEST(quic_address_codec_path_eq_uses_stock_ipv6_semantics) {
    ngtcp2_path_storage first{};
    ngtcp2_path_storage other_scope{};
    ruvia::detail::fill_quic_path(first, ipv6(1, 4000, 2), ipv6(2, 4433, 7));
    ruvia::detail::fill_quic_path(other_scope, ipv6(1, 4000, 3), ipv6(2, 4433, 8));

    RUVIA_CHECK_EQ(first.path.local.addr, &first.local_addrbuf.sa);
    RUVIA_CHECK_EQ(first.path.remote.addr, &first.remote_addrbuf.sa);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(first.path.local).port_, 4000);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(first.path.remote).port_, 4433);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(first.path.local).scope_id_, 2U);
    RUVIA_CHECK_EQ(ruvia::detail::decode_quic_address(first.path.remote).scope_id_, 7U);
    RUVIA_CHECK(ngtcp2_path_eq(&first.path, &other_scope.path));

    ngtcp2_path_storage changed_port{};
    ruvia::detail::fill_quic_path(changed_port, ipv6(1, 4001, 2), ipv6(2, 4433, 7));
    RUVIA_CHECK(!ngtcp2_path_eq(&first.path, &changed_port.path));

    ngtcp2_path_storage changed_ip{};
    ruvia::detail::fill_quic_path(changed_ip, ipv6(1, 4000, 2), ipv6(3, 4433, 7));
    RUVIA_CHECK(!ngtcp2_path_eq(&first.path, &changed_ip.path));
}

RUVIA_TEST(quic_address_codec_rejects_invalid_native_addresses) {
    ngtcp2_sockaddr_union invalid_family{};
    invalid_family.sa.sa_family = static_cast<decltype(invalid_family.sa.sa_family)>(0x7fff);
    ngtcp2_addr invalid{.addr = &invalid_family.sa,
        .addrlen = static_cast<ngtcp2_socklen>(sizeof(ngtcp2_sockaddr))};
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::detail::decode_quic_address(invalid); }));

    ngtcp2_sockaddr_union short_ipv4{};
    [[maybe_unused]] const auto encoded_ipv4 =
        ruvia::detail::encode_quic_address(short_ipv4, ipv4(1, 4433));
    ngtcp2_addr short_address{.addr = &short_ipv4.sa,
        .addrlen = static_cast<ngtcp2_socklen>(sizeof(ngtcp2_sockaddr_in) - 1)};
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::detail::decode_quic_address(short_address); }));

    ngtcp2_addr null_address{.addr = nullptr, .addrlen = 0};
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::detail::decode_quic_address(null_address); }));
}

RUVIA_TEST(quic_address_codec_rejects_invalid_public_family) {
    auto address = ipv4(1, 4433);
    address.family_ = static_cast<ruvia::quic_address_family>(0xff);
    ngtcp2_sockaddr_union storage{};
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        (void)ruvia::detail::encode_quic_address(storage, address);
    }));

    ngtcp2_path_storage path{};
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        ruvia::detail::fill_quic_path(path, address, ipv4(2, 4433));
    }));
}
