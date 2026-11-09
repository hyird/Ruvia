#pragma once

#include <cerrno>
#include <cstdint>
#include <system_error>

#if defined(_WIN32)
#include <winerror.h>
#endif

#include <asio/error.hpp>

namespace ruvia::detail {

enum class accept_error_action : std::uint8_t { retry,
    report_and_retry,
    fail };

// Peer disappearance and readiness races are normal accept outcomes. Resource
// and network failures leave the listener usable but must remain observable.
[[nodiscard]] inline accept_error_action classify_accept_error(const std::error_code& error) noexcept {
    if (error == asio::error::connection_aborted || error == asio::error::connection_reset ||
        error == asio::error::eof || error == asio::error::try_again ||
        error == asio::error::would_block || error == asio::error::interrupted
#if defined(_WIN32)
        || (error.category() == asio::system_category() && error.value() == ERROR_NETNAME_DELETED)
#endif
    ) {
        return accept_error_action::retry;
    }
    if (error == asio::error::no_descriptors || error == asio::error::no_buffer_space ||
        error == asio::error::no_memory || error == std::errc::too_many_files_open_in_system ||
        error == std::errc::protocol_error || error == asio::error::network_down ||
        error == asio::error::network_unreachable || error == asio::error::host_unreachable ||
        error == asio::error::no_protocol_option || error == asio::error::operation_not_supported ||
        error == asio::error::no_permission || error == asio::error::timed_out) {
        return accept_error_action::report_and_retry;
    }
#if defined(EHOSTDOWN) && defined(ENONET)
    if (error.category() == asio::system_category() && (error.value() == EHOSTDOWN || error.value() == ENONET)) {
        return accept_error_action::report_and_retry;
    }
#endif
    return accept_error_action::fail;
}

}  // namespace ruvia::detail
