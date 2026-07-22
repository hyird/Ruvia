#pragma once

#include <asio/ip/tcp.hpp>

#include <cstddef>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia::detail {

using DbResolvedAddresses = std::pmr::vector<std::pmr::string>;

[[nodiscard]] DbResolvedAddresses collectDbResolvedAddresses(
    const asio::ip::tcp::resolver::results_type& results,
    std::pmr::memory_resource* resource);

// Connector/C accepts comma-separated hosts and performs failover only while
// opening the transport. IPv6 literals in a multi-host list must be bracketed
// so their colons are not parsed as a per-host port separator; a lone IPv6
// literal remains unbracketed because Connector/C only invokes that list parser
// when a comma is present.
[[nodiscard]] std::pmr::string makeMariaDbResolvedHostList(
    std::span<const std::pmr::string> addresses,
    std::pmr::memory_resource* resource);

struct PostgreSqlResolvedHostList final {
    std::pmr::string hosts;
    std::pmr::string addresses;
};

// libpq requires host and hostaddr lists to contain the same number of items.
// Repeating the logical hostname preserves TLS/GSS/password-file identity while
// the numeric hostaddr list suppresses libpq's blocking DNS lookup.
[[nodiscard]] PostgreSqlResolvedHostList makePostgreSqlResolvedHostList(
    std::string_view logicalHost,
    std::span<const std::pmr::string> addresses,
    std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
