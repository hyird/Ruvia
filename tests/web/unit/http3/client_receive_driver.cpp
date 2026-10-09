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
#include <variant>
#include <vector>

#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_qpack_connection.h"
#include "ruvia/http/http3_var_int.h"

#include "http3/http3_client_receive_driver.h"
#include "test_harness.h"

namespace {
using driver_type = ruvia::detail::http3_client_receive_driver;
using engine_type = ruvia::detail::http3_client_sans_io_session_engine;
using read_type = ruvia::quic_stream_read_result;

std::vector<char> frame(std::uint64_t type, std::span<const char> payload_value) {
    std::vector<char> output(16);
    const auto header_value = ruvia::encode_http3_var_int(output, type);
    const auto length = ruvia::encode_http3_var_int(
        std::span<char>(output).subspan(std::get<0>(header_value)), payload_value.size());
    output.resize(std::get<0>(header_value) + std::get<0>(length));
    output.insert(output.end(), payload_value.begin(), payload_value.end());
    return output;
}
std::vector<char> response_head() {
    std::pmr::monotonic_buffer_resource temp;
    constexpr std::array fields_value{ruvia::http3_field_section_field_view{":status", "200"},
        ruvia::http3_field_section_field_view{"x-received", "owned"}};
    const auto encoded = ruvia::encode_http3_field_section(fields_value, &temp);
    return frame(1, std::get<0>(encoded));
}
struct fake_read final {
    std::vector<char> wire_;
    std::size_t position_{};
    std::size_t max_chunk_{3};
    bool reset_{};

    read_type operator()(std::uint64_t, std::span<char> destination) {
        if (reset_) {
            return {.status_ = ruvia::quic_stream_read_status::reset};
        }
        if (position_ == wire_.size()) {
            return {.status_ = ruvia::quic_stream_read_status::fin};
        }
        const auto size = std::min({destination.size(), wire_.size() - position_, max_chunk_});
        std::copy_n(wire_.data() + position_, size, destination.data());
        position_ += size;
        return {.status_ = ruvia::quic_stream_read_status::data, .size_ = size};
    }
};

struct nested_receive final {
    driver_type* driver_{};
    std::string owned_body_;
    bool attempted_{};
    bool rejected_{};
    bool nested_read_{};
};
void try_recursive_read(void* raw, const ruvia::http3_connection_event& event) {
    if (event.kind_ != ruvia::http3_connection_event_kind::body) {
        return;
    }
    auto& owner_value = *static_cast<nested_receive*>(raw);
    owner_value.attempted_ = true;
    try {
        (void)owner_value.driver_->drive(event.stream_id_,
            [&owner_value](std::uint64_t, std::span<char> output) -> read_type {
                owner_value.nested_read_ = true;
                output[0] = '?';
                return {.status_ = ruvia::quic_stream_read_status::data, .size_ = 1};
            });
    } catch (const std::logic_error&) {
        owner_value.rejected_ = true;
    }
    owner_value.owned_body_.append(event.body_.data(), event.body_.size());
}
}  // namespace

RUVIA_TEST(http3_client_receive_driver_preserves_only_peer_reset_codes_without_narrowing) {
    std::pmr::unsynchronized_pool_resource pool;
    engine_type engine(&pool);
    driver_type driver(engine);
    const std::array<std::optional<std::uint64_t>, 4> codes{
        std::nullopt, 0, static_cast<std::uint64_t>(ruvia::http3_connection_error_code::request_rejected),
        (std::uint64_t{1} << 62) - 1};
    for (std::size_t index = 0; index < codes.size(); ++index) {
        const auto id = static_cast<std::uint64_t>(index * 4);
        RUVIA_CHECK(engine.register_request(id, ruvia::http_known_method::post).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        const auto reset = driver.drive(id, [&](std::uint64_t, std::span<char>) -> read_type {
            return {.status_ = ruvia::quic_stream_read_status::reset, .peer_reset_error_code_ = codes[index]};
        });
        RUVIA_CHECK(reset.status_ == driver_type::status_type::stream_reset);
        RUVIA_CHECK(reset.peer_reset_error_code_ == codes[index]);
        RUVIA_CHECK(reset.peer_reports_unprocessed_ == (index == 2));
        RUVIA_CHECK(engine.response(id)->reset_ && !engine.response(id)->complete_);
        RUVIA_CHECK(engine.release(id));
    }
    RUVIA_CHECK(engine.register_request(16, ruvia::http_known_method::post).scope_ ==
                ruvia::http3_connection_error_scope::none);
    fake_read responded{.wire_ = response_head(), .max_chunk_ = driver_type::read_block_bytes};
    RUVIA_CHECK(driver.drive(16, responded).status_ == driver_type::status_type::progress);
    const auto contradicted = driver.drive(16, [](std::uint64_t, std::span<char>) -> read_type {
        return {.status_ = ruvia::quic_stream_read_status::reset,
            .peer_reset_error_code_ = static_cast<std::uint64_t>(ruvia::http3_connection_error_code::request_rejected)};
    });
    RUVIA_CHECK(contradicted.status_ == driver_type::status_type::stream_reset);
    RUVIA_CHECK(!contradicted.peer_reports_unprocessed_);
    RUVIA_CHECK(engine.release(16));
    // A critical-stream reset is a connection error, never a retryable request rejection.
    fake_read control{.wire_ = {0}, .max_chunk_ = 8};
    const auto settings = frame(4, {});
    control.wire_.insert(control.wire_.end(), settings.begin(), settings.end());
    RUVIA_CHECK(driver.drive(3, control).status_ == driver_type::status_type::progress);
    const auto failed = driver.drive(3, [](std::uint64_t, std::span<char>) -> read_type {
        return {.status_ = ruvia::quic_stream_read_status::reset,
            .peer_reset_error_code_ = static_cast<std::uint64_t>(ruvia::http3_connection_error_code::request_rejected)};
    });
    RUVIA_CHECK(failed.status_ == driver_type::status_type::connection_error);
    RUVIA_CHECK(!failed.peer_reset_error_code_ && !failed.peer_reports_unprocessed_);
}

RUVIA_TEST(http3_client_receive_driver_feeds_interleaved_frames_and_publishes_only_valid_fin) {
    std::pmr::unsynchronized_pool_resource pool;
    engine_type engine(&pool);
    driver_type driver(engine);
    RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(engine.register_request(4, ruvia::http_known_method::head).scope_ ==
                ruvia::http3_connection_error_scope::none);
    fake_read get{.wire_ = response_head(), .max_chunk_ = 2};
    const auto payload_value = frame(0, std::span<const char>("ok", 2));
    get.wire_.insert(get.wire_.end(), payload_value.begin(), payload_value.end());
    fake_read head{.wire_ = response_head(), .max_chunk_ = 1};
    bool get_complete{};
    bool head_complete{};
    for (int i = 0; i < 80 && (!get_complete || !head_complete); ++i) {
        if (!get_complete) {
            const auto result_value = driver.drive(0, get);
            RUVIA_CHECK(result_value.status_ == driver_type::status_type::progress ||
                        result_value.status_ == driver_type::status_type::response_complete);
            get_complete = result_value.status_ == driver_type::status_type::response_complete;
        }
        if (!head_complete) {
            const auto result_value = driver.drive(4, head);
            RUVIA_CHECK(result_value.status_ == driver_type::status_type::progress ||
                        result_value.status_ == driver_type::status_type::response_complete);
            head_complete = result_value.status_ == driver_type::status_type::response_complete;
        }
    }
    RUVIA_CHECK(get_complete && head_complete);
    const auto response = engine.response(0);
    const auto head_response = engine.response(4);
    RUVIA_CHECK(response && response->status_ == 200 && response->complete_);
    RUVIA_CHECK(response && std::string_view(response->body_.data(), response->body_.size()) == "ok");
    RUVIA_CHECK(response && response->headers_.size() == 1 &&
                response->headers_.front().value_ == "owned");
    RUVIA_CHECK(head_response && head_response->complete_ && head_response->body_.empty());
    RUVIA_CHECK(engine.release(0));
    RUVIA_CHECK(engine.release(4));
}

RUVIA_TEST(http3_client_receive_driver_rejects_reentry_before_touching_borrowed_scratch) {
    std::pmr::unsynchronized_pool_resource pool;
    engine_type engine(&pool);
    driver_type driver(engine);
    nested_receive observed_value{.driver_ = &driver};
    RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get,
                          {.callback_ = try_recursive_read, .context_ = &observed_value})
                    .scope_ ==
                ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(engine.feed(0, response_head()).scope_ == ruvia::http3_connection_error_scope::none);
    fake_read input{.wire_ = frame(0, std::span<const char>("ok", 2)),
        .max_chunk_ = driver_type::read_block_bytes};
    const auto data = driver.drive(0, input);
    RUVIA_CHECK(data.status_ == driver_type::status_type::progress);
    RUVIA_CHECK(observed_value.attempted_ && observed_value.rejected_ && !observed_value.nested_read_);
    RUVIA_CHECK(observed_value.owned_body_ == "ok");
    RUVIA_CHECK(driver.drive(0, input).status_ == driver_type::status_type::response_complete);
    RUVIA_CHECK(engine.response(0)->complete_ && engine.release(0));
}

RUVIA_TEST(http3_client_receive_driver_honors_application_read_capacity_without_losing_frames) {
    std::pmr::unsynchronized_pool_resource pool;
    engine_type engine(&pool);
    driver_type driver(engine);
    RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    fake_read input{.wire_ = response_head(), .max_chunk_ = driver_type::read_block_bytes};
    const auto data = frame(0, std::span<const char>("hello", 5));
    input.wire_.insert(input.wire_.end(), data.begin(), data.end());
    bool touched{};
    const auto blocked = driver.drive(0, [&touched](std::uint64_t, std::span<char>) -> read_type {
            touched = true;
            return {.status_ = ruvia::quic_stream_read_status::closed}; }, 0);
    RUVIA_CHECK(blocked.status_ == driver_type::status_type::blocked && !touched);
    bool complete_value{};
    for (int i = 0; i < 40 && !complete_value; ++i) {
        const auto result_value = driver.drive(0, [&input, &ruvia_ctx](std::uint64_t id, std::span<char> output) -> read_type {
                RUVIA_CHECK(output.size() <= 4);
                return input(id, output); }, 4);
        RUVIA_CHECK(result_value.bytes_ <= 4);
        RUVIA_CHECK(result_value.status_ == driver_type::status_type::progress ||
                    result_value.status_ == driver_type::status_type::response_complete);
        complete_value = result_value.status_ == driver_type::status_type::response_complete;
    }
    RUVIA_CHECK(complete_value);
    const auto response = engine.response(0);
    RUVIA_CHECK(response && response->complete_ &&
                std::string_view(response->body_.data(), response->body_.size()) == "hello");
    RUVIA_CHECK(engine.release(0));
}

RUVIA_TEST(http3_client_receive_driver_peer_critical_fin_fails_whole_connection) {
    std::pmr::unsynchronized_pool_resource pool;
    engine_type engine(&pool);
    driver_type driver(engine);
    RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    fake_read control{.wire_ = {0}, .max_chunk_ = 8};
    const auto settings = frame(4, {});
    control.wire_.insert(control.wire_.end(), settings.begin(), settings.end());
    while (control.position_ != control.wire_.size()) {
        RUVIA_CHECK(driver.drive(3, control).status_ == driver_type::status_type::progress);
    }
    const auto closed = driver.drive(3, control);
    RUVIA_CHECK(closed.status_ == driver_type::status_type::connection_error);
    RUVIA_CHECK(closed.protocol_.code_ == ruvia::http3_connection_error_code::closed_critical_stream);
    RUVIA_CHECK(engine.response(0)->result_.scope_ == ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK(engine.release(0));
}

RUVIA_TEST(http3_client_receive_driver_transport_failure_wakes_every_incomplete_response) {
    std::pmr::unsynchronized_pool_resource pool;
    engine_type engine(&pool);
    driver_type driver(engine);
    RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(engine.register_request(4, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    const auto failed = driver.drive(0, [](std::uint64_t, std::span<char>) -> read_type {
        return {.status_ = ruvia::quic_stream_read_status::closed};
    });
    RUVIA_CHECK(failed.status_ == driver_type::status_type::transport_error);
    RUVIA_CHECK(failed.protocol_.status_ == ruvia::detail::http3_client_sans_io_session_status::transport_error);
    RUVIA_CHECK(failed.protocol_.scope_ == ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK(engine.response(0)->result_.status_ ==
                ruvia::detail::http3_client_sans_io_session_status::transport_error);
    RUVIA_CHECK(engine.response(4)->result_.status_ ==
                ruvia::detail::http3_client_sans_io_session_status::transport_error);
    RUVIA_CHECK(engine.release(0));
    RUVIA_CHECK(engine.release(4));
    RUVIA_CHECK(engine.register_request(8, ruvia::http_known_method::get).status_ ==
                ruvia::detail::http3_client_sans_io_session_status::transport_error);
}

RUVIA_TEST(http3_client_receive_driver_does_not_mask_earlier_connection_protocol_failure) {
    std::pmr::unsynchronized_pool_resource pool;
    engine_type engine(&pool);
    driver_type driver(engine);
    RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    const auto unexpected = frame(0, std::span<const char>("bad", 3));
    const auto protocol = engine.feed(0, unexpected);
    RUVIA_CHECK(protocol.scope_ == ruvia::http3_connection_error_scope::connection);
    const auto result_value = driver.drive(0, [](std::uint64_t, std::span<char>) -> read_type {
        return {.status_ = ruvia::quic_stream_read_status::closed};
    });
    RUVIA_CHECK(result_value.status_ == driver_type::status_type::connection_error);
    RUVIA_CHECK(result_value.protocol_.code_ == protocol.code_);
    RUVIA_CHECK(engine.release(0));
}

RUVIA_TEST(http3_client_receive_driver_rejects_invalid_transport_byte_count_before_feeding) {
    std::pmr::unsynchronized_pool_resource pool;
    engine_type engine(&pool);
    driver_type driver(engine);
    RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    const auto wrong = driver.drive(0, [](std::uint64_t, std::span<char>) -> read_type { return {.status_ = ruvia::quic_stream_read_status::data, .size_ = 5}; }, 4);
    RUVIA_CHECK(wrong.status_ == driver_type::status_type::transport_error);
    RUVIA_CHECK(engine.response(0)->result_.status_ ==
                ruvia::detail::http3_client_sans_io_session_status::transport_error);
    RUVIA_CHECK(!engine.response(0)->complete_);
    RUVIA_CHECK(engine.release(0));
}

RUVIA_TEST(http3_client_receive_driver_retains_blocked_suffix_and_resumes_after_encoder_instructions) {
    std::pmr::unsynchronized_pool_resource resource;
    engine_type engine(&resource, {.connection_ = {.qpack_max_table_capacity_ = 256, .qpack_blocked_streams_ = 2}});
    driver_type driver(engine);
    RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(engine.register_request(4, ruvia::http_known_method::get).scope_ == ruvia::http3_connection_error_scope::none);
    ruvia::http3_qpack_encoder encoder({.max_table_capacity_ = 256, .max_blocked_streams_ = 2}, &resource);
    constexpr std::array fields_value{ruvia::http3_field_section_field_view{":status", "200"}, ruvia::http3_field_section_field_view{"x-dynamic", "retained"}};
    const auto section = encoder.encode(0, fields_value);
    RUVIA_CHECK((section.index() == 0));
    if ((section.index() != 0)) {
        return;
    }
    fake_read input;
    input.wire_ = frame(1, std::get<0>(section));
    const auto data = frame(0, std::span<const char>("payload", 7));
    input.wire_.insert(input.wire_.end(), data.begin(), data.end());
    input.max_chunk_ = input.wire_.size();
    RUVIA_CHECK(driver.drive(0, input).status_ == driver_type::status_type::progress);
    const auto blocked_position = input.position_;
    RUVIA_CHECK(driver.drive(0, input).status_ == driver_type::status_type::blocked);
    RUVIA_CHECK_EQ(input.position_, blocked_position);
    fake_read unrelated{.wire_ = response_head(), .max_chunk_ = 1024};
    RUVIA_CHECK(driver.drive(4, unrelated).status_ == driver_type::status_type::progress);
    RUVIA_CHECK(driver.drive(4, unrelated).status_ == driver_type::status_type::response_complete);
    std::vector<char> instructions{2};
    const auto pending = encoder.pending_encoder_output();
    instructions.insert(instructions.end(), pending.begin(), pending.end());
    const auto updated = engine.feed(7, instructions);
    RUVIA_CHECK(updated.scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(driver.drive(0, input).status_ == driver_type::status_type::progress);
    RUVIA_CHECK(driver.drive(0, input).status_ == driver_type::status_type::response_complete);
    const auto response = engine.response(0);
    RUVIA_CHECK(response && response->complete_);
    if (response) {
        RUVIA_CHECK_EQ(std::string_view(response->body_.data(), response->body_.size()), "payload");
        RUVIA_CHECK_EQ(response->headers_.size(), std::size_t{1});
        if (!response->headers_.empty()) {
            RUVIA_CHECK_EQ(response->headers_[0].value_, std::string_view("retained"));
        }
    }
    RUVIA_CHECK(!engine.pending_decoder_output().empty());
    RUVIA_CHECK(encoder.consume_decoder(engine.pending_decoder_output()).index() == 0);
    RUVIA_CHECK(engine.consume_decoder_output(engine.pending_decoder_output().size()));
    RUVIA_CHECK(engine.release(0));
    RUVIA_CHECK(engine.release(4));
}

RUVIA_TEST(http3_client_receive_driver_peer_reset_retires_qpack_blocked_suffix_without_encoder_progress) {
    std::pmr::unsynchronized_pool_resource resource;
    engine_type engine(&resource, {.connection_ = {.qpack_max_table_capacity_ = 256, .qpack_blocked_streams_ = 2}});
    driver_type driver(engine);
    RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get).scope_ == ruvia::http3_connection_error_scope::none);
    ruvia::http3_qpack_encoder encoder({.max_table_capacity_ = 256, .max_blocked_streams_ = 2}, &resource);
    const std::array fields_value{ruvia::http3_field_section_field_view{":status", "200"}, ruvia::http3_field_section_field_view{"x-custom", "retained"}};
    const auto section = encoder.encode(0, fields_value);
    RUVIA_CHECK((section.index() == 0));
    auto wire = frame(1, std::get<0>(section));
    const auto data = frame(0, std::span<const char>("payload", 7));
    wire.insert(wire.end(), data.begin(), data.end());
    const auto pending = driver.drive(0, [&](std::uint64_t, std::span<char> bytes_value) -> read_type { std::copy(wire.begin(), wire.end(), bytes_value.begin()); return {.status_ = ruvia::quic_stream_read_status::data, .size_ = wire.size()}; });
    RUVIA_CHECK(pending.protocol_.status_ == ruvia::detail::http3_client_sans_io_session_status::qpack_blocked);
    const auto reset = driver.accept_reset(0, static_cast<std::uint64_t>(ruvia::http3_connection_error_code::request_cancelled));
    RUVIA_CHECK(reset.status_ == driver_type::status_type::stream_reset);
    RUVIA_CHECK(!reset.peer_reports_unprocessed_);
    RUVIA_CHECK_EQ(reset.peer_reset_error_code_, std::optional{static_cast<std::uint64_t>(ruvia::http3_connection_error_code::request_cancelled)});
    RUVIA_CHECK(!engine.pending_decoder_output().empty());
    RUVIA_CHECK(engine.release(0));
    RUVIA_CHECK(engine.register_request(4, ruvia::http_known_method::get).scope_ == ruvia::http3_connection_error_scope::none);
    const auto sibling = driver.drive(4, [](std::uint64_t, std::span<char>) -> read_type { return {}; });
    RUVIA_CHECK(sibling.status_ == driver_type::status_type::blocked);
    RUVIA_CHECK(engine.cancel_request(4));
    RUVIA_CHECK(engine.release(4));
}
