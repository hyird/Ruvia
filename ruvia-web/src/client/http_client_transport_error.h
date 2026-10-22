#pragma once

#include <string>
#include <string_view>

#include "ruvia/core/failure_report.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_tcp_transport.h"

namespace ruvia::detail {

[[nodiscard]] inline http_client_error::code_type http_client_abort_error(client_abort_reason reason) noexcept {
    switch (reason) {
        case client_abort_reason::timeout:
            return http_client_error::code_type::timeout;
        case client_abort_reason::cancelled:
            return http_client_error::code_type::cancelled;
        case client_abort_reason::none:
        case client_abort_reason::closing:
            break;
    }
    return http_client_error::code_type::closing;
}

// The HTTP client's mapping of a failed client_tcp_transport result.
[[nodiscard]] inline http_client_error::code_type http_client_transport_error(
    const client_transport_result& result) noexcept {
    switch (result.status_) {
        case client_transport_status::aborted:
            return http_client_abort_error(result.abort_);
        case client_transport_status::resolve_failed:
            return http_client_error::code_type::resolve_failed;
        case client_transport_status::connect_failed:
            return http_client_error::code_type::connect_failed;
        case client_transport_status::tls_failed:
            return http_client_error::code_type::tls_failed;
        case client_transport_status::ok:
        case client_transport_status::eof:
        case client_transport_status::io_failed:
            break;
    }
    return result.error_ == std::errc::protocol_error ? http_client_error::code_type::protocol_error
                                                      : http_client_error::code_type::io_error;
}

[[nodiscard]] inline std::string_view http_client_abort_message(client_abort_reason reason) noexcept {
    switch (reason) {
        case client_abort_reason::timeout:
            return "http client request timed out";
        case client_abort_reason::cancelled:
            return "http client request cancelled";
        case client_abort_reason::none:
        case client_abort_reason::closing:
            break;
    }
    return "http client pool is closing";
}

// `eof_message` describes a peer close where the caller still needed bytes.
[[noreturn]] inline void throw_http_client_transport_error(
    const client_transport_result& result, std::string_view eof_message) {
    if (result.status_ == client_transport_status::aborted) {
        throw http_client_error(http_client_abort_error(result.abort_), http_client_abort_message(result.abort_));
    }
    if (result.status_ == client_transport_status::eof) {
        throw http_client_error(http_client_error::code_type::io_error, eof_message);
    }
    throw http_client_error(http_client_transport_error(result), error_code_text(result.error_));
}

}  // namespace ruvia::detail
