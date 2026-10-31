#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/web/detail/router/prefix_fallback.h"
#include "ruvia/web/error.h"

#include "context/context_access.h"
#include "http/http_error_response.h"
#include "http/unsupported_request_content_coding.h"
#include "ratelimit/rate_limit_decision.h"
#include "router/route_table.h"
#include "server/inbound_buffer_resource.h"
#include "server/inbound_transport_error.h"

// Turning a failed request into a response: the error a thrown exception really
// carries, the metadata that survives onto the response, and the scoped error /
// not-found handlers that get the last word.

namespace ruvia {

namespace {

// The error a thrown exception really carries, owned by the request arena so
// an application error handler may suspend while it is read.
struct classified_error final {
    http_error error_;
    // Not an HTTP-mapped failure: answered with a generic 500.
    bool unexpected_{false};
};

[[nodiscard]] classified_error classify_exception(
    std::pmr::memory_resource* resource, const std::exception_ptr& exception) {
    try {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
        return {.error_ = http_error({.status_ = ruvia::http_status::internal_server_error,
                                         .message_ = "unhandled exception"},
                    resource)};
    } catch (const detail::unsupported_request_content_coding& error) {
        return {.error_ = http_error({.status_ = http_unsupported_content_coding::status(),
                                         .code_ = "unsupported_content_coding",
                                         .message_ = error.what()},
                    resource)};
    } catch (const detail::rate_limit_rejected&) {
        const auto error = detail::rate_limit_rejection_error();
        return {.error_ = http_error(
                    {.status_ = error.status(), .code_ = error.code(), .message_ = error.message()}, resource)};
    } catch (const http_error& error) {
        return {.error_ = http_error(error, resource)};
    } catch (const http_protocol_error& error) {
        return {.error_ = http_error(
                    {.status_ = error.status(), .code_ = error.code(), .message_ = error.what()},
                    resource)};
    } catch (const detail::inbound_buffer_limit_error&) {
        return {.error_ = http_error({.status_ = ruvia::http_status::service_unavailable,
                                         .code_ = "inbound_buffer_limit",
                                         .message_ = "inbound buffer capacity exhausted"},
                    resource)};
    } catch (const blocking_operation_rejected& error) {
        // The blocking pool refused the work or the worker is going away. That
        // is capacity, not a bug in the request: answer it like any other
        // overload instead of a 500. The message is the framework's own and
        // names no application internals.
        return {.error_ = http_error({.status_ = ruvia::http_status::service_unavailable,
                                         .code_ = "blocking_pool_unavailable",
                                         .message_ = error.what()},
                    resource)};
    } catch (...) {
        // An unexpected exception (e.g. a database/library error) may carry
        // internal detail -- table names, query fragments, file paths. Do NOT echo
        // what() to the client: an info without a message renders the generic
        // "Internal Server Error" body. The exception_ptr is still set on the context, so an on_error
        // handler can log or inspect the full detail server-side; without one it
        // is reported to stderr (report_unexpected_failure).
        return {.error_ = http_error({.status_ = ruvia::http_status::internal_server_error}, resource),
            .unexpected_ = true};
    }
}

// An unexpected exception answered without an application error handler would
// otherwise leave no server-side trace at all.
void report_unexpected_failure(const classified_error& error_info, const std::exception_ptr& exception,
    detail::http_error_handler_ref error_handler) noexcept {
    if (error_info.unexpected_ && error_handler == nullptr) {
        ruvia::report_unhandled_failure("request handler", exception);
    }
}

// A failed I/O on the connection's own transport -- a handler's interim head
// or the 100 Continue its body read sent to a peer that went away -- is not a
// handler fault and cannot be answered: no response reaches that peer. It is
// not turned into a response but propagated to the session, which ends the
// connection; the connection failure sink then classifies it, so a peer that
// reset is a normal end and anything else is reported once, as itself.
void rethrow_inbound_transport_error(const std::exception_ptr& exception) {
    try {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
    } catch (const detail::inbound_transport_error&) {
        throw;
        // Any other failure is answered below.
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
}

void apply_exception_response_metadata(http_response& response, const std::exception_ptr& exception) {
    try {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
    } catch (const detail::unsupported_request_content_coding& error) {
        response.header_stable_view("Accept-Encoding", error.accepted_codings());
    } catch (const detail::rate_limit_rejected& rejected) {
        detail::apply_rate_limit_rejection_headers(response, rejected.rejection());
        // Classification only; the caller still owns and dispatches the exception.
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
}

}  // namespace

task<http_response> detail::route_table::handle_error(const http_request& request,
    request_memory& memory, http_error_info error, context_services services) const {
    const auto error_handler = error_handler_for(request.path());
    // Nothing to wrap and no handler: the original allocation-free path.
    if (error_handler == nullptr && plan_->unmatched_middleware_count_ == 0) {
        auto response = make_default_error_response(memory.resource(), error);
        co_return response;
    }

    auto context_value = detail::context_access::make(memory, request, services.with_routes(*this));
    auto terminal = [this, error, error_handler](context& terminal_context) -> task<http_response> {
        if (error_handler == nullptr) {
            co_return make_default_error_response(terminal_context.arena(), error);
        }
        co_return co_await handle_error(terminal_context, error, error_handler);
    };
    // Drivers call this to answer a rejection or a recovery outcome, so it must
    // always produce a response: a failure escaping the wrapping middleware
    // chain (for example a middleware that neither responds nor awaits next())
    // is answered the way dispatch answers it, without re-entering the chain.
    std::exception_ptr exception;
    try {
        auto response = co_await run_unmatched_chain(
            context_value, callback_access::bind<task<http_response>(context&)>(terminal));
        co_return response;
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await handle_exception(request, memory, exception, services, error_handler);
}

task<http_response> detail::route_table::handle_exception(const http_request& request,
    request_memory& memory, std::exception_ptr exception, context_services services) const {
    return handle_exception(request, memory, std::move(exception), std::move(services),
        error_handler_for(request.path()));
}

task<http_response> detail::route_table::handle_exception(const http_request& request,
    request_memory& memory, std::exception_ptr exception, context_services services,
    http_error_handler_ref error_handler) const {
    rethrow_inbound_transport_error(exception);
    if (error_handler == nullptr) {
        const auto error_info = classify_exception(memory.resource(), exception);
        report_unexpected_failure(error_info, exception, nullptr);
        auto response = make_default_error_response(memory.resource(), error_info.error_.info());
        apply_exception_response_metadata(response, exception);
        co_return response;
    }

    auto context_value = detail::context_access::make(memory, request, services.with_routes(*this));
    co_return co_await handle_exception(context_value, exception, error_handler);
}

task<http_response> detail::route_table::handle_error(
    context& context_value, http_error_info error, http_error_handler_ref error_handler) const {
    auto response = co_await invoke_error_handler(context_value, error, error_handler);
    co_return response;
}

task<http_response> detail::route_table::handle_not_found(
    const http_request& request, request_memory& memory, context_services services) const {
    const auto not_found_handler = not_found_handler_for(request.path());
    if (not_found_handler == nullptr && plan_->unmatched_middleware_count_ == 0) {
        auto response = make_default_error_response(memory.resource(),
            http_error_info({.status_ = ruvia::http_status::not_found, .message_ = "route not found"}));
        co_return response;
    }

    auto context_value = detail::context_access::make(memory, request, services.with_routes(*this));

    // The handler's own failure keeps going through handle_exception, exactly as
    // before; wrapping it in the chain must not change which layer answers it.
    auto terminal = [this, not_found_handler](context& terminal_context) -> task<http_response> {
        if (not_found_handler == nullptr) {
            co_return make_default_error_response(terminal_context.arena(),
                http_error_info(
                    {.status_ = ruvia::http_status::not_found, .message_ = "route not found"}));
        }
        std::exception_ptr exception;
        try {
            co_return co_await not_found_handler(terminal_context);
        } catch (...) {
            exception = std::current_exception();
        }
        co_return co_await handle_exception(terminal_context, exception);
    };
    auto response =
        co_await run_unmatched_chain(context_value, callback_access::bind<task<http_response>(context&)>(terminal));
    co_return response;
}

task<http_response> detail::route_table::handle_exception(
    context& context_value, std::exception_ptr exception) const {
    return handle_exception(context_value, std::move(exception),
        error_handler_for(detail::context_access::request(context_value).path()));
}

task<http_response> detail::route_table::handle_exception(context& context_value,
    std::exception_ptr exception, http_error_handler_ref error_handler) const {
    rethrow_inbound_transport_error(exception);
    detail::context_access::set_error(context_value, exception);
    const auto error_info = classify_exception(context_value.arena(), exception);
    report_unexpected_failure(error_info, exception, error_handler);

    auto response = co_await handle_error(context_value, error_info.error_.info(), error_handler);
    apply_exception_response_metadata(response, exception);
    co_return response;
}

// Which handler answers a failure on a given path: the most specific prefix
// scope that covers it, or the plan-wide fallback.

namespace {

// Deepest-first stored order: the first hit is the tightest scope. Matching
// follows path_is_under_prefix: whole, percent-decoded path segments, so "/api"
// scopes "/api" and "/api/x" but never "/apix".
template <typename stored_type>
[[nodiscard]] auto select_prefix_handler(
    const std::pmr::vector<stored_type>& stored, std::string_view path) noexcept {
    for (const auto& candidate : stored) {
        const std::string_view prefix(candidate.prefix_);
        if (detail::path_is_under_prefix(path, prefix)) {
            return candidate.handler_;
        }
    }
    return decltype(stored.front().handler_){nullptr};
}

}  // namespace

detail::http_error_handler_ref detail::route_table::error_handler_for(
    std::string_view path) const noexcept {
    const auto handler = select_prefix_handler(plan_->prefix_error_handlers_, path);
    return handler != nullptr ? handler : plan_->error_handler_;
}

detail::http_not_found_handler_ref detail::route_table::not_found_handler_for(
    std::string_view path) const noexcept {
    const auto handler = select_prefix_handler(plan_->prefix_not_found_handlers_, path);
    return handler != nullptr ? handler : plan_->not_found_handler_;
}

}  // namespace ruvia
