#pragma once

#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/worker_wait_result.h"
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

// The one message of each error class an exchange reports without a more
// specific transport or protocol description.
[[nodiscard]] inline std::string_view http_client_error_message(http_client_error::code_type code) noexcept {
    switch (code) {
        case http_client_error::code_type::timeout:
            return "HTTP client request timed out";
        case http_client_error::code_type::cancelled:
            return "HTTP client request cancelled";
        case http_client_error::code_type::closing:
            return "HTTP client pool is closing";
        case http_client_error::code_type::response_too_large:
            return "HTTP response exceeds configured byte limit";
        case http_client_error::code_type::tls_failed:
            return "HTTP client TLS connection failed";
        case http_client_error::code_type::io_error:
            return "HTTP client connection failed";
        case http_client_error::code_type::protocol_error:
            return "HTTP protocol failed";
        default:
            return "HTTP request failed";
    }
}

[[noreturn]] inline void throw_http_client_abort(client_abort_reason reason) {
    const auto code = http_client_abort_error(reason);
    throw http_client_error(code, http_client_error_message(code));
}

// A non-value wait outcome of the pool's slot, connect or HTTP/1 exchange
// schedulers; `operation` names the wait in timeout and cancellation messages.
[[noreturn]] inline void throw_http_client_wait_failure(worker_wait_status status, std::string_view operation) {
    switch (status) {
        case worker_wait_status::timed_out:
            throw http_client_error(http_client_error::code_type::timeout, std::string(operation) + " timed out");
        case worker_wait_status::cancelled:
            throw http_client_error(http_client_error::code_type::cancelled, std::string(operation) + " cancelled");
        case worker_wait_status::closed:
            throw http_client_error(http_client_error::code_type::closing,
                http_client_error_message(http_client_error::code_type::closing));
        case worker_wait_status::value:
        case worker_wait_status::worker_stopping:
            break;
    }
    std::terminate();
}

// The code of a failure the client reported as http_client_error.
[[nodiscard]] inline std::optional<http_client_error::code_type> http_client_failure_code(
    const std::exception_ptr& failure) noexcept {
    try {
        std::rethrow_exception(failure);
    } catch (const http_client_error& error) {
        return error.code();
    } catch (...) {
        return std::nullopt;
    }
}

// `eof_message` describes a peer close where the caller still needed bytes.
[[noreturn]] inline void throw_http_client_transport_error(
    const client_transport_result& result, std::string_view eof_message) {
    if (result.status_ == client_transport_status::aborted) {
        throw_http_client_abort(result.abort_);
    }
    if (result.status_ == client_transport_status::eof) {
        throw http_client_error(http_client_error::code_type::io_error, eof_message);
    }
    throw http_client_error(http_client_transport_error(result), error_code_text(result.error_));
}

}  // namespace ruvia::detail
