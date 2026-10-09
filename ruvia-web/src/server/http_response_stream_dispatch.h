#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/task.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/streaming.h"

#include "http/streaming_access.h"
#include "router/route_table.h"

namespace ruvia {
class context;  // only used as context* in a type-erased bind thunk; web supplies the definition
}

namespace ruvia::detail {

template <typename sink>
task<void> response_stream_write_thunk(void* target, std::string_view chunk) {
    co_await static_cast<sink*>(target)->write(chunk);
}

template <typename sink>
task<void> response_stream_end_thunk(void* target, std::span<const http_header_view> trailers) {
    co_await static_cast<sink*>(target)->end(trailers);
}

template <typename sink>
task<timer_sleep_result> response_stream_sleep_thunk(
    void* target, std::chrono::milliseconds duration, const stop_token& stop_token_value) {
    co_return co_await static_cast<sink*>(target)->sleep(duration, stop_token_value);
}

template <typename sink>
bool response_stream_aborted_thunk(void* target) noexcept {
    return static_cast<sink*>(target)->aborted();
}

template <typename sink>
void response_stream_bind_context_thunk(
    void* target, context* context_value, task<http_response> (*streaming_head)(context&)) {
    static_cast<sink*>(target)->bind_context(context_value, streaming_head);
}

template <typename sink>
void response_stream_release_context_thunk(void* target) noexcept {
    static_cast<sink*>(target)->release_context();
}

template <typename sink>
bool response_stream_committed_thunk(void* target) noexcept {
    return static_cast<sink*>(target)->committed();
}

template <typename sink>
[[nodiscard]] response_stream_writer make_response_stream_writer(
    sink& sink_value, std::pmr::memory_resource& resource) noexcept {
    return streaming_access::make_response_stream_writer(resource, &sink_value, &response_stream_write_thunk<sink>,
        &response_stream_end_thunk<sink>, &response_stream_sleep_thunk<sink>,
        &response_stream_bind_context_thunk<sink>, &response_stream_release_context_thunk<sink>,
        &response_stream_committed_thunk<sink>, &response_stream_aborted_thunk<sink>);
}

class response_stream_completed final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class response_stream_dispatch_result;

    explicit constexpr response_stream_completed(http_status_code status) noexcept
        : status_(status) {}

    http_status_code status_;
};

class response_stream_peer_aborted_before_commit final {
private:
    friend class response_stream_dispatch_result;

    constexpr response_stream_peer_aborted_before_commit() noexcept = default;
};

class response_stream_peer_aborted_after_commit final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class response_stream_dispatch_result;

    explicit constexpr response_stream_peer_aborted_after_commit(http_status_code status) noexcept
        : status_(status) {}

    http_status_code status_;
};

// The handler failed after its head was already on the wire. The status is what
// the peer was told; the exception is why it will never receive the rest. The
// status alone cannot be reported to anyone -- it says 200 -- so the exception
// travels with it to the transport, which is the last owner able to report it.
class response_stream_failed_after_commit final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    // Never null.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }

private:
    friend class response_stream_dispatch_result;

    response_stream_failed_after_commit(http_status_code status, std::exception_ptr exception) noexcept
        : status_(status),
          exception_(std::move(exception)) {}

    http_status_code status_;
    std::exception_ptr exception_;
};

class response_stream_route_response final {
public:
    [[nodiscard]] http_response take_response() && noexcept {
        return std::move(response_);
    }

private:
    friend class response_stream_dispatch_result;

    explicit response_stream_route_response(http_response response) noexcept
        : response_(std::move(response)) {}

    http_response response_;
};

class response_stream_recovered_failure final {
public:
    [[nodiscard]] http_response take_response() && noexcept {
        return std::move(response_);
    }

private:
    friend class response_stream_dispatch_result;

    explicit response_stream_recovered_failure(http_response response) noexcept
        : response_(std::move(response)) {}

    http_response response_;
};

// Each route terminal is one alternative. Commit-bearing alternatives own the
// exact wire status, response-bearing alternatives own the only movable response,
// and a pre-commit peer abort owns neither. HTTP/1 and HTTP/2 need no second
// outcome discriminator.
class response_stream_dispatch_result final {
public:
    [[nodiscard]] static response_stream_dispatch_result make_completed(
        http_status_code status) noexcept {
        return response_stream_dispatch_result(response_stream_completed(status));
    }

    [[nodiscard]] static response_stream_dispatch_result make_peer_aborted_before_commit() noexcept {
        return response_stream_dispatch_result(response_stream_peer_aborted_before_commit{});
    }

    [[nodiscard]] static response_stream_dispatch_result make_peer_aborted_after_commit(
        http_status_code status) noexcept {
        return response_stream_dispatch_result(response_stream_peer_aborted_after_commit(status));
    }

    [[nodiscard]] static response_stream_dispatch_result make_failed_after_commit(
        http_status_code status, std::exception_ptr exception) noexcept {
        return response_stream_dispatch_result(
            response_stream_failed_after_commit(status, std::move(exception)));
    }

    [[nodiscard]] static response_stream_dispatch_result make_route_response(
        http_response response) noexcept {
        return response_stream_dispatch_result(response_stream_route_response(std::move(response)));
    }

    [[nodiscard]] static response_stream_dispatch_result make_recovered_failure(
        http_response response) noexcept {
        return response_stream_dispatch_result(response_stream_recovered_failure(std::move(response)));
    }

    [[nodiscard]] const response_stream_completed* completed() const& noexcept {
        return std::get_if<response_stream_completed>(&value_);
    }
    const response_stream_completed* completed() const&& = delete;

    [[nodiscard]] const response_stream_peer_aborted_before_commit* peer_aborted_before_commit()
        const& noexcept {
        return std::get_if<response_stream_peer_aborted_before_commit>(&value_);
    }
    [[nodiscard]] const response_stream_peer_aborted_before_commit* peer_aborted_before_commit() const&& =
        delete;

    [[nodiscard]] const response_stream_peer_aborted_after_commit* peer_aborted_after_commit()
        const& noexcept {
        return std::get_if<response_stream_peer_aborted_after_commit>(&value_);
    }
    const response_stream_peer_aborted_after_commit* peer_aborted_after_commit() const&& = delete;

    [[nodiscard]] const response_stream_failed_after_commit* failed_after_commit() const& noexcept {
        return std::get_if<response_stream_failed_after_commit>(&value_);
    }
    const response_stream_failed_after_commit* failed_after_commit() const&& = delete;

    [[nodiscard]] response_stream_route_response* route_response() & noexcept {
        return std::get_if<response_stream_route_response>(&value_);
    }
    response_stream_route_response* route_response() && = delete;

    [[nodiscard]] response_stream_recovered_failure* recovered_failure() & noexcept {
        return std::get_if<response_stream_recovered_failure>(&value_);
    }
    response_stream_recovered_failure* recovered_failure() && = delete;

    [[nodiscard]] std::optional<http_status_code> committed_status() const noexcept {
        if (const auto* value = completed()) {
            return value->status();
        }
        if (const auto* value = peer_aborted_after_commit()) {
            return value->status();
        }
        if (const auto* value = failed_after_commit()) {
            return value->status();
        }
        return std::nullopt;
    }

private:
    using value_type = std::variant<response_stream_completed, response_stream_peer_aborted_before_commit,
        response_stream_peer_aborted_after_commit, response_stream_failed_after_commit,
        response_stream_route_response, response_stream_recovered_failure>;

    template <typename alternative_type>
    explicit response_stream_dispatch_result(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    value_type value_;
};

template <typename sink>
[[nodiscard]] http_status_code committed_response_stream_status(const sink& sink_value) {
    const auto* plan = sink_value.commit_plan();
    if (plan == nullptr) {
        throw std::logic_error("response stream has no committed protocol plan");
    }
    return plan->response_status();
}

// Drives a response-stream route over an already-constructed sink. peer_aborted
// is a predicate consulted after the handler returns so a transport that can be
// reset out from under the handler (HTTP/2, where the reset flag is a bit-field)
// can distinguish an abort before any final head from one after the commit plan
// exists; HTTP/1 passes a constant-false predicate, which folds away. Connection
// persistence remains a transport concern after this helper returns a buffered
// pre-commit error response.
template <typename sink, typename peer_aborted_type>
task<response_stream_dispatch_result> dispatch_response_stream_with(sink& sink_value, const route_table& routes_value,
    const http_request& request, const resolved_route& route, request_memory& request_memory_value,
    context_services services, peer_aborted_type peer_aborted) {
    auto response_stream = make_response_stream_writer(sink_value, *request_memory_value.upstream_resource());

    std::exception_ptr exception;
    try {
        auto result_value = co_await routes_value.dispatch_response_stream(
            request, route, request_memory_value, response_stream, services);
        if (peer_aborted()) {
            if (!sink_value.committed()) {
                co_return response_stream_dispatch_result::make_peer_aborted_before_commit();
            }
            co_return response_stream_dispatch_result::make_peer_aborted_after_commit(
                committed_response_stream_status(sink_value));
        }
        if (!result_value.has_value()) {
            if (!sink_value.committed()) {
                throw std::logic_error("handled response stream has no committed protocol plan");
            }
            co_return response_stream_dispatch_result::make_completed(
                committed_response_stream_status(sink_value));
        }
        if (sink_value.committed()) {
            throw std::logic_error("buffered stream result followed a committed response head");
        }
        co_return response_stream_dispatch_result::make_route_response(std::move(*result_value));
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception == nullptr) {
        throw std::logic_error("response stream dispatch left no terminal result");
    }
    if (peer_aborted()) {
        if (!sink_value.committed()) {
            co_return response_stream_dispatch_result::make_peer_aborted_before_commit();
        }
        co_return response_stream_dispatch_result::make_peer_aborted_after_commit(
            committed_response_stream_status(sink_value));
    }
    if (sink_value.committed()) {
        // Past the point of no return: the status cannot be changed and no
        // error body can be appended, so the exception is handed to the
        // transport rather than dropped with the connection.
        co_return response_stream_dispatch_result::make_failed_after_commit(
            committed_response_stream_status(sink_value), std::move(exception));
    }
    auto response = co_await routes_value.handle_exception(request, request_memory_value, exception, services);
    co_return response_stream_dispatch_result::make_recovered_failure(std::move(response));
}

}  // namespace ruvia::detail
