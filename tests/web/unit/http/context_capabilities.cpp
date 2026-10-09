#include "context/context_capabilities.h"

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/context.h"

#include "body/http_request_body_facade.h"
#include "context/context_access.h"
#include "context/context_services.h"
#include "context_services_fixture.h"
#include "http/request_body_loader.h"
#include "http/session_access.h"
#include "http/streaming_access.h"
#include "server/request_deadline.h"
#include "test_harness.h"
#include "test_io_context.h"
#include "websocket/websocket_access.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis.h"
#endif

#include <chrono>
#include <concepts>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <asio/io_context.hpp>

namespace {

ruvia::task<std::string_view> load_body(void*) {
    co_return "lazy-body";
}

ruvia::task<void> discard_body(void*) {
    co_return;
}

ruvia::task<std::optional<std::span<const std::byte>>> read_body(void*) {
    co_return std::nullopt;
}

struct output_sink final {
    std::pmr::string scratch_{std::pmr::get_default_resource()};
};

ruvia::task<void> write_output(void*, std::string_view) {
    co_return;
}

ruvia::task<void> end_output(void*, std::span<const ruvia::http_header_view>) {
    co_return;
}

ruvia::task<ruvia::timer_sleep_result> sleep_output(
    void*, std::chrono::milliseconds, const ruvia::stop_token&) {
    co_return ruvia::timer_sleep_result::elapsed;
}

void bind_output(void*, ruvia::context*, ruvia::task<ruvia::http_response> (*)(ruvia::context&)) noexcept {}

bool output_false(void*) noexcept {
    return false;
}

void release_output_context(void*) noexcept {}

ruvia::response_stream_writer make_response_stream_writer(output_sink& sink_value) noexcept {
    return ruvia::detail::streaming_access::make_response_stream_writer(*ruvia::detail::process_resource(), &sink_value, &write_output, &end_output,
        &sleep_output, &bind_output, &release_output_context, &output_false, &output_false);
}

ruvia::task<std::optional<ruvia::websocket_message>> read_websocket(void*) {
    co_return std::nullopt;
}

ruvia::task<void> write_websocket(void*, ruvia::websocket_opcode, std::string_view, bool) {
    co_return;
}

ruvia::task<void> close_websocket(void*, ruvia::websocket_close_options) {
    co_return;
}

ruvia::http_request make_request(std::pmr::memory_resource* resource) {
    auto [request, parse_error] = ruvia::make_parsed_http_request("GET", "/", {}, {}, resource);
    if (parse_error) {
        throw std::logic_error("invalid context capability test request");
    }
    return std::move(request);
}

struct bound_body_reader final {
    explicit bound_body_reader(int value) noexcept
        : value_(value) {}

    ruvia::task<std::optional<std::span<const std::byte>>> read() {
        co_return std::nullopt;
    }

    int value_;
};

struct bound_body_loader final {
    explicit bound_body_loader(int value) noexcept
        : value_(value) {}

    ruvia::task<std::string_view> read_all() {
        co_return std::string_view{};
    }
    ruvia::task<void> discard() {
        co_return;
    }

    int value_;
};

}  // namespace

RUVIA_TEST(request_body_capability_binding_constructs_target_and_facade_atomically) {
    ruvia::detail::body_reader_binding<bound_body_reader> reader_value(17);
    ruvia::detail::request_body_loader_binding<bound_body_loader> loader(23);

    RUVIA_CHECK_EQ(reader_value.reader().value_, 17);
    RUVIA_CHECK_EQ(loader.loader().value_, 23);

    const auto base = ruvia::test::test_context_services();
    const auto streaming = base.with_streaming_request_body(reader_value.facade());
    const auto lazy = base.with_lazy_request_body(loader.facade());
    RUVIA_CHECK(&streaming.request_body_source().streaming()->reader() == &reader_value.facade());
    RUVIA_CHECK(&lazy.request_body_source().lazy()->loader() == &loader.facade());
}

RUVIA_TEST(context_request_body_source_has_one_active_alternative) {
    ruvia::detail::request_body_loader loader(nullptr, &load_body, &discard_body);
    std::optional<ruvia::body_reader> reader;
    ruvia::detail::streaming_access::emplace_body_reader(reader, nullptr, &read_body);

    const auto base = ruvia::test::test_context_services();
    RUVIA_CHECK(base.request_body_source().buffered() != nullptr);
    RUVIA_CHECK(base.request_body_source().lazy() == nullptr);
    RUVIA_CHECK(base.request_body_source().streaming() == nullptr);

    const auto lazy = base.with_lazy_request_body(loader);
    RUVIA_CHECK(lazy.request_body_source().buffered() == nullptr);
    RUVIA_CHECK(lazy.request_body_source().lazy() != nullptr);
    RUVIA_CHECK(lazy.request_body_source().streaming() == nullptr);
    RUVIA_CHECK(&lazy.request_body_source().lazy()->loader() == &loader);

    const auto streaming = lazy.with_streaming_request_body(*reader);
    RUVIA_CHECK(streaming.request_body_source().buffered() == nullptr);
    RUVIA_CHECK(streaming.request_body_source().lazy() == nullptr);
    RUVIA_CHECK(streaming.request_body_source().streaming() != nullptr);
    RUVIA_CHECK(&streaming.request_body_source().streaming()->reader() == &*reader);

    // Functional service refinement must not mutate either earlier value.
    RUVIA_CHECK(base.request_body_source().buffered() != nullptr);
    RUVIA_CHECK(lazy.request_body_source().lazy() != nullptr);
}

RUVIA_TEST(context_services_borrows_address_stable_worker_and_stop_token) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto handle = attachment.loop().handle();
    const ruvia::stop_token stop_token;
    const ruvia::detail::context_services services(handle, stop_token);
    const auto derived = services.with_plain_transport("127.0.0.1");
    RUVIA_CHECK(&services.worker() == &handle);
    RUVIA_CHECK(&derived.worker() == &handle);
    RUVIA_CHECK(&services.get_stop_token() == &stop_token);
    RUVIA_CHECK(&derived.get_stop_token() == &stop_token);
}

RUVIA_TEST(context_services_rejects_an_invalid_worker_binding) {
    const ruvia::worker_handle worker;
    const ruvia::stop_token stop_token;
    bool rejected = false;
    try {
        const ruvia::detail::context_services services(worker, stop_token);
        static_cast<void>(services);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(context_rejects_unconfigured_worker_clients_consistently) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto request = make_request(memory.resource());
    auto context_value =
        ruvia::detail::context_access::make(memory, request, ruvia::test::test_context_services());

    bool http_client_rejected = false;
    try {
        static_cast<void>(context_value.get_http_client());
    } catch (const ruvia::http_client_error& error) {
        http_client_rejected = error.code() == ruvia::http_client_error::code_type::not_configured;
    }
    RUVIA_CHECK(http_client_rejected);

#ifdef RUVIA_ENABLE_DATABASE
    bool database_rejected = false;
    try {
        static_cast<void>(context_value.db());
    } catch (const ruvia::db_error& error) {
        database_rejected = error.code() == ruvia::db_error::code_type::not_configured;
    }
    RUVIA_CHECK(database_rejected);
#endif

#ifdef RUVIA_ENABLE_REDIS
    bool redis_rejected = false;
    try {
        static_cast<void>(context_value.redis());
    } catch (const ruvia::redis_error& error) {
        redis_rejected = error.code() == ruvia::redis_error::code_type::not_configured;
    }
    RUVIA_CHECK(redis_rejected);
#endif
}

RUVIA_TEST(context_session_capability_requires_explicit_middleware_binding) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto request = make_request(memory.resource());
    auto context_value =
        ruvia::detail::context_access::make(memory, request, ruvia::test::test_context_services());

    RUVIA_CHECK(!context_value.try_session().has_value());
    bool rejected = false;
    try {
        static_cast<void>(context_value.session());
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);

    ruvia::detail::session_access::bind(context_value);
    auto session_value = context_value.session();
    session_value.set("user=42");
    RUVIA_CHECK_EQ(session_value.data(), std::string_view("user=42"));
    RUVIA_CHECK(context_value.try_session().has_value());
}

RUVIA_TEST(context_exposes_the_server_shutdown_stop_token) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto request = make_request(memory.resource());
    ruvia::stop_source source;
    const auto token = source.token();
    const ruvia::detail::context_services services(ruvia::test::test_worker_handle(), token);
    const auto context_value = ruvia::detail::context_access::make(memory, request, services);

    RUVIA_CHECK(context_value.get_stop_token().stoppable());
    RUVIA_CHECK(!context_value.get_stop_token().stop_requested());
    source.request_stop();
    RUVIA_CHECK(context_value.get_stop_token().stop_requested());
}

RUVIA_TEST(context_services_bind_request_deadline_and_stop_token_atomically) {
    ruvia::stop_source worker_stop;
    const auto worker_token = worker_stop.token();
    const ruvia::detail::context_services base(ruvia::test::test_worker_handle(), worker_token);
    ruvia::detail::request_deadline deadline(worker_token);

    const auto request = base.with_request_deadline(deadline);

    RUVIA_CHECK(request.request_deadline() == &deadline);
    RUVIA_CHECK(&request.get_stop_token() == &deadline.token());
    RUVIA_CHECK(base.request_deadline() == nullptr);
    RUVIA_CHECK(&base.get_stop_token() == &worker_token);
}

RUVIA_TEST(context_response_output_has_one_active_alternative) {
    output_sink sink;
    auto writer = make_response_stream_writer(sink);
    const auto worker_handle_value = ruvia::test::test_worker_handle();
    auto websocket_value = ruvia::detail::websocket_access::make(
        *ruvia::detail::process_resource(), worker_handle_value, nullptr, &read_websocket, &write_websocket, &close_websocket);

    const auto base = ruvia::test::test_context_services();
    RUVIA_CHECK(base.response_output().buffered() != nullptr);
    RUVIA_CHECK(base.response_output().response_stream() == nullptr);
    RUVIA_CHECK(base.response_output().get_websocket() == nullptr);

    const auto streaming = base.with_response_stream(writer);
    RUVIA_CHECK(streaming.response_output().buffered() == nullptr);
    RUVIA_CHECK(streaming.response_output().response_stream() != nullptr);
    RUVIA_CHECK(streaming.response_output().get_websocket() == nullptr);
    RUVIA_CHECK(&streaming.response_output().response_stream()->writer() == &writer);

    const auto websocket_output = ruvia::detail::context_response_output::websocket_value(websocket_value);
    RUVIA_CHECK(websocket_output.buffered() == nullptr);
    RUVIA_CHECK(websocket_output.response_stream() == nullptr);
    RUVIA_CHECK(websocket_output.get_websocket() != nullptr);
    RUVIA_CHECK(&websocket_output.get_websocket()->get_websocket() == &websocket_value);

    RUVIA_CHECK(base.response_output().buffered() != nullptr);
    RUVIA_CHECK(streaming.response_output().response_stream() != nullptr);
}

RUVIA_TEST(context_applies_listener_alt_svc_and_allows_application_override_or_removal) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto request = make_request(memory.resource());
    constexpr std::string_view automatic = "h3=\":443\"; ma=86400";
    auto context_value = ruvia::detail::context_access::make(memory, request,
        ruvia::test::test_context_services().with_automatic_alt_svc(automatic));

    const auto default_response = context_value.text("default");
    RUVIA_CHECK_EQ(default_response.header("Alt-Svc"), automatic);

    context_value.header("Alt-Svc", "clear");
    const auto overridden = context_value.text("override");
    RUVIA_CHECK_EQ(overridden.header("alt-svc"), std::string_view("clear"));

    context_value.remove_header("ALT-SVC");
    const auto removed = context_value.text("removed");
    RUVIA_CHECK(!removed.header("Alt-Svc").has_value());
}

RUVIA_TEST(context_copies_typed_capabilities_into_public_facades) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto request = make_request(memory.resource());

    std::optional<ruvia::body_reader> reader;
    ruvia::detail::streaming_access::emplace_body_reader(reader, nullptr, &read_body);
    auto body_context = ruvia::detail::context_access::make(
        memory, request, ruvia::test::test_context_services().with_streaming_request_body(*reader));
    RUVIA_CHECK(&body_context.req().get_body_reader() == &*reader);

    output_sink sink;
    auto writer = make_response_stream_writer(sink);
    auto stream_context = ruvia::detail::context_access::make(
        memory, request, ruvia::test::test_context_services().with_response_stream(writer));
    RUVIA_CHECK(&stream_context.stream() == &writer);
    (void)stream_context.stream_sse();
    const auto sse_head = ruvia::detail::context_access::streaming_head(stream_context);
    RUVIA_CHECK_EQ(sse_head.header("Content-Type"), std::string_view("text/event-stream"));
    RUVIA_CHECK_EQ(sse_head.header("Cache-Control"), std::string_view("no-cache"));

    const auto worker_handle_value = ruvia::test::test_worker_handle();
    auto websocket_value = ruvia::detail::websocket_access::make(
        *ruvia::detail::process_resource(), worker_handle_value, nullptr, &read_websocket, &write_websocket, &close_websocket);
    auto websocket_context =
        ruvia::detail::context_access::make(memory, request, ruvia::test::test_context_services());
    {
        ruvia::detail::context_websocket_binding binding(websocket_context, websocket_value);
        RUVIA_CHECK(&websocket_context.get_websocket() == &websocket_value);
    }
    bool unavailable_after_scope = false;
    try {
        (void)websocket_context.get_websocket();
    } catch (const std::logic_error&) {
        unavailable_after_scope = true;
    }
    RUVIA_CHECK(unavailable_after_scope);
}

RUVIA_TEST(context_websocket_binding_restores_capability_during_unwind) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto request = make_request(memory.resource());
    auto context_value =
        ruvia::detail::context_access::make(memory, request, ruvia::test::test_context_services());
    const auto worker_handle_value = ruvia::test::test_worker_handle();
    auto websocket_value = ruvia::detail::websocket_access::make(
        *ruvia::detail::process_resource(), worker_handle_value, nullptr, &read_websocket, &write_websocket, &close_websocket);

    try {
        ruvia::detail::context_websocket_binding binding(context_value, websocket_value);
        RUVIA_CHECK(&context_value.get_websocket() == &websocket_value);
        throw std::runtime_error("leave websocket scope");
    } catch (const std::runtime_error&) {
    }

    bool unavailable_after_unwind = false;
    try {
        (void)context_value.get_websocket();
    } catch (const std::logic_error&) {
        unavailable_after_unwind = true;
    }
    RUVIA_CHECK(unavailable_after_unwind);
}

RUVIA_TEST(context_request_exposes_matched_route_path) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto request = make_request(memory.resource());
    auto context_value = ruvia::detail::context_access::make(
        memory, request, "/items/:id", 0, ruvia::test::test_context_services());

    const auto facade = context_value.req();
    RUVIA_CHECK_EQ(facade.route_path(), std::string_view("/items/:id"));
}

RUVIA_TEST(context_lazy_request_caches_share_one_typed_storage_owner) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    const ruvia::http_header_view headers[]{{"Cookie", "theme=dark"}};
    auto [request, parse_error] = ruvia::make_parsed_http_request(
        "GET", "/?name=ruvia&name=web", headers, {}, memory.resource());
    RUVIA_CHECK(!parse_error);

    const std::string_view names[]{"id"};
    const std::string_view values[]{"42"};
    auto context_value = ruvia::detail::context_access::make(
        memory, request, "/items/:id", names, values, 1, 0, ruvia::test::test_context_services());
    const auto* const owner_value = ruvia::detail::context_access::request_storage(context_value);
    RUVIA_CHECK(owner_value != nullptr);
    RUVIA_CHECK(!ruvia::detail::context_access::request_cookies_materialized(context_value));
    RUVIA_CHECK(!ruvia::detail::context_access::request_query_materialized(context_value));
    RUVIA_CHECK(!ruvia::detail::context_access::route_params_materialized(context_value));

    (void)context_value.req().header_fields();
    (void)context_value.req().query_fields();
    (void)context_value.req().cookie_fields();
    (void)context_value.req().param_fields();
    RUVIA_CHECK(ruvia::detail::context_access::request_storage(context_value) == owner_value);
    RUVIA_CHECK(ruvia::detail::context_access::request_cookies_materialized(context_value));
    RUVIA_CHECK(ruvia::detail::context_access::request_query_materialized(context_value));
    RUVIA_CHECK(ruvia::detail::context_access::route_params_materialized(context_value));
}
