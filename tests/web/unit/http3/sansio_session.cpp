#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/bytes.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_local_critical_streams.h"

#include "http3/http3_sans_io_session_engine.h"
#include "http3/http3_server_body_budget.h"
#include "memory_resource_fixture.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "routing_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

class switchable_counting_memory_resource final : public std::pmr::memory_resource {
public:
    void reject_allocations(bool value = true) noexcept {
        rejecting_ = value;
    }

    [[nodiscard]] std::size_t allocation_count() const noexcept {
        return allocation_count_;
    }

    [[nodiscard]] std::size_t deallocation_count() const noexcept {
        return deallocation_count_;
    }

    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return live_allocations_;
    }

    [[nodiscard]] std::size_t rejected_allocation_count() const noexcept {
        return rejected_allocation_count_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (rejecting_) {
            ++rejected_allocation_count_;
            throw std::bad_alloc();
        }
        void* const storage = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++allocation_count_;
        ++live_allocations_;
        return storage;
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
        --live_allocations_;
        ++deallocation_count_;
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool rejecting_{false};
    std::size_t allocation_count_{0};
    std::size_t deallocation_count_{0};
    std::size_t live_allocations_{0};
    std::size_t rejected_allocation_count_{0};
};

std::string frame(std::uint64_t type, std::string_view payload_value) {
    std::array<char, ruvia::http3_frame_header_max_bytes> header_value{};
    const auto encoded = ruvia::encode_http3_frame_header(header_value, type, payload_value.size());
    if ((encoded.index() != 0)) {
        throw std::runtime_error("fixture frame header failed to encode");
    }
    std::string result_value(header_value.data(), std::get<0>(encoded));
    result_value.append(payload_value);
    return result_value;
}

std::string request_headers(ruvia::worker_memory& worker_value, std::string_view method,
    std::string_view path, std::optional<std::uint64_t> body_length = std::nullopt,
    std::span<const ruvia::http3_field_section_field_view> fields = {},
    std::string_view protocol = {}) {
    auto head = ruvia::encode_http3_client_request_head({.method_ = method,
                                                            .scheme_ = "https",
                                                            .authority_ = "example.test",
                                                            .path_ = path,
                                                            .fields_ = fields,
                                                            .body_length_ = body_length,
                                                            .protocol_ = protocol,
                                                            .peer_enable_connect_protocol_ = !protocol.empty()},
        {}, worker_value.resource());
    if ((head.index() != 0)) {
        throw std::runtime_error("fixture request head failed to encode");
    }
    return frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
        std::string_view(std::get<0>(head).field_section_.data(), std::get<0>(head).field_section_.size()));
}

std::string data(std::string_view payload_value) {
    return frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data), payload_value);
}

}  // namespace

RUVIA_TEST(http3_session_routes_extension_method_by_exact_token) {
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    implementation.register_extension_method_route("PROPFIND",
        std::pmr::string("/dav", std::pmr::get_default_resource()),
        ruvia::detail::route_handler_type(nullptr, &routing_test::dummy_handler),
        ruvia::detail::request_body_mode::buffered,
        std::span<const ruvia::detail::controller_middleware_descriptor>{},
        std::span<const ruvia::detail::controller_middleware_descriptor>{});
    implementation.finalize();

    ruvia::worker_memory worker;
    ruvia::detail::http3_sans_io_session_engine session_value(implementation.route_table(), worker);
    const auto head = request_headers(worker, "PROPFIND", "/dav");
    RUVIA_CHECK(session_value.feed(0, head, true).status_ == ruvia::http3_connection_status::message_end);

    const auto* request = session_value.request(0);
    const auto* resolution = session_value.resolution(0);
    RUVIA_CHECK(request != nullptr && request->request().method() == "PROPFIND");
    RUVIA_CHECK(resolution != nullptr && resolution->resolved() != nullptr);
    if (resolution != nullptr && resolution->resolved() != nullptr) {
        RUVIA_CHECK(resolution->resolved()->route().path() == "/dav");
    }
}

RUVIA_TEST(http3_session_maps_websocket_connect_to_websocket_route) {
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    implementation.register_websocket_route(ruvia::http_known_method::get,
        std::pmr::string("/socket", std::pmr::get_default_resource()),
        ruvia::detail::route_stream_handler_type(nullptr, &routing_test::dummy_stream_handler),
        std::span<const ruvia::detail::controller_middleware_descriptor>{},
        std::span<const ruvia::detail::controller_middleware_descriptor>{});
    implementation.finalize();

    ruvia::worker_memory worker;
    ruvia::detail::http3_sans_io_session_engine session_value(implementation.route_table(), worker);
    const auto head = request_headers(worker, "CONNECT", "/socket", std::nullopt, {}, "websocket");
    RUVIA_CHECK(session_value.feed(0, head, true).status_ == ruvia::http3_connection_status::message_end);

    const auto* request = session_value.request(0);
    const auto* resolution = session_value.resolution(0);
    RUVIA_CHECK(request != nullptr && request->request().method() == "CONNECT");
    RUVIA_CHECK(request != nullptr && request->extended_connect_protocol() == "websocket");
    RUVIA_CHECK(resolution != nullptr && resolution->resolved() != nullptr);
    if (resolution != nullptr && resolution->resolved() != nullptr) {
        RUVIA_CHECK(resolution->resolved()->route().endpoint().get_websocket() != nullptr);
    }
}

RUVIA_TEST(http3_server_priority_updates_remain_live_through_request_lease) {
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::get, "/priority");
    implementation.finalize();
    ruvia::worker_memory worker;
    ruvia::detail::http3_sans_io_session_engine session_value(implementation.route_table(), worker);
    auto prefixes = ruvia::http3_local_critical_streams::create({});
    const auto prefix = std::get<0>(prefixes).control_prefix();
    RUVIA_CHECK(session_value.feed(2, std::string_view(prefix.data(), prefix.size())).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(session_value.feed(0, request_headers(worker, "GET", "/priority"), true).status_ == ruvia::http3_connection_status::message_end);
    auto lease_value = session_value.acquire_request(0);
    RUVIA_CHECK(lease_value.has_value());
    if (!lease_value) {
        return;
    }
    const auto* observed_value = session_value.request_priority_update(0);
    RUVIA_CHECK(observed_value && !*observed_value);
    std::array<char, 32> bytes_value{};
    for (const auto urgency : {1, 7, 2}) {
        const auto encoded = ruvia::encode_http3_priority_update(bytes_value, {.element_id_ = 0,
                                                                                  .fields_ = {.urgency_ = static_cast<std::uint8_t>(urgency), .incremental_ = urgency == 7}});
        RUVIA_CHECK((encoded.index() == 0));
        if ((encoded.index() != 0)) {
            return;
        }
        RUVIA_CHECK(session_value.feed(2, std::string_view(bytes_value.data(), std::get<0>(encoded))).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(observed_value == session_value.request_priority_update(0));
        RUVIA_CHECK(observed_value->has_value() && observed_value->value().urgency_ == urgency && observed_value->value().incremental_ == (urgency == 7));
    }
    lease_value.reset();
    RUVIA_CHECK(session_value.release(0));
    RUVIA_CHECK(!session_value.request_priority_update(0));
}

RUVIA_TEST(http3_buffered_sans_io_session_lease_pins_requests_across_reset_and_stop_without_allocation) {
    using engine_type = ruvia::detail::http3_sans_io_session_engine;
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();
    ruvia::test::counting_memory_resource allocations;
    {
        ruvia::worker_memory worker(allocations);
        engine_type session_value(implementation.route_table(), worker);
        const std::string payload_value(64 * 1024, 'p');
        const auto wire = data(std::string_view(payload_value));
        const std::array fields_value{ruvia::http3_field_section_field_view{"x-lease", "retained"}};
        const auto head = request_headers(worker, "POST", "/items", payload_value.size(), fields_value);
        RUVIA_CHECK(session_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(!session_value.acquire_request(0));
        RUVIA_CHECK(session_value.feed(0, wire, true).status_ == ruvia::http3_connection_status::message_end);
        const auto before_acquire = allocations.allocation_count();
        auto initial_value = session_value.acquire_request(0);
        RUVIA_CHECK(initial_value.has_value());
        if (!initial_value) {
            return;
        }
        std::optional<engine_type::request_lease_type> held(std::in_place, std::move(*initial_value));
        initial_value.reset();
        RUVIA_CHECK_EQ(allocations.allocation_count(), before_acquire);
        RUVIA_CHECK(!session_value.acquire_request(0) && !session_value.release(0));
        RUVIA_CHECK(held->resolution().resolved() != nullptr);
        std::optional<std::size_t> cache_baseline;
        for (std::uint64_t step = 1; step <= 32; ++step) {
            const auto id = step * 4;
            RUVIA_CHECK(session_value.feed(id, head).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.feed(id, wire, true).status_ == ruvia::http3_connection_status::message_end);
            auto lease_value = session_value.acquire_request(id);
            RUVIA_CHECK(lease_value.has_value());
            if (!lease_value) {
                return;
            }
            const auto with_result = allocations.live_allocations();
            if (step % 3 != 0) {
                if (step % 3 == 1) {
                    (void)session_value.feed(id, {}, false, true);
                } else {
                    RUVIA_CHECK(session_value.cancel_request(id));
                    RUVIA_CHECK(!session_value.cancel_request(id));
                }
                RUVIA_CHECK(session_value.request(id) != nullptr);
                RUVIA_CHECK(lease_value->request().body_bytes() == payload_value.size());
                lease_value.reset();
                RUVIA_CHECK(session_value.request(id) == nullptr);
            } else {
                lease_value.reset();
                RUVIA_CHECK(!session_value.acquire_request(id));
                RUVIA_CHECK(session_value.release(id));
            }
            // The worker pool may retain blocks for later requests.
            RUVIA_CHECK(allocations.live_allocations() <= with_result);
            if (cache_baseline) {
                RUVIA_CHECK_EQ(allocations.live_allocations(), *cache_baseline);
            } else {
                cache_baseline = allocations.live_allocations();
            }
            const auto bytes_value = held->request().request().body_bytes();
            RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()) == payload_value);
            RUVIA_CHECK(held->request().request().header("x-lease") == "retained");
        }
        const auto invalid = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::settings), {});
        const auto failure = session_value.feed(132, invalid);
        RUVIA_CHECK(failure.scope_ == ruvia::http3_connection_error_scope::connection);
        session_value.stop();
        RUVIA_CHECK(session_value.feed(0, {}).code_ == failure.code_);
        RUVIA_CHECK(session_value.terminated() && session_value.active_stream_count() == 1);
        RUVIA_CHECK(!session_value.acquire_request(0) && !session_value.release(0));
        RUVIA_CHECK(held->request().body_complete() && held->request().body_bytes() == payload_value.size());
        const auto before_return = allocations.live_allocations();
        held.reset();
        RUVIA_CHECK_EQ(session_value.active_stream_count(), std::size_t{0});
        RUVIA_CHECK(allocations.live_allocations() <= before_return);
    }
    RUVIA_CHECK_EQ(allocations.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(allocations.allocation_count(), allocations.deallocation_count());
}

RUVIA_TEST(http3_buffered_sans_io_session_cancels_incomplete_headers_and_bodies_without_disturbing_lease) {
    using engine_type = ruvia::detail::http3_sans_io_session_engine;
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();
    ruvia::test::counting_memory_resource allocations;
    {
        ruvia::worker_memory worker(allocations);
        engine_type session_value(implementation.route_table(), worker, {.max_live_streams_ = 2});
        const auto head = request_headers(worker, "POST", "/items");
        (void)session_value.feed(0, head);
        (void)session_value.feed(0, data("held"), true);
        auto held = session_value.acquire_request(0);
        RUVIA_CHECK(held.has_value());
        if (!held) {
            return;
        }
        const auto wire = data(std::string_view(std::string(65536, 'p')));
        RUVIA_CHECK(!session_value.cancel_request(2) && !session_value.cancel_request(4));
        std::optional<std::size_t> cached;
        for (std::uint64_t step = 1; step <= 16; ++step) {
            const auto id = step * 8;
            RUVIA_CHECK(session_value.feed(id, std::string_view(head).substr(0, 3)).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.request(id) == nullptr);
            RUVIA_CHECK(session_value.cancel_request(id));
            RUVIA_CHECK(!session_value.cancel_request(id));
            RUVIA_CHECK(session_value.feed(id + 4, head).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.feed(id + 4, wire).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(!session_value.acquire_request(id + 4));
            const auto before_cancel = allocations.live_allocations();
            RUVIA_CHECK(session_value.cancel_request(id + 4));
            RUVIA_CHECK(!session_value.cancel_request(id + 4));
            RUVIA_CHECK(allocations.live_allocations() <= before_cancel);
            if (cached) {
                RUVIA_CHECK_EQ(allocations.live_allocations(), *cached);
            } else {
                cached = allocations.live_allocations();
            }
            RUVIA_CHECK_EQ(session_value.active_stream_count(), std::size_t{1});
            const auto body = held->request().request().body_bytes();
            RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()) == "held");
        }
        RUVIA_CHECK(session_value.cancel_request(0));
        RUVIA_CHECK(!session_value.cancel_request(0));
        held.reset();
        RUVIA_CHECK_EQ(session_value.active_stream_count(), std::size_t{0});
        session_value.stop();
        RUVIA_CHECK(!session_value.cancel_request(0));
    }
    RUVIA_CHECK_EQ(allocations.allocation_count(), allocations.deallocation_count());
}

RUVIA_TEST(http3_buffered_sans_io_session_stop_releases_partial_protocol_head_while_lease_survives) {
    using engine_type = ruvia::detail::http3_sans_io_session_engine;
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();
    ruvia::test::counting_memory_resource allocations;
    {
        ruvia::worker_memory worker(allocations);
        engine_type session_value(implementation.route_table(), worker);
        const auto head = request_headers(worker, "POST", "/items");
        (void)session_value.feed(0, head);
        (void)session_value.feed(0, data("held"), true);
        auto lease_value = session_value.acquire_request(0);
        RUVIA_CHECK(lease_value.has_value());
        if (!lease_value) {
            return;
        }
        const std::string value(60000, 'z');
        const std::array fields_value{ruvia::http3_field_section_field_view{"x-large", value}};
        const auto large = request_headers(worker, "POST", "/items", std::nullopt, fields_value);
        RUVIA_CHECK(session_value.feed(4, std::string_view(large).substr(0, large.size() / 2)).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.request(4) == nullptr);
        const auto before_stop = allocations.live_allocations();
        session_value.stop();
        RUVIA_CHECK(allocations.live_allocations() <= before_stop);
        RUVIA_CHECK_EQ(session_value.active_stream_count(), std::size_t{1});
        const auto body = lease_value->request().request().body_bytes();
        RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()) == "held");
        lease_value.reset();
        RUVIA_CHECK_EQ(session_value.active_stream_count(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(allocations.allocation_count(), allocations.deallocation_count());
}

RUVIA_TEST(http3_buffered_sans_io_session_retired_lease_keeps_its_body_budget) {
    using engine_type = ruvia::detail::http3_sans_io_session_engine;
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();
    ruvia::worker_memory worker;
    engine_type session_value(implementation.route_table(), worker,
        {.max_buffered_body_bytes_ = 8, .max_buffered_bytes_in_flight_ = 6});
    const auto head = request_headers(worker, "POST", "/items");
    (void)session_value.feed(0, head);
    (void)session_value.feed(0, data("abc"), true);
    auto lease_value = session_value.acquire_request(0);
    RUVIA_CHECK(lease_value.has_value());
    if (!lease_value) {
        return;
    }
    (void)session_value.feed(0, {}, false, true);
    RUVIA_CHECK(!session_value.acquire_request(0));
    (void)session_value.feed(4, head);
    (void)session_value.feed(4, data("abcd"), true);
    RUVIA_CHECK(session_value.rejection(4) == engine_type::rejection_type::in_flight_body_capacity);
    RUVIA_CHECK(session_value.release(4));
    lease_value.reset();
    RUVIA_CHECK(session_value.request(0) == nullptr);
    (void)session_value.feed(8, head);
    (void)session_value.feed(8, data("abcd"), true);
    RUVIA_CHECK(session_value.stream_state(8) == engine_type::stream_state_type::ready);
    RUVIA_CHECK(session_value.release(8));
}

RUVIA_TEST(http3_buffered_sans_io_sessions_share_worker_body_budget_across_connections) {
    using engine_type = ruvia::detail::http3_sans_io_session_engine;
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();

    ruvia::worker_memory worker;
    ruvia::detail::http3_server_body_budget budget(6);
    const ruvia::detail::http3_sans_io_session_limits limits{.max_buffered_body_bytes_ = 16,
        .max_live_streams_ = 4,
        .max_buffered_bytes_in_flight_ = 16};
    engine_type first(implementation.route_table(), worker, budget, limits);
    engine_type second(implementation.route_table(), worker, budget, limits);
    const auto head = request_headers(worker, "POST", "/items");

    RUVIA_CHECK(first.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(first.feed(0, data("abcd"), true).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK_EQ(budget.used(), 4U);
    RUVIA_CHECK_EQ(budget.available(), 2U);

    RUVIA_CHECK(second.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
    const auto exhausted = second.feed(0, data("xyz"), true);
    RUVIA_CHECK(exhausted.scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(second.rejection(0) == engine_type::rejection_type::worker_body_budget_exhausted);
    RUVIA_CHECK(second.request(0) != nullptr && second.request(0)->body_bytes() == 0);
    RUVIA_CHECK_EQ(budget.used(), 4U);
    RUVIA_CHECK(second.release(0));

    RUVIA_CHECK(first.release(0));
    RUVIA_CHECK_EQ(budget.used(), 0U);
    RUVIA_CHECK(second.feed(4, head).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(second.feed(4, data("xyz"), true).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(second.request(4) != nullptr && second.request(4)->body_bytes() == 3);
    RUVIA_CHECK_EQ(budget.used(), 3U);
    RUVIA_CHECK(second.release(4));
    RUVIA_CHECK_EQ(budget.used(), 0U);
}

RUVIA_TEST(http3_buffered_sans_io_sessions_keep_per_connection_body_limit_independent) {
    using engine_type = ruvia::detail::http3_sans_io_session_engine;
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();

    ruvia::worker_memory worker;
    ruvia::detail::http3_server_body_budget budget(32);
    const ruvia::detail::http3_sans_io_session_limits limits{.max_buffered_body_bytes_ = 16,
        .max_live_streams_ = 4,
        .max_buffered_bytes_in_flight_ = 3};
    engine_type first(implementation.route_table(), worker, budget, limits);
    engine_type second(implementation.route_table(), worker, budget, limits);
    const auto head = request_headers(worker, "POST", "/items");

    RUVIA_CHECK(first.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(first.feed(0, data("ab"), true).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(second.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(second.feed(0, data("cd"), true).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK_EQ(budget.used(), 4U);

    RUVIA_CHECK(first.feed(4, head).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(first.feed(4, data("ef"), true).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(first.rejection(4) == engine_type::rejection_type::in_flight_body_capacity);
    RUVIA_CHECK(first.request(4) != nullptr && first.request(4)->body_bytes() == 0);
    RUVIA_CHECK_EQ(budget.used(), 4U);
    RUVIA_CHECK(first.release(4));
    RUVIA_CHECK(first.release(0));
    RUVIA_CHECK_EQ(budget.used(), 2U);
    RUVIA_CHECK(second.release(0));
    RUVIA_CHECK_EQ(budget.used(), 0U);
}

RUVIA_TEST(http3_buffered_sans_io_session_reset_and_stop_keep_leased_bodies_reserved) {
    using engine_type = ruvia::detail::http3_sans_io_session_engine;
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();

    ruvia::worker_memory worker;
    ruvia::detail::http3_server_body_budget budget(8);
    const ruvia::detail::http3_sans_io_session_limits limits{.max_buffered_body_bytes_ = 8,
        .max_live_streams_ = 4,
        .max_buffered_bytes_in_flight_ = 8};
    engine_type reset_session(implementation.route_table(), worker, budget, limits);
    engine_type stopped_session(implementation.route_table(), worker, budget, limits);
    engine_type probe_value(implementation.route_table(), worker, budget, limits);
    const auto head = request_headers(worker, "POST", "/items");

    RUVIA_CHECK(reset_session.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(reset_session.feed(0, data("held"), true).status_ ==
                ruvia::http3_connection_status::message_end);
    auto reset_lease = reset_session.acquire_request(0);
    RUVIA_CHECK(reset_lease.has_value());
    if (!reset_lease) {
        return;
    }
    RUVIA_CHECK(reset_session.feed(0, {}, false, true).scope_ ==
                ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(!reset_session.acquire_request(0).has_value());
    RUVIA_CHECK_EQ(budget.used(), 4U);

    RUVIA_CHECK(stopped_session.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(stopped_session.feed(0, data("stay"), true).status_ ==
                ruvia::http3_connection_status::message_end);
    auto stopped_lease = stopped_session.acquire_request(0);
    RUVIA_CHECK(stopped_lease.has_value());
    if (!stopped_lease) {
        return;
    }
    stopped_session.stop();
    RUVIA_CHECK_EQ(budget.used(), 8U);
    RUVIA_CHECK_EQ(reset_lease->request().request().body_bytes().size(), 4U);
    RUVIA_CHECK_EQ(stopped_lease->request().request().body_bytes().size(), 4U);

    RUVIA_CHECK(probe_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
    const auto exhausted = probe_value.feed(0, data("x"), true);
    RUVIA_CHECK(exhausted.scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(probe_value.rejection(0) == engine_type::rejection_type::worker_body_budget_exhausted);
    RUVIA_CHECK_EQ(budget.used(), 8U);
    RUVIA_CHECK(probe_value.release(0));

    reset_lease.reset();
    RUVIA_CHECK_EQ(budget.used(), 4U);
    RUVIA_CHECK(probe_value.feed(4, head).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(probe_value.feed(4, data("four"), true).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK_EQ(budget.used(), 8U);
    RUVIA_CHECK(probe_value.release(4));
    RUVIA_CHECK_EQ(budget.used(), 4U);
    const auto retained = stopped_lease->request().request().body_bytes();
    RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(retained.data()), retained.size()) ==
                "stay");
    stopped_lease.reset();
    RUVIA_CHECK_EQ(budget.used(), 0U);
}

RUVIA_TEST(http3_buffered_sans_io_session_returns_shared_budget_on_repeated_reject_cancel_and_release) {
    using engine_type = ruvia::detail::http3_sans_io_session_engine;
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();

    ruvia::test::counting_memory_resource allocations;
    {
        ruvia::worker_memory worker(allocations);
        ruvia::detail::http3_server_body_budget budget(11);
        engine_type session_value(implementation.route_table(), worker, budget,
            {.max_buffered_body_bytes_ = 16, .max_live_streams_ = 4, .max_buffered_bytes_in_flight_ = 20});
        const auto head = request_headers(worker, "POST", "/items");
        for (std::uint64_t step = 1; step <= 8; ++step) {
            const auto id = step * 16;
            RUVIA_CHECK(session_value.feed(id, head).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.feed(id, data("ab")).scope_ ==
                        ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK_EQ(budget.used(), 2U);
            RUVIA_CHECK(session_value.feed(id, data("1234567890")).scope_ ==
                        ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.rejection(id) == engine_type::rejection_type::worker_body_budget_exhausted);
            RUVIA_CHECK(session_value.request(id) != nullptr && session_value.request(id)->body_bytes() == 0);
            RUVIA_CHECK_EQ(budget.used(), 0U);
            RUVIA_CHECK(session_value.feed(id, {}, true).status_ ==
                        ruvia::http3_connection_status::message_end);
            RUVIA_CHECK(session_value.release(id));
            RUVIA_CHECK_EQ(budget.used(), 0U);

            RUVIA_CHECK(session_value.feed(id + 4, head).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.feed(id + 4, data("ok"), true).status_ ==
                        ruvia::http3_connection_status::message_end);
            RUVIA_CHECK_EQ(budget.used(), 2U);
            RUVIA_CHECK(session_value.release(id + 4));
            RUVIA_CHECK_EQ(budget.used(), 0U);

            RUVIA_CHECK(session_value.feed(id + 8, head).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.feed(id + 8, data("abc")).scope_ ==
                        ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK_EQ(budget.used(), 3U);
            RUVIA_CHECK(session_value.cancel_request(id + 8));
            RUVIA_CHECK(!session_value.cancel_request(id + 8));
            RUVIA_CHECK_EQ(budget.used(), 0U);
        }
        RUVIA_CHECK_EQ(session_value.active_stream_count(), 0U);
        RUVIA_CHECK_EQ(budget.used(), 0U);
        {
            engine_type abandoned(implementation.route_table(), worker, budget,
                {.max_buffered_body_bytes_ = 16,
                    .max_live_streams_ = 4,
                    .max_buffered_bytes_in_flight_ = 20});
            RUVIA_CHECK(abandoned.feed(1000, head).scope_ ==
                        ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(abandoned.feed(1000, data("held")).scope_ ==
                        ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK_EQ(budget.used(), 4U);
        }
        RUVIA_CHECK_EQ(budget.used(), 0U);
    }
    RUVIA_CHECK_EQ(allocations.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(allocations.allocation_count(), allocations.deallocation_count());
}

RUVIA_TEST(http3_buffered_sans_io_session_rolls_back_shared_reservation_when_append_throws) {
    using engine_type = ruvia::detail::http3_sans_io_session_engine;
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();

    switchable_counting_memory_resource upstream;
    {
        ruvia::worker_memory worker(upstream);
        ruvia::detail::http3_server_body_budget budget(256 * 1024);
        engine_type session_value(implementation.route_table(), worker, budget,
            {.max_buffered_body_bytes_ = 256 * 1024,
                .max_live_streams_ = 4,
                .max_buffered_bytes_in_flight_ = 256 * 1024});
        const auto head = request_headers(worker, "POST", "/items");
        RUVIA_CHECK(session_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, data("seed")).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK_EQ(budget.used(), 4U);
        const std::string payload_value(128 * 1024, 'b');
        const auto wire = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data), payload_value);
        upstream.reject_allocations();
        const auto failure = session_value.feed(0, wire, true);
        RUVIA_CHECK(upstream.rejected_allocation_count() != 0);
        RUVIA_CHECK(failure.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(failure.code_ == ruvia::http3_connection_error_code::internal_error);
        RUVIA_CHECK(session_value.terminated());
        RUVIA_CHECK_EQ(budget.used(), 0U);
        RUVIA_CHECK_EQ(session_value.active_stream_count(), 0U);
        upstream.reject_allocations(false);
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_buffered_sans_io_session_handler_lease_survives_suspension_and_unwinds_after_join) {
    using engine_type = ruvia::detail::http3_sans_io_session_engine;
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource allocations;
    {
        ruvia::worker_memory worker(allocations);
        std::exception_ptr failure;
        auto operation = [&]() -> ruvia::task<void> {
            try {
                const std::string payload_value(65536, 'p');
                const auto wire = data(std::string_view(payload_value));
                const auto head = request_headers(worker, "POST", "/items", payload_value.size());
                for (unsigned scenario = 0; scenario < 3; ++scenario) {
                    engine_type session_value(implementation.route_table(), worker);
                    ruvia::worker_signal started(worker_handle_value), resume(worker_handle_value);
                    RUVIA_CHECK(session_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
                    RUVIA_CHECK(session_value.feed(0, wire, true).status_ == ruvia::http3_connection_status::message_end);
                    auto lease_value = session_value.acquire_request(0);
                    if (!lease_value) {
                        throw std::runtime_error("request lease unavailable");
                    }
                    bool ran = false;
                    bool resumed = false;
                    auto handler = [&](engine_type::request_lease_type parameter) -> ruvia::task<void> {
                        auto request = std::move(parameter);
                        ran = true;
                        started.notify();
                        co_await resume.wait();
                        const auto bytes_value = request.request().request().body_bytes();
                        RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()) == payload_value);
                        resumed = true;
                        if (scenario == 2) {
                            throw std::runtime_error("handler failed");
                        }
                    };
                    if (scenario == 0) {
                        {
                            auto cold = handler(std::move(*lease_value));
                        }
                        RUVIA_CHECK(!ran);
                        RUVIA_CHECK(session_value.release(0));
                        continue;
                    }
                    ruvia::task_scope tasks(worker_handle_value, {.resource_ = worker.resource()});
                    bool completed = false;
                    auto watchdog_value = [&]() -> ruvia::task<void> {
                        const auto result_value = co_await ruvia::sleep_for(worker_handle_value,
                            std::chrono::milliseconds(500), tasks.get_stop_token());
                        if (!completed && result_value == ruvia::timer_sleep_result::elapsed) {
                            RUVIA_CHECK(false);
                            session_value.stop();
                            started.notify();
                            resume.notify();
                        }
                    };
                    tasks.spawn(handler(std::move(*lease_value)));
                    tasks.spawn(watchdog_value());
                    co_await started.wait();
                    const auto before_stop = allocations.live_allocations();
                    if (scenario == 1) {
                        RUVIA_CHECK(session_value.cancel_request(0));
                        RUVIA_CHECK(!session_value.terminated());
                    } else {
                        session_value.stop();
                    }
                    RUVIA_CHECK(ran && !resumed && session_value.active_stream_count() == 1);
                    RUVIA_CHECK_EQ(allocations.live_allocations(), before_stop);
                    resume.notify();
                    completed = true;
                    tasks.request_stop();
                    bool handler_failed = false;
                    try {
                        co_await tasks.join();
                    } catch (const std::runtime_error&) {
                        handler_failed = true;
                    }
                    RUVIA_CHECK(resumed && handler_failed == (scenario == 2));
                    RUVIA_CHECK_EQ(session_value.active_stream_count(), std::size_t{0});
                    RUVIA_CHECK(allocations.live_allocations() <= before_stop);
                }
            } catch (...) {
                failure = std::current_exception();
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(operation());
        attachment.run();
        root.get();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    RUVIA_CHECK_EQ(allocations.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(allocations.allocation_count(), allocations.deallocation_count());
}

RUVIA_TEST(http3_buffered_sans_io_session_stages_independent_streams_and_copies_requests) {
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();

    ruvia::worker_memory worker;
    ruvia::detail::http3_sans_io_session_engine session_value(
        implementation.route_table(), worker, {.max_buffered_body_bytes_ = 16});
    const auto head_a = request_headers(worker, "POST", "/items", 3);
    const auto head_b = request_headers(worker, "POST", "/missing");
    RUVIA_CHECK(session_value.feed(0, head_a).status_ == ruvia::http3_connection_status::need_more_data);
    RUVIA_CHECK(session_value.feed(4, head_b).status_ == ruvia::http3_connection_status::need_more_data);
    RUVIA_CHECK(session_value.active_stream_count() == 2);
    RUVIA_CHECK(session_value.stream_state(0) ==
                ruvia::detail::http3_sans_io_session_engine::stream_state_type::receiving);
    RUVIA_CHECK(session_value.resolution(0) != nullptr && session_value.resolution(0)->resolved() != nullptr);
    RUVIA_CHECK(session_value.resolution(4) != nullptr && session_value.resolution(4)->not_found() != nullptr);

    const auto body = data("abc");
    RUVIA_CHECK(session_value.feed(0, body, true).status_ == ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(session_value.stream_state(0) ==
                ruvia::detail::http3_sans_io_session_engine::stream_state_type::ready);
    const auto* request = session_value.request(0);
    RUVIA_CHECK(request != nullptr && request->body_complete());
    RUVIA_CHECK(request != nullptr && request->request().body_bytes().size() == 3);
    RUVIA_CHECK(session_value.stream_state(4) ==
                ruvia::detail::http3_sans_io_session_engine::stream_state_type::receiving);
    RUVIA_CHECK(!session_value.release(4));
    RUVIA_CHECK(session_value.release(0));
    (void)session_value.feed(4, {}, false, true);
    RUVIA_CHECK(session_value.request(4) == nullptr);
    RUVIA_CHECK(session_value.active_stream_count() == 0);
}

RUVIA_TEST(http3_buffered_sans_io_session_drops_failed_stream_without_terminating_peers) {
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();

    ruvia::worker_memory worker;
    ruvia::detail::http3_sans_io_session_engine session_value(
        implementation.route_table(), worker, {.max_buffered_body_bytes_ = 16});
    const auto bad_head = request_headers(worker, "POST", "/items", 1);
    const auto good_head = request_headers(worker, "POST", "/items");
    RUVIA_CHECK(session_value.feed(0, bad_head).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(session_value.feed(4, good_head).scope_ == ruvia::http3_connection_error_scope::none);
    const auto too_long = data("ab");
    const auto failure = session_value.feed(0, too_long);
    RUVIA_CHECK(failure.scope_ == ruvia::http3_connection_error_scope::stream);
    RUVIA_CHECK(session_value.request(0) == nullptr);
    RUVIA_CHECK_EQ(session_value.active_stream_count(), 1U);
    RUVIA_CHECK(!session_value.terminated());
    const auto remaining = data("ok");
    RUVIA_CHECK(session_value.feed(4, remaining, true).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(session_value.request(4) != nullptr && session_value.request(4)->body_complete());
}

RUVIA_TEST(http3_buffered_sans_io_session_applies_strict_body_and_expectation_rejections_early) {
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();

    ruvia::worker_memory worker;
    ruvia::detail::http3_sans_io_session_engine session_value(
        implementation.route_table(), worker, {.max_buffered_body_bytes_ = 3});
    const auto head = request_headers(worker, "POST", "/items");
    RUVIA_CHECK(session_value.feed(0, head).status_ == ruvia::http3_connection_status::need_more_data);
    const auto first = data("ab");
    const auto second = data("cd");
    RUVIA_CHECK(session_value.feed(0, first).status_ == ruvia::http3_connection_status::need_more_data);
    RUVIA_CHECK(session_value.feed(0, second).status_ == ruvia::http3_connection_status::need_more_data);
    RUVIA_CHECK(session_value.rejection(0) ==
                ruvia::detail::http3_sans_io_session_engine::rejection_type::body_too_large);

    const std::array expect{ruvia::http3_field_section_field_view{"expect", "custom-expectation"}};
    const auto expect_head = request_headers(worker, "POST", "/items", std::nullopt, expect);
    RUVIA_CHECK(session_value.feed(4, expect_head).status_ == ruvia::http3_connection_status::need_more_data);
    RUVIA_CHECK(session_value.rejection(4) ==
                ruvia::detail::http3_sans_io_session_engine::rejection_type::expectation_unsupported);
    session_value.stop();
    RUVIA_CHECK(session_value.terminated());
    RUVIA_CHECK(session_value.active_stream_count() == 0);
}

RUVIA_TEST(http3_buffered_sans_io_session_bounds_live_runtimes_after_request_fin) {
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();

    ruvia::worker_memory worker;
    ruvia::detail::http3_sans_io_session_engine session_value(implementation.route_table(), worker,
        {.max_buffered_body_bytes_ = 4, .max_live_streams_ = 1, .max_buffered_bytes_in_flight_ = 8});
    const auto head = request_headers(worker, "POST", "/items");
    RUVIA_CHECK(session_value.feed(0, head, true).status_ == ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(session_value.stream_state(0) ==
                ruvia::detail::http3_sans_io_session_engine::stream_state_type::ready);
    RUVIA_CHECK_EQ(session_value.active_stream_count(), 1U);
    const auto excess = session_value.feed(4, head, true);
    RUVIA_CHECK(excess.scope_ == ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK(excess.code_ == ruvia::http3_connection_error_code::excessive_load);
    RUVIA_CHECK(session_value.terminated());
    RUVIA_CHECK_EQ(session_value.active_stream_count(), 0U);
    RUVIA_CHECK(session_value.feed(8, head).code_ == ruvia::http3_connection_error_code::excessive_load);
}

RUVIA_TEST(http3_buffered_sans_io_session_releases_aggregate_body_budget_on_rejection_and_retirement) {
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();

    ruvia::worker_memory worker;
    ruvia::detail::http3_sans_io_session_engine session_value(implementation.route_table(), worker,
        {.max_buffered_body_bytes_ = 4, .max_live_streams_ = 3, .max_buffered_bytes_in_flight_ = 5});
    const auto head = request_headers(worker, "POST", "/items");
    RUVIA_CHECK(session_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
    const auto four = data("abcd");
    RUVIA_CHECK(session_value.feed(0, four, true).status_ == ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(session_value.feed(4, head).scope_ == ruvia::http3_connection_error_scope::none);
    const auto one = data("x");
    const auto two = data("yz");
    RUVIA_CHECK(session_value.feed(4, one).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(session_value.feed(4, two).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(session_value.rejection(4) ==
                ruvia::detail::http3_sans_io_session_engine::rejection_type::in_flight_body_capacity);
    RUVIA_CHECK(session_value.request(4) != nullptr && session_value.request(4)->body_bytes() == 0);
    RUVIA_CHECK(!session_value.release(4));
    RUVIA_CHECK(session_value.release(0));
    RUVIA_CHECK(session_value.feed(8, head).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(session_value.feed(8, two, true).status_ == ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(session_value.request(8) != nullptr && session_value.request(8)->body_bytes() == 2);
    RUVIA_CHECK(session_value.feed(4, {}, true).status_ == ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(session_value.release(4));
    RUVIA_CHECK_EQ(session_value.active_stream_count(), 1U);
    RUVIA_CHECK(!session_value.terminated());
}

RUVIA_TEST(http3_body_allocations_share_worker_budget_and_remain_pinned_by_lease) {
    using engine_type = ruvia::detail::http3_sans_io_session_engine;
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    routing_test::add_route(implementation, ruvia::http_known_method::post, "/items");
    implementation.finalize();
    ruvia::worker_memory worker;
    ruvia::detail::inbound_buffer_resource shared(worker.resource(), 1500);
    engine_type first(implementation.route_table(), worker,
        {.inbound_buffer_pool_ = &shared, .max_inbound_buffer_bytes_ = 1200});
    engine_type second(implementation.route_table(), worker,
        {.inbound_buffer_pool_ = &shared, .max_inbound_buffer_bytes_ = 1200});
    const auto head = request_headers(worker, "POST", "/items");
    const std::string payload_value(1024, 'a');
    const auto chunk = data(std::string_view(payload_value));
    RUVIA_CHECK(first.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
    const auto first_result = first.feed(0, chunk, true);
    RUVIA_CHECK(first_result.status_ == ruvia::http3_connection_status::message_end);
    auto lease_value = first.acquire_request(0);
    RUVIA_CHECK(lease_value.has_value());
    if (!lease_value) {
        return;
    }
    RUVIA_CHECK(shared.used() >= 1024);
    const auto held = shared.used();
    first.stop();
    RUVIA_CHECK_EQ(shared.used(), held);
    RUVIA_CHECK_EQ(lease_value->request().request().body_bytes().size(), std::size_t{1024});
    RUVIA_CHECK(second.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
    const auto second_empty_bytes = shared.used() - held;
    RUVIA_CHECK(second.feed(0, chunk, true).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(second.rejection(0) == engine_type::rejection_type::worker_body_budget_exhausted);
    RUVIA_CHECK(!second.terminated());
    RUVIA_CHECK_EQ(shared.used(), held + second_empty_bytes);
    RUVIA_CHECK(second.request(0) != nullptr && second.request(0)->body_bytes() == 0);
    RUVIA_CHECK_EQ(ruvia::as_chars(lease_value->request().request().body_bytes()), std::string_view(payload_value));
    lease_value.reset();
    // The rejected stream keeps its empty containers until session retirement.
    RUVIA_CHECK_EQ(shared.used(), second_empty_bytes);
    RUVIA_CHECK(second.feed(4, head).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(second.feed(4, chunk, true).status_ == ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(shared.used() >= 1024);
    second.stop();
    RUVIA_CHECK_EQ(shared.used(), std::size_t{0});
}
