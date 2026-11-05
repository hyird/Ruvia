#pragma once

#include <cstdint>
#include <system_error>

namespace ruvia::detail {

enum class accept_error_action : std::uint8_t { retry,
    report_and_retry,
    fail };

// Peer disappearance and readiness races are normal accept outcomes. Resource
// and network failures leave the listener usable but must remain observable.
[[nodiscard]] accept_error_action classify_accept_error(const std::error_code& error) noexcept;

}  // namespace ruvia::detail
