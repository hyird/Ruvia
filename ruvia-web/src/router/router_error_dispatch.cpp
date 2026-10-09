#include <algorithm>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/web/detail/router/prefix_fallback.h"
#include "ruvia/web/error.h"
#include "ruvia/web/validation.h"

#include "context/context_access.h"
#include "http/http_error_response.h"
#include "http/unsupported_request_content_coding.h"
#include "router/route_dispatch_services.h"
#include "router/route_table.h"
#include "server/inbound_buffer_resource.h"

// Turning a failed request into a response: the error a thrown exception really
// carries, the metadata that survives onto the response, and the scoped error /
// not-found handlers that get the last word.

namespace ruvia {

namespace {

struct owned_http_error_info;
void assign_exception_error(owned_http_error_info& error_info, const std::exception_ptr& exception);

struct owned_http_error_info final {
    http_error_info info_{};
    std::pmr::string status_text_;
    std::pmr::string code_;
    std::pmr::string message_;
    std::pmr::vector<validation_issue> validation_issues_;

    explicit owned_http_error_info(http_error_info source_value, std::pmr::memory_resource* resource)
        : status_text_(resource),
          code_(resource),
          message_(resource),
          validation_issues_(resource) {
        assign(source_value);
    }

    owned_http_error_info(std::pmr::memory_resource* resource, const std::exception_ptr& exception)
        : owned_http_error_info(http_error_info({.status_ = ruvia::http_status::internal_server_error,
                                    .message_ = "unhandled exception"}),
              resource) {
        assign_exception_error(*this, exception);
    }

    void assign(http_error_info source_value) {
        status_text_.assign(source_value.status_text().data(), source_value.status_text().size());
        code_.assign(source_value.code().data(), source_value.code().size());
        message_.assign(source_value.message().data(), source_value.message().size());
        std::pmr::vector<validation_issue> copied(validation_issues_.get_allocator().resource());
        const auto issues = source_value.validation_issues().first(
            std::min(source_value.validation_issues().size(), max_validation_issues));
        copied.reserve(issues.size());
        for (const auto& issue : issues) {
            copied.push_back(
                detail::validation_issue_access::copy(issue, copied.get_allocator().resource()));
        }
        validation_issues_ = std::move(copied);

        info_ = http_error_info({.status_ = source_value.status(),
            .code_ = code_,
            .message_ = message_,
            .status_text_ = status_text_,
            .validation_issues_ = validation_issues_});
    }
};

void assign_exception_error(owned_http_error_info& error_info, const std::exception_ptr& exception) {
    try {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
    } catch (const validation_error& error) {
        error_info.assign(error.info());
    } catch (const detail::unsupported_request_content_coding& error) {
        error_info.assign(http_error_info({.status_ = http_unsupported_content_coding::status(),
            .code_ = "unsupported_content_coding",
            .message_ = error.what()}));
    } catch (const http_error& error) {
        error_info.assign(error.info());
    } catch (const http_protocol_error& error) {
        error_info.assign(http_error_info({.status_ = error.status(), .message_ = error.what()}));
    } catch (const detail::inbound_buffer_limit_error&) {
        error_info.assign(http_error_info({.status_ = ruvia::http_status::service_unavailable,
            .code_ = "inbound_buffer_limit",
            .message_ = "inbound buffer capacity exhausted"}));
    } catch (const blocking_operation_rejected& error) {
        // The blocking pool refused the work or the worker is going away. That
        // is capacity, not a bug in the request: answer it like any other
        // overload instead of a 500. The message is the framework's own and
        // names no application internals.
        error_info.assign(http_error_info({.status_ = ruvia::http_status::service_unavailable,
            .code_ = "blocking_pool_unavailable",
            .message_ = error.what()}));
    } catch (const std::exception&) {
        // An unexpected exception (e.g. a database/library error) may carry
        // internal detail -- table names, query fragments, file paths. Do NOT echo
        // what() to the client: normalize_error renders a generic "Internal Server
        // Error" body. The exception_ptr is still set on the context, so an on_error
        // handler can log or inspect the full detail server-side.
        error_info.assign(http_error_info({.status_ = ruvia::http_status::internal_server_error}));
    } catch (...) {
        error_info.assign(http_error_info({.status_ = ruvia::http_status::internal_server_error}));
    }
}

[[nodiscard]] bool is_unsupported_request_content_coding(const std::exception_ptr& exception) noexcept {
    try {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
    } catch (const detail::unsupported_request_content_coding&) {
        return true;
        // Classification only; the caller still owns and dispatches the exception.
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
    return false;
}

void apply_exception_response_metadata(http_response& response, const std::exception_ptr& exception) {
    if (is_unsupported_request_content_coding(exception)) {
        response.header("Accept-Encoding", http_supported_request_content_codings());
    }
}

void apply_automatic_response_headers(
    http_response& response, const detail::context_services& services) {
    if (!services.automatic_alt_svc().empty()) {
        response.header("Alt-Svc", services.automatic_alt_svc());
    }
}

void apply_automatic_response_headers(http_response& response, context& context_value) {
    if (response.header("Alt-Svc").has_value()) {
        return;
    }
    const auto alt_svc = detail::context_access::response_storage(context_value).header("Alt-Svc");
    if (alt_svc.has_value()) {
        response.header("Alt-Svc", *alt_svc);
    }
}

}  // namespace

task<http_response> detail::route_table::handle_error(const http_request& request,
    request_memory& memory, http_error_info error, context_services services) const {
    const auto error_handler = error_handler_for(request.path());
    // Nothing to wrap and no handler: the original allocation-free path.
    if (error_handler == nullptr && unmatched_middleware_count_ == 0) {
        auto response = make_default_error_response(memory.resource(), error);
        apply_automatic_response_headers(response, services);
        co_return response;
    }

    auto context_value = detail::context_access::make(memory, request,
        with_route_handlers(services, *this, error_handler, not_found_handler_for(request.path())));
    auto terminal = [this, error, error_handler](context& terminal_context) -> task<http_response> {
        if (error_handler == nullptr) {
            co_return make_default_error_response(terminal_context.arena(), error);
        }
        co_return co_await handle_error(terminal_context, error);
    };
    const auto terminal_ref = make_callable_ref<http_response, context&>(terminal);
    auto response = co_await run_unmatched_chain(context_value, terminal_ref);
    apply_automatic_response_headers(response, context_value);
    co_return response;
}

task<http_response> detail::route_table::handle_exception(const http_request& request,
    request_memory& memory, std::exception_ptr exception, context_services services) const {
    if (error_handler_for(request.path()) == nullptr) {
        owned_http_error_info error_info(memory.resource(), exception);
        auto response = make_default_error_response(memory.resource(), error_info.info_);
        apply_exception_response_metadata(response, exception);
        apply_automatic_response_headers(response, services);
        co_return response;
    }

    auto context_value = detail::context_access::make(memory, request,
        with_route_handlers(
            services, *this, error_handler_for(request.path()), not_found_handler_for(request.path())));
    co_return co_await handle_exception(context_value, exception);
}

task<http_response> detail::route_table::handle_error(context& context_value, http_error_info error) const {
    auto response = co_await invoke_error_handler(
        context_value, error, error_handler_for(detail::context_access::request(context_value).path()));
    apply_automatic_response_headers(response, context_value);
    co_return response;
}

task<http_response> detail::route_table::handle_not_found(
    const http_request& request, request_memory& memory, context_services services) const {
    const auto not_found_handler = not_found_handler_for(request.path());
    if (not_found_handler == nullptr && unmatched_middleware_count_ == 0) {
        auto response = make_default_error_response(memory.resource(),
            http_error_info({.status_ = ruvia::http_status::not_found, .message_ = "route not found"}));
        apply_automatic_response_headers(response, services);
        co_return response;
    }

    auto context_value = detail::context_access::make(memory, request,
        with_route_handlers(services, *this, error_handler_for(request.path()), not_found_handler));

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
    const auto terminal_ref = make_callable_ref<http_response, context&>(terminal);
    auto response = co_await run_unmatched_chain(context_value, terminal_ref);
    apply_automatic_response_headers(response, context_value);
    co_return response;
}

task<http_response> detail::route_table::handle_exception(
    context& context_value, std::exception_ptr exception) const {
    detail::context_access::set_error(context_value, exception);
    owned_http_error_info error_info(context_value.arena(), exception);

    auto response = co_await handle_error(context_value, error_info.info_);
    apply_exception_response_metadata(response, exception);
    co_return response;
}

// Which handler answers a failure on a given path: the most specific prefix
// scope that covers it, or the table-wide fallback. The dispatch above is the
// only caller.

void detail::route_table::set_error_handler(http_error_handler_ref_type handler) noexcept {
    error_handler_ = handler;
}

void detail::route_table::set_not_found_handler(http_not_found_handler_ref_type handler) noexcept {
    not_found_handler_ = handler;
}

namespace {

template <typename stored_type, typename registration_type>
void replace_prefix_handlers(std::pmr::vector<stored_type>& stored, std::pmr::memory_resource* resource,
    std::span<const registration_type> handlers) {
    std::pmr::vector<stored_type> normalized(resource);
    normalized.reserve(handlers.size());
    for (const auto& registration : handlers) {
        if (registration.handler_ == nullptr) {
            throw std::invalid_argument("fallback handler must not be null");
        }
        const auto prefix = detail::normalize_fallback_prefix(registration.prefix_);
        for (const auto& existing : normalized) {
            if (std::string_view(existing.prefix_) == prefix) {
                throw std::invalid_argument("duplicate fallback prefix");
            }
        }
        normalized.emplace_back(resource, prefix, registration.handler_);
    }
    // Longest prefix first: selection is a first-match scan. Equal lengths
    // cannot nest, so their relative order is irrelevant; keep it stable.
    std::ranges::stable_sort(normalized, [](const stored_type& left, const stored_type& right) noexcept {
        return left.prefix_.size() > right.prefix_.size();
    });
    stored = std::move(normalized);
}

// Longest-first stored order: the first hit is the tightest scope. A prefix
// matches on whole path segments only, so "/api" scopes "/api" and "/api/x"
// but never "/apix".
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

void detail::route_table::set_prefix_error_handlers(std::span<const http_prefix_error_handler> handlers) {
    replace_prefix_handlers(prefix_error_handlers_, resource_, handlers);
}

void detail::route_table::set_prefix_not_found_handlers(
    std::span<const http_prefix_not_found_handler> handlers) {
    replace_prefix_handlers(prefix_not_found_handlers_, resource_, handlers);
}

detail::http_error_handler_ref_type detail::route_table::error_handler_for(
    std::string_view path) const noexcept {
    const auto handler = select_prefix_handler(prefix_error_handlers_, path);
    return handler != nullptr ? handler : error_handler_;
}

detail::http_not_found_handler_ref_type detail::route_table::not_found_handler_for(
    std::string_view path) const noexcept {
    const auto handler = select_prefix_handler(prefix_not_found_handlers_, path);
    return handler != nullptr ? handler : not_found_handler_;
}

}  // namespace ruvia
