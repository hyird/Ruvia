#include "db/db_host_resolution.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"

#include "client/client_host_resolver.h"

namespace ruvia::detail {
namespace {

void append_list_separator(std::pmr::string& output) {
    if (!output.empty()) {
        output.push_back(',');
    }
}

void append_db_resolved_address(db_resolved_addresses_type& addresses,
    const asio::ip::address& endpoint, db_driver driver) {
    std::string address;
    try {
        address = endpoint.to_string();
    } catch (const std::system_error& error) {
        throw db_error(db_error::code_type::resolve_failed, driver,
            system_error_text(error), error.code().value());
    }
    if (std::ranges::none_of(addresses, [&address](const std::pmr::string& existing) {
            return std::string_view(existing) == address;
        })) {
        addresses.emplace_back(address.data(), address.size());
    }
}

}  // namespace

db_resolved_addresses_type collect_db_resolved_addresses(const client_resolve_result& results,
    db_driver driver, std::pmr::memory_resource* resource) {
    const auto resolved = pmr_resource_or_default(resource);
    db_resolved_addresses_type addresses(resolved);
    if (results.literal_) {
        append_db_resolved_address(addresses, results.literal_->address(), driver);
    } else {
        for (const auto& result : results.addresses_) {
            append_db_resolved_address(addresses, result.endpoint().address(), driver);
        }
    }
    if (addresses.empty()) {
        throw db_error(
            db_error::code_type::resolve_failed, driver, "database host resolved to no addresses");
    }
    return addresses;
}

std::pmr::string make_mariadb_resolved_host_list(
    std::span<const std::pmr::string> addresses, std::pmr::memory_resource* resource) {
    const auto resolved = pmr_resource_or_default(resource);
    std::pmr::string output(resolved);
    const bool multiple = addresses.size() > 1;
    for (const auto& address : addresses) {
        append_list_separator(output);
        if (multiple && (address.find(':') != std::string_view::npos)) {
            output.push_back('[');
            output.append(address);
            output.push_back(']');
        } else {
            output.append(address);
        }
    }
    if (output.empty()) {
        throw std::invalid_argument("MariaDB resolved host list must not be empty");
    }
    return output;
}

postgresql_resolved_host_list make_postgresql_resolved_host_list(std::string_view logical_host,
    std::span<const std::pmr::string> addresses, std::pmr::memory_resource* resource) {
    if (logical_host.empty() || addresses.empty()) {
        throw std::invalid_argument("PostgreSQL resolved host list must not be empty");
    }
    const auto resolved = pmr_resource_or_default(resource);
    postgresql_resolved_host_list output{std::pmr::string(resolved), std::pmr::string(resolved)};
    for (const auto& address : addresses) {
        append_list_separator(output.hosts_);
        append_list_separator(output.addresses_);
        output.hosts_.append(logical_host);
        output.addresses_.append(address);
    }
    return output;
}

}  // namespace ruvia::detail
