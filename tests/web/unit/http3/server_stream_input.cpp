#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_qpack_connection.h"
#include "ruvia/http/http3_var_int.h"

#include "http3/http3_sans_io_session_engine.h"
#include "http3/http3_server_stream_input.h"
#include "http3/http3_stream_buffer.h"
#include "memory_resource_fixture.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "routing_fixture.h"
#include "test_harness.h"

namespace {

using input_type = ruvia::detail::http3_server_stream_input;
using stream_buffer = ruvia::detail::http3_stream_buffer;
using message_id_type = ruvia::detail::http3_stream_id;
using control_type = ruvia::detail::http3_stream_control;
using engine_type = ruvia::detail::http3_sans_io_session_engine;

constexpr std::uint64_t epoch = 17;
constexpr std::uint64_t generation = 29;

struct routes final {
    ruvia::detail::router router_;
    ruvia::detail::router_impl& implementation_{ruvia::detail::router_impl::from(router_)};

    routes() {
        routing_test::add_route(implementation_, ruvia::http_known_method::post, "/items");
        implementation_.finalize();
    }
};

struct fixture final {
    routes routes_;
    ruvia::worker_memory worker_;
    engine_type session_;
    stream_buffer buffer_;
    input_type input_;

    explicit fixture(std::size_t capacity = 16, ruvia::http3_connection_config connection = {.enable_connect_protocol_ = true})
        : routes_(),
          worker_(),
          session_(routes_.implementation_.route_table(), worker_, {.connection_ = connection}),
          buffer_(16, 16, 8),
          input_(session_, worker_, epoch, generation, capacity) {}
};

std::string frame(std::uint64_t type, std::string_view payload_value) {
    std::array<char, ruvia::http3_frame_header_max_bytes> header_value{};
    const auto encoded = ruvia::encode_http3_frame_header(header_value, type, payload_value.size());
    if ((encoded.index() != 0)) {
        throw std::runtime_error("HTTP/3 test frame header encoding failed");
    }
    std::string result_value(header_value.data(), std::get<0>(encoded));
    result_value.append(payload_value);
    return result_value;
}

std::string request_headers(ruvia::worker_memory& worker_value, std::string_view method,
    std::string_view path, std::optional<std::uint64_t> body_length = std::nullopt) {
    auto head = ruvia::encode_http3_client_request_head({.method_ = method,
                                                            .scheme_ = "https",
                                                            .authority_ = "example.test",
                                                            .path_ = path,
                                                            .body_length_ = body_length},
        {}, worker_value.resource());
    if ((head.index() != 0)) {
        throw std::runtime_error("HTTP/3 test request head encoding failed");
    }
    return frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
        std::string_view(std::get<0>(head).field_section_.data(), std::get<0>(head).field_section_.size()));
}

std::string request_wire(ruvia::worker_memory& worker_value, std::string_view body) {
    std::string wire = request_headers(worker_value, "POST", "/items", body.size());
    wire += frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data), body);
    return wire;
}

bool queue_data(fixture& fixture_value, message_id_type id, std::string_view bytes_value) {
    const auto input = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(bytes_value.data()), bytes_value.size());
    const auto result_value = fixture_value.buffer_.try_send(id, input);
    return result_value == stream_buffer::send_result::sent;
}

bool queue_control(fixture& fixture_value, const control_type& control) {
    const auto result_value = fixture_value.buffer_.try_send_control(control);
    return result_value == stream_buffer::control_result::sent;
}

std::optional<input_type::result_type> receive_data(fixture& fixture_value) {
    stream_buffer::borrowed_block block;
    if (!fixture_value.buffer_.try_receive(block)) {
        return std::nullopt;
    }
    auto result_value = fixture_value.input_.accept_data(block);
    block.release();
    return result_value;
}

std::optional<input_type::result_type> receive_control(fixture& fixture_value) {
    control_type control;
    if (!fixture_value.buffer_.try_receive_control(control)) {
        return std::nullopt;
    }
    return fixture_value.input_.accept_control(control);
}

control_type fin(std::uint64_t stream_id, std::uint64_t final_size) {
    return {control_type::kind::stream_fin, {epoch, generation, stream_id}, final_size};
}

control_type reset(std::uint64_t stream_id, std::uint64_t published_bytes,
    ruvia::http3_connection_error_code error_code = ruvia::http3_connection_error_code::request_cancelled) {
    return {.kind_ = control_type::kind::stream_reset,
        .id_ = {epoch, generation, stream_id},
        .value_ = published_bytes,
        .stream_reset_error_code_ = error_code};
}

bool body_matches(const engine_type& session_value, std::uint64_t stream_id, std::string_view expected) {
    const auto* request = session_value.request(stream_id);
    if (request == nullptr) {
        return false;
    }
    const auto body = request->request().body_bytes();
    return std::string_view(reinterpret_cast<const char*>(body.data()), body.size()) == expected;
}

}  // namespace

RUVIA_TEST(http3_server_stream_input_counts_request_streams_but_not_peer_unidirectional_streams) {
    fixture fixture;
    RUVIA_CHECK(fixture.input_.accept_control(reset(2, 1)).status_ ==
                input_type::status_type::deferred_reset);
    RUVIA_CHECK_EQ(fixture.input_.observed_request_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(fixture.input_.active_request_stream_count(), std::size_t{0});

    RUVIA_CHECK(fixture.input_.accept_control(reset(0, 0)).status_ == input_type::status_type::reset);
    RUVIA_CHECK_EQ(fixture.input_.observed_request_stream_count(), std::size_t{1});
    RUVIA_CHECK_EQ(fixture.input_.active_request_stream_count(), std::size_t{0});
}

RUVIA_TEST(http3_server_stream_input_defers_fin_across_independent_buffer_lanes_and_interleaved_streams) {
    fixture fixture;
    const auto first = request_wire(fixture.worker_, "alpha");
    const auto second = request_wire(fixture.worker_, "bravo");
    const auto first_cut = first.size() / 3;
    const auto second_cut = second.size() / 3;
    RUVIA_CHECK(first_cut > 0 && second_cut > 0);

    RUVIA_CHECK(queue_data(fixture,
        {epoch, generation, 0, {}, true}, std::string_view(first).substr(0, first_cut)));
    RUVIA_CHECK(queue_data(fixture, {epoch, generation, 4}, std::string_view(second).substr(0, second_cut)));
    RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, std::string_view(first).substr(first_cut, first_cut)));
    RUVIA_CHECK(queue_data(fixture, {epoch, generation, 4}, std::string_view(second).substr(second_cut, second_cut)));
    RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, std::string_view(first).substr(first_cut * 2)));
    RUVIA_CHECK(queue_data(fixture, {epoch, generation, 4}, std::string_view(second).substr(second_cut * 2)));
    RUVIA_CHECK(queue_control(fixture, fin(0, first.size())));
    RUVIA_CHECK(queue_control(fixture, fin(4, second.size())));

    const auto first_fin = receive_control(fixture);
    const auto second_fin = receive_control(fixture);
    RUVIA_CHECK(first_fin.has_value() && first_fin->status_ == input_type::status_type::deferred_fin);
    RUVIA_CHECK(second_fin.has_value() && second_fin->status_ == input_type::status_type::deferred_fin);

    std::array<input_type::status_type, 6> statuses{};
    for (auto& status : statuses) {
        const auto result_value = receive_data(fixture);
        RUVIA_CHECK(result_value.has_value());
        status = result_value ? result_value->status_ : input_type::status_type::invalid_input;
    }
    RUVIA_CHECK(statuses[0] == input_type::status_type::fed && statuses[1] == input_type::status_type::fed);
    RUVIA_CHECK(statuses[2] == input_type::status_type::fed && statuses[3] == input_type::status_type::fed);
    RUVIA_CHECK(statuses[4] == input_type::status_type::finished && statuses[5] == input_type::status_type::finished);
    RUVIA_CHECK(body_matches(fixture.session_, 0, "alpha"));
    RUVIA_CHECK(body_matches(fixture.session_, 4, "bravo"));
    RUVIA_CHECK(fixture.input_.received_early_data(0));
    RUVIA_CHECK(!fixture.input_.received_early_data(4));
    RUVIA_CHECK(fixture.input_.tracked_stream_count() == 2);
    RUVIA_CHECK_EQ(fixture.input_.observed_request_stream_count(), std::size_t{2});
    RUVIA_CHECK_EQ(fixture.input_.active_request_stream_count(), std::size_t{0});
}

RUVIA_TEST(http3_server_stream_input_defers_peer_reset_until_published_headers_are_consumed) {
    fixture fixture;
    const auto headers = request_headers(fixture.worker_, "POST", "/items");
    RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, headers));
    RUVIA_CHECK(queue_control(fixture, reset(0, headers.size())));

    const auto reset_result = receive_control(fixture);
    RUVIA_CHECK(reset_result.has_value() && reset_result->status_ == input_type::status_type::deferred_reset);
    RUVIA_CHECK(fixture.session_.request(0) == nullptr);
    const auto applied_reset = receive_data(fixture);
    RUVIA_CHECK(applied_reset.has_value() && applied_reset->status_ == input_type::status_type::reset);
    RUVIA_CHECK(fixture.session_.request(0) == nullptr);

    RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, headers));
    const auto still_closed = receive_data(fixture);
    RUVIA_CHECK(still_closed.has_value() && still_closed->status_ == input_type::status_type::final_size_error);
    RUVIA_CHECK(fixture.input_.tracked_stream_count() == 1);
    RUVIA_CHECK(fixture.input_.stopped() && fixture.session_.terminated());
}

RUVIA_TEST(http3_server_stream_input_defers_reset_across_multiple_data_blocks_and_checks_the_barrier) {
    fixture fixture;
    const auto wire = request_wire(fixture.worker_, "barrier");
    const auto first_cut = wire.size() / 3;
    const auto second_cut = first_cut * 2;
    RUVIA_CHECK(first_cut > 0 && second_cut < wire.size());
    const message_id_type id{epoch, generation, 0};
    RUVIA_CHECK(queue_data(fixture, id, std::string_view(wire).substr(0, first_cut)));
    RUVIA_CHECK(queue_data(fixture, id,
        std::string_view(wire).substr(first_cut, second_cut - first_cut)));
    RUVIA_CHECK(queue_data(fixture, id, std::string_view(wire).substr(second_cut)));
    RUVIA_CHECK(queue_control(fixture, reset(0, wire.size(),
                                           ruvia::http3_connection_error_code::request_rejected)));

    const auto deferred = receive_control(fixture);
    RUVIA_CHECK(deferred && deferred->status_ == input_type::status_type::deferred_reset);
    const auto first = receive_data(fixture);
    const auto second = receive_data(fixture);
    const auto last = receive_data(fixture);
    RUVIA_CHECK(first && first->status_ == input_type::status_type::deferred_reset);
    RUVIA_CHECK(second && second->status_ == input_type::status_type::deferred_reset);
    RUVIA_CHECK(last && last->status_ == input_type::status_type::reset);
    RUVIA_CHECK(fixture.input_.accept_control(reset(0, wire.size(),
                                                  ruvia::http3_connection_error_code::request_rejected))
                    .status_ == input_type::status_type::closed_stream);
    RUVIA_CHECK(!fixture.input_.stopped());
    RUVIA_CHECK(fixture.session_.request(0) == nullptr);
}

RUVIA_TEST(http3_server_stream_input_defers_critical_stream_reset_until_its_type_is_fed) {
    fixture fixture;
    const message_id_type id{epoch, generation, 2};
    const auto reset_control = control_type{.kind_ = control_type::kind::stream_reset,
        .id_ = id,
        .value_ = 1,
        .stream_reset_error_code_ = ruvia::http3_connection_error_code::request_cancelled};
    RUVIA_CHECK(fixture.input_.accept_control(reset_control).status_ == input_type::status_type::deferred_reset);
    const std::array<char, 1> control_type_value{0};
    RUVIA_CHECK(queue_data(fixture, id, std::string_view(control_type_value.data(), control_type_value.size())));
    const auto closed_critical = receive_data(fixture);
    RUVIA_CHECK(closed_critical && closed_critical->status_ == input_type::status_type::protocol_error);
    RUVIA_CHECK(closed_critical->protocol_.scope_ == ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK(closed_critical->protocol_.code_ == ruvia::http3_connection_error_code::closed_critical_stream);
    RUVIA_CHECK(fixture.input_.stopped() && fixture.session_.terminated());
}

RUVIA_TEST(http3_server_stream_input_rejects_reset_barrier_overrun_and_conflicting_duplicates_locally) {
    {
        fixture fixture;
        const auto headers = request_headers(fixture.worker_, "POST", "/items");
        RUVIA_CHECK(fixture.input_.accept_control(reset(0, headers.size() - 1)).status_ ==
                    input_type::status_type::deferred_reset);
        RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, headers));
        const auto overrun = receive_data(fixture);
        RUVIA_CHECK(overrun && overrun->status_ == input_type::status_type::final_size_error);
        RUVIA_CHECK(overrun->protocol_.scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(fixture.session_.request(0) == nullptr);
        RUVIA_CHECK(fixture.input_.stopped() && fixture.session_.terminated());
    }
    {
        fixture fixture;
        const auto original = reset(0, 8, ruvia::http3_connection_error_code::request_rejected);
        RUVIA_CHECK(fixture.input_.accept_control(original).status_ == input_type::status_type::deferred_reset);
        RUVIA_CHECK(fixture.input_.accept_control(original).status_ == input_type::status_type::deferred_reset);
        const auto conflict = fixture.input_.accept_control(
            reset(0, 9, ruvia::http3_connection_error_code::request_rejected));
        RUVIA_CHECK(conflict.status_ == input_type::status_type::final_size_error);
        RUVIA_CHECK(conflict.protocol_.scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(fixture.input_.stopped() && fixture.session_.terminated());
    }
    {
        fixture fixture;
        const auto first_reset = reset(0, 8, ruvia::http3_connection_error_code::request_rejected);
        RUVIA_CHECK(fixture.input_.accept_control(first_reset).status_ == input_type::status_type::deferred_reset);
        const auto code_conflict = fixture.input_.accept_control(
            reset(0, 8, ruvia::http3_connection_error_code::message_error));
        RUVIA_CHECK(code_conflict.status_ == input_type::status_type::final_size_error);
        RUVIA_CHECK(code_conflict.protocol_.scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(fixture.input_.stopped() && fixture.session_.terminated());
    }
    {
        fixture fixture;
        const auto too_large_for_var_int = fixture.input_.accept_control(
            reset(0, ruvia::http3_var_int_max + 1));
        RUVIA_CHECK(too_large_for_var_int.status_ == input_type::status_type::final_size_error);
        RUVIA_CHECK(too_large_for_var_int.protocol_.scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(fixture.input_.stopped() && fixture.session_.terminated());
    }
    {
        fixture fixture;
        RUVIA_CHECK(fixture.input_.accept_control(reset(0, 0)).status_ == input_type::status_type::reset);
        RUVIA_CHECK(fixture.input_.accept_control(reset(0, 0)).status_ == input_type::status_type::closed_stream);
        const auto conflict = fixture.input_.accept_control(reset(0, 1));
        RUVIA_CHECK(conflict.status_ == input_type::status_type::final_size_error);
        RUVIA_CHECK(conflict.protocol_.scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(fixture.input_.stopped() && fixture.session_.terminated());
    }
}

RUVIA_TEST(http3_server_stream_input_stores_deferred_reset_in_its_preallocated_stream_slot) {
    ruvia::test::counting_memory_resource upstream;
    {
        routes routes;
        ruvia::worker_memory worker(upstream);
        engine_type session_value(routes.implementation_.route_table(), worker);
        input_type input(session_value, worker, epoch, generation, 4);
        const auto allocations = upstream.allocation_count();
        const auto deferred = input.accept_control(reset(0, 12));
        RUVIA_CHECK(deferred.status_ == input_type::status_type::deferred_reset);
        RUVIA_CHECK_EQ(upstream.allocation_count(), allocations);
        RUVIA_CHECK_EQ(input.tracked_stream_count(), std::size_t{1});
        input.stop();
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_stream_input_local_cancellation_tombstones_deferred_and_completed_requests) {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        fixture fixture;
        const auto wire = request_wire(fixture.worker_, "retained");
        RUVIA_CHECK(fixture.input_.cancel_request(2).status_ == input_type::status_type::invalid_input);
        RUVIA_CHECK(fixture.input_.accept_control(fin(0, wire.size())).status_ == input_type::status_type::deferred_fin);
        std::optional<engine_type::request_lease_type> lease;
        if (scenario > 0) {
            const auto bytes_value = std::string_view(wire).substr(0, scenario == 1 ? wire.size() - 1 : wire.size());
            RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, bytes_value));
            const auto result_value = receive_data(fixture);
            RUVIA_CHECK(result_value && result_value->status_ == (scenario == 1 ? input_type::status_type::fed : input_type::status_type::finished));
            if (scenario == 2) {
                auto acquired = fixture.session_.acquire_request(0);
                RUVIA_CHECK(acquired.has_value());
                if (acquired) {
                    lease.emplace(std::move(*acquired));
                }
            }
        }
        RUVIA_CHECK(fixture.input_.cancel_request(0).status_ == input_type::status_type::local_cancelled);
        RUVIA_CHECK(fixture.input_.cancel_request(0).status_ == input_type::status_type::closed_stream);
        RUVIA_CHECK(fixture.input_.accept_control(fin(0, wire.size() + 1)).status_ == input_type::status_type::closed_stream);
        RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, wire));
        const auto late = receive_data(fixture);
        RUVIA_CHECK(late && late->status_ == input_type::status_type::closed_stream);
        if (lease) {
            RUVIA_CHECK(body_matches(fixture.session_, 0, "retained"));
        }
        lease.reset();
        RUVIA_CHECK(fixture.session_.request(0) == nullptr);
        RUVIA_CHECK(!fixture.input_.stopped());
        RUVIA_CHECK(fixture.input_.accept_control(fin(4, wire.size())).status_ == input_type::status_type::deferred_fin);
        RUVIA_CHECK(queue_data(fixture, {epoch, generation, 4}, wire));
        const auto sibling = receive_data(fixture);
        RUVIA_CHECK(sibling && sibling->status_ == input_type::status_type::finished);
        RUVIA_CHECK(body_matches(fixture.session_, 4, "retained"));
    }
}

RUVIA_TEST(http3_server_stream_input_rejects_foreign_and_stale_buffer_identity_without_mutation) {
    fixture fixture;
    const auto headers = request_headers(fixture.worker_, "POST", "/items");
    const control_type foreign_reset{.kind_ = control_type::kind::stream_reset,
        .id_ = {epoch + 1, generation, 0},
        .value_ = 0};
    const control_type stale_reset{.kind_ = control_type::kind::stream_reset,
        .id_ = {epoch, generation + 1, 0},
        .value_ = 0};
    RUVIA_CHECK(fixture.input_.accept_control(foreign_reset).status_ == input_type::status_type::foreign_epoch);
    RUVIA_CHECK(fixture.input_.accept_control(stale_reset).status_ == input_type::status_type::stale_connection);
    RUVIA_CHECK(fixture.input_.tracked_stream_count() == 0);

    RUVIA_CHECK(queue_data(fixture, {epoch + 1, generation, 0}, headers));
    const auto foreign_data = receive_data(fixture);
    RUVIA_CHECK(foreign_data.has_value() && foreign_data->status_ == input_type::status_type::foreign_epoch);
    RUVIA_CHECK(queue_data(fixture, {epoch, generation + 1, 0}, headers));
    const auto stale_data = receive_data(fixture);
    RUVIA_CHECK(stale_data.has_value() && stale_data->status_ == input_type::status_type::stale_connection);
    RUVIA_CHECK(fixture.input_.tracked_stream_count() == 0);
    RUVIA_CHECK(fixture.session_.request(0) == nullptr);

    RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, headers));
    const auto current_data = receive_data(fixture);
    RUVIA_CHECK(current_data.has_value() && current_data->status_ == input_type::status_type::fed);
    RUVIA_CHECK(fixture.input_.tracked_stream_count() == 1);
    RUVIA_CHECK(fixture.session_.request(0) != nullptr);
}

RUVIA_TEST(http3_server_stream_input_rejects_invalid_borrow_and_stream_id_without_admission) {
    fixture fixture;
    stream_buffer::borrowed_block empty;
    RUVIA_CHECK(fixture.input_.accept_data(empty).status_ == input_type::status_type::invalid_input);
    RUVIA_CHECK(fixture.input_.accept_control(reset(ruvia::http3_var_int_max + 1, 0)).status_ ==
                input_type::status_type::invalid_input);
    const auto headers = request_headers(fixture.worker_, "POST", "/items");
    RUVIA_CHECK(queue_data(fixture, {epoch, generation, ruvia::http3_var_int_max + 1}, headers));
    const auto invalid = receive_data(fixture);
    RUVIA_CHECK(invalid && invalid->status_ == input_type::status_type::invalid_input);
    RUVIA_CHECK(fixture.input_.tracked_stream_count() == 0 && fixture.session_.active_stream_count() == 0);
    RUVIA_CHECK(!fixture.input_.stopped());
}

RUVIA_TEST(http3_server_stream_input_routes_shared_buffer_without_consuming_foreign_blocks) {
    routes routes;
    ruvia::worker_memory worker;
    engine_type first_session(routes.implementation_.route_table(), worker);
    engine_type second_session(routes.implementation_.route_table(), worker);
    stream_buffer buffer(4, 4, 2);
    input_type first_input(first_session, worker, epoch, generation, 4);
    input_type second_input(second_session, worker, epoch, generation + 1, 4);
    const auto headers = request_headers(worker, "POST", "/items");
    const auto bytes_value = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(headers.data()), headers.size());
    const auto first_sent = buffer.try_send({epoch, generation, 0}, bytes_value);
    const auto second_sent = buffer.try_send({epoch, generation + 1, 4}, bytes_value);
    RUVIA_CHECK(first_sent == stream_buffer::send_result::sent);
    RUVIA_CHECK(second_sent == stream_buffer::send_result::sent);

    stream_buffer::borrowed_block block;
    RUVIA_CHECK(buffer.try_receive(block));
    RUVIA_CHECK(second_input.accept_data(block).status_ == input_type::status_type::stale_connection);
    RUVIA_CHECK(second_input.tracked_stream_count() == 0);
    RUVIA_CHECK(first_input.accept_data(block).status_ == input_type::status_type::fed);
    block.release();

    RUVIA_CHECK(buffer.try_receive(block));
    RUVIA_CHECK(first_input.accept_data(block).status_ == input_type::status_type::stale_connection);
    RUVIA_CHECK(first_input.tracked_stream_count() == 1);
    RUVIA_CHECK(first_session.request(4) == nullptr);
    RUVIA_CHECK(second_input.accept_data(block).status_ == input_type::status_type::fed);
    block.release();
    RUVIA_CHECK(first_input.tracked_stream_count() == 1);
    RUVIA_CHECK(second_input.tracked_stream_count() == 1);
    RUVIA_CHECK(second_session.request(4) != nullptr);
}

RUVIA_TEST(http3_server_stream_input_validates_final_size_and_never_feeds_past_it) {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        fixture fixture;
        const auto headers = request_headers(fixture.worker_, "POST", "/items");
        std::optional<engine_type::request_lease_type> lease;
        input_type::result_type failure;
        if (scenario < 2) {
            RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, headers));
            const auto fed = receive_data(fixture);
            RUVIA_CHECK(fed && fed->status_ == input_type::status_type::fed);
            if (scenario == 1) {
                RUVIA_CHECK(fixture.input_.accept_control(fin(0, headers.size())).status_ == input_type::status_type::finished);
                RUVIA_CHECK(fixture.input_.accept_control(fin(0, headers.size())).status_ == input_type::status_type::duplicate_fin);
                auto acquired = fixture.session_.acquire_request(0);
                RUVIA_CHECK(acquired.has_value());
                if (acquired) {
                    lease.emplace(std::move(*acquired));
                }
            }
            failure = fixture.input_.accept_control(fin(0, scenario == 0 ? headers.size() - 1 : headers.size() + 1));
        } else if (scenario == 2) {
            RUVIA_CHECK(fixture.input_.accept_control(fin(0, 1)).status_ == input_type::status_type::deferred_fin);
            RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, headers));
            const auto overrun = receive_data(fixture);
            RUVIA_CHECK(overrun.has_value());
            if (overrun) {
                failure = *overrun;
            }
        } else if (scenario == 3) {
            failure = fixture.input_.accept_control(fin(0, ruvia::http3_var_int_max + 1));
        } else {
            RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, headers));
            (void)receive_data(fixture);
            RUVIA_CHECK(fixture.input_.accept_control(fin(0, headers.size())).status_ == input_type::status_type::finished);
            RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, "x"));
            const auto excess = receive_data(fixture);
            RUVIA_CHECK(excess.has_value());
            if (excess) {
                failure = *excess;
            }
        }
        RUVIA_CHECK(failure.status_ == input_type::status_type::final_size_error);
        RUVIA_CHECK(failure.protocol_.scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(fixture.input_.stopped() && fixture.session_.terminated());
        if (lease) {
            RUVIA_CHECK(lease->request().body_complete());
        }
        lease.reset();
        RUVIA_CHECK(fixture.session_.request(0) == nullptr);
        RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, "late"));
        const auto late = receive_data(fixture);
        RUVIA_CHECK(late && late->status_ == input_type::status_type::stopped);
    }
}

RUVIA_TEST(http3_server_stream_input_reset_after_fin_retires_request_without_invalidating_lease) {
    fixture fixture;
    const auto wire = request_wire(fixture.worker_, "retained");
    RUVIA_CHECK(fixture.input_.accept_control(fin(0, wire.size())).status_ == input_type::status_type::deferred_fin);
    RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, wire));
    const auto completed = receive_data(fixture);
    RUVIA_CHECK(completed && completed->status_ == input_type::status_type::finished);
    auto lease_value = fixture.session_.acquire_request(0);
    RUVIA_CHECK(lease_value.has_value());
    RUVIA_CHECK(fixture.input_.accept_control(reset(0, wire.size())).status_ == input_type::status_type::reset);
    RUVIA_CHECK(body_matches(fixture.session_, 0, "retained"));
    lease_value.reset();
    RUVIA_CHECK(fixture.session_.request(0) == nullptr);
    RUVIA_CHECK(fixture.input_.accept_control(reset(0, wire.size())).status_ == input_type::status_type::closed_stream);
    RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, wire));
    const auto late = receive_data(fixture);
    RUVIA_CHECK(late && late->status_ == input_type::status_type::final_size_error);
    RUVIA_CHECK(fixture.input_.stopped() && fixture.session_.terminated());
}

RUVIA_TEST(http3_server_stream_input_returns_complete_session_feed_error_scope) {
    fixture fixture;
    // The frame carries one byte while the request advertises two, producing
    // an HTTP message error when the queued FIN is finally fed.
    auto invalid = request_headers(fixture.worker_, "POST", "/items", 2);
    invalid += frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data), "x");
    RUVIA_CHECK(queue_control(fixture, fin(0, invalid.size())));
    const auto deferred = receive_control(fixture);
    RUVIA_CHECK(deferred.has_value() && deferred->status_ == input_type::status_type::deferred_fin);
    RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, invalid));
    const auto failure = receive_data(fixture);
    RUVIA_CHECK(failure.has_value() && failure->status_ == input_type::status_type::protocol_error);
    RUVIA_CHECK(failure.has_value() && failure->protocol_.status_ == ruvia::http3_connection_status::stream_error);
    RUVIA_CHECK(failure.has_value() && failure->protocol_.scope_ == ruvia::http3_connection_error_scope::stream);
    RUVIA_CHECK(failure.has_value() &&
                failure->protocol_.code_ == ruvia::http3_connection_error_code::message_error);
    RUVIA_CHECK(!fixture.session_.terminated());
    RUVIA_CHECK(queue_data(fixture, {epoch, generation, 0}, "ignored"));
    const auto terminal = receive_data(fixture);
    RUVIA_CHECK(terminal.has_value() && terminal->status_ == input_type::status_type::closed_stream);
}

RUVIA_TEST(http3_server_stream_input_bounds_tombstones_and_connection_stop_drops_late_data) {
    fixture full(1);
    RUVIA_CHECK(queue_control(full, reset(0, 0)));
    const auto reset_result = receive_control(full);
    RUVIA_CHECK(reset_result.has_value() && reset_result->status_ == input_type::status_type::reset);
    const auto headers = request_headers(full.worker_, "POST", "/items");
    RUVIA_CHECK(queue_data(full, {epoch, generation, 4}, headers));
    const auto capacity = receive_data(full);
    RUVIA_CHECK(capacity.has_value() && capacity->status_ == input_type::status_type::capacity_exhausted);
    RUVIA_CHECK(full.input_.tracked_stream_count() == 1);
    RUVIA_CHECK(full.session_.request(4) == nullptr);
    RUVIA_CHECK(full.input_.stopped() && full.session_.terminated());

    fixture stopped;
    RUVIA_CHECK(queue_data(stopped, {epoch, generation, 0}, headers));
    const auto before_stop = receive_data(stopped);
    RUVIA_CHECK(before_stop.has_value() && before_stop->status_ == input_type::status_type::fed);
    const control_type connection_closed{control_type::kind::connection_closed,
        {epoch, generation, 0}, 0};
    RUVIA_CHECK(queue_control(stopped, connection_closed));
    const auto closed = receive_control(stopped);
    RUVIA_CHECK(closed.has_value() && closed->status_ == input_type::status_type::connection_closed);
    RUVIA_CHECK(stopped.input_.stopped() && stopped.session_.terminated());
    RUVIA_CHECK(queue_data(stopped, {epoch, generation, 0}, headers));
    const auto late = receive_data(stopped);
    RUVIA_CHECK(late.has_value() && late->status_ == input_type::status_type::stopped);
    RUVIA_CHECK(stopped.input_.tracked_stream_count() == 1);
}

RUVIA_TEST(http3_server_stream_input_consumes_bounded_local_buffer_with_immediate_capacity_recovery) {
    routes routes;
    ruvia::worker_memory encoding_memory;
    const std::string payload_value(65536, 'p');
    const auto wire = request_wire(encoding_memory, payload_value);
    ruvia::test::counting_memory_resource upstream;
    {
        ruvia::worker_memory worker(upstream);
        engine_type session_value(routes.implementation_.route_table(), worker);
        input_type input(session_value, worker, epoch, generation, 8);
        stream_buffer buffer(1, 1, 1);
        // Independent CONTROL may arrive first; the final byte count remains
        // a barrier until the bounded DATA lane delivers every preceding byte.
        RUVIA_CHECK(buffer.try_send_control(fin(0, wire.size())) == stream_buffer::control_result::sent);
        control_type control;
        RUVIA_CHECK(buffer.try_receive_control(control));
        RUVIA_CHECK(input.accept_control(control).status_ == input_type::status_type::deferred_fin);
        std::size_t offset = 0;
        while (offset < wire.size()) {
            const auto count = std::min(stream_buffer::max_block_bytes, wire.size() - offset);
            const auto bytes_value = std::as_bytes(std::span(wire.data() + offset, count));
            RUVIA_CHECK(buffer.try_send({epoch, generation, 0}, bytes_value) == stream_buffer::send_result::sent);
            RUVIA_CHECK(buffer.try_send({epoch, generation, 0}, bytes_value) == stream_buffer::send_result::full);
            stream_buffer::borrowed_block block;
            RUVIA_CHECK(buffer.try_receive(block));
            RUVIA_CHECK(buffer.try_send({epoch, generation, 0}, bytes_value) == stream_buffer::send_result::no_block);
            const auto result_value = input.accept_data(block);
            block.release();
            offset += count;
            RUVIA_CHECK(result_value.status_ == (offset == wire.size() ? input_type::status_type::finished : input_type::status_type::fed));
        }
        RUVIA_CHECK(body_matches(session_value, 0, payload_value));
        auto lease_value = session_value.acquire_request(0);
        RUVIA_CHECK(lease_value.has_value());
        input.stop();
        RUVIA_CHECK(body_matches(session_value, 0, payload_value));
        lease_value.reset();
        RUVIA_CHECK_EQ(session_value.active_stream_count(), std::size_t{0});
        RUVIA_CHECK(buffer.stop());
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_stream_input_returns_worker_pmr_state_at_connection_retirement) {
    ruvia::test::counting_memory_resource upstream;
    {
        routes routes;
        {
            ruvia::worker_memory worker(upstream);
            {
                engine_type session_value(routes.implementation_.route_table(), worker);
                {
                    input_type input(session_value, worker, epoch, generation, 8);
                    const auto startup_allocations = upstream.allocation_count();
                    for (std::uint64_t stream_id = 0; stream_id < 24; stream_id += 4) {
                        const auto result_value = input.accept_control(reset(stream_id, 0));
                        RUVIA_CHECK(result_value.status_ == input_type::status_type::reset);
                    }
                    RUVIA_CHECK(input.tracked_stream_count() == 6);
                    RUVIA_CHECK_EQ(upstream.allocation_count(), startup_allocations);
                    input.stop();
                }
            }
        }
    }
    RUVIA_CHECK(upstream.live_allocations() == 0);
    RUVIA_CHECK(upstream.allocation_count() == upstream.deallocation_count());
}

RUVIA_TEST(http3_server_stream_input_retains_qpack_blocked_suffix_and_fin_until_encoder_advances) {
    fixture fixture_value(16, {.qpack_max_table_capacity_ = 256, .qpack_blocked_streams_ = 2, .enable_connect_protocol_ = true});
    ruvia::http3_qpack_encoder encoder({.max_table_capacity_ = 256, .max_blocked_streams_ = 2}, fixture_value.worker_.resource());
    const std::array<ruvia::http3_field_section_field_view, 1> fields_value{{{"x-dynamic", "retained"}}};
    auto head = ruvia::encode_http3_client_request_head(encoder, 0,
        {.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .path_ = "/items", .fields_ = fields_value, .body_length_ = 7},
        {}, fixture_value.worker_.resource());
    RUVIA_CHECK((head.index() == 0));
    if ((head.index() != 0)) {
        return;
    }
    auto wire = frame(1, std::string_view(std::get<0>(head).field_section_.data(), std::get<0>(head).field_section_.size())) + frame(0, "payload");
    RUVIA_CHECK(queue_data(fixture_value, {epoch, generation, 0}, wire));
    stream_buffer::borrowed_block block;
    RUVIA_CHECK(fixture_value.buffer_.try_receive(block));
    const auto accepted = fixture_value.input_.accept_data(block);
    block.release();
    RUVIA_CHECK(accepted.status_ == input_type::status_type::deferred_qpack);
    RUVIA_CHECK(!fixture_value.input_.can_accept_input(0));
    RUVIA_CHECK(fixture_value.input_.accept_control({.kind_ = control_type::kind::stream_fin, .id_ = {epoch, generation, 0}, .value_ = wire.size()}).status_ == input_type::status_type::deferred_qpack);
    RUVIA_CHECK(!fixture_value.input_.resume_qpack());
    std::string instructions(1, char{2});
    const auto pending = encoder.pending_encoder_output();
    instructions.append(pending.data(), pending.size());
    RUVIA_CHECK(queue_data(fixture_value, {epoch, generation, 6}, instructions));
    RUVIA_CHECK(fixture_value.buffer_.try_receive(block));
    RUVIA_CHECK(fixture_value.input_.accept_data(block).status_ == input_type::status_type::fed);
    block.release();
    const auto resumed = fixture_value.input_.resume_qpack();
    RUVIA_CHECK(resumed && resumed->stream_id_ == 0 && resumed->result_.status_ == input_type::status_type::finished);
    RUVIA_CHECK(fixture_value.input_.can_accept_input(0));
    RUVIA_CHECK(fixture_value.session_.stream_state(0) == engine_type::stream_state_type::ready);
    const auto* request = fixture_value.session_.request(0);
    RUVIA_CHECK(request != nullptr);
    if (request) {
        const auto body = request->request().body_bytes();
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), "payload");
        RUVIA_CHECK_EQ(request->request().header("x-dynamic").value_or(""), "retained");
    }
    RUVIA_CHECK(!fixture_value.input_.resume_qpack());
}

RUVIA_TEST(http3_server_stream_input_reset_after_blocked_fin_releases_suffix_and_cancels_decoder_section) {
    fixture fixture_value(16, {.qpack_max_table_capacity_ = 256, .qpack_blocked_streams_ = 2, .enable_connect_protocol_ = true});
    ruvia::http3_qpack_encoder encoder({.max_table_capacity_ = 256, .max_blocked_streams_ = 2}, fixture_value.worker_.resource());
    const std::array fields_value{ruvia::http3_field_section_field_view{"x-dynamic", "retained"}};
    auto head = ruvia::encode_http3_client_request_head(encoder, 0,
        {.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .path_ = "/items", .fields_ = fields_value, .body_length_ = 7}, {}, fixture_value.worker_.resource());
    RUVIA_CHECK((head.index() == 0));
    if ((head.index() != 0)) {
        return;
    }
    const auto wire = frame(1, {std::get<0>(head).field_section_.data(), std::get<0>(head).field_section_.size()}) + frame(0, "payload");
    RUVIA_CHECK(queue_data(fixture_value, {epoch, generation, 0}, wire));
    stream_buffer::borrowed_block block;
    RUVIA_CHECK(fixture_value.buffer_.try_receive(block));
    RUVIA_CHECK(fixture_value.input_.accept_data(block).status_ == input_type::status_type::deferred_qpack);
    block.release();
    RUVIA_CHECK(fixture_value.input_.accept_control({control_type::kind::stream_fin, {epoch, generation, 0}, wire.size()}).status_ == input_type::status_type::deferred_qpack);
    RUVIA_CHECK(fixture_value.input_.accept_control({control_type::kind::stream_reset, {epoch, generation, 0}, wire.size()}).status_ == input_type::status_type::reset);
    RUVIA_CHECK(!fixture_value.input_.stopped());
    RUVIA_CHECK(!fixture_value.input_.resume_qpack());
    RUVIA_CHECK_EQ(fixture_value.input_.active_request_stream_count(), std::size_t{0});
    RUVIA_CHECK(!fixture_value.session_.pending_qpack_decoder_output().empty());
}
