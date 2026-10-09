#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/context.h"
#include "ruvia/web/session.h"

#include "context_services_fixture.h"
#include "http/session_access.h"
#include "router/route_table.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "server/http_response_stream_dispatch.h"
#include "server/http_response_stream_sink.h"
#include "test_harness.h"

namespace {

using ruvia::context;
using ruvia::http_header_view;
using ruvia::http_known_method;
using ruvia::http_response;
using ruvia::http_response_coding_selection;
using ruvia::http_response_stream_commit_plan;
using ruvia::http_response_stream_framing;
using ruvia::http_response_stream_kind;
using ruvia::http_response_trailer_intent;
using ruvia::task;
using ruvia::detail::controller_middleware_descriptor;
using ruvia::detail::response_stream_dispatch_result;
using ruvia::detail::route_stream_handler_type;

struct borrow_test_stream final {};

struct borrow_test_scanner_entry final {
    void touch() noexcept {}
};

using http1_borrow_test_sink_type =
    ruvia::detail::response_stream_sink<borrow_test_stream, borrow_test_scanner_entry>;

class capturing_stream_sink final {
public:
    using streaming_head_thunk_type = task<http_response> (*)(context&);

    explicit capturing_stream_sink(bool fail_uncommitted_end) noexcept
        : fail_uncommitted_end_(fail_uncommitted_end) {}

    void bind_context(context* context_value, streaming_head_thunk_type streaming_head) noexcept {
        context_ = context_value;
        streaming_head_ = streaming_head;
    }

    void release_context() noexcept {
        context_ = nullptr;
        streaming_head_ = nullptr;
    }

    [[nodiscard]] bool committed() const noexcept {
        return commit_plan_.has_value();
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const noexcept {
        return commit_plan_.has_value() ? &*commit_plan_ : nullptr;
    }

    [[nodiscard]] bool aborted() const noexcept {
        return false;
    }

    task<void> write(std::string_view chunk) {
        if (!chunk.empty()) {
            co_await commit(http_response_trailer_intent::none);
        }
        co_return;
    }

    task<void> end(std::span<const http_header_view> trailers) {
        if (fail_uncommitted_end_ && !commit_plan_.has_value()) {
            throw std::runtime_error("peer aborted before the test sink committed a final head");
        }
        co_await commit(trailers.empty() ? http_response_trailer_intent::none : http_response_trailer_intent::present);
        co_return;
    }

    task<ruvia::timer_sleep_result> sleep(std::chrono::milliseconds, const ruvia::stop_token&) {
        co_return ruvia::timer_sleep_result::elapsed;
    }

private:
    task<void> commit(http_response_trailer_intent trailer_intent) {
        if (commit_plan_.has_value()) {
            co_return;
        }
        if (context_ == nullptr || streaming_head_ == nullptr) {
            throw std::logic_error("test response stream context is not bound");
        }
        const auto response = co_await streaming_head_(*context_);
        commit_plan_.emplace(
            ruvia::plan_http_response_stream_commit(http_response_stream_framing::http1_chunked,
                http_known_method::get, response.status(), trailer_intent));
    }

    context* context_{nullptr};
    streaming_head_thunk_type streaming_head_{nullptr};
    std::optional<http_response_stream_commit_plan> commit_plan_;
    bool fail_uncommitted_end_{false};
};

task<void> stream_with_status(void* target, context& context_value) {
    context_value.status(*static_cast<const ruvia::http_status_code*>(target));
    co_await context_value.stream().write("payload");
}

task<void> stream_without_commit(void* target, context& context_value) {
    context_value.status(*static_cast<const ruvia::http_status_code*>(target));
    co_return;
}

task<void> fail_after_commit(void* target, context& context_value) {
    context_value.status(*static_cast<const ruvia::http_status_code*>(target));
    co_await context_value.stream().write("partial");
    throw std::runtime_error("stream failed after commit");
}

[[nodiscard]] std::pmr::string route_path(std::string_view value) {
    return std::pmr::string(value, std::pmr::get_default_resource());
}

[[nodiscard]] response_stream_dispatch_result dispatch_stream(
    route_stream_handler_type handler, bool peer_aborted) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    impl.register_response_stream_route(http_known_method::get, route_path("/stream"), handler,
        std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{});
    impl.finalize();
    const auto& routes_value = impl.route_table();

    ruvia::worker_memory worker;
    ruvia::request_memory request_memory(worker);
    auto [request, parse_error] = ruvia::make_parsed_http_request(
        "GET", "/stream", {}, {}, request_memory.resource());
    if (parse_error.has_value()) {
        throw std::logic_error("test response stream request was invalid");
    }
    const auto resolution = routes_value.resolve(request);
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        throw std::logic_error("test response stream route did not resolve");
    }

    capturing_stream_sink sink(peer_aborted);
    asio::io_context io(1);
    std::optional<response_stream_dispatch_result> result;
    std::exception_ptr exception;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                result.emplace(co_await ruvia::as_awaitable(
                    ruvia::detail::dispatch_response_stream_with(sink, routes_value, request, *resolved,
                        request_memory, ruvia::test::test_context_services(),
                        [peer_aborted]() noexcept { return peer_aborted; })));
            } catch (...) {
                exception = std::current_exception();
            }
        },
        asio::detached);
    io.run();
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (!result.has_value()) {
        throw std::logic_error("test response stream dispatch produced no result");
    }
    return std::move(*result);
}

}  // namespace

RUVIA_TEST(response_stream_dispatch_preserves_exact_committed_status) {
    ruvia::http_status_code status = ruvia::http_status::multi_status;
    auto result_value = dispatch_stream(route_stream_handler_type(&status, &stream_with_status), false);
    const auto* completed = result_value.completed();
    RUVIA_CHECK(completed != nullptr);
    if (completed != nullptr) {
        RUVIA_CHECK_EQ(completed->status(), status);
    }
    RUVIA_CHECK(result_value.route_response() == nullptr);
    RUVIA_CHECK(result_value.recovered_failure() == nullptr);
}

RUVIA_TEST(response_stream_dispatch_distinguishes_precommit_peer_abort) {
    ruvia::http_status_code status = ruvia::http_status::accepted;
    auto result_value = dispatch_stream(route_stream_handler_type(&status, &stream_without_commit), true);
    RUVIA_CHECK(result_value.peer_aborted_before_commit() != nullptr);
    RUVIA_CHECK(!result_value.committed_status().has_value());
}

RUVIA_TEST(response_stream_dispatch_distinguishes_committed_peer_abort) {
    ruvia::http_status_code status = ruvia::http_status::partial_content;
    auto result_value = dispatch_stream(route_stream_handler_type(&status, &stream_with_status), true);
    const auto* aborted = result_value.peer_aborted_after_commit();
    RUVIA_CHECK(aborted != nullptr);
    if (aborted != nullptr) {
        RUVIA_CHECK_EQ(aborted->status(), status);
    }
    RUVIA_CHECK(result_value.peer_aborted_before_commit() == nullptr);
}

RUVIA_TEST(response_stream_dispatch_end_commits_bodyless_status) {
    ruvia::http_status_code status = ruvia::http_status::no_content;
    auto result_value = dispatch_stream(route_stream_handler_type(&status, &stream_without_commit), false);
    const auto* completed = result_value.completed();
    RUVIA_CHECK(completed != nullptr);
    RUVIA_CHECK(result_value.peer_aborted_before_commit() == nullptr);
    RUVIA_CHECK(result_value.route_response() == nullptr);
    RUVIA_CHECK(result_value.recovered_failure() == nullptr);
    if (completed != nullptr) {
        RUVIA_CHECK_EQ(completed->status(), status);
    }
}

RUVIA_TEST(response_stream_dispatch_preserves_committed_failure_status) {
    ruvia::http_status_code status = ruvia::http_status::service_unavailable;
    auto result_value = dispatch_stream(route_stream_handler_type(&status, &fail_after_commit), false);
    const auto* failed = result_value.failed_after_commit();
    RUVIA_CHECK(failed != nullptr);
    if (failed != nullptr) {
        RUVIA_CHECK_EQ(failed->status(), status);
    }
}

RUVIA_TEST(response_stream_dispatch_types_precommit_failure_response) {
    http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::bad_gateway);
    auto result_value = response_stream_dispatch_result::make_recovered_failure(std::move(response));

    auto* recovered_failure = result_value.recovered_failure();
    RUVIA_CHECK(recovered_failure != nullptr);
    RUVIA_CHECK(!result_value.committed_status().has_value());
    RUVIA_CHECK(result_value.peer_aborted_before_commit() == nullptr);
    if (recovered_failure != nullptr) {
        const auto recovered = std::move(*recovered_failure).take_response();
        RUVIA_CHECK_EQ(recovered.status(), ruvia::http_status::bad_gateway);
    }
}

RUVIA_TEST(response_stream_first_write_commits_session_and_freezes_mutations) {
    struct session_observation {
        bool frozen_{false};
        bool retained_{false};
    } state;
    auto handler = [](void* target, context& context_value) -> task<void> {
        auto& observed_value = *static_cast<session_observation*>(target);
        ruvia::detail::session_access::bind(context_value);
        auto session_value = context_value.session();
        session_value.set("user=1");
        co_await context_value.stream().write("first");
        try {
            session_value.set("user=2");
        } catch (const std::logic_error&) {
            observed_value.frozen_ = true;
        }
        co_await context_value.stream().write("second");
        observed_value.retained_ = session_value.data() == "user=1";
    };
    auto result_value = dispatch_stream(route_stream_handler_type(&state, handler), false);
    RUVIA_CHECK(result_value.completed() != nullptr);
    RUVIA_CHECK(state.frozen_);
    RUVIA_CHECK(state.retained_);
}
