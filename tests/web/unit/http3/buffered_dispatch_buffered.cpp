#include <variant>

#include "http3_buffered_dispatch_fixture.h"

namespace {

ruvia::task<void> exercise_route_and_publish(
    fixture& fixture_value, ruvia::testing::test_context& ruvia_ctx) {
    const std::array fields_value{
        ruvia::http3_field_section_field_view{"x-input", "present"},
        ruvia::http3_field_section_field_view{"cookie", "session=one"},
        ruvia::http3_field_section_field_view{"cookie", "other=two"}};
    feed_request(fixture_value, 0, "POST", "/items", "payload", fields_value);
    auto dispatch = fixture_value.make_dispatch(
        0, fixture_value.services_.with_tls_transport("127.0.0.1"));
    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::not_ready);
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.handler_calls_, std::size_t{0});
    RUVIA_CHECK(co_await dispatch.prepare() == dispatch_type::prepare_status_type::prepared);
    RUVIA_CHECK(co_await dispatch.prepare() == dispatch_type::prepare_status_type::already_prepared);
    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::not_ready);
    const auto not_ready = dispatch.publish_step();
    RUVIA_CHECK(not_ready.status_ == dispatch_type::publish_status_type::not_ready);
    RUVIA_CHECK(not_ready.block_reason_ == block_reason_type::none);
    RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::response_ready);
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.handler_calls_, std::size_t{1});
    RUVIA_CHECK(fixture_value.routes_.handlers_.request_read_correctly_);
    bool unexpected_no_deadline_callback = false;
    RUVIA_CHECK(!dispatch.register_publication_deadline_callback(
        [&unexpected_no_deadline_callback]() noexcept { unexpected_no_deadline_callback = true; }));
    RUVIA_CHECK(!unexpected_no_deadline_callback);
    RUVIA_CHECK(dispatch.response_ready());
    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::data);

    const auto allocations = fixture_value.upstream_.allocation_count();
    const auto returns = fixture_value.upstream_.deallocation_count();
    const auto live_allocations = fixture_value.upstream_.live_allocations();
    for (unsigned attempt_value = 0; attempt_value < 4; ++attempt_value) {
        RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::data);
    }
    RUVIA_CHECK_EQ(fixture_value.upstream_.allocation_count(), allocations);
    RUVIA_CHECK_EQ(fixture_value.upstream_.deallocation_count(), returns);
    RUVIA_CHECK_EQ(fixture_value.upstream_.live_allocations(), live_allocations);
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.handler_calls_, std::size_t{1});
    publication_demand_type foreign_demand{publication_demand_type::data};
    std::thread foreign_worker([&] { foreign_demand = dispatch.publication_demand(); });
    foreign_worker.join();
    RUVIA_CHECK(foreign_demand == publication_demand_type::wrong_worker);

    published_wire wire;
    publish_and_drain(dispatch, fixture_value, 0, wire, ruvia_ctx, true);
    RUVIA_CHECK(wire.identity_matched_);
    RUVIA_CHECK_EQ(wire.final_wire_bytes_.value_or(0), wire.bytes_.size());
    RUVIA_CHECK_EQ(wire.final_wire_bytes_.value_or(0), dispatch.published_wire_bytes());
    decoded_response response;
    const auto decoded = decode_published(wire, ruvia::http_known_method::post, 0,
        fixture_value.worker_.resource(), response);
    RUVIA_CHECK(decoded.status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(response.final_heads_, std::size_t{1});
    RUVIA_CHECK_EQ(response.status_, std::uint16_t{200});
    RUVIA_CHECK(response.dispatch_header_ == "buffered");
    RUVIA_CHECK(response.body_ == "buffered-h3-ok");
    RUVIA_CHECK_EQ(response.message_ends_, std::size_t{1});
    RUVIA_CHECK(dispatch.complete());
    RUVIA_CHECK(fixture_value.session_.request(0) == nullptr);
}

ruvia::task<void> exercise_head_and_file(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    feed_request(fixture_value, 0, "HEAD", "/file");
    auto head = fixture_value.make_dispatch(0, fixture_value.services_);
    RUVIA_CHECK(co_await head.run_handler() == dispatch_type::run_status_type::response_ready);
    RUVIA_CHECK(head.publication_demand() == publication_demand_type::data);
    const auto head_headers = head.publish_step();
    RUVIA_CHECK(head_headers.status_ == dispatch_type::publish_status_type::bytes_published);
    RUVIA_CHECK(head.publication_demand() == publication_demand_type::control);
    published_wire head_wire;
    publish_and_drain(head, fixture_value, 0, head_wire, ruvia_ctx);
    decoded_response head_response;
    const auto head_decoded = decode_published(head_wire, ruvia::http_known_method::head, 0,
        fixture_value.worker_.resource(), head_response);
    RUVIA_CHECK(head_decoded.status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(head_response.final_heads_, std::size_t{1});
    RUVIA_CHECK_EQ(head_response.content_length_.value_or(0), std::uint64_t{5});
    RUVIA_CHECK_EQ(head_response.body_events_, std::size_t{0});
    RUVIA_CHECK(head.complete());

    feed_request(fixture_value, 4, "GET", "/file");
    auto file = fixture_value.make_dispatch(4, fixture_value.services_);
    RUVIA_CHECK(co_await file.run_handler() == dispatch_type::run_status_type::response_ready);
    RUVIA_CHECK(file.failure() == nullptr);
    published_wire unavailable_wire;
    publish_and_drain(file, fixture_value, 4, unavailable_wire, ruvia_ctx);
    decoded_response unavailable;
    RUVIA_CHECK(decode_published(unavailable_wire, ruvia::http_known_method::get, 4, fixture_value.worker_.resource(), unavailable).status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(unavailable.status_, std::uint16_t{503});

    feed_request(fixture_value, 8, "GET", "/empty-file");
    auto empty_file = fixture_value.make_dispatch(8, fixture_value.services_);
    RUVIA_CHECK(co_await empty_file.run_handler() == dispatch_type::run_status_type::response_ready);
    RUVIA_CHECK(empty_file.publication_demand() == publication_demand_type::data);
    const auto empty_file_headers = empty_file.publish_step();
    RUVIA_CHECK(empty_file_headers.status_ == dispatch_type::publish_status_type::bytes_published);
    RUVIA_CHECK(empty_file.publication_demand() == publication_demand_type::control);
    published_wire empty_file_wire;
    publish_and_drain(empty_file, fixture_value, 8, empty_file_wire, ruvia_ctx);
    decoded_response empty_file_response;
    const auto empty_file_decoded = decode_published(empty_file_wire, ruvia::http_known_method::get, 8,
        fixture_value.worker_.resource(), empty_file_response);
    RUVIA_CHECK(empty_file_decoded.status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(empty_file_response.status_, std::uint16_t{200});
    RUVIA_CHECK_EQ(empty_file_response.content_length_.value_or(1), std::uint64_t{0});
    RUVIA_CHECK_EQ(empty_file_response.body_events_, std::size_t{0});
    RUVIA_CHECK(empty_file_response.body_.empty());
    const auto empty_file_frame = ruvia::decode_http3_frame(
        std::span<const char>(empty_file_wire.bytes_.data(), empty_file_wire.bytes_.size()));
    RUVIA_CHECK((empty_file_frame.index() == 0));
    if ((empty_file_frame.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(empty_file_frame).type_,
            static_cast<std::uint64_t>(ruvia::http3_frame_type::headers));
        RUVIA_CHECK_EQ(std::get<0>(empty_file_frame).encoded_bytes_, empty_file_wire.bytes_.size());
    }
    RUVIA_CHECK(empty_file.complete());
}

ruvia::task<void> exercise_cold_and_error(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    feed_request(fixture_value, 0, "POST", "/items", "payload");
    {
        auto cold = fixture_value.make_dispatch(0, fixture_value.services_);
        {
            auto task_value = cold.prepare();
        }
        {
            auto task_value = cold.run_handler();
        }
    }
    auto still_ready = fixture_value.session_.acquire_request(0);
    RUVIA_CHECK(still_ready.has_value());
    still_ready.reset();
    RUVIA_CHECK(fixture_value.session_.release(0));

    feed_request(fixture_value, 4, "GET", "/throw");
    auto failure = fixture_value.make_dispatch(4, fixture_value.services_);
    RUVIA_CHECK(co_await failure.prepare() == dispatch_type::prepare_status_type::prepared);
    RUVIA_CHECK(co_await failure.prepare() == dispatch_type::prepare_status_type::already_prepared);
    RUVIA_CHECK(co_await failure.run_handler() == dispatch_type::run_status_type::response_ready);
    RUVIA_CHECK(fixture_value.routes_.handlers_.error_handler_called_);
    published_wire wire;
    publish_and_drain(failure, fixture_value, 4, wire, ruvia_ctx);
    decoded_response response;
    const auto decoded = decode_published(
        wire, ruvia::http_known_method::get, 4, fixture_value.worker_.resource(), response);
    RUVIA_CHECK(decoded.status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(response.status_, std::uint16_t{500});
    RUVIA_CHECK(response.error_header_ == "used");
    RUVIA_CHECK(response.body_ == "handled-error");
}

ruvia::task<void> exercise_escaping_failure(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    fixture_value.routes_.handlers_.throw_from_error_handler_ = true;
    feed_request(fixture_value, 0, "GET", "/throw");
    {
        auto dispatch = fixture_value.make_dispatch(0, fixture_value.services_);
        RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::response_ready);
        published_wire wire;
        publish_and_drain(dispatch, fixture_value, 0, wire, ruvia_ctx);
        decoded_response response;
        RUVIA_CHECK(decode_published(wire, ruvia::http_known_method::get, 0,
                        fixture_value.worker_.resource(), response)
                        .status_ == ruvia::http3_client_response_status::message_end);
        RUVIA_CHECK_EQ(response.status_, std::uint16_t{500});
        RUVIA_CHECK(fixture_value.session_.request(0) == nullptr);
    }
#ifndef _WIN32
    // MSVC may satisfy this request from the worker pool without reaching
    // the upstream allocator.
    fixture_value.routes_.handlers_.throw_from_error_handler_ = false;
    feed_request(fixture_value, 4, "GET", "/large");
    {
        auto dispatch = fixture_value.make_dispatch(4, fixture_value.services_);
        fixture_value.allocations_.reject_ = true;
        const auto result_value = co_await dispatch.run_handler();
        fixture_value.allocations_.reject_ = false;
        RUVIA_CHECK(result_value == dispatch_type::run_status_type::failed);
        RUVIA_CHECK(dispatch.failure() != nullptr && !dispatch.handler_active());
        RUVIA_CHECK(fixture_value.session_.request(4) == nullptr);
    }
#endif
    buffer::borrowed_block block;
    control_type control;
    RUVIA_CHECK(!fixture_value.outbound_.try_receive(block) && !fixture_value.outbound_.try_receive_control(control));
    RUVIA_CHECK(!fixture_value.session_.terminated());
}

ruvia::task<void> exercise_repeated_request_memory(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    fixture_value.routes_.handlers_.response_body_.assign(32 * 1024, 'r');
    feed_request(fixture_value, 100, "POST", "/items", "payload");
    auto retained = fixture_value.session_.acquire_request(100);
    RUVIA_CHECK(retained.has_value());
    if (!retained) {
        co_return;
    }
    const auto retained_body = retained->request().request().body_bytes();
    const auto retained_header = retained->request().request().header("host");
    std::size_t warmed_live_allocations{};

    for (std::uint64_t index = 0; index < 8; ++index) {
        const auto stream_id = index * 4;
        feed_request(fixture_value, stream_id, "GET", "/large");
        auto dispatch = fixture_value.make_dispatch(stream_id, fixture_value.services_);
        RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::response_ready);
        published_wire wire;
        publish_and_drain(dispatch, fixture_value, stream_id, wire, ruvia_ctx);
        RUVIA_CHECK(dispatch.complete());
        RUVIA_CHECK_EQ(wire.final_wire_bytes_.value_or(0), wire.bytes_.size());
        if (index == 0) {
            warmed_live_allocations = fixture_value.upstream_.live_allocations();
        } else {
            RUVIA_CHECK_EQ(fixture_value.upstream_.live_allocations(), warmed_live_allocations);
        }
        const auto body = retained->request().request().body_bytes();
        RUVIA_CHECK(body.size() == retained_body.size());
        RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()) ==
                    "payload");
        RUVIA_CHECK(retained->request().request().header("host") == retained_header);
    }

    retained.reset();
    RUVIA_CHECK(fixture_value.session_.release(100));
}

ruvia::task<void> exercise_early_provenance_and_replay_policy(
    fixture& fixture_value, ruvia::testing::test_context& ruvia_ctx) {
    const std::array early_header{ruvia::http3_field_section_field_view{"early-data", "1"}};
    feed_request(fixture_value, 0, "GET", "/throw", {}, early_header, true);
    auto rejected = fixture_value.make_dispatch(0, fixture_value.services_, {}, true);
    RUVIA_CHECK(co_await rejected.run_handler() == dispatch_type::run_status_type::response_ready);
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.handler_calls_, std::size_t{0});
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.replay_safe_middleware_calls_, std::size_t{0});
    published_wire rejected_wire;
    publish_and_drain(rejected, fixture_value, 0, rejected_wire, ruvia_ctx);
    decoded_response rejected_response;
    const auto rejected_result = decode_published(rejected_wire,
        ruvia::http_known_method::get, 0, fixture_value.worker_.resource(), rejected_response);
    RUVIA_CHECK(rejected_result.status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(rejected_response.status_, std::uint16_t{425});
    RUVIA_CHECK(rejected.complete());

    feed_request(fixture_value, 4, "GET", "/throw", {}, early_header, false);
    auto spoofed = fixture_value.make_dispatch(4, fixture_value.services_, {}, false);
    RUVIA_CHECK(co_await spoofed.run_handler() == dispatch_type::run_status_type::response_ready);
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.handler_calls_, std::size_t{1});
    RUVIA_CHECK(!fixture_value.routes_.handlers_.early_data_info_.received_from_early_data());
    RUVIA_CHECK(fixture_value.routes_.handlers_.early_data_info_.upstream_declared_early_data());
    published_wire spoofed_wire;
    publish_and_drain(spoofed, fixture_value, 4, spoofed_wire, ruvia_ctx);
    RUVIA_CHECK(spoofed.complete());

    feed_request(fixture_value, 8, "GET", "/early-safe", {}, {}, true);
    auto safe = fixture_value.make_dispatch(8, fixture_value.services_, {}, true);
    RUVIA_CHECK(co_await safe.run_handler() == dispatch_type::run_status_type::response_ready);
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.handler_calls_, std::size_t{2});
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.replay_safe_middleware_calls_, std::size_t{1});
    RUVIA_CHECK(fixture_value.routes_.handlers_.early_data_info_.received_from_early_data());
    RUVIA_CHECK(!fixture_value.routes_.handlers_.early_data_info_.upstream_declared_early_data());
    published_wire safe_wire;
    publish_and_drain(safe, fixture_value, 8, safe_wire, ruvia_ctx);
    decoded_response safe_response;
    const auto safe_result = decode_published(safe_wire,
        ruvia::http_known_method::get, 8, fixture_value.worker_.resource(), safe_response);
    RUVIA_CHECK(safe_result.status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(safe_response.status_, std::uint16_t{200});
    RUVIA_CHECK(safe.complete());
}

ruvia::task<void> exercise_websocket_handshake_failures(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    for (const auto& [stream_id, version, expected_version, expected_code, request_ended] : {
             std::tuple<std::uint64_t, std::string_view, std::string_view,
                 std::string_view, bool>{
                 0, "12", "13", "websocket_version_unsupported", false},
             {4, "", "", "invalid_websocket_handshake", false},
             {8, "12", "13", "websocket_version_unsupported", true}}) {
        feed_websocket_request(fixture_value.session_, fixture_value.worker_, stream_id, version);
        if (request_ended) {
            const auto fin = fixture_value.session_.feed(stream_id, {}, true);
            RUVIA_CHECK(fin.scope_ == ruvia::http3_connection_error_scope::none);
        }
        auto dispatch = fixture_value.make_dispatch(stream_id, fixture_value.services_);
        RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::response_ready);
        RUVIA_CHECK(fixture_value.routes_.handlers_.error_handler_called_);
        RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.error_code_, expected_code);
        published_wire wire;
        publish_and_drain(dispatch, fixture_value, stream_id, wire, ruvia_ctx);
        decoded_response response;
        const auto decoded = decode_published(wire, ruvia::http_known_method::connect,
            stream_id, fixture_value.worker_.resource(), response);
        RUVIA_CHECK(decoded.status_ == ruvia::http3_client_response_status::message_end);
        RUVIA_CHECK_EQ(response.status_, std::uint16_t{400});
        RUVIA_CHECK_EQ(response.error_header_, "used");
        RUVIA_CHECK_EQ(response.websocket_version_header_, expected_version);
        RUVIA_CHECK(response.connection_header_.empty());
        RUVIA_CHECK(response.upgrade_header_.empty());
        RUVIA_CHECK(response.websocket_accept_header_.empty());
        RUVIA_CHECK_EQ(response.body_, "handled-error");
        if (!request_ended) {
            RUVIA_CHECK(fixture_value.session_.request(stream_id) != nullptr);
            const auto fin = fixture_value.session_.feed(stream_id, {}, true);
            RUVIA_CHECK(fin.scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(fixture_value.session_.release(stream_id));
        }
        RUVIA_CHECK(fixture_value.session_.request(stream_id) == nullptr);
        fixture_value.routes_.handlers_.error_handler_called_ = false;
    }
}

ruvia::task<void> exercise_buffered_recovery_coding(fixture& fixture_value, bool websocket_value,
    ruvia::testing::test_context& ruvia_ctx) {
    auto& state_value = fixture_value.routes_.handlers_;
    state_value.response_body_.assign(2048, 'a');
    state_value.error_body_.assign(2048, 'e');
    state_value.response_no_transform_ = true;
    state_value.first_error_no_transform_ = websocket_value;
    for (unsigned mode = 0; mode != 4; ++mode) {
        state_value.error_handler_calls_ = 0;
        state_value.error_code_.clear();
        state_value.error_no_transform_ = mode == 1;
        fixture_value.options_.compression_.emplace();
        if (mode == 2) {
            fixture_value.options_.compression_.reset();
        }
        const auto accept_encoding = mode == 3 ? "identity;q=0, *;q=0" : "gzip, identity;q=0";
        const std::uint64_t stream_id = mode * 4;
        if (websocket_value) {
            feed_websocket_request(fixture_value.session_, fixture_value.worker_, stream_id, "12", accept_encoding);
        } else {
            const std::array fields_value{ruvia::http3_field_section_field_view{"accept-encoding", accept_encoding}};
            feed_request(fixture_value, stream_id, "GET", "/large", {}, fields_value);
        }
        auto dispatch = fixture_value.make_dispatch(stream_id, fixture_value.services_);
        RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::response_ready);
        RUVIA_CHECK_EQ(state_value.error_handler_calls_, websocket_value ? std::size_t{2} : std::size_t{1});
        RUVIA_CHECK_EQ(state_value.error_code_, std::string("not_acceptable"));
        published_wire wire;
        publish_and_drain(dispatch, fixture_value, stream_id, wire, ruvia_ctx);
        decoded_response response;
        RUVIA_CHECK(decode_published(wire, websocket_value ? ruvia::http_known_method::connect : ruvia::http_known_method::get,
                        stream_id, fixture_value.worker_.resource(), response)
                        .status_ == ruvia::http3_client_response_status::message_end);
        RUVIA_CHECK_EQ(response.status_, std::uint16_t{406});
        RUVIA_CHECK_EQ(response.final_heads_, std::size_t{1});
        RUVIA_CHECK_EQ(response.error_header_, std::string("used"));
        if (!websocket_value && mode == 0) {
            RUVIA_CHECK_EQ(response.content_encoding_, std::string("gzip"));
            const auto decoded_body = ruvia::decode_http_content(ruvia::http_content_coding::gzip, response.body_,
                {.max_decoded_bytes_ = state_value.error_body_.size(), .resource_ = fixture_value.worker_.resource()});
            RUVIA_CHECK(decoded_body.decoded() != nullptr);
            if (const auto* content = decoded_body.decoded()) {
                RUVIA_CHECK_EQ(content->bytes(), state_value.error_body_);
            }
        } else {
            RUVIA_CHECK(response.content_encoding_.empty());
            RUVIA_CHECK_EQ(response.body_, state_value.error_body_);
        }
        if (websocket_value) {
            RUVIA_CHECK(fixture_value.session_.request(stream_id) != nullptr);
            const auto fin = fixture_value.session_.feed(stream_id, {}, true);
            RUVIA_CHECK(fin.scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(fixture_value.session_.release(stream_id));
        }
        RUVIA_CHECK(fixture_value.session_.request(stream_id) == nullptr);
    }
}

}  // namespace

RUVIA_TEST(http3_buffered_dispatch_rejects_untrusted_and_unsafe_early_requests_before_middleware) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    fixture fixture(worker_handle_value, upstream);
    run_worker_task(attachment, exercise_early_provenance_and_replay_policy(fixture, ruvia_ctx));
}

RUVIA_TEST(http3_buffered_dispatch_routes_body_and_publishes_bounded_wire_response) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    fixture fixture(worker_handle_value, upstream);
    run_worker_task(attachment, exercise_route_and_publish(fixture, ruvia_ctx));
}

RUVIA_TEST(http3_buffered_dispatch_supports_head_file_metadata_and_rejects_file_payload) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    fixture fixture(worker_handle_value, upstream);
    run_worker_task(attachment, exercise_head_and_file(fixture, ruvia_ctx));
}

RUVIA_TEST(http3_buffered_dispatch_cold_tasks_do_not_lease_and_router_errors_use_error_handler) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    fixture fixture(worker_handle_value, upstream);
    run_worker_task(attachment, exercise_cold_and_error(fixture, ruvia_ctx));
}

RUVIA_TEST(http3_buffered_dispatch_error_handler_fallback_and_allocation_failure_release_storage) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_handle_value, upstream);
        run_worker_task(attachment, exercise_escaping_failure(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_buffered_dispatch_returns_repeated_request_memory_and_preserves_leased_sibling) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_handle_value, upstream);
        run_worker_task(attachment, exercise_repeated_request_memory(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_buffered_recovery_preserves_ordinary_and_websocket_coding_policies) {
    for (const bool websocket : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
        const auto worker_value = attachment.loop().handle();
        ruvia::test::counting_memory_resource resource;
        {
            fixture fixture(worker_value, resource);
            run_worker_task(attachment, exercise_buffered_recovery_coding(fixture, websocket, ruvia_ctx));
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
    }
}

ruvia::task<void> exercise_buffered_recovery_cancellation(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, bool websocket_value, ruvia::testing::test_context& ruvia_ctx) {
    auto& state_value = fixture_value.routes_.handlers_;
    ruvia::worker_signal error_started(worker_value);
    state_value.error_started_ = &error_started;
    state_value.suspend_error_call_ = websocket_value ? 2 : 1;
    state_value.response_no_transform_ = true;
    state_value.first_error_no_transform_ = websocket_value;
    const auto accept_encoding = "gzip, identity;q=0";
    if (websocket_value) {
        feed_websocket_request(fixture_value.session_, fixture_value.worker_, 0, "12", accept_encoding);
    } else {
        const std::array fields_value{ruvia::http3_field_section_field_view{"accept-encoding", accept_encoding}};
        feed_request(fixture_value, 0, "GET", "/large", {}, fields_value);
    }
    auto dispatch = fixture_value.make_dispatch(0, fixture_value.services_);
    ruvia::task_scope tasks(worker_value, {.resource_ = fixture_value.worker_.resource()});
    ruvia::worker_signal finished(worker_value);
    auto status = dispatch_type::run_status_type::failed;
    bool joined = false;
    tasks.spawn(run_owner(dispatch, status, joined, finished));
    co_await error_started.wait();
    RUVIA_CHECK(dispatch.handler_active());
    dispatch.cancel();
    co_await finished.wait();
    co_await tasks.join();
    RUVIA_CHECK(joined && status == dispatch_type::run_status_type::cancelled);
    RUVIA_CHECK(!dispatch.handler_active() && !dispatch.response_ready());
    RUVIA_CHECK_EQ(dispatch.published_wire_bytes(), std::uint64_t{0});
    buffer::borrowed_block block;
    control_type control;
    RUVIA_CHECK(!fixture_value.outbound_.try_receive(block) && !fixture_value.outbound_.try_receive_control(control));
    RUVIA_CHECK(fixture_value.session_.request(0) == nullptr);
}

RUVIA_TEST(http3_buffered_recovery_cancellation_joins_error_handler_without_publishing) {
    for (const bool websocket : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
        const auto worker_value = attachment.loop().handle();
        ruvia::test::counting_memory_resource resource;
        {
            fixture fixture(worker_value, resource);
            run_worker_task(attachment, exercise_buffered_recovery_cancellation(fixture, worker_value, websocket, ruvia_ctx));
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
    }
}

RUVIA_TEST(http3_buffered_dispatch_websocket_handshake_failure_applies_required_headers) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_handle_value, upstream);
        fixture.executor_ = attachment.loop().executor();
        run_worker_task(attachment, exercise_websocket_handshake_failures(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}
