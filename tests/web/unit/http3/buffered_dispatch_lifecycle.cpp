#include <variant>

#include "http3_buffered_dispatch_fixture.h"

namespace {

ruvia::task<void> dispatch_watchdog(const ruvia::worker_handle& worker_handle_value,
    dispatch_type& dispatch, input_type& input, std::uint64_t stream_id, ruvia::worker_signal& started,
    ruvia::worker_signal& finished, bool& joined, ruvia::stop_token stop_token_value, bool& expired) {
    const auto sleep = co_await ruvia::sleep_for(worker_handle_value, 2s, stop_token_value);
    if (sleep != ruvia::timer_sleep_result::elapsed || joined) {
        co_return;
    }
    expired = true;
    dispatch.cancel();
    (void)input.cancel_request(stream_id);
    started.notify();
    finished.notify();
}

ruvia::task<void> exercise_cancellation_and_join(
    fixture& fixture_value, const ruvia::worker_handle& worker_handle_value,
    ruvia::testing::test_context& ruvia_ctx) {
    fixture_value.options_.deadline_ = ruvia::deadline_config{.handler_ = 1min};
    fixture_value.routes_.handlers_.allocate_error_headers_ = true;
    fixture_value.routes_.handlers_.response_body_.assign(2048, 'e');
    ruvia::worker_signal started(worker_handle_value);
    fixture_value.routes_.handlers_.started_ = &started;
    feed_request(fixture_value, 0, "GET", "/suspend");
    auto dispatch = fixture_value.make_dispatch(0, fixture_value.services_);
    ruvia::worker_signal finished(worker_handle_value);
    ruvia::stop_source watchdog_stop;
    ruvia::task_scope tasks(worker_handle_value, {.resource_ = fixture_value.worker_.resource()});
    dispatch_type::run_status_type status{dispatch_type::run_status_type::failed};
    bool joined = false;
    bool watchdog_expired = false;
    tasks.spawn(run_owner(dispatch, status, joined, finished));
    tasks.spawn(dispatch_watchdog(worker_handle_value, dispatch, fixture_value.input_, 0, started, finished,
        joined, watchdog_stop.token(), watchdog_expired));
    co_await started.wait();
    if (!watchdog_expired) {
        RUVIA_CHECK(dispatch.handler_active());
        dispatch.cancel();
        RUVIA_CHECK(fixture_value.input_.cancel_request(0).status_ == input_type::status_type::local_cancelled);
    }
    co_await finished.wait();
    watchdog_stop.request_stop();
    co_await tasks.join();

    RUVIA_CHECK(!watchdog_expired);
    RUVIA_CHECK(joined);
    RUVIA_CHECK(fixture_value.routes_.handlers_.handler_resumed_);
    RUVIA_CHECK(fixture_value.routes_.handlers_.error_handler_called_);
    RUVIA_CHECK(status == dispatch_type::run_status_type::cancelled);
    RUVIA_CHECK(dispatch.cancellation_reason() == dispatch_type::cancellation_reason_type::explicit_value);
    RUVIA_CHECK(!dispatch.handler_active());
    RUVIA_CHECK(!dispatch.response_ready());
    RUVIA_CHECK(fixture_value.session_.request(0) == nullptr);
    buffer::borrowed_block block;
    RUVIA_CHECK(!fixture_value.outbound_.try_receive(block));
    control_type control;
    RUVIA_CHECK(!fixture_value.outbound_.try_receive_control(control));

    feed_request(fixture_value, 4, "GET", "/large");
    auto partial = fixture_value.make_dispatch(4, fixture_value.services_);
    RUVIA_CHECK(co_await partial.run_handler() == dispatch_type::run_status_type::response_ready);
    const auto published = partial.publish_step();
    RUVIA_CHECK(published.status_ == dispatch_type::publish_status_type::bytes_published);
    RUVIA_CHECK(partial.publication_demand() == publication_demand_type::data);
    const auto data_blocked = partial.publish_step();
    RUVIA_CHECK(data_blocked.status_ == dispatch_type::publish_status_type::backpressured);
    RUVIA_CHECK(data_blocked.block_reason_ == block_reason_type::data);
    partial.cancel();
    RUVIA_CHECK(partial.publication_demand() == publication_demand_type::local_cancelled);
    RUVIA_CHECK(fixture_value.input_.cancel_request(4).status_ == input_type::status_type::local_cancelled);
    const auto cancelled = partial.publish_step();
    RUVIA_CHECK(cancelled.status_ == dispatch_type::publish_status_type::cancelled);
    RUVIA_CHECK(cancelled.block_reason_ == block_reason_type::none);
    published_wire partial_wire;
    drain_buffer(fixture_value.outbound_, {epoch, generation, 4}, partial_wire);
    RUVIA_CHECK(!partial_wire.bytes_.empty());
    RUVIA_CHECK(!partial_wire.final_wire_bytes_.has_value());
    RUVIA_CHECK(fixture_value.session_.request(4) == nullptr);
}

ruvia::task<void> exercise_suspending_handler_stop(fixture& fixture_value,
    const ruvia::worker_handle& worker_handle_value, bool stop_worker,
    ruvia::testing::test_context& ruvia_ctx) {
    fixture_value.options_.deadline_ = ruvia::deadline_config{.handler_ = stop_worker ? 1min : 50ms};
    ruvia::worker_signal started(worker_handle_value);
    ruvia::worker_signal finished(worker_handle_value);
    ruvia::stop_source watchdog_stop;
    fixture_value.routes_.handlers_.started_ = &started;
    feed_request(fixture_value, 0, "GET", "/suspend");
    auto dispatch = fixture_value.make_dispatch(0, fixture_value.services_);
    ruvia::task_scope tasks(worker_handle_value, {.resource_ = fixture_value.worker_.resource()});
    dispatch_type::run_status_type status{dispatch_type::run_status_type::failed};
    bool joined = false;
    bool watchdog_expired = false;
    tasks.spawn(run_owner(dispatch, status, joined, finished));
    tasks.spawn(dispatch_watchdog(worker_handle_value, dispatch, fixture_value.input_, 0, started, finished,
        joined, watchdog_stop.token(), watchdog_expired));

    co_await started.wait();
    if (!watchdog_expired && stop_worker) {
        fixture_value.worker_stop_source_.request_stop();
    }
    co_await finished.wait();
    watchdog_stop.request_stop();
    co_await tasks.join();

    RUVIA_CHECK(!watchdog_expired);
    RUVIA_CHECK(joined);
    RUVIA_CHECK(fixture_value.routes_.handlers_.handler_resumed_);
    RUVIA_CHECK(fixture_value.routes_.handlers_.error_handler_called_);
    RUVIA_CHECK(!dispatch.handler_active());

    RUVIA_CHECK(status == dispatch_type::run_status_type::cancelled);
    RUVIA_CHECK(!dispatch.response_ready());
    RUVIA_CHECK(dispatch.cancellation_reason() ==
                (stop_worker ? dispatch_type::cancellation_reason_type::worker_stop
                             : dispatch_type::cancellation_reason_type::deadline));
    buffer::borrowed_block block;
    control_type control;
    RUVIA_CHECK(!fixture_value.outbound_.try_receive(block));
    RUVIA_CHECK(!fixture_value.outbound_.try_receive_control(control));
}

ruvia::task<void> warm_dispatch(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    feed_request(fixture_value, 0, "GET", "/large");
    auto dispatch = fixture_value.make_dispatch(0, fixture_value.services_);
    RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::response_ready);
    published_wire wire;
    publish_and_drain(dispatch, fixture_value, 0, wire, ruvia_ctx);
    RUVIA_CHECK(dispatch.complete());
    RUVIA_CHECK(fixture_value.session_.request(0) == nullptr);
}

ruvia::task<void> exercise_buffer_close_during_data(
    fixture& fixture_value, ruvia::testing::test_context& ruvia_ctx) {
    feed_request(fixture_value, 4, "GET", "/large");
    auto dispatch = fixture_value.make_dispatch(4, fixture_value.services_);
    RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::response_ready);

    published_wire wire;
    const message_id_type id{epoch, generation, 4};
    const auto headers = dispatch.publish_step();
    RUVIA_CHECK(headers.status_ == dispatch_type::publish_status_type::bytes_published);
    drain_buffer(fixture_value.outbound_, id, wire);
    const auto data_header = dispatch.publish_step();
    RUVIA_CHECK(data_header.status_ == dispatch_type::publish_status_type::bytes_published);
    drain_buffer(fixture_value.outbound_, id, wire);

    const std::span<const char> published(wire.bytes_.data(), wire.bytes_.size());
    const auto headers_frame = ruvia::decode_http3_frame(published);
    if ((headers_frame.index() != 0)) {
        RUVIA_CHECK(false);
        co_return;
    }
    RUVIA_CHECK_EQ(std::get<0>(headers_frame).type_, static_cast<std::uint64_t>(ruvia::http3_frame_type::headers));
    const auto data_frame_header = ruvia::decode_http3_frame_header(
        published.subspan(std::get<0>(headers_frame).encoded_bytes_));
    if ((data_frame_header.index() != 0)) {
        RUVIA_CHECK(false);
        co_return;
    }
    RUVIA_CHECK_EQ(std::get<0>(data_frame_header).type_, static_cast<std::uint64_t>(ruvia::http3_frame_type::data));
    RUVIA_CHECK(std::get<0>(data_frame_header).length_ > 0);
    RUVIA_CHECK_EQ(std::get<0>(headers_frame).encoded_bytes_ + std::get<0>(data_frame_header).encoded_bytes_, published.size());
    RUVIA_CHECK(!wire.final_wire_bytes_.has_value());

    const auto before_failure = dispatch.published_wire_bytes();
    RUVIA_CHECK(fixture_value.outbound_.stop());
    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::local_buffer_stopped);
    const auto failure = dispatch.publish_step();
    RUVIA_CHECK(failure.status_ == dispatch_type::publish_status_type::failed);
    RUVIA_CHECK(failure.block_reason_ == block_reason_type::none);
    RUVIA_CHECK_EQ(dispatch.published_wire_bytes(), before_failure);
    RUVIA_CHECK(!dispatch.complete());
    drain_buffer(fixture_value.outbound_, id, wire);
    RUVIA_CHECK(!wire.final_wire_bytes_.has_value());
    control_type control;
    RUVIA_CHECK(!fixture_value.outbound_.try_receive_control(control));
    RUVIA_CHECK(fixture_value.session_.request(4) == nullptr);
}

ruvia::task<void> exercise_buffer_close_during_fin(
    fixture& fixture_value, ruvia::testing::test_context& ruvia_ctx) {
    feed_request(fixture_value, 4, "GET", "/large");
    auto dispatch = fixture_value.make_dispatch(4, fixture_value.services_);
    RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::response_ready);

    published_wire wire;
    const message_id_type id{epoch, generation, 4};
    for (unsigned segment = 0; segment < 3; ++segment) {
        const auto published = dispatch.publish_step();
        RUVIA_CHECK(published.status_ == dispatch_type::publish_status_type::bytes_published);
        if (published.status_ != dispatch_type::publish_status_type::bytes_published) {
            co_return;
        }
        drain_buffer(fixture_value.outbound_, id, wire);
    }

    std::size_t offset = 0;
    std::size_t headers_frames = 0;
    std::size_t data_frames = 0;
    std::string body;
    for (unsigned frame_index = 0; offset < wire.bytes_.size() && frame_index < 4; ++frame_index) {
        const auto decoded = ruvia::decode_http3_frame(
            std::span<const char>(wire.bytes_.data() + offset, wire.bytes_.size() - offset));
        if ((decoded.index() != 0)) {
            RUVIA_CHECK(false);
            co_return;
        }
        if (std::get<0>(decoded).type_ == static_cast<std::uint64_t>(ruvia::http3_frame_type::headers)) {
            ++headers_frames;
        } else if (std::get<0>(decoded).type_ == static_cast<std::uint64_t>(ruvia::http3_frame_type::data)) {
            ++data_frames;
            body.append(std::get<0>(decoded).payload_.data(), std::get<0>(decoded).payload_.size());
        }
        offset += std::get<0>(decoded).encoded_bytes_;
    }
    RUVIA_CHECK_EQ(offset, wire.bytes_.size());
    RUVIA_CHECK_EQ(headers_frames, std::size_t{1});
    RUVIA_CHECK_EQ(data_frames, std::size_t{1});
    RUVIA_CHECK(body == fixture_value.routes_.handlers_.response_body_);
    RUVIA_CHECK(!dispatch.complete());
    RUVIA_CHECK(!wire.final_wire_bytes_.has_value());

    const auto control_filler = fixture_value.outbound_.try_send_control(
        {control_type::kind::writable, id, 0});
    RUVIA_CHECK(control_accepted(control_filler));
    const auto blocked_fin = dispatch.publish_step();
    RUVIA_CHECK(blocked_fin.status_ == dispatch_type::publish_status_type::backpressured);
    RUVIA_CHECK(blocked_fin.block_reason_ == block_reason_type::control);

    const auto before_failure = dispatch.published_wire_bytes();
    RUVIA_CHECK(fixture_value.outbound_.stop());
    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::local_buffer_stopped);
    const auto failure = dispatch.publish_step();
    RUVIA_CHECK(failure.status_ == dispatch_type::publish_status_type::failed);
    RUVIA_CHECK(failure.block_reason_ == block_reason_type::none);
    RUVIA_CHECK_EQ(dispatch.published_wire_bytes(), before_failure);
    RUVIA_CHECK(!dispatch.complete());
    drain_buffer(fixture_value.outbound_, id, wire);
    RUVIA_CHECK(!wire.final_wire_bytes_.has_value());
    control_type control;
    RUVIA_CHECK(!fixture_value.outbound_.try_receive_control(control));
    RUVIA_CHECK(fixture_value.session_.request(4) == nullptr);
}

}  // namespace

RUVIA_TEST(http3_buffered_dispatch_cancellation_stops_and_joins_handler_before_discarding_error_response) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    fixture fixture(worker_handle_value, upstream);
    run_worker_task(attachment, exercise_cancellation_and_join(fixture, worker_handle_value, ruvia_ctx));
}

ruvia::task<void> exercise_worker_stop_and_deadline(
    const ruvia::worker_handle& worker_handle_value, ruvia::testing::test_context& ruvia_ctx) {
    ruvia::test::counting_memory_resource worker_stop_upstream;
    {
        fixture fixture(worker_handle_value, worker_stop_upstream);
        co_await exercise_suspending_handler_stop(fixture, worker_handle_value, true, ruvia_ctx);
        RUVIA_CHECK(fixture.session_.request(0) == nullptr);
    }
    RUVIA_CHECK_EQ(worker_stop_upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(worker_stop_upstream.allocation_count(), worker_stop_upstream.deallocation_count());

    ruvia::test::counting_memory_resource deadline_upstream;
    {
        fixture fixture(worker_handle_value, deadline_upstream);
        co_await exercise_suspending_handler_stop(fixture, worker_handle_value, false, ruvia_ctx);
        RUVIA_CHECK(fixture.session_.request(0) == nullptr);
    }
    RUVIA_CHECK_EQ(deadline_upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(deadline_upstream.allocation_count(), deadline_upstream.deallocation_count());
}

ruvia::task<void> exercise_publication_deadline_registration(
    const ruvia::worker_handle& worker_handle_value, ruvia::testing::test_context& ruvia_ctx) {
    ruvia::test::counting_memory_resource upstream;
    fixture fixture(worker_handle_value, upstream);

    fixture.options_.deadline_ = ruvia::deadline_config{.handler_ = 1min};
    feed_request(fixture, 0, "GET", "/large");
    auto explicitly_cancelled = fixture.make_dispatch(0, fixture.services_);
    RUVIA_CHECK(co_await explicitly_cancelled.run_handler() ==
                dispatch_type::run_status_type::response_ready);
    std::size_t explicit_callbacks = 0;
    RUVIA_CHECK(explicitly_cancelled.register_publication_deadline_callback(
        [&explicit_callbacks]() noexcept { ++explicit_callbacks; }));
    explicitly_cancelled.cancel();
    RUVIA_CHECK_EQ(explicit_callbacks, std::size_t{1});
    RUVIA_CHECK(explicitly_cancelled.cancellation_reason() ==
                dispatch_type::cancellation_reason_type::explicit_value);
    RUVIA_CHECK(explicitly_cancelled.publish_step().status_ ==
                dispatch_type::publish_status_type::cancelled);
    const auto explicit_fin = fixture.input_.accept_control({control_type::kind::stream_fin,
        {epoch, generation, 0}, request_wire(fixture.worker_, "GET", "/large").size()});
    RUVIA_CHECK(explicit_fin.status_ == input_type::status_type::duplicate_fin);

    fixture.options_.deadline_ = ruvia::deadline_config{.handler_ = 20ms};
    feed_request(fixture, 4, "GET", "/large");
    auto already_stopped = fixture.make_dispatch(4, fixture.services_);
    RUVIA_CHECK(co_await already_stopped.run_handler() == dispatch_type::run_status_type::response_ready);
    RUVIA_CHECK(co_await ruvia::sleep_for(worker_handle_value, 60ms, fixture.worker_stop_) ==
                ruvia::timer_sleep_result::elapsed);
    std::size_t stopped_callbacks = 0;
    RUVIA_CHECK(already_stopped.register_publication_deadline_callback(
        [&stopped_callbacks]() noexcept { ++stopped_callbacks; }));
    RUVIA_CHECK_EQ(stopped_callbacks, std::size_t{1});
    RUVIA_CHECK(already_stopped.cancellation_reason() ==
                dispatch_type::cancellation_reason_type::deadline);
    RUVIA_CHECK(already_stopped.publish_step().status_ == dispatch_type::publish_status_type::cancelled);
    const auto expired_fin = fixture.input_.accept_control({control_type::kind::stream_fin,
        {epoch, generation, 4}, request_wire(fixture.worker_, "GET", "/large").size()});
    RUVIA_CHECK(expired_fin.status_ == input_type::status_type::duplicate_fin);

    feed_request(fixture, 8, "GET", "/large");
    auto completed = fixture.make_dispatch(8, fixture.services_);
    RUVIA_CHECK(co_await completed.run_handler() == dispatch_type::run_status_type::response_ready);
    std::size_t late_callbacks = 0;
    RUVIA_CHECK(completed.register_publication_deadline_callback(
        [&late_callbacks]() noexcept { ++late_callbacks; }));
    published_wire wire;
    publish_and_drain(completed, fixture, 8, wire, ruvia_ctx);
    RUVIA_CHECK(completed.complete());
    RUVIA_CHECK(co_await ruvia::sleep_for(worker_handle_value, 40ms, fixture.worker_stop_) ==
                ruvia::timer_sleep_result::elapsed);
    RUVIA_CHECK_EQ(late_callbacks, std::size_t{0});
    RUVIA_CHECK(completed.complete());
    RUVIA_CHECK(fixture.session_.request(0) == nullptr);
    RUVIA_CHECK(fixture.session_.request(4) == nullptr);
    RUVIA_CHECK(fixture.session_.request(8) == nullptr);
}

ruvia::task<void> exercise_buffer_close_stages(
    const ruvia::worker_handle& worker_handle_value, ruvia::testing::test_context& ruvia_ctx) {
    ruvia::test::counting_memory_resource data_upstream;
    {
        fixture fixture(worker_handle_value, data_upstream);
        co_await warm_dispatch(fixture, ruvia_ctx);
        const auto warmed_pool_allocations = data_upstream.live_allocations();
        co_await exercise_buffer_close_during_data(fixture, ruvia_ctx);
        // worker_memory may retain returned blocks in its pool until teardown.
        RUVIA_CHECK_EQ(data_upstream.live_allocations(), warmed_pool_allocations);
    }
    RUVIA_CHECK_EQ(data_upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(data_upstream.allocation_count(), data_upstream.deallocation_count());

    ruvia::test::counting_memory_resource fin_upstream;
    {
        fixture fixture(worker_handle_value, fin_upstream);
        co_await warm_dispatch(fixture, ruvia_ctx);
        const auto warmed_pool_allocations = fin_upstream.live_allocations();
        co_await exercise_buffer_close_during_fin(fixture, ruvia_ctx);
        RUVIA_CHECK_EQ(fin_upstream.live_allocations(), warmed_pool_allocations);
    }
    RUVIA_CHECK_EQ(fin_upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(fin_upstream.allocation_count(), fin_upstream.deallocation_count());
}

RUVIA_TEST(http3_buffered_dispatch_combines_worker_stop_and_handler_deadline_through_router) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    run_worker_task(attachment, exercise_worker_stop_and_deadline(worker_handle_value, ruvia_ctx));
}

RUVIA_TEST(http3_buffered_dispatch_registers_publication_deadline_inline_and_latches_cancellation) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    run_worker_task(attachment,
        exercise_publication_deadline_registration(worker_handle_value, ruvia_ctx));
}

RUVIA_TEST(http3_buffered_dispatch_fails_without_fin_when_outbound_closes_during_data_or_fin) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    run_worker_task(attachment, exercise_buffer_close_stages(worker_handle_value, ruvia_ctx));
}
