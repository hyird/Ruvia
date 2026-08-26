#include "ruvia/web/detail/db/DbHostResolution.h"

#include "ruvia/core/memory/PmrResource.h"

#include <algorithm>
#include <stdexcept>
#include <system_error>

namespace ruvia::detail {
namespace {

void appendListSeparator(std::pmr::string& output) {
    if (!output.empty()) {
        output.push_back(',');
    }
}

}  // namespace

DbResolvedAddresses collectDbResolvedAddresses(const asio::ip::tcp::resolver::results_type& results,
    DbDriver driver, std::pmr::memory_resource* resource) {
    const auto resolved = pmrResourceOrDefault(resource);
    DbResolvedAddresses addresses(resolved);
    for (const auto& result : results) {
        std::error_code error;
        const auto address = result.endpoint().address().to_string(error);
        if (error) {
            throw DbError(DbError::Code::kResolveFailed, driver,
                std::system_error(error, "formatting resolved database address failed").what(),
                error.value());
        }
        if (std::ranges::none_of(addresses, [&address](const std::pmr::string& existing) {
                return std::string_view(existing) == address;
            })) {
            addresses.emplace_back(address.data(), address.size());
        }
    }
    if (addresses.empty()) {
        throw DbError(
            DbError::Code::kResolveFailed, driver, "database host resolved to no addresses");
    }
    return addresses;
}

std::pmr::string makeMariaDbResolvedHostList(
    std::span<const std::pmr::string> addresses, std::pmr::memory_resource* resource) {
    const auto resolved = pmrResourceOrDefault(resource);
    std::pmr::string output(resolved);
    const bool multiple = addresses.size() > 1;
    for (const auto& address : addresses) {
        appendListSeparator(output);
        if (multiple && address.find(':') != std::string_view::npos) {
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

PostgreSqlResolvedHostList makePostgreSqlResolvedHostList(std::string_view logicalHost,
    std::span<const std::pmr::string> addresses, std::pmr::memory_resource* resource) {
    if (logicalHost.empty() || addresses.empty()) {
        throw std::invalid_argument("PostgreSQL resolved host list must not be empty");
    }
    const auto resolved = pmrResourceOrDefault(resource);
    PostgreSqlResolvedHostList output{std::pmr::string(resolved), std::pmr::string(resolved)};
    for (const auto& address : addresses) {
        appendListSeparator(output.hosts);
        appendListSeparator(output.addresses);
        output.hosts.append(logicalHost);
        output.addresses.append(address);
    }
    return output;
}

}  // namespace ruvia::detail
