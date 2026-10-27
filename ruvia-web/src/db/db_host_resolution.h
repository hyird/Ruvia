#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/db/db_types.h"

namespace ruvia::detail {

struct client_resolve_result;

using db_resolved_addresses_type = std::pmr::vector<std::pmr::string>;

[[nodiscard]] db_resolved_addresses_type collect_db_resolved_addresses(
    const client_resolve_result& results, db_driver driver, std::pmr::memory_resource* resource);

// Connector/C accepts comma-separated hosts and performs failover only while
// opening the transport. IPv6 literals in a multi-host list must be bracketed
// so their colons are not parsed as a per-host port separator; a lone IPv6
// literal remains unbracketed because Connector/C only invokes that list parser
// when a comma is present.
[[nodiscard]] std::pmr::string make_mariadb_resolved_host_list(
    std::span<const std::pmr::string> addresses, std::pmr::memory_resource* resource);

struct postgresql_resolved_host_list final {
    std::pmr::string hosts_;
    std::pmr::string addresses_;
};

// libpq requires host and hostaddr lists to contain the same number of items.
// Repeating the logical hostname preserves TLS/GSS/password-file identity while
// the numeric hostaddr list suppresses libpq's blocking DNS lookup.
[[nodiscard]] postgresql_resolved_host_list make_postgresql_resolved_host_list(
    std::string_view logical_host, std::span<const std::pmr::string> addresses,
    std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
