#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <asio/co_spawn.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_var_int.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/web/http_client_types.h"

#include "client/http_client_response_decoding.h"
#include "client/http_client_response_state.h"
#include "client/http_client_result_budget.h"
#include "http3/http3_client_body_budget.h"
#include "http3/http3_client_receive_driver.h"
#include "http3/http3_client_response_delivery.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
using delivery_type = ruvia::detail::http3_client_response_delivery;
using driver_type = ruvia::detail::http3_client_receive_driver;
using engine_type = ruvia::detail::http3_client_sans_io_session_engine;
using read_type = ruvia::quic_stream_read_result;
using state_type = ruvia::detail::http_client_response_state;
using body_budget_lease_type = ruvia::detail::http3_client_body_budget::lease_type;

static_assert(!std::is_copy_constructible_v<body_budget_lease_type>);
static_assert(!std::is_copy_assignable_v<body_budget_lease_type>);
static_assert(std::is_nothrow_move_constructible_v<body_budget_lease_type>);
static_assert(std::is_nothrow_move_assignable_v<body_budget_lease_type>);

class test_worker final {
public:
    explicit test_worker(asio::io_context& io)
        : attachment_(ruvia::attach_event_loop(io, {.queue_capacity_ = 8})),
          handle_(attachment_.loop().handle()) {}

    ruvia::event_loop_attachment attachment_;
    ruvia::worker_handle handle_;
};

template <typename operation_type>
void run_operation(test_worker& worker_value, asio::io_context& io, operation_type&& operation) {
    std::exception_ptr failure;
    asio::co_spawn(io, ruvia::as_awaitable(operation()),
        [&worker_value, &failure](std::exception_ptr error) {
            failure = error;
            worker_value.attachment_.stop();
        });
    worker_value.attachment_.run();
    io.restart();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

std::vector<char> frame(std::uint64_t type, std::span<const char> payload_value) {
    std::vector<char> output(16);
    const auto header_value = ruvia::encode_http3_var_int(output, type);
    const auto length = ruvia::encode_http3_var_int(
        std::span<char>(output).subspan(std::get<0>(header_value)), payload_value.size());
    output.resize(std::get<0>(header_value) + std::get<0>(length));
    output.insert(output.end(), payload_value.begin(), payload_value.end());
    return output;
}

void append_frame(std::vector<char>& wire, std::uint64_t type, std::string_view payload_value) {
    const auto encoded = frame(type, std::span<const char>(payload_value.data(), payload_value.size()));
    wire.insert(wire.end(), encoded.begin(), encoded.end());
}

std::vector<char> response_head(std::string_view status = "200",
    std::optional<std::string_view> content_length = {},
    std::optional<std::string_view> content_encoding = {}) {
    std::pmr::monotonic_buffer_resource temp;
    std::vector<ruvia::http3_field_section_field_view> fields_value{{":status", status}, {"x-owned", "copied"}};
    if (content_length) {
        fields_value.push_back({"content-length", *content_length});
    }
    if (content_encoding) {
        fields_value.push_back({"content-encoding", *content_encoding});
    }
    const auto encoded = ruvia::encode_http3_field_section(fields_value, &temp);
    return frame(1, std::get<0>(encoded));
}

std::vector<char> response_trailer() {
    std::pmr::monotonic_buffer_resource temp;
    constexpr std::array fields_value{ruvia::http3_field_section_field_view{"x-trailer", "done"}};
    const auto encoded = ruvia::encode_http3_field_section(fields_value, &temp);
    return frame(1, std::get<0>(encoded));
}

struct fake_read final {
    std::vector<char> wire_;
    std::size_t position_{};
    std::size_t max_chunk_{4096};
    bool fin_{};
    bool reset_{};

    read_type operator()(std::uint64_t, std::span<char> destination) {
        if (reset_) {
            return {.status_ = ruvia::quic_stream_read_status::reset};
        }
        if (position_ == wire_.size()) {
            return {.status_ = fin_ ? ruvia::quic_stream_read_status::fin : ruvia::quic_stream_read_status::would_block};
        }
        const auto size = std::min({destination.size(), wire_.size() - position_, max_chunk_});
        std::copy_n(wire_.data() + position_, size, destination.data());
        position_ += size;
        return {.status_ = ruvia::quic_stream_read_status::data, .size_ = size};
    }
};

struct wake_counter final {
    std::size_t notifications_{};

    static void notify(void* context_value) noexcept {
        ++static_cast<wake_counter*>(context_value)->notifications_;
    }
};

bool plan_matches(const std::optional<ruvia::http_response_body_plan>& plan,
    ruvia::http_known_method method, std::uint16_t status,
    ruvia::http_response_content_semantics semantics, bool body_suppressed) {
    return plan && plan->request_method() == method &&
           plan->response_status() == ruvia::http_status_code::from_value(status) &&
           plan->content_semantics() == semantics &&
           plan->body_suppressed() == body_suppressed;
}
}  // namespace

RUVIA_TEST(http3_client_response_delivery_copies_events_without_invalidating_returned_body_view) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::counting_memory_resource resource;
    ruvia::detail::http3_client_body_budget body_budget(4096);
    {
        state_type state_value(worker.handle_, &resource);
        const auto before_lease_allocations = resource.allocation_count();
        delivery_type delivery(state_value, &body_budget);
        RUVIA_CHECK_EQ(resource.allocation_count(), before_lease_allocations);
        engine_type engine(&resource);
        driver_type driver(engine);
        const std::string first_body(512, 'a');
        const std::string second_body(1024, 'b');
        std::string_view returned_view;

        auto operation = [&]() -> ruvia::task<void> {
            RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get,
                                  delivery.event_sink())
                            .scope_ == ruvia::http3_connection_error_scope::none);

            fake_read initial_value{.wire_ = response_head()};
            append_frame(initial_value.wire_, 0, first_body);
            const auto head_and_body = driver.drive(0, initial_value);
            RUVIA_CHECK(head_and_body.status_ == driver_type::status_type::progress);
            RUVIA_CHECK(state_value.head_ready_);
            RUVIA_CHECK(state_value.status_.value() == 200);
            const auto plan = delivery.response_body_plan();
            RUVIA_CHECK(plan && plan->request_method() == ruvia::http_known_method::get &&
                        plan->response_status() == ruvia::http_status::ok);
            RUVIA_CHECK(state_value.protocol_version_ == ruvia::http_protocol_version::http3);
            RUVIA_CHECK(state_value.headers_.size() == 1);
            RUVIA_CHECK(state_value.headers_.front().name() == "x-owned");
            RUVIA_CHECK(state_value.headers_.front().value() == "copied");
            RUVIA_CHECK(!state_value.complete_);

            const auto first = co_await state_value.consume_body<std::string_view>();
            RUVIA_CHECK(first.has_value());
            RUVIA_CHECK_EQ(first->size(), first_body.size());
            returned_view = *first;
            const auto* const returned_address = returned_view.data();

            fake_read later{.wire_ = {}};
            append_frame(later.wire_, 0, second_body);
            const auto data = driver.drive(0, later);
            RUVIA_CHECK(data.status_ == driver_type::status_type::progress);
            RUVIA_CHECK_EQ(state_value.buffered_.size(), first_body.size());
            RUVIA_CHECK(state_value.buffered_.data() == returned_address);
            RUVIA_CHECK(returned_view == first_body);
            RUVIA_CHECK(std::string_view(state_value.pending_) == second_body);

            // Trailers are a separate HEADERS field section, so use a fresh input
            // after the body bytes have already been consumed by the first drive.
            fake_read trailers{.wire_ = response_trailer()};
            RUVIA_CHECK(driver.drive(0, trailers).status_ == driver_type::status_type::progress);
            RUVIA_CHECK(state_value.trailers_.size() == 1);
            RUVIA_CHECK(state_value.trailers_.front().name() == "x-trailer");
            RUVIA_CHECK(state_value.trailers_.front().value() == "done");

            fake_read end{.fin_ = true};
            const auto finished = driver.drive(0, end);
            RUVIA_CHECK(finished.status_ == driver_type::status_type::response_complete);
            RUVIA_CHECK(!state_value.complete_);
            RUVIA_CHECK(!state_value.failure_);
            RUVIA_CHECK(!state_value.error_code_);
            RUVIA_CHECK(delivery.commit(finished) == delivery_type::commit_status_type::committed);
            RUVIA_CHECK(state_value.complete_);
            RUVIA_CHECK(returned_view == first_body);
            RUVIA_CHECK(std::string_view(state_value.pending_) == second_body);
            RUVIA_CHECK_EQ(body_budget.used(), first_body.size() + second_body.size());
            RUVIA_CHECK(engine.release(0));
        };
        run_operation(worker, io, operation);
    }
    RUVIA_CHECK_EQ(body_budget.used(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_keeps_final_plans_for_decoder_and_terminal_paths) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::counting_memory_resource resource;
    auto operation = [&]() -> ruvia::task<void> {
        struct case_value final {
            ruvia::http_known_method method_;
            std::string_view status_;
            std::optional<std::string_view> content_length_;
            std::string_view body_;
            std::uint16_t status_code_;
            ruvia::http_response_content_semantics semantics_;
            bool body_suppressed_;
        };
        constexpr std::array cases{
            case_value{ruvia::http_known_method::get, "200", "3", "abc", 200,
                ruvia::http_response_content_semantics::with_content, false},
            case_value{ruvia::http_known_method::get, "200", {}, {}, 200,
                ruvia::http_response_content_semantics::with_content, false},
            case_value{ruvia::http_known_method::head, "200", "5", {}, 200,
                ruvia::http_response_content_semantics::without_content, true},
            case_value{ruvia::http_known_method::get, "204", {}, {}, 204,
                ruvia::http_response_content_semantics::without_content, true},
            case_value{ruvia::http_known_method::get, "304", "7", {}, 304,
                ruvia::http_response_content_semantics::without_content, true},
        };
        for (const auto& item : cases) {
            state_type state_value(worker.handle_, &resource);
            delivery_type delivery(state_value);
            engine_type engine(&resource);
            driver_type driver(engine);
            RUVIA_CHECK(engine.register_request(0, item.method_, delivery.event_sink()).scope_ ==
                        ruvia::http3_connection_error_scope::none);
            fake_read input{.wire_ = response_head(item.status_, item.content_length_), .fin_ = true};
            if (!item.body_.empty()) {
                append_frame(input.wire_, 0, item.body_);
            }
            RUVIA_CHECK(driver.drive(0, input).status_ == driver_type::status_type::progress);
            RUVIA_CHECK(plan_matches(delivery.response_body_plan(), item.method_, item.status_code_,
                item.semantics_, item.body_suppressed_));
            if (item.method_ == ruvia::http_known_method::head) {
                const auto length = std::find_if(state_value.headers_.begin(), state_value.headers_.end(),
                    [](const ruvia::http_header& header_value) { return header_value.name() == "content-length"; });
                RUVIA_CHECK(length != state_value.headers_.end() && length->value() == "5");
            }
            if (!item.body_.empty()) {
                RUVIA_CHECK(std::string_view(state_value.pending_) == item.body_);
            }
            const auto complete_value = driver.drive(0, input);
            RUVIA_CHECK(complete_value.status_ == driver_type::status_type::response_complete);
            RUVIA_CHECK(delivery.commit(complete_value) == delivery_type::commit_status_type::committed);
            RUVIA_CHECK(plan_matches(delivery.response_body_plan(), item.method_, item.status_code_,
                item.semantics_, item.body_suppressed_));
            RUVIA_CHECK(engine.release(0));
        }

        state_type informational_state(worker.handle_, &resource);
        delivery_type informational_delivery(informational_state);
        engine_type informational_engine(&resource);
        driver_type informational_driver(informational_engine);
        RUVIA_CHECK(informational_engine.register_request(0, ruvia::http_known_method::get,
                                            informational_delivery.event_sink())
                        .scope_ == ruvia::http3_connection_error_scope::none);
        fake_read informational{.wire_ = response_head("103")};
        RUVIA_CHECK(informational_driver.drive(0, informational).status_ == driver_type::status_type::progress);
        RUVIA_CHECK(!informational_delivery.response_body_plan());
        RUVIA_CHECK_EQ(informational_state.informational_.size(), std::size_t{1});
        RUVIA_CHECK_EQ(informational_state.informational_.front().status().value(), std::uint16_t{103});
        RUVIA_CHECK_EQ(informational_state.informational_.front().headers().front().value(), "copied");
        fake_read final_head{.wire_ = response_head("200"), .fin_ = true};
        RUVIA_CHECK(informational_driver.drive(0, final_head).status_ == driver_type::status_type::progress);
        RUVIA_CHECK(plan_matches(informational_delivery.response_body_plan(), ruvia::http_known_method::get,
            200, ruvia::http_response_content_semantics::with_content, false));
        const auto informational_end = informational_driver.drive(0, final_head);
        RUVIA_CHECK(informational_delivery.commit(informational_end) == delivery_type::commit_status_type::committed);
        RUVIA_CHECK(plan_matches(informational_delivery.response_body_plan(), ruvia::http_known_method::get,
            200, ruvia::http_response_content_semantics::with_content, false));
        RUVIA_CHECK(informational_engine.release(0));

        state_type reset_state(worker.handle_, &resource);
        delivery_type reset_delivery(reset_state);
        engine_type reset_engine(&resource);
        driver_type reset_driver(reset_engine);
        RUVIA_CHECK(reset_engine.register_request(0, ruvia::http_known_method::get, reset_delivery.event_sink()).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        fake_read observed_head{.wire_ = response_head("200")};
        RUVIA_CHECK(reset_driver.drive(0, observed_head).status_ == driver_type::status_type::progress);
        RUVIA_CHECK(plan_matches(reset_delivery.response_body_plan(), ruvia::http_known_method::get, 200,
            ruvia::http_response_content_semantics::with_content, false));
        fake_read reset{.reset_ = true};
        const auto reset_result = reset_driver.drive(0, reset);
        RUVIA_CHECK(reset_delivery.commit(reset_result) == delivery_type::commit_status_type::committed);
        RUVIA_CHECK(plan_matches(reset_delivery.response_body_plan(), ruvia::http_known_method::get, 200,
            ruvia::http_response_content_semantics::with_content, false));
        RUVIA_CHECK(reset_engine.release(0));

        state_type no_head_state(worker.handle_, &resource);
        delivery_type no_head_delivery(no_head_state);
        engine_type no_head_engine(&resource);
        driver_type no_head_driver(no_head_engine);
        RUVIA_CHECK(no_head_engine.register_request(0, ruvia::http_known_method::get, no_head_delivery.event_sink()).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        fake_read no_head_reset{.reset_ = true};
        RUVIA_CHECK(no_head_delivery.commit(no_head_driver.drive(0, no_head_reset)) ==
                    delivery_type::commit_status_type::committed);
        RUVIA_CHECK(!no_head_delivery.response_body_plan());
        RUVIA_CHECK(no_head_engine.release(0));
        co_return;
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_uses_physical_buffered_occupancy_and_defers_overflow) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::counting_memory_resource resource;
    {
        state_type state_value(worker.handle_, &resource);
        state_value.buffered_limit_ = 8;
        state_value.buffered_.assign("abcde");
        state_value.offset_ = 4;
        state_value.pending_.assign("xy");
        delivery_type delivery(state_value);
        engine_type engine(&resource);
        driver_type driver(engine);

        auto operation = [&]() -> ruvia::task<void> {
            RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get,
                                  delivery.event_sink())
                            .scope_ == ruvia::http3_connection_error_scope::none);
            fake_read head{.wire_ = response_head()};
            RUVIA_CHECK(driver.drive(0, head).status_ == driver_type::status_type::progress);

            const auto allowance = delivery.read_allowance(64);
            RUVIA_CHECK(allowance.status_ == delivery_type::read_status_type::ready);
            RUVIA_CHECK_EQ(allowance.bytes_, std::size_t{1});

            fake_read body{.wire_ = {}};
            append_frame(body.wire_, 0, "zz");
            // A compliant producer must stop once its body storage is full.
            // Exercise overflow independently by deliberately delivering a
            // complete DATA block larger than the available single byte.
            const auto result_value = driver.drive(0, body);
            RUVIA_CHECK(result_value.status_ == driver_type::status_type::progress);

            RUVIA_CHECK(delivery.retirement_reason() ==
                        delivery_type::retirement_reason_type::response_too_large);
            const auto blocked = delivery.read_allowance(64);
            RUVIA_CHECK(blocked.status_ == delivery_type::read_status_type::retirement_required);
            RUVIA_CHECK_EQ(blocked.bytes_, std::size_t{0});
            RUVIA_CHECK(delivery.commit(result_value) == delivery_type::commit_status_type::retirement_required);
            RUVIA_CHECK(!state_value.complete_);
            RUVIA_CHECK(!state_value.error_code_);
            RUVIA_CHECK_EQ(std::string_view(state_value.pending_), std::string_view("xy"));
            RUVIA_CHECK_EQ(std::string_view(state_value.buffered_), std::string_view("abcde"));

            // Represents the owner completing QUIC STOP_SENDING and parser
            // retirement after drive() returned; neither is performed by callback.
            RUVIA_CHECK(engine.cancel_request(0));
            RUVIA_CHECK(delivery.commit_retirement_failure(
                ruvia::http_client_error::code_type::response_too_large));
            RUVIA_CHECK(state_value.complete_);
            RUVIA_CHECK(state_value.error_code_ == static_cast<std::uint8_t>(
                                                       ruvia::http_client_error::code_type::response_too_large));
            RUVIA_CHECK(engine.release(0));
            co_return;
        };
        run_operation(worker, io, operation);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_commits_reset_only_after_owner_observes_it) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::counting_memory_resource resource;
    {
        state_type state_value(worker.handle_, &resource);
        delivery_type delivery(state_value);
        engine_type engine(&resource);
        driver_type driver(engine);

        auto operation = [&]() -> ruvia::task<void> {
            RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get,
                                  delivery.event_sink())
                            .scope_ == ruvia::http3_connection_error_scope::none);
            fake_read head{.wire_ = response_head()};
            RUVIA_CHECK(driver.drive(0, head).status_ == driver_type::status_type::progress);
            fake_read reset{.reset_ = true};
            const auto failed = driver.drive(0, reset);
            RUVIA_CHECK(failed.status_ == driver_type::status_type::stream_reset);
            RUVIA_CHECK(!state_value.complete_);
            RUVIA_CHECK(!state_value.error_code_);
            RUVIA_CHECK(delivery.commit(failed) == delivery_type::commit_status_type::committed);
            RUVIA_CHECK(state_value.complete_);
            RUVIA_CHECK(state_value.error_code_ == static_cast<std::uint8_t>(
                                                       ruvia::http_client_error::code_type::protocol_error));
            RUVIA_CHECK(engine.release(0));
            co_return;
        };
        run_operation(worker, io, operation);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_overflow_isolates_other_streams) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::counting_memory_resource resource;
    auto operation = [&]() -> ruvia::task<void> {
        state_type rejected(worker.handle_, &resource);
        state_type accepted(worker.handle_, &resource);
        rejected.buffered_limit_ = 1;
        rejected.collect_all_ = accepted.collect_all_ = true;
        delivery_type first(rejected);
        delivery_type second(accepted);
        engine_type engine(&resource);
        driver_type driver(engine);
        RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get,
                              first.event_sink())
                        .scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(engine.register_request(4, ruvia::http_known_method::get,
                              second.event_sink())
                        .scope_ == ruvia::http3_connection_error_scope::none);
        fake_read overflowing{.wire_ = response_head()};
        append_frame(overflowing.wire_, 0, "too large");
        const auto overflow = driver.drive(0, overflowing);
        RUVIA_CHECK(overflow.status_ == driver_type::status_type::progress);
        RUVIA_CHECK(first.commit(overflow) == delivery_type::commit_status_type::retirement_required);
        RUVIA_CHECK(!rejected.complete_ && !accepted.complete_);
        // Simulated transport ceases all delivery before local parser retirement.
        RUVIA_CHECK(engine.cancel_request(0));
        RUVIA_CHECK(engine.release(0));
        RUVIA_CHECK(first.commit_retirement_failure(ruvia::http_client_error::code_type::response_too_large));

        fake_read normal{.wire_ = response_head(), .fin_ = true};
        append_frame(normal.wire_, 0, "survives");
        const auto progress_value = driver.drive(4, normal);
        RUVIA_CHECK(second.commit(progress_value) == delivery_type::commit_status_type::pending);
        const auto end = driver.drive(4, normal);
        RUVIA_CHECK(second.commit(end) == delivery_type::commit_status_type::committed);
        RUVIA_CHECK(accepted.complete_ && !accepted.error_code_ && accepted.pending_ == "survives");
        RUVIA_CHECK(engine.release(4));
        RUVIA_CHECK(engine.register_request(8, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(engine.cancel_request(8) && engine.release(8));
        co_return;
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_connection_error_precedes_local_overflow) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::counting_memory_resource resource;
    auto operation = [&]() -> ruvia::task<void> {
        state_type state_value(worker.handle_, &resource);
        state_value.buffered_limit_ = 0;
        state_value.collect_all_ = true;
        delivery_type delivery(state_value);
        engine_type engine(&resource);
        driver_type driver(engine);
        RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get,
                              delivery.event_sink())
                        .scope_ == ruvia::http3_connection_error_scope::none);
        fake_read input{.wire_ = response_head()};
        append_frame(input.wire_, 0, "overflow");
        append_frame(input.wire_, 4, "");  // SETTINGS is illegal on a response stream.
        const auto result_value = driver.drive(0, input);
        RUVIA_CHECK(delivery.retirement_reason() == delivery_type::retirement_reason_type::response_too_large);
        RUVIA_CHECK(result_value.status_ == driver_type::status_type::connection_error);
        // The fake transport is now considered closed; only then commit failure.
        RUVIA_CHECK(delivery.commit(result_value) == delivery_type::commit_status_type::committed);
        RUVIA_CHECK(state_value.complete_ && state_value.error_code_ ==
                                                 static_cast<std::uint8_t>(ruvia::http_client_error::code_type::protocol_error));
        RUVIA_CHECK(!delivery.commit_retirement_failure(ruvia::http_client_error::code_type::response_too_large));
        RUVIA_CHECK(engine.release(0));
        co_return;
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http3_client_response_delivery_collect_all_wakes_paused_producer_and_preserves_retry) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::counting_memory_resource resource;
    auto result_budget = std::make_shared<ruvia::detail::http_client_result_budget_domain>(
        ruvia::http_client_result_budget_config{});
    auto operation = [&]() -> ruvia::task<void> {
        state_type state_value(worker.handle_, &resource);
        state_value.result_budget_domain_ = &result_budget;
        state_value.buffered_limit_ = 3;
        delivery_type delivery(state_value);
        engine_type engine(&resource);
        driver_type driver(engine);
        ruvia::task_scope tasks(worker.handle_, {.resource_ = &resource});
        ruvia::worker_signal paused(worker.handle_);
        RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get,
                              delivery.event_sink())
                        .scope_ == ruvia::http3_connection_error_scope::none);
        fake_read initial_value{.wire_ = response_head()};
        append_frame(initial_value.wire_, 0, "abc");
        RUVIA_CHECK(driver.drive(0, initial_value).status_ == driver_type::status_type::progress);
        RUVIA_CHECK(state_value.pending_ == "abc" && !state_value.complete_);
        fake_read tail{.wire_ = response_trailer(), .fin_ = true};
        bool waited_for_space = false;
        auto producer_value = [&]() -> ruvia::task<void> {
            try {
                for (std::size_t step = 0; step < tail.wire_.size() + 4 && !state_value.complete_; ++step) {
                    const auto allowance = delivery.read_allowance(driver_type::read_block_bytes);
                    if (allowance.status_ == delivery_type::read_status_type::backpressured) {
                        waited_for_space = true;
                        paused.notify();
                        co_await state_value.space_signal_.wait();
                        continue;
                    }
                    if (allowance.status_ != delivery_type::read_status_type::ready || allowance.bytes_ == 0) {
                        throw std::logic_error("collecting producer cannot make protocol progress");
                    }
                    const auto input = driver.drive(0, tail, allowance.bytes_);
                    (void)delivery.commit(input);
                }
                if (!state_value.complete_) {
                    throw std::logic_error("collecting producer exceeded work bound");
                }
            } catch (...) {
                // Simulated transport terminates delivery before engine teardown.
                (void)engine.stop();
                (void)delivery.commit_failure(std::current_exception());
                paused.notify();
            }
        };
        tasks.spawn(producer_value());
        co_await paused.wait();
        bool too_small = false;
        try {
            (void)co_await ruvia::make_scoped_operation(state_value.body_operation_scope_, state_value.read_all(2));
        } catch (const ruvia::http_client_error& error) {
            too_small = error.code() == ruvia::http_client_error::code_type::response_too_large;
        }
        co_await tasks.join();
        RUVIA_CHECK(waited_for_space && too_small && state_value.complete_);
        RUVIA_CHECK(!state_value.failure_ && !state_value.error_code_);
        RUVIA_CHECK(state_value.pending_ == "abc" && state_value.offset_ == 0);
        const auto body = co_await ruvia::make_scoped_operation(state_value.body_operation_scope_, state_value.read_all(3));
        const auto body_bytes = body.bytes();
        RUVIA_CHECK(body_bytes.size() == 3 && body_bytes.front() == std::byte{'a'} &&
                    body_bytes.back() == std::byte{'c'});
        RUVIA_CHECK_EQ(state_value.trailers_.size(), std::size_t{1});
        RUVIA_CHECK(engine.release(0));
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_collect_all_probes_through_trailers_and_fin) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::counting_memory_resource resource;
    auto operation = [&]() -> ruvia::task<void> {
        for (const auto limit : {std::size_t{0}, std::size_t{3}}) {
            for (std::size_t size = 0; size <= limit + 1; ++size) {
                state_type state_value(worker.handle_, &resource);
                state_value.buffered_limit_ = limit;
                state_value.collect_all_ = true;
                delivery_type delivery(state_value);
                engine_type engine(&resource);
                driver_type driver(engine);
                RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get,
                                      delivery.event_sink())
                                .scope_ == ruvia::http3_connection_error_scope::none);
                fake_read input{.wire_ = response_head(), .fin_ = true};
                append_frame(input.wire_, 0, std::string(size, 'x'));
                append_frame(input.wire_, 0, "");
                const auto trailer = response_trailer();
                input.wire_.insert(input.wire_.end(), trailer.begin(), trailer.end());
                bool settled = false;
                for (std::size_t step = 0; step < input.wire_.size() + 2 && !settled; ++step) {
                    const auto allowance = delivery.read_allowance(4);
                    RUVIA_CHECK(allowance.status_ == delivery_type::read_status_type::ready);
                    if (allowance.bytes_ == 0) {
                        break;
                    }
                    const auto result_value = driver.drive(0, input, allowance.bytes_);
                    const auto committed = delivery.commit(result_value);
                    if (committed == delivery_type::commit_status_type::retirement_required) {
                        RUVIA_CHECK(size > limit);
                        // Fake transport stops delivering this stream here.
                        if (!engine.response(0)) {
                            RUVIA_CHECK(engine.cancel_request(0));
                        }
                        RUVIA_CHECK(delivery.commit_retirement_failure(
                            ruvia::http_client_error::code_type::response_too_large));
                    }
                    settled = state_value.complete_;
                }
                RUVIA_CHECK(settled);
                RUVIA_CHECK(state_value.pending_.size() <= limit);
                if (size <= limit) {
                    RUVIA_CHECK(!state_value.error_code_ && !state_value.failure_);
                    RUVIA_CHECK_EQ(state_value.pending_.size(), size);
                    RUVIA_CHECK_EQ(state_value.trailers_.size(), std::size_t{1});
                } else {
                    RUVIA_CHECK(state_value.error_code_ == static_cast<std::uint8_t>(
                                                               ruvia::http_client_error::code_type::response_too_large));
                }
                RUVIA_CHECK(engine.release(0));
            }
        }
        co_return;
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_shared_budget_backpressures_per_stream_and_resumes_after_consumption) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::counting_memory_resource resource;
    auto operation = [&]() -> ruvia::task<void> {
        ruvia::detail::http3_client_body_budget budget(6);
        state_type first_state(worker.handle_, &resource);
        state_type second_state(worker.handle_, &resource);
        state_type waiting_state(worker.handle_, &resource);
        state_type framing_state(worker.handle_, &resource);
        first_state.buffered_limit_ = second_state.buffered_limit_ = 3;
        waiting_state.buffered_limit_ = framing_state.buffered_limit_ = 3;
        framing_state.collect_all_ = true;
        delivery_type first(first_state, &budget);
        delivery_type second(second_state, &budget);
        delivery_type waiting(waiting_state, &budget);
        delivery_type framing(framing_state, &budget);
        engine_type engine(&resource, budget);
        driver_type driver(engine);
        RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get, first.event_sink()).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(engine.register_request(4, ruvia::http_known_method::get, second.event_sink()).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        fake_read first_head{.wire_ = response_head()};
        RUVIA_CHECK(driver.drive(0, first_head).status_ == driver_type::status_type::progress);
        fake_read first_body{.wire_ = frame(0, std::span<const char>("abc", 3))};
        for (std::size_t step = 0; step < first_body.wire_.size() + 2 && first_state.pending_.size() != 3;
            ++step) {
            const auto allowance = first.read_allowance(driver_type::read_block_bytes);
            RUVIA_CHECK(allowance.status_ == delivery_type::read_status_type::ready);
            RUVIA_CHECK(driver.drive(0, first_body, allowance.bytes_).status_ == driver_type::status_type::progress);
        }
        RUVIA_CHECK_EQ(first_state.pending_, std::string_view("abc"));
        RUVIA_CHECK(first.read_allowance(driver_type::read_block_bytes).status_ ==
                    delivery_type::read_status_type::backpressured);
        RUVIA_CHECK_EQ(budget.used(), std::size_t{3});

        fake_read second_head{.wire_ = response_head()};
        RUVIA_CHECK(driver.drive(4, second_head).status_ == driver_type::status_type::progress);
        RUVIA_CHECK(second.read_allowance(driver_type::read_block_bytes).status_ ==
                    delivery_type::read_status_type::ready);
        fake_read second_body{.wire_ = frame(0, std::span<const char>("xyz", 3))};
        for (std::size_t step = 0; step < second_body.wire_.size() + 2 && second_state.pending_.size() != 3;
            ++step) {
            const auto allowance = second.read_allowance(driver_type::read_block_bytes);
            RUVIA_CHECK(allowance.status_ == delivery_type::read_status_type::ready);
            RUVIA_CHECK(driver.drive(4, second_body, allowance.bytes_).status_ == driver_type::status_type::progress);
        }
        RUVIA_CHECK_EQ(second_state.pending_, std::string_view("xyz"));
        RUVIA_CHECK_EQ(budget.used(), std::size_t{6});
        RUVIA_CHECK(waiting.read_allowance(driver_type::read_block_bytes).status_ ==
                    delivery_type::read_status_type::backpressured);
        const auto framing_probe = framing.read_allowance(driver_type::read_block_bytes);
        RUVIA_CHECK(framing_probe.status_ == delivery_type::read_status_type::ready && framing_probe.bytes_ == 1);

        const auto borrowed = co_await first_state.consume_body<std::string_view>();
        RUVIA_CHECK(borrowed && *borrowed == "abc");
        RUVIA_CHECK_EQ(budget.used(), std::size_t{6});
        first_state.release_consumed_body_prefix();
        first.reconcile_body_bytes();
        RUVIA_CHECK_EQ(budget.used(), std::size_t{3});
        RUVIA_CHECK(first.read_allowance(driver_type::read_block_bytes).status_ ==
                    delivery_type::read_status_type::ready);
        RUVIA_CHECK(waiting.read_allowance(driver_type::read_block_bytes).status_ ==
                    delivery_type::read_status_type::ready);
        RUVIA_CHECK(framing.read_allowance(driver_type::read_block_bytes).status_ ==
                    delivery_type::read_status_type::ready);

        RUVIA_CHECK(engine.cancel_request(0) && engine.release(0));
        RUVIA_CHECK(engine.cancel_request(4) && engine.release(4));
        first_state.discard_response_body();
        first.reconcile_body_bytes();
        second_state.discard_response_body();
        second.reconcile_body_bytes();
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        co_return;
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_two_collect_all_streams_parse_trailers_and_fin_at_shared_limit) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::counting_memory_resource resource;
    auto operation = [&]() -> ruvia::task<void> {
        ruvia::detail::http3_client_body_budget budget(6);
        state_type first_state(worker.handle_, &resource);
        state_type second_state(worker.handle_, &resource);
        first_state.buffered_limit_ = second_state.buffered_limit_ = 4;
        first_state.collect_all_ = second_state.collect_all_ = true;
        delivery_type first(first_state, &budget);
        delivery_type second(second_state, &budget);
        engine_type engine(&resource, budget);
        driver_type driver(engine);
        RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get, first.event_sink()).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(engine.register_request(4, ruvia::http_known_method::get, second.event_sink()).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        fake_read first_head{.wire_ = response_head()};
        fake_read second_head{.wire_ = response_head()};
        RUVIA_CHECK(driver.drive(0, first_head).status_ == driver_type::status_type::progress);
        RUVIA_CHECK(driver.drive(4, second_head).status_ == driver_type::status_type::progress);

        fake_read first_body{.wire_ = frame(0, std::span<const char>("abc", 3))};
        for (std::size_t step = 0; step < first_body.wire_.size() + 2 && first_state.pending_.size() != 3;
            ++step) {
            const auto allowance = first.read_allowance(driver_type::read_block_bytes);
            RUVIA_CHECK(allowance.status_ == delivery_type::read_status_type::ready);
            RUVIA_CHECK(driver.drive(0, first_body, allowance.bytes_).status_ == driver_type::status_type::progress);
        }
        fake_read second_body{.wire_ = frame(0, std::span<const char>("xyz", 3))};
        for (std::size_t step = 0; step < second_body.wire_.size() + 2 && second_state.pending_.size() != 3;
            ++step) {
            const auto allowance = second.read_allowance(driver_type::read_block_bytes);
            RUVIA_CHECK(allowance.status_ == delivery_type::read_status_type::ready);
            RUVIA_CHECK(driver.drive(4, second_body, allowance.bytes_).status_ == driver_type::status_type::progress);
        }
        RUVIA_CHECK_EQ(first_state.pending_, std::string_view("abc"));
        RUVIA_CHECK_EQ(second_state.pending_, std::string_view("xyz"));
        RUVIA_CHECK_EQ(budget.used(), std::size_t{6});

        const auto trailer = response_trailer();
        fake_read first_tail{.wire_ = trailer, .fin_ = true};
        fake_read second_tail{.wire_ = trailer, .fin_ = true};
        auto finish_value = [&](std::uint64_t id, fake_read& input, delivery_type& delivery, state_type& state_value) {
            for (std::size_t step = 0; step < input.wire_.size() + 2 && !state_value.complete_; ++step) {
                const auto allowance = delivery.read_allowance(driver_type::read_block_bytes);
                RUVIA_CHECK(allowance.status_ == delivery_type::read_status_type::ready);
                RUVIA_CHECK_EQ(allowance.bytes_, std::size_t{1});
                const auto result_value = driver.drive(id, input, allowance.bytes_);
                if (result_value.status_ == driver_type::status_type::response_complete) {
                    RUVIA_CHECK(delivery.commit(result_value) == delivery_type::commit_status_type::committed);
                } else {
                    RUVIA_CHECK(result_value.status_ == driver_type::status_type::progress);
                }
            }
            RUVIA_CHECK(state_value.complete_ && state_value.trailers_.size() == 1);
        };
        finish_value(0, first_tail, first, first_state);
        finish_value(4, second_tail, second, second_state);
        RUVIA_CHECK(engine.release(0));
        RUVIA_CHECK(engine.release(4));
        first_state.discard_response_body();
        first.reconcile_body_bytes();
        second_state.discard_response_body();
        second.reconcile_body_bytes();
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        co_return;
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_budget_survives_connection_generations_and_wakes_drivers) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::counting_memory_resource resource;
    ruvia::detail::http3_client_body_budget budget(32);
    wake_counter old_generation_wake;
    wake_counter new_generation_wake;
    ruvia::detail::http3_client_body_budget::wake_registration_type old_generation_registration(
        budget, wake_counter::notify, &old_generation_wake);
    ruvia::detail::http3_client_body_budget::wake_registration_type new_generation_registration(
        budget, wake_counter::notify, &new_generation_wake);
    auto operation = [&]() -> ruvia::task<void> {
        const std::string first_body(32, 'a');
        const std::string second_body(16, 'b');
        state_type old_generation_state(worker.handle_, &resource);
        state_type new_generation_state(worker.handle_, &resource);
        delivery_type old_generation(old_generation_state, &budget);
        delivery_type new_generation(new_generation_state, &budget);
        engine_type old_engine(&resource);
        engine_type new_engine(&resource);
        driver_type old_driver(old_engine);
        driver_type new_driver(new_engine);
        RUVIA_CHECK(old_engine.register_request(0, ruvia::http_known_method::get,
                                  old_generation.event_sink())
                        .scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(new_engine.register_request(0, ruvia::http_known_method::get,
                                  new_generation.event_sink())
                        .scope_ == ruvia::http3_connection_error_scope::none);

        fake_read first_input{.wire_ = response_head()};
        append_frame(first_input.wire_, 0, first_body);
        for (std::size_t step = 0; step < first_input.wire_.size() + 2 &&
                                   old_generation_state.pending_.size() != first_body.size();
            ++step) {
            const auto allowance = old_generation.read_allowance(driver_type::read_block_bytes);
            RUVIA_CHECK(allowance.status_ == delivery_type::read_status_type::ready);
            RUVIA_CHECK(old_driver.drive(0, first_input, allowance.bytes_).status_ ==
                        driver_type::status_type::progress);
        }
        RUVIA_CHECK_EQ(budget.used(), first_body.size());
        RUVIA_CHECK(new_generation.read_allowance(driver_type::read_block_bytes).status_ ==
                    delivery_type::read_status_type::backpressured);
        // Changing the read policy must wake the QUIC driver even if no
        // storage has yet been freed (the peer may have trailers or FIN).
        const auto old_before_policy = old_generation_wake.notifications_;
        const auto new_before_policy = new_generation_wake.notifications_;
        old_generation_state.collect_all_ = true;
        old_generation_state.notify_producer_space();
        RUVIA_CHECK_EQ(budget.used(), first_body.size());
        RUVIA_CHECK_EQ(old_generation_wake.notifications_, old_before_policy + 1);
        RUVIA_CHECK_EQ(new_generation_wake.notifications_, new_before_policy + 1);

        const auto borrowed = co_await old_generation_state.consume_body<std::string_view>();
        RUVIA_CHECK(borrowed && *borrowed == first_body);
        RUVIA_CHECK_EQ(budget.used(), first_body.size());
        // The borrowed bytes remain charged until the next body operation frees
        // the consumed buffered prefix; this release wakes both generations.
        const auto old_before_release = old_generation_wake.notifications_;
        const auto new_before_release = new_generation_wake.notifications_;
        old_generation_state.release_consumed_body_prefix();
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        RUVIA_CHECK_EQ(old_generation_wake.notifications_, old_before_release + 2);
        RUVIA_CHECK_EQ(new_generation_wake.notifications_, new_before_release + 2);
        RUVIA_CHECK(new_generation.read_allowance(driver_type::read_block_bytes).status_ ==
                    delivery_type::read_status_type::ready);

        fake_read second_input{.wire_ = response_head()};
        append_frame(second_input.wire_, 0, second_body);
        for (std::size_t step = 0; step < second_input.wire_.size() + 2 &&
                                   new_generation_state.pending_.size() != second_body.size();
            ++step) {
            const auto allowance = new_generation.read_allowance(driver_type::read_block_bytes);
            RUVIA_CHECK(allowance.status_ == delivery_type::read_status_type::ready);
            RUVIA_CHECK(new_driver.drive(0, second_input, allowance.bytes_).status_ ==
                        driver_type::status_type::progress);
        }
        RUVIA_CHECK_EQ(budget.used(), second_body.size());
        const auto old_before_discard = old_generation_wake.notifications_;
        const auto new_before_discard = new_generation_wake.notifications_;
        new_generation_state.discard_response_body();
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        RUVIA_CHECK_EQ(old_generation_wake.notifications_, old_before_discard + 2);
        RUVIA_CHECK_EQ(new_generation_wake.notifications_, new_before_discard + 2);
        RUVIA_CHECK(old_engine.cancel_request(0) && old_engine.release(0));
        RUVIA_CHECK(new_engine.cancel_request(0) && new_engine.release(0));
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_isolates_callback_bad_alloc_and_rolls_back_body_reservation) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::rejecting_memory_resource rejecting;
    ruvia::test::counting_memory_resource resource;
    auto operation = [&]() -> ruvia::task<void> {
        ruvia::detail::http3_client_body_budget budget(1024);
        state_type failing_state(worker.handle_, &rejecting);
        state_type sibling_state(worker.handle_, &resource);
        delivery_type failing(failing_state, &budget);
        delivery_type sibling(sibling_state, &budget);
        engine_type engine(&resource, budget);
        driver_type driver(engine);
        RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get, failing.event_sink()).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(engine.register_request(4, ruvia::http_known_method::get, sibling.event_sink()).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        fake_read failing_head{.wire_ = response_head()};
        RUVIA_CHECK(driver.drive(0, failing_head).status_ == driver_type::status_type::progress);
        rejecting.reject_allocations(true, 32);
        const std::string body(64, 'x');
        fake_read failing_body{.wire_ = frame(0, std::span<const char>(body.data(), body.size()))};
        RUVIA_CHECK(driver.drive(0, failing_body).status_ == driver_type::status_type::progress);
        RUVIA_CHECK(failing.retirement_reason() == delivery_type::retirement_reason_type::callback_failure);
        RUVIA_CHECK(failing.callback_failure() != nullptr);
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});

        fake_read sibling_input{.wire_ = response_head()};
        append_frame(sibling_input.wire_, 0, "safe");
        RUVIA_CHECK(driver.drive(4, sibling_input).status_ == driver_type::status_type::progress);
        RUVIA_CHECK_EQ(sibling_state.pending_, std::string_view("safe"));
        RUVIA_CHECK_EQ(budget.used(), std::size_t{4});

        RUVIA_CHECK(engine.cancel_request(0));
        RUVIA_CHECK(failing.commit_failure(failing.callback_failure()));
        RUVIA_CHECK(engine.release(0));
        fake_read sibling_fin{.fin_ = true};
        const auto complete_value = driver.drive(4, sibling_fin);
        RUVIA_CHECK(complete_value.status_ == driver_type::status_type::response_complete);
        RUVIA_CHECK(sibling.commit(complete_value) == delivery_type::commit_status_type::committed);
        RUVIA_CHECK(engine.release(4));
        sibling_state.discard_response_body();
        sibling.reconcile_body_bytes();
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        rejecting.reject_allocations(false);
        co_return;
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_connection_protocol_error_wins_after_callback_allocation_failure) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::rejecting_memory_resource rejecting;
    ruvia::test::counting_memory_resource resource;
    auto operation = [&]() -> ruvia::task<void> {
        ruvia::detail::http3_client_body_budget budget(1024);
        state_type state_value(worker.handle_, &rejecting);
        delivery_type delivery(state_value, &budget);
        engine_type engine(&resource, budget);
        driver_type driver(engine);
        RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get, delivery.event_sink()).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        fake_read head{.wire_ = response_head()};
        RUVIA_CHECK(driver.drive(0, head).status_ == driver_type::status_type::progress);

        rejecting.reject_allocations(true, 32);
        const std::string body(64, 'x');
        auto wire = frame(0, std::span<const char>(body.data(), body.size()));
        append_frame(wire, 4, {});
        fake_read malformed{.wire_ = std::move(wire)};
        const auto result_value = driver.drive(0, malformed);
        RUVIA_CHECK(result_value.status_ == driver_type::status_type::connection_error);
        RUVIA_CHECK(result_value.protocol_.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(delivery.retirement_reason() == delivery_type::retirement_reason_type::callback_failure);
        RUVIA_CHECK(delivery.callback_failure() != nullptr);
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        RUVIA_CHECK(delivery.commit_failure(delivery.callback_failure()));
        (void)engine.stop();
        RUVIA_CHECK(engine.release(0));
        rejecting.reject_allocations(false);
        co_return;
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_holds_compressed_bytes_until_fin_and_decode_success) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::counting_memory_resource resource;
    ruvia::detail::http3_client_body_budget receive_budget(4096);
    auto operation = [&]() -> ruvia::task<void> {
        constexpr std::string_view plain = "decoded after the final response byte";
        auto encoded = ruvia::encode_http_content(ruvia::http_content_coding::gzip, plain,
            {.max_encoded_bytes_ = 4096, .resource_ = &resource});
        RUVIA_CHECK(encoded.encoded() != nullptr);
        const auto encoded_bytes = encoded.encoded()->bytes();
        state_type state_value(worker.handle_, &resource);
        state_value.buffered_limit_ = 1024;
        delivery_type delivery(state_value, &receive_budget);
        engine_type engine(&resource);
        driver_type driver(engine);
        RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get, delivery.event_sink()).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        fake_read input{.wire_ = response_head("200", {}, "gzip")};
        append_frame(input.wire_, 0, encoded_bytes);
        RUVIA_CHECK(driver.drive(0, input).status_ == driver_type::status_type::progress);
        RUVIA_CHECK(state_value.body_decode_required_ && state_value.collect_all_ && !state_value.complete_);
        RUVIA_CHECK_EQ(std::string_view(state_value.pending_), encoded_bytes);
        RUVIA_CHECK_EQ(receive_budget.used(), encoded_bytes.size());

        bool read_returned = false;
        bool watchdog_expired = false;
        std::optional<std::string_view> read_value;
        ruvia::task_scope tasks(worker.handle_, {.resource_ = &resource});
        auto consumer = [&]() -> ruvia::task<void> {
            read_value = co_await state_value.consume_body<std::string_view>();
            read_returned = true;
        };
        auto watchdog_value = [&]() -> ruvia::task<void> {
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
            if (!read_returned) {
                watchdog_expired = true;
                state_value.failure_ = std::make_exception_ptr(std::runtime_error("compressed read watchdog"));
                state_value.complete_ = true;
                state_value.data_signal_.notify();
            }
        };
        tasks.spawn(consumer());
        tasks.spawn(watchdog_value());
        (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(2));
        RUVIA_CHECK(!read_returned);

        fake_read fin{.fin_ = true};
        const auto finished = driver.drive(0, fin);
        RUVIA_CHECK(finished.status_ == driver_type::status_type::response_complete);
        ruvia::detail::decode_http_client_response_content_encoding(state_value, true, 1024);
        RUVIA_CHECK(!state_value.body_decode_required_);
        RUVIA_CHECK_EQ(std::string_view(state_value.buffered_), plain);
        RUVIA_CHECK_EQ(receive_budget.used(), plain.size());
        RUVIA_CHECK(delivery.commit(finished) == delivery_type::commit_status_type::committed);
        co_await tasks.join();
        RUVIA_CHECK(read_returned && !watchdog_expired && read_value && *read_value == plain);
        state_value.discard_response_body();
        RUVIA_CHECK_EQ(receive_budget.used(), std::size_t{0});
        RUVIA_CHECK(engine.release(0));
        co_return;
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_decode_errors_and_decoded_limits_publish_no_encoded_bytes) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::counting_memory_resource resource;
    ruvia::detail::http3_client_body_budget receive_budget(8192);
    auto operation = [&]() -> ruvia::task<void> {
        struct case_value final {
            std::string_view bytes_;
            std::size_t decoded_limit_;
            ruvia::http_client_error::code_type error_;
        };
        constexpr std::array cases{
            case_value{"not a gzip member", 1024, ruvia::http_client_error::code_type::protocol_error},
            case_value{"decoded output exceeds its bound", 4, ruvia::http_client_error::code_type::response_too_large},
        };
        for (const auto& item : cases) {
            std::string wire_body(item.bytes_);
            if (item.error_ == ruvia::http_client_error::code_type::response_too_large) {
                auto compressed = ruvia::encode_http_content(ruvia::http_content_coding::gzip,
                    item.bytes_, {.max_encoded_bytes_ = 4096, .resource_ = &resource});
                RUVIA_CHECK(compressed.encoded() != nullptr);
                wire_body.assign(compressed.encoded()->bytes());
            }
            state_type state_value(worker.handle_, &resource);
            state_type sibling_state(worker.handle_, &resource);
            state_value.buffered_limit_ = sibling_state.buffered_limit_ = 1024;
            delivery_type delivery(state_value, &receive_budget);
            delivery_type sibling_delivery(sibling_state, &receive_budget);
            engine_type engine(&resource);
            driver_type driver(engine);
            RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get,
                                  delivery.event_sink())
                            .scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(engine.register_request(4, ruvia::http_known_method::get,
                                  sibling_delivery.event_sink())
                            .scope_ == ruvia::http3_connection_error_scope::none);
            fake_read input{.wire_ = response_head("200", {}, "gzip")};
            append_frame(input.wire_, 0, wire_body);
            RUVIA_CHECK(driver.drive(0, input).status_ == driver_type::status_type::progress);
            RUVIA_CHECK(state_value.body_decode_required_ && state_value.collect_all_);
            RUVIA_CHECK_EQ(receive_budget.used(), wire_body.size());
            fake_read fin{.fin_ = true};
            const auto finished = driver.drive(0, fin);
            RUVIA_CHECK(finished.status_ == driver_type::status_type::response_complete);
            std::exception_ptr failure;
            try {
                ruvia::detail::decode_http_client_response_content_encoding(
                    state_value, true, item.decoded_limit_);
            } catch (const ruvia::http_client_error& error) {
                RUVIA_CHECK(error.code() == item.error_);
                failure = std::current_exception();
            }
            RUVIA_CHECK(failure != nullptr);
            RUVIA_CHECK(delivery.commit_failure(failure));
            RUVIA_CHECK(state_value.complete_ && state_value.buffered_.empty() && state_value.pending_.empty());
            RUVIA_CHECK_EQ(receive_budget.used(), std::size_t{0});
            bool read_failed = false;
            try {
                (void)co_await state_value.read_all(1024);
            } catch (const ruvia::http_client_error& error) {
                read_failed = error.code() == item.error_;
            }
            RUVIA_CHECK(read_failed);
            fake_read sibling_input{.wire_ = response_head()};
            append_frame(sibling_input.wire_, 0, "sibling");
            RUVIA_CHECK(driver.drive(4, sibling_input).status_ == driver_type::status_type::progress);
            fake_read sibling_fin{.fin_ = true};
            const auto sibling_finished = driver.drive(4, sibling_fin);
            RUVIA_CHECK(sibling_finished.status_ == driver_type::status_type::response_complete);
            RUVIA_CHECK(sibling_delivery.commit(sibling_finished) == delivery_type::commit_status_type::committed);
            RUVIA_CHECK(sibling_state.complete_ && sibling_state.pending_ == "sibling");
            RUVIA_CHECK_EQ(receive_budget.used(), std::size_t{7});
            sibling_state.discard_response_body();
            RUVIA_CHECK_EQ(receive_budget.used(), std::size_t{0});
            RUVIA_CHECK(engine.release(0));
            RUVIA_CHECK(engine.release(4));
        }
        co_return;
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(http3_client_response_delivery_parser_registration_allocation_failure_leaves_no_entry) {
    ruvia::test::rejecting_memory_resource resource;
    engine_type engine(&resource);
    std::size_t callbacks{};
    const auto sink_value = ruvia::detail::http3_client_response_event_sink{
        .callback_ = [](void* context_value, const ruvia::http3_connection_event&) {
            ++*static_cast<std::size_t*>(context_value);
        },
        .context_ = &callbacks,
    };
    const auto allocations_before = resource.allocation_count();
    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)engine.register_request(0, ruvia::http_known_method::get, sink_value);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK_EQ(resource.allocation_count(), allocations_before + 1);
    RUVIA_CHECK_EQ(engine.live_stream_count(), std::size_t{0});
    RUVIA_CHECK(!engine.response(0));
    RUVIA_CHECK_EQ(callbacks, std::size_t{0});

    resource.reject_allocations(false);
    RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get, sink_value).scope_ ==
                ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK_EQ(engine.live_stream_count(), std::size_t{1});
    RUVIA_CHECK(engine.stop().status_ ==
                ruvia::detail::http3_client_sans_io_session_status::transport_error);
    RUVIA_CHECK(engine.response(0).has_value());
    RUVIA_CHECK(engine.release(0));
    RUVIA_CHECK_EQ(engine.live_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(callbacks, std::size_t{0});
}
