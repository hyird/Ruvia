#pragma once

#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_wait_result.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_tcp_transport.h"

namespace ruvia::detail {

[[nodiscard]] inline std::error_code http_client_abort_error(client_abort_reason reason) noexcept {
    switch (reason) {
        case client_abort_reason::timeout:
            return std::make_error_code(std::errc::timed_out);
        case client_abort_reason::cancelled:
            return std::make_error_code(std::errc::operation_canceled);
        case client_abort_reason::none:
        case client_abort_reason::closing:
            break;
    }
    return make_error_code(http_client_errc::closing);
}

// The HTTP client's mapping of a failed client_tcp_transport result.
[[nodiscard]] inline std::error_code http_client_transport_error(
    const client_transport_result& result) noexcept {
    switch (result.status_) {
        case client_transport_status::aborted:
            return http_client_abort_error(result.abort_);
        case client_transport_status::resolve_failed:
            return make_error_code(http_client_errc::resolve_failed);
        case client_transport_status::connect_failed:
            return make_error_code(http_client_errc::connect_failed);
        case client_transport_status::tls_failed:
            return make_error_code(http_client_errc::tls_failed);
        case client_transport_status::ok:
        case client_transport_status::eof:
        case client_transport_status::io_failed:
            break;
    }
    return result.error_ == std::errc::protocol_error ? make_error_code(http_client_errc::protocol_error)
                                                      : make_error_code(http_client_errc::io_error);
}

// The one message of each error class an exchange reports without a more
// specific transport or protocol description.
[[nodiscard]] inline std::string_view http_client_error_message(std::error_code code) noexcept {
    if (code == std::make_error_code(std::errc::timed_out)) {
        return "HTTP client request timed out";
    }
    if (code == std::make_error_code(std::errc::operation_canceled)) {
        return "HTTP client request cancelled";
    }
    if (code == make_error_code(http_client_errc::closing)) {
        return "HTTP client pool is closing";
    }
    if (code == make_error_code(http_client_errc::response_too_large)) {
        return "HTTP response exceeds configured byte limit";
    }
    if (code == make_error_code(http_client_errc::tls_failed)) {
        return "HTTP client TLS connection failed";
    }
    if (code == make_error_code(http_client_errc::io_error)) {
        return "HTTP client connection failed";
    }
    if (code == make_error_code(http_client_errc::protocol_error)) {
        return "HTTP protocol failed";
    }
    if (code == make_error_code(http_client_errc::handshake_rejected)) {
        return "WebSocket handshake was rejected";
    }
    if (code == make_error_code(http_client_errc::message_too_large)) {
        return "WebSocket message exceeds configured byte limit";
    }
    return "HTTP request failed";
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
            throw http_client_error(std::make_error_code(std::errc::timed_out),
                std::pmr::string(operation, process_resource()).append(" timed out"));
        case worker_wait_status::cancelled:
            throw http_client_error(std::make_error_code(std::errc::operation_canceled),
                std::pmr::string(operation, process_resource()).append(" cancelled"));
        case worker_wait_status::closed:
        case worker_wait_status::worker_stopping:
            throw http_client_error(make_error_code(http_client_errc::closing),
                http_client_error_message(make_error_code(http_client_errc::closing)));
        case worker_wait_status::value:
            break;
    }
    std::terminate();
}

// The code of a failure the client reported as http_client_error.
[[nodiscard]] inline std::optional<std::error_code> http_client_failure_code(
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
        throw http_client_error(make_error_code(http_client_errc::io_error), eof_message);
    }
    throw http_client_error(http_client_transport_error(result), error_code_text(result.error_));
}

}  // namespace ruvia::detail
