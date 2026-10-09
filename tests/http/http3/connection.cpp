#include <algorithm>
#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_local_critical_streams.h"
#include "ruvia/http/http3_qpack_connection.h"
#include "ruvia/http/http3_var_int.h"

#include "test_harness.h"

namespace {

struct captured_events final {
    std::vector<std::string> methods_;
    std::vector<std::string> protocols_;
    std::vector<std::string> paths_;
    std::vector<std::string> bodies_;
    std::vector<std::string> trailers_;
    std::vector<std::string> trailer_values_;
    std::vector<bool> never_indexed_;
    std::unordered_map<std::uint64_t, std::size_t> body_index_;
    std::vector<std::uint64_t> ended_;
    std::vector<std::uint64_t> reset_;
    std::size_t response_plan_count_{0};
};

void ignore_event(void*, const ruvia::http3_connection_event&) {}

struct client_captured final {
    std::unordered_map<std::uint64_t, std::uint16_t> statuses_;
    std::unordered_map<std::uint64_t, std::string> bodies_;
    std::unordered_map<std::uint64_t, std::vector<ruvia::http3_connection_event_kind>> kinds_;
    std::unordered_map<std::uint64_t, std::vector<std::optional<ruvia::http_response_body_plan>>> response_body_plans_;
    std::vector<std::uint64_t> ended_;
    std::vector<std::uint64_t> reset_;
};

void capture_client(void* opaque, const ruvia::http3_connection_event& event) {
    auto& result_value = *static_cast<client_captured*>(opaque);
    result_value.kinds_[event.stream_id_].push_back(event.kind_);
    result_value.response_body_plans_[event.stream_id_].emplace_back(event.response_body_plan_);
    if (event.kind_ == ruvia::http3_connection_event_kind::informational_head ||
        event.kind_ == ruvia::http3_connection_event_kind::final_head) {
        result_value.statuses_[event.stream_id_] = event.head_->status_;
    } else if (event.kind_ == ruvia::http3_connection_event_kind::body ||
               event.kind_ == ruvia::http3_connection_event_kind::tunnel_data) {
        result_value.bodies_[event.stream_id_].append(event.body_.data(), event.body_.size());
    } else if (event.kind_ == ruvia::http3_connection_event_kind::message_end) {
        result_value.ended_.push_back(event.stream_id_);
    } else if (event.kind_ == ruvia::http3_connection_event_kind::reset) {
        result_value.reset_.push_back(event.stream_id_);
    }
}

std::vector<char> response_message(std::pmr::memory_resource* resource, std::uint16_t status,
    std::optional<std::uint64_t> content_length, std::string_view body, bool send_data) {
    const auto status_text = std::to_string(status);
    const auto length_text = content_length ? std::to_string(*content_length) : std::string{};
    const std::array<ruvia::http3_field_section_field_view, 2> fields_value{{{":status", status_text}, {"content-length", length_text}}};
    const auto section = ruvia::encode_http3_field_section(std::span(fields_value).first(content_length ? 2 : 1), resource);
    std::vector<char> result_value(16);
    const auto head = ruvia::encode_http3_frame_header(result_value, 1, std::get<0>(section).size());
    result_value.resize(std::get<0>(head));
    result_value.insert(result_value.end(), std::get<0>(section).begin(), std::get<0>(section).end());
    if (send_data) {
        std::array<char, 16> frame{};
        const auto data = ruvia::encode_http3_frame_header(frame, 0, body.size());
        result_value.insert(result_value.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(std::get<0>(data)));
        result_value.insert(result_value.end(), body.begin(), body.end());
    }
    return result_value;
}

std::vector<char> response_wire(std::pmr::memory_resource* resource, std::uint16_t status,
    std::string_view body) {
    return response_message(resource, status, body.size(), body, true);
}

bool same_plan(const ruvia::http_response_body_plan* left, const ruvia::http_response_body_plan* right) {
    return left != nullptr && right != nullptr && left->request_method() == right->request_method() &&
           left->response_status() == right->response_status() && left->content_semantics() == right->content_semantics() &&
           left->status_allows_body() == right->status_allows_body() && left->body_suppressed() == right->body_suppressed();
}

const ruvia::http_response_body_plan* response_plan(const client_captured& captured_value, std::uint64_t stream_id,
    std::size_t index) {
    const auto found = captured_value.response_body_plans_.find(stream_id);
    return found != captured_value.response_body_plans_.end() && index < found->second.size() && found->second[index]
               ? &*found->second[index]
               : nullptr;
}

void capture(void* opaque, const ruvia::http3_connection_event& event) {
    auto& result_value = *static_cast<captured_events*>(opaque);
    if (event.response_body_plan_) {
        ++result_value.response_plan_count_;
    }
    switch (event.kind_) {
        case ruvia::http3_connection_event_kind::request_head:
            result_value.methods_.emplace_back(event.head_->method_);
            result_value.protocols_.emplace_back(event.head_->protocol_);
            result_value.paths_.emplace_back(event.head_->path_);
            result_value.body_index_[event.stream_id_] = result_value.bodies_.size();
            result_value.bodies_.emplace_back();
            break;
        case ruvia::http3_connection_event_kind::push_stream:
        case ruvia::http3_connection_event_kind::push_promise:
        case ruvia::http3_connection_event_kind::push_canceled:
        case ruvia::http3_connection_event_kind::origin_advertisement:
        case ruvia::http3_connection_event_kind::priority_update:
        case ruvia::http3_connection_event_kind::informational_head:
        case ruvia::http3_connection_event_kind::final_head:
            break;
        case ruvia::http3_connection_event_kind::tunnel_data:
            result_value.bodies_[result_value.body_index_[event.stream_id_]].append(event.body_.data(), event.body_.size());
            break;
        case ruvia::http3_connection_event_kind::body:
            result_value.bodies_[result_value.body_index_[event.stream_id_]].append(event.body_.data(), event.body_.size());
            break;
        case ruvia::http3_connection_event_kind::message_end:
            result_value.ended_.push_back(event.stream_id_);
            break;
        case ruvia::http3_connection_event_kind::reset:
            result_value.reset_.push_back(event.stream_id_);
            break;
        case ruvia::http3_connection_event_kind::trailer_field:
            result_value.trailers_.emplace_back(event.trailer_.name_);
            result_value.trailer_values_.emplace_back(event.trailer_.value_);
            result_value.never_indexed_.push_back(event.trailer_.never_indexed_);
            break;
    }
}

std::vector<char> request_wire(std::pmr::memory_resource* resource, std::string_view method,
    std::string_view path, std::string_view body) {
    const std::array<ruvia::http3_field_section_field_view, 4> fields_value{{{":method", method}, {":scheme", "https"}, {":authority", "example.test"}, {":path", path}}};
    const auto section = ruvia::encode_http3_field_section(fields_value, resource);
    std::vector<char> result_value(128);
    auto header_value = ruvia::encode_http3_frame_header(result_value, 1, std::get<0>(section).size());
    result_value.resize(std::get<0>(header_value) + std::get<0>(section).size());
    std::copy(std::get<0>(section).begin(), std::get<0>(section).end(), result_value.begin() + static_cast<std::ptrdiff_t>(std::get<0>(header_value)));
    std::array<char, 16> frame_header{};
    const auto body_header = ruvia::encode_http3_frame_header(frame_header, 0, body.size());
    result_value.insert(result_value.end(), frame_header.begin(), frame_header.begin() + static_cast<std::ptrdiff_t>(std::get<0>(body_header)));
    result_value.insert(result_value.end(), body.begin(), body.end());
    return result_value;
}

struct counting_resource final : std::pmr::memory_resource {
    std::size_t allocations_{0};
    std::size_t deallocations_{0};
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* ptr, std::size_t size, std::size_t alignment) override {
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(ptr, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

}  // namespace

RUVIA_TEST(http3_connection_single_request_capacity_accepts_all_peer_critical_streams) {
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http3_connection connection(ruvia::http3_peer_role::client, &resource, {.max_active_streams_ = 1});
    RUVIA_CHECK(connection.register_client_request(0, ruvia::http_known_method::connect).status_ == ruvia::http3_connection_status::need_more_data);
    const auto prefixes = ruvia::http3_local_critical_streams::create({.enable_connect_protocol_ = true});
    RUVIA_CHECK((prefixes.index() == 0));
    if ((prefixes.index() != 0)) {
        return;
    }
    RUVIA_CHECK(connection.feed(3, std::get<0>(prefixes).control_prefix(), false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(connection.feed(7, std::get<0>(prefixes).qpack_encoder_prefix(), false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(connection.feed(11, std::get<0>(prefixes).qpack_decoder_prefix(), false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(connection.peer_settings() && connection.peer_settings()->enable_connect_protocol_);
    RUVIA_CHECK(connection.register_client_request(4, ruvia::http_known_method::get).status_ == ruvia::http3_connection_status::stream_error);
    RUVIA_CHECK(connection.active_request_count() == 1);
}

RUVIA_TEST(http3_connection_demultiplexes_fragmented_parallel_requests_and_preserves_callback_results) {
    counting_resource resource;
    captured_events captured;
    {
        ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource);
        const auto first = request_wire(&resource, "POST", "/one", "abc");
        const auto second = request_wire(&resource, "GET", "/two", "xy");
        for (std::size_t i = 0; i < first.size(); ++i) {
            const auto result_value = connection.feed(0, std::span(first).subspan(i, 1), i + 1 == first.size(), false,
                capture, &captured);
            RUVIA_CHECK(result_value.status_ == (i + 1 == first.size()
                                                        ? ruvia::http3_connection_status::message_end
                                                        : ruvia::http3_connection_status::need_more_data));
        }
        RUVIA_CHECK(connection.feed(4, second, true, false, capture, &captured).status_ ==
                    ruvia::http3_connection_status::message_end);
        RUVIA_CHECK_EQ(captured.methods_.size(), 2U);
        RUVIA_CHECK_EQ(captured.methods_[0], "POST");
        RUVIA_CHECK_EQ(captured.methods_[1], "GET");
        RUVIA_CHECK_EQ(captured.response_plan_count_, std::size_t{0});
        RUVIA_CHECK_EQ(captured.bodies_[0], "abc");
        RUVIA_CHECK_EQ(captured.bodies_[1], "xy");
        RUVIA_CHECK_EQ(connection.active_request_count(), 0U);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(http3_connection_delivers_extended_connect_request_heads) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource, {.enable_connect_protocol_ = true});
    captured_events captured;
    const std::array fields_value{
        ruvia::http3_field_section_field_view{":method", "CONNECT"},
        ruvia::http3_field_section_field_view{":protocol", "websocket"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/socket?channel=42"},
    };
    const auto section = ruvia::encode_http3_field_section(fields_value, &resource);
    RUVIA_CHECK((section.index() == 0));
    if ((section.index() != 0)) {
        return;
    }
    std::array<char, 16> prefix{};
    const auto prefix_size = ruvia::encode_http3_frame_header(prefix, 1, std::get<0>(section).size());
    RUVIA_CHECK((prefix_size.index() == 0));
    if ((prefix_size.index() != 0)) {
        return;
    }
    std::vector<char> wire(prefix.begin(), prefix.begin() + static_cast<std::ptrdiff_t>(std::get<0>(prefix_size)));
    wire.insert(wire.end(), std::get<0>(section).begin(), std::get<0>(section).end());
    ruvia::http3_connection disabled(ruvia::http3_peer_role::server, &resource);
    RUVIA_CHECK(disabled.feed(0, wire, false, false, ignore_event, nullptr).code_ ==
                ruvia::http3_connection_error_code::message_error);
    RUVIA_CHECK(connection.feed(0, wire, false, false, capture, &captured).status_ ==
                ruvia::http3_connection_status::need_more_data);
    RUVIA_CHECK(connection.feed(0, {}, true, false, capture, &captured).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK_EQ(captured.methods_.size(), 1U);
    if (captured.methods_.size() == 1 && captured.protocols_.size() == 1 && captured.paths_.size() == 1) {
        RUVIA_CHECK_EQ(captured.methods_.front(), "CONNECT");
        RUVIA_CHECK_EQ(captured.protocols_.front(), "websocket");
        RUVIA_CHECK_EQ(captured.paths_.front(), "/socket?channel=42");
    }

    const std::array invalid_fields{
        ruvia::http3_field_section_field_view{":method", "GET"},
        ruvia::http3_field_section_field_view{":protocol", "websocket"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/socket"},
    };
    const auto invalid_section = ruvia::encode_http3_field_section(invalid_fields, &resource);
    RUVIA_CHECK((invalid_section.index() == 0));
    if ((invalid_section.index() != 0)) {
        return;
    }
    const auto invalid_prefix_size = ruvia::encode_http3_frame_header(prefix, 1, std::get<0>(invalid_section).size());
    RUVIA_CHECK((invalid_prefix_size.index() == 0));
    if ((invalid_prefix_size.index() != 0)) {
        return;
    }
    std::vector<char> invalid_wire(prefix.begin(),
        prefix.begin() + static_cast<std::ptrdiff_t>(std::get<0>(invalid_prefix_size)));
    invalid_wire.insert(invalid_wire.end(), std::get<0>(invalid_section).begin(), std::get<0>(invalid_section).end());
    ruvia::http3_connection invalid_connection(ruvia::http3_peer_role::server, &resource);
    const auto invalid_result = invalid_connection.feed(0, invalid_wire, false, false, capture, &captured);
    RUVIA_CHECK(invalid_result.status_ == ruvia::http3_connection_status::stream_error);
    RUVIA_CHECK(invalid_result.scope_ == ruvia::http3_connection_error_scope::stream);
    RUVIA_CHECK(invalid_result.code_ == ruvia::http3_connection_error_code::message_error);
}

RUVIA_TEST(http3_connection_handles_settings_stream_errors_and_request_reset_independently) {
    counting_resource resource;
    {
        ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource);
        constexpr std::array<char, 3> settings{0x00, 0x04, 0x00};
        const auto accepted = connection.feed(2, settings, false, false, capture, nullptr);
        RUVIA_CHECK(accepted.status_ == ruvia::http3_connection_status::need_more_data);
        RUVIA_CHECK(connection.peer_settings().has_value());

        captured_events captured;
        const auto partial = request_wire(&resource, "GET", "/cancel", "");
        RUVIA_CHECK(connection.feed(0, std::span(partial).first(1), false, false, capture, &captured).status_ ==
                    ruvia::http3_connection_status::need_more_data);
        RUVIA_CHECK(connection.feed(0, {}, false, true, capture, &captured).status_ ==
                    ruvia::http3_connection_status::need_more_data);
        RUVIA_CHECK(connection.feed(0, {}, false, true, capture, &captured).status_ ==
                    ruvia::http3_connection_status::need_more_data);
        RUVIA_CHECK_EQ(captured.reset_.size(), 1U);
        RUVIA_CHECK_EQ(connection.active_request_count(), 0U);

        constexpr std::array<char, 3> bad_control{0x00, 0x00, 0x00};
        const auto error = connection.feed(6, bad_control, false, false, capture, &captured);
        RUVIA_CHECK(error.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(error.code_ == ruvia::http3_connection_error_code::stream_creation_error);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(http3_connection_delivers_trailers_synchronously_and_finishes_the_message) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource);
    captured_events captured;
    const auto initial_value = request_wire(&resource, "GET", "/trailers", "");
    RUVIA_CHECK(connection.feed(0, initial_value, false, false, capture, &captured).status_ ==
                ruvia::http3_connection_status::need_more_data);
    const std::string name = "x-" + std::string(80, 'n');
    const std::string value(100, 'v');
    const std::array<ruvia::http3_field_section_field_view, 1> fields_value{{{name, value, true}}};
    const auto section = ruvia::encode_http3_field_section(fields_value, &resource);
    std::array<char, 16> frame{};
    const auto header_value = ruvia::encode_http3_frame_header(frame, 1, std::get<0>(section).size());
    std::vector<char> wire(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(std::get<0>(header_value)));
    wire.insert(wire.end(), std::get<0>(section).begin(), std::get<0>(section).end());
    RUVIA_CHECK(connection.feed(0, wire, true, false, capture, &captured).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK_EQ(captured.trailers_.size(), 1U);
    RUVIA_CHECK_EQ(captured.trailers_[0], name);
    RUVIA_CHECK_EQ(captured.trailer_values_[0], value);
    RUVIA_CHECK(captured.never_indexed_[0]);
    RUVIA_CHECK_EQ(captured.ended_.size(), 1U);
}

RUVIA_TEST(http3_connection_rejects_forbidden_request_trailer_fields) {
    std::pmr::monotonic_buffer_resource resource;
    for (const auto field : {ruvia::http3_field_section_field_view{"authorization", "Bearer secret"},
             ruvia::http3_field_section_field_view{"content-type", "application/json"}}) {
        ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource);
        captured_events captured;
        const auto initial_value = request_wire(&resource, "POST", "/trailers", "");
        RUVIA_CHECK(connection.feed(0, initial_value, false, false, capture, &captured).status_ ==
                    ruvia::http3_connection_status::need_more_data);
        const std::array fields_value{field};
        const auto section = ruvia::encode_http3_field_section(fields_value, &resource);
        std::array<char, 16> prefix{};
        const auto prefix_size = ruvia::encode_http3_frame_header(prefix, 1, std::get<0>(section).size());
        std::vector<char> wire(prefix.begin(), prefix.begin() + static_cast<std::ptrdiff_t>(std::get<0>(prefix_size)));
        wire.insert(wire.end(), std::get<0>(section).begin(), std::get<0>(section).end());
        const auto result_value = connection.feed(0, wire, true, false, capture, &captured);
        RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::stream);
        RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::message_error);
        RUVIA_CHECK(captured.trailers_.empty());
    }
}

RUVIA_TEST(http3_connection_maps_qpack_critical_stream_errors_to_connection_scope) {
    std::pmr::monotonic_buffer_resource resource;
    captured_events captured;
    {
        ruvia::http3_connection encoder(ruvia::http3_peer_role::server, &resource);
        constexpr std::array<char, 2> bad_encoder{0x02, 0x00};
        const auto result_value = encoder.feed(2, bad_encoder, false, false, capture, &captured);
        RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::qpack_encoder_stream_error);
    }
    {
        ruvia::http3_connection decoder(ruvia::http3_peer_role::server, &resource);
        constexpr std::array<char, 2> bad_decoder{0x03, 0x00};
        const auto result_value = decoder.feed(2, bad_decoder, false, false, capture, &captured);
        RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::qpack_decoder_stream_error);
    }
}

RUVIA_TEST(http3_connection_maps_qpack_decompression_and_reserved_frames_to_connection_errors) {
    std::pmr::monotonic_buffer_resource resource;
    captured_events captured;
    {
        ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource);
        constexpr std::array<char, 4> dynamic_reference{0x01, 0x02, 0x01, 0x00};
        const auto result_value = connection.feed(0, dynamic_reference, false, false, capture, &captured);
        RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::qpack_decompression_failed);
    }
    {
        ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource);
        constexpr std::array<char, 2> reserved_frame{0x02, 0x00};
        const auto result_value = connection.feed(0, reserved_frame, false, false, capture, &captured);
        RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::frame_unexpected);
    }
}

RUVIA_TEST(http3_connection_validates_all_trailers_before_delivering_any) {
    constexpr std::array<std::string_view, 6> invalid_names{
        "connection", "X-Test", "x bad", ":path", std::string_view("x\0bad", 5), "\x80-name"};
    for (const auto name : invalid_names) {
        std::pmr::monotonic_buffer_resource resource;
        ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource);
        captured_events captured;
        const auto initial_value = request_wire(&resource, "GET", "/bad-trailer", "");
        RUVIA_CHECK(connection.feed(0, initial_value, false, false, capture, &captured).status_ ==
                    ruvia::http3_connection_status::need_more_data);
        const std::array<ruvia::http3_field_section_field_view, 2> fields_value{{{"x-valid", "yes"}, {name, "close"}}};
        const auto section = ruvia::encode_http3_field_section(fields_value, &resource);
        std::array<char, 16> frame{};
        const auto header_value = ruvia::encode_http3_frame_header(frame, 1, std::get<0>(section).size());
        std::vector<char> wire(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(std::get<0>(header_value)));
        wire.insert(wire.end(), std::get<0>(section).begin(), std::get<0>(section).end());
        const auto result_value = connection.feed(0, wire, true, false, capture, &captured);
        RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::stream);
        RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::message_error);
        RUVIA_CHECK(captured.trailers_.empty());
        RUVIA_CHECK(captured.ended_.empty());
        RUVIA_CHECK_EQ(connection.active_request_count(), 0U);
    }
}

struct reentry final {
    ruvia::http3_connection* connection_{nullptr};
    std::size_t count_{0};
    std::size_t rejected_{0};
};

void reenter(void* opaque, const ruvia::http3_connection_event&) {
    auto& state_value = *static_cast<reentry*>(opaque);
    ++state_value.count_;
    try {
        (void)state_value.connection_->feed(0, {}, false, false, reenter, opaque);
    } catch (const std::logic_error&) {
        ++state_value.rejected_;
    }
}

RUVIA_TEST(http3_connection_rejects_callback_reentry_for_all_request_events) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource);
    reentry state_value{.connection_ = &connection};

    auto head_and_body = request_wire(&resource, "POST", "/reenter", "x");
    RUVIA_CHECK(connection.feed(0, head_and_body, true, false, reenter, &state_value).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK_EQ(state_value.count_, 3U);
    RUVIA_CHECK_EQ(state_value.rejected_, 3U);

    state_value = {.connection_ = &connection};
    const auto initial_value = request_wire(&resource, "GET", "/trailer-reenter", "");
    RUVIA_CHECK(connection.feed(4, initial_value, false, false, reenter, &state_value).status_ ==
                ruvia::http3_connection_status::need_more_data);
    const std::array<ruvia::http3_field_section_field_view, 1> fields_value{{{"x-end", "yes"}}};
    const auto section = ruvia::encode_http3_field_section(fields_value, &resource);
    std::array<char, 16> frame{};
    const auto header_value = ruvia::encode_http3_frame_header(frame, 1, std::get<0>(section).size());
    std::vector<char> wire(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(std::get<0>(header_value)));
    wire.insert(wire.end(), std::get<0>(section).begin(), std::get<0>(section).end());
    RUVIA_CHECK(connection.feed(4, wire, true, false, reenter, &state_value).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK_EQ(state_value.count_, 3U);
    RUVIA_CHECK_EQ(state_value.rejected_, 3U);

    state_value = {.connection_ = &connection};
    const auto partial = request_wire(&resource, "GET", "/reset-reenter", "");
    RUVIA_CHECK(connection.feed(8, std::span(partial).first(1), false, false, reenter, &state_value).status_ ==
                ruvia::http3_connection_status::need_more_data);
    RUVIA_CHECK(connection.feed(8, {}, false, true, reenter, &state_value).status_ ==
                ruvia::http3_connection_status::need_more_data);
    RUVIA_CHECK_EQ(state_value.count_, 1U);
    RUVIA_CHECK_EQ(state_value.rejected_, 1U);
}

RUVIA_TEST(http3_connection_callback_exception_ends_unrecoverable_feed) {
    counting_resource resource;
    {
        ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource);
        const auto wire = request_wire(&resource, "POST", "/exception", "ok");
        auto throwing = +[](void*, const ruvia::http3_connection_event& event) {
            if (event.kind_ == ruvia::http3_connection_event_kind::request_head) {
                throw std::runtime_error("request head owner failed");
            }
        };
        bool failed = false;
        try {
            (void)connection.feed(0, wire, true, false, throwing, nullptr);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        const auto later = connection.feed(4, {}, false, false, ignore_event, nullptr);
        RUVIA_CHECK(later.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(later.code_ == ruvia::http3_connection_error_code::internal_error);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(http3_connection_does_not_retain_closed_sparse_stream_ids) {
    counting_resource resource;
    ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource);
    const auto request = request_wire(&resource, "GET", "/sparse", "");
    const auto before = resource.allocations_;
    std::size_t retained_after_warmup = 0;
    for (std::uint64_t stream = 0; stream < 4000; stream += 4) {
        RUVIA_CHECK(connection.feed(stream, request, true, false, ignore_event, nullptr).status_ ==
                    ruvia::http3_connection_status::message_end);
        RUVIA_CHECK_EQ(connection.active_request_count(), 0U);
        if (stream == 36) {
            retained_after_warmup = resource.allocations_ - resource.deallocations_;
        }
    }
    const auto retained = resource.allocations_ - resource.deallocations_;
    RUVIA_CHECK(retained <= retained_after_warmup);
    RUVIA_CHECK(resource.allocations_ >= before);
}

RUVIA_TEST(http3_connection_accepts_control_settings_and_ignores_zero_length_data) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource);
    captured_events captured;
    constexpr std::array<char, 4> settings{0x00, 0x04, 0x00, 0x00};
    const auto setting_result = connection.feed(2, settings, false, false, capture, &captured);
    RUVIA_CHECK(setting_result.scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(connection.peer_settings().has_value());

    const auto initial_value = request_wire(&resource, "GET", "/zero-data", "");
    RUVIA_CHECK(connection.feed(0, initial_value, false, false, capture, &captured).status_ ==
                ruvia::http3_connection_status::need_more_data);
    constexpr std::array<char, 2> zero_data{0x00, 0x00};
    RUVIA_CHECK(connection.feed(0, zero_data, true, false, capture, &captured).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK_EQ(captured.bodies_.size(), 1U);
    RUVIA_CHECK_EQ(captured.bodies_[0], "");
    RUVIA_CHECK_EQ(captured.ended_.size(), 1U);
}

RUVIA_TEST(http3_connection_rejects_content_length_mismatch_and_releases_request_state) {
    counting_resource resource;
    {
        ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource);
        const std::array<ruvia::http3_field_section_field_view, 5> fields_value{{{":method", "POST"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}, {"content-length", "4"}}};
        const auto section = ruvia::encode_http3_field_section(fields_value, &resource);
        std::array<char, 128> wire{};
        const auto header_value = ruvia::encode_http3_frame_header(wire, 1, std::get<0>(section).size());
        std::copy(std::get<0>(section).begin(), std::get<0>(section).end(), wire.begin() + static_cast<std::ptrdiff_t>(std::get<0>(header_value)));
        captured_events captured;
        const auto result_value = connection.feed(0,
            std::span<const char>(wire).first(std::get<0>(header_value) + std::get<0>(section).size()), true, false, capture, &captured);
        RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::stream);
        RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::message_error);
        RUVIA_CHECK_EQ(connection.active_request_count(), 0U);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(http3_connection_prioritizes_connection_frame_error_over_same_input_stream_error) {
    counting_resource resource;
    {
        ruvia::http3_connection connection(ruvia::http3_peer_role::server, &resource);
        const std::array<ruvia::http3_field_section_field_view, 5> fields_value{{{":method", "POST"}, {":scheme", "https"}, {":authority", "example.test"},
            {":path", "/"}, {"content-length", "0"}}};
        const auto section = ruvia::encode_http3_field_section(fields_value, &resource);
        std::array<char, 16> prefix{};
        const auto prefix_size = ruvia::encode_http3_frame_header(prefix, 1, std::get<0>(section).size());
        std::pmr::vector<char> wire(&resource);
        wire.insert(wire.end(), prefix.begin(), prefix.begin() + std::get<0>(prefix_size));
        wire.insert(wire.end(), std::get<0>(section).begin(), std::get<0>(section).end());
        wire.insert(wire.end(), {0x0, 0x1, 'x', 0x2, 0x0});
        captured_events captured;
        const auto result_value = connection.feed(0, wire, false, false, capture, &captured);
        RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::frame_unexpected);
        RUVIA_CHECK_EQ(captured.bodies_.size(), 1U);
        RUVIA_CHECK(captured.bodies_[0].empty());
        RUVIA_CHECK(captured.ended_.empty());
        RUVIA_CHECK_EQ(connection.active_request_count(), 0U);
        const auto subsequent = connection.feed(4, {}, false, false, capture, &captured);
        RUVIA_CHECK(subsequent.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(subsequent.code_ == ruvia::http3_connection_error_code::frame_unexpected);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(http3_client_connection_prioritizes_frame_error_over_same_input_response_error) {
    counting_resource resource;
    {
        ruvia::http3_connection connection(ruvia::http3_peer_role::client, &resource);
        RUVIA_CHECK(connection.register_client_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        const std::array<ruvia::http3_field_section_field_view, 2> fields_value{{{":status", "200"}, {"content-length", "0"}}};
        const auto section = ruvia::encode_http3_field_section(fields_value, &resource);
        std::array<char, 16> prefix{};
        const auto prefix_size = ruvia::encode_http3_frame_header(prefix, 1, std::get<0>(section).size());
        std::pmr::vector<char> wire(&resource);
        wire.insert(wire.end(), prefix.begin(), prefix.begin() + std::get<0>(prefix_size));
        wire.insert(wire.end(), std::get<0>(section).begin(), std::get<0>(section).end());
        wire.insert(wire.end(), {0x0, 0x1, 'x', 0x2, 0x0});
        client_captured captured;
        const auto result_value = connection.feed(0, wire, false, false, capture_client, &captured);
        RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::frame_unexpected);
        RUVIA_CHECK_EQ(captured.statuses_.at(0), 200U);
        RUVIA_CHECK(captured.ended_.empty());
        RUVIA_CHECK_EQ(connection.active_request_count(), 0U);
        const auto subsequent = connection.feed(4, {}, false, false, capture_client, &captured);
        RUVIA_CHECK(subsequent.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(subsequent.code_ == ruvia::http3_connection_error_code::frame_unexpected);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(http3_connection_rejects_unauthorized_client_push_and_ignores_unknown_uni_streams) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection connection(ruvia::http3_peer_role::client, &resource);
    constexpr std::array<char, 1> push_type_value{0x01};
    const auto push = connection.feed(3, push_type_value, false, false, ignore_event, nullptr);
    RUVIA_CHECK(push.status_ == ruvia::http3_connection_status::connection_error);
    RUVIA_CHECK(push.scope_ == ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK(push.code_ == ruvia::http3_connection_error_code::id_error);
    ruvia::http3_connection closed_push(ruvia::http3_peer_role::client, &resource);
    const auto closed = closed_push.feed(3, push_type_value, true, false, ignore_event, nullptr);
    RUVIA_CHECK(closed.scope_ == ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK(closed.code_ == ruvia::http3_connection_error_code::id_error);

    ruvia::http3_connection unknown(ruvia::http3_peer_role::client, &resource);
    constexpr std::array<char, 2> unknown_type{0x40, 0x21};
    RUVIA_CHECK(unknown.feed(3, std::span(unknown_type).first(1), false, false, ignore_event, nullptr).status_ ==
                ruvia::http3_connection_status::need_more_data);
    RUVIA_CHECK(unknown.feed(3, std::span(unknown_type).subspan(1), false, false, ignore_event, nullptr).status_ ==
                ruvia::http3_connection_status::need_more_data);
    RUVIA_CHECK(unknown.register_client_request(0, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(unknown.feed(0, {}, false, false, ignore_event, nullptr).status_ ==
                ruvia::http3_connection_status::need_more_data);

    ruvia::http3_connection illegal(ruvia::http3_peer_role::client, &resource);
    const auto result_value = illegal.feed(1, {}, false, false, ignore_event, nullptr);
    RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::stream_creation_error);
}

RUVIA_TEST(http3_connection_routes_registered_client_responses_and_releases_terminal_state) {
    counting_resource resource;
    client_captured captured;
    std::string saved_body;
    {
        ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource);
        RUVIA_CHECK(client.register_client_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(client.register_client_request(4, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK_EQ(client.active_request_count(), 2U);
        const auto duplicate = client.register_client_request(0, ruvia::http_known_method::post);
        RUVIA_CHECK(duplicate.scope_ == ruvia::http3_connection_error_scope::stream);
        RUVIA_CHECK(duplicate.code_ == ruvia::http3_connection_error_code::stream_creation_error);
        const auto unregistered = client.feed(8, {}, false, false, capture_client, &captured);
        RUVIA_CHECK(unregistered.scope_ == ruvia::http3_connection_error_scope::stream);
        RUVIA_CHECK(unregistered.code_ == ruvia::http3_connection_error_code::stream_creation_error);

        const auto first = response_wire(&resource, 200, "alpha");
        const auto second = response_wire(&resource, 404, "beta");
        for (std::size_t i = 0; i < first.size(); ++i) {
            const auto result_value = client.feed(0, std::span(first).subspan(i, 1), i + 1 == first.size(),
                false, capture_client, &captured);
            RUVIA_CHECK(result_value.status_ == (i + 1 == first.size()
                                                        ? ruvia::http3_connection_status::message_end
                                                        : ruvia::http3_connection_status::need_more_data));
        }
        saved_body = captured.bodies_[0];
        RUVIA_CHECK(client.feed(4, second, true, false, capture_client, &captured).status_ ==
                    ruvia::http3_connection_status::message_end);
        RUVIA_CHECK_EQ(captured.statuses_[0], 200U);
        RUVIA_CHECK_EQ(captured.statuses_[4], 404U);
        RUVIA_CHECK_EQ(captured.bodies_[0], "alpha");
        RUVIA_CHECK_EQ(captured.bodies_[4], "beta");
        RUVIA_CHECK_EQ(saved_body, "alpha");
        const auto& first_kinds = captured.kinds_.at(0);
        RUVIA_CHECK(first_kinds.size() >= 3);
        RUVIA_CHECK(first_kinds.front() == ruvia::http3_connection_event_kind::final_head);
        RUVIA_CHECK(first_kinds.back() == ruvia::http3_connection_event_kind::message_end);
        RUVIA_CHECK(response_plan(captured, 0, 0) != nullptr &&
                    response_plan(captured, 0, 0)->content_semantics() == ruvia::http_response_content_semantics::with_content);
        for (std::size_t i = 1; i + 1 < first_kinds.size(); ++i) {
            RUVIA_CHECK(first_kinds[i] == ruvia::http3_connection_event_kind::body);
            RUVIA_CHECK(!captured.response_body_plans_.at(0)[i]);
        }
        RUVIA_CHECK(same_plan(response_plan(captured, 0, 0), response_plan(captured, 0, first_kinds.size() - 1)));
        RUVIA_CHECK(response_plan(captured, 4, 0) != nullptr);
        RUVIA_CHECK(response_plan(captured, 4, 0)->response_status().value() == 404);
        RUVIA_CHECK(same_plan(response_plan(captured, 4, 0), response_plan(captured, 4, 2)));
        RUVIA_CHECK_EQ(client.active_request_count(), 0U);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(http3_connection_client_bridge_preserves_final_response_body_plans) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource);
    client_captured captured;
    const std::array registrations{
        std::pair{0U, ruvia::http_known_method::get},
        std::pair{4U, ruvia::http_known_method::head},
        std::pair{8U, ruvia::http_known_method::get},
        std::pair{12U, ruvia::http_known_method::get},
        std::pair{16U, ruvia::http_known_method::connect},
    };
    for (const auto& [stream_id, method] : registrations) {
        RUVIA_CHECK(client.register_client_request(stream_id, method).scope_ == ruvia::http3_connection_error_scope::none);
    }

    auto informational = response_message(&resource, 103, std::nullopt, {}, false);
    auto get_wire = response_message(&resource, 200, 3, "abc", true);
    informational.insert(informational.end(), get_wire.begin(), get_wire.end());
    const auto head_wire = response_message(&resource, 200, 123, {}, false);
    const auto no_content_wire = response_message(&resource, 204, std::nullopt, {}, false);
    const auto not_modified_wire = response_message(&resource, 304, 123, {}, false);
    const auto connect_wire = response_message(&resource, 200, std::nullopt, "tunnel", true);
    const std::array<std::pair<std::uint64_t, std::vector<char>>, 5> responses_value{{{0, std::move(informational)}, {4, head_wire}, {8, no_content_wire}, {12, not_modified_wire}, {16, connect_wire}}};
    for (const auto& [stream_id, wire] : responses_value) {
        RUVIA_CHECK(client.feed(stream_id, wire, true, false, capture_client, &captured).status_ ==
                    ruvia::http3_connection_status::message_end);
    }

    RUVIA_CHECK((captured.kinds_.at(0) == std::vector<ruvia::http3_connection_event_kind>{
                                              ruvia::http3_connection_event_kind::informational_head, ruvia::http3_connection_event_kind::final_head,
                                              ruvia::http3_connection_event_kind::body, ruvia::http3_connection_event_kind::message_end}));
    RUVIA_CHECK(!captured.response_body_plans_.at(0)[0]);
    RUVIA_CHECK(response_plan(captured, 0, 1) != nullptr &&
                response_plan(captured, 0, 1)->content_semantics() == ruvia::http_response_content_semantics::with_content);
    RUVIA_CHECK(!captured.response_body_plans_.at(0)[2]);
    RUVIA_CHECK(same_plan(response_plan(captured, 0, 1), response_plan(captured, 0, 3)));

    for (const auto stream_id : {4U, 8U, 12U}) {
        RUVIA_CHECK((captured.kinds_.at(stream_id) == std::vector<ruvia::http3_connection_event_kind>{
                                                          ruvia::http3_connection_event_kind::final_head, ruvia::http3_connection_event_kind::message_end}));
        const auto* plan = response_plan(captured, stream_id, 0);
        RUVIA_CHECK(plan != nullptr && plan->content_semantics() == ruvia::http_response_content_semantics::without_content);
        RUVIA_CHECK(plan != nullptr && plan->body_suppressed());
        RUVIA_CHECK(same_plan(plan, response_plan(captured, stream_id, 1)));
    }
    RUVIA_CHECK((captured.kinds_.at(16) == std::vector<ruvia::http3_connection_event_kind>{
                                               ruvia::http3_connection_event_kind::final_head, ruvia::http3_connection_event_kind::tunnel_data,
                                               ruvia::http3_connection_event_kind::message_end}));
    const auto* connect_plan = response_plan(captured, 16, 0);
    RUVIA_CHECK(connect_plan != nullptr && connect_plan->content_semantics() == ruvia::http_response_content_semantics::connect_tunnel);
    RUVIA_CHECK(connect_plan != nullptr && connect_plan->body_suppressed());
    RUVIA_CHECK(!captured.response_body_plans_.at(16)[1]);
    RUVIA_CHECK(same_plan(connect_plan, response_plan(captured, 16, 2)));
    RUVIA_CHECK_EQ(captured.bodies_[0], "abc");
    RUVIA_CHECK_EQ(captured.bodies_[16], "tunnel");
    RUVIA_CHECK_EQ(client.active_request_count(), 0U);
}

RUVIA_TEST(http3_connection_server_extended_connect_emits_tunnel_data_and_rejects_trailers) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection server(ruvia::http3_peer_role::server, &resource, {.enable_connect_protocol_ = true});
    captured_events captured;
    const std::array fields_value{
        ruvia::http3_field_section_field_view{":method", "CONNECT"},
        ruvia::http3_field_section_field_view{":protocol", "websocket"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/socket"},
        ruvia::http3_field_section_field_view{"content-length", "0"},
    };
    const auto section = ruvia::encode_http3_field_section(fields_value, &resource);
    std::array<char, 16> head_prefix{};
    const auto head_size = ruvia::encode_http3_frame_header(head_prefix, 1, std::get<0>(section).size());
    std::vector<char> head_wire(head_prefix.begin(), head_prefix.begin() +
                                                         static_cast<std::ptrdiff_t>(std::get<0>(head_size)));
    head_wire.insert(head_wire.end(), std::get<0>(section).begin(), std::get<0>(section).end());
    RUVIA_CHECK(server.feed(0, head_wire, false, false, capture, &captured).status_ ==
                ruvia::http3_connection_status::need_more_data);
    RUVIA_CHECK_EQ(captured.methods_.size(), 1U);

    std::array<char, 16> data_prefix{};
    const auto data_size = ruvia::encode_http3_frame_header(data_prefix, 0, 3);
    std::vector<char> data_wire(data_prefix.begin(), data_prefix.begin() +
                                                         static_cast<std::ptrdiff_t>(std::get<0>(data_size)));
    data_wire.insert(data_wire.end(), {'w', 's', '!'});
    RUVIA_CHECK(server.feed(0, data_wire, true, false, capture, &captured).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK_EQ(captured.bodies_.front(), "ws!");
    RUVIA_CHECK(captured.trailers_.empty());
    RUVIA_CHECK_EQ(captured.ended_.size(), 1U);

    const auto trailer_section = ruvia::encode_http3_field_section(
        std::array<ruvia::http3_field_section_field_view, 1>{{{"x-trailer", "forbidden"}}}, &resource);
    std::array<char, 16> trailer_prefix{};
    const auto trailer_size = ruvia::encode_http3_frame_header(trailer_prefix, 1, std::get<0>(trailer_section).size());
    std::vector<char> trailer_wire(trailer_prefix.begin(), trailer_prefix.begin() +
                                                               static_cast<std::ptrdiff_t>(std::get<0>(trailer_size)));
    trailer_wire.insert(trailer_wire.end(), std::get<0>(trailer_section).begin(), std::get<0>(trailer_section).end());
    captured_events trailer_capture;
    RUVIA_CHECK(server.feed(4, head_wire, false, false, capture, &trailer_capture).status_ ==
                ruvia::http3_connection_status::need_more_data);
    const auto trailer_result = server.feed(4, trailer_wire, false, false, capture, &trailer_capture);
    RUVIA_CHECK(trailer_result.scope_ == ruvia::http3_connection_error_scope::stream);
    RUVIA_CHECK(trailer_result.code_ == ruvia::http3_connection_error_code::message_error);
}

RUVIA_TEST(http3_connection_client_reset_and_stream_errors_are_isolated) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource);
    client_captured captured;
    RUVIA_CHECK(client.register_client_request(0, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(client.register_client_request(4, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    const auto reset = client.feed(0, {}, false, true, capture_client, &captured);
    RUVIA_CHECK(reset.status_ == ruvia::http3_connection_status::reset);
    RUVIA_CHECK_EQ(captured.reset_.size(), 1U);
    RUVIA_CHECK_EQ(client.active_request_count(), 1U);
    const std::array<ruvia::http3_field_section_field_view, 2> bad_fields{{{":status", "200"}, {"content-length", "2"}}};
    const auto bad_section = ruvia::encode_http3_field_section(bad_fields, &resource);
    std::array<char, 16> bad_frame_header{};
    const auto bad_header_length = ruvia::encode_http3_frame_header(bad_frame_header, 1, std::get<0>(bad_section).size());
    std::vector<char> bad_wire(bad_frame_header.begin(), bad_frame_header.begin() + static_cast<std::ptrdiff_t>(std::get<0>(bad_header_length)));
    bad_wire.insert(bad_wire.end(), std::get<0>(bad_section).begin(), std::get<0>(bad_section).end());
    std::array<char, 16> data_header{};
    const auto data_header_length = ruvia::encode_http3_frame_header(data_header, 0, 1);
    bad_wire.insert(bad_wire.end(), data_header.begin(), data_header.begin() + static_cast<std::ptrdiff_t>(std::get<0>(data_header_length)));
    bad_wire.push_back('x');
    const auto bad_stream = client.feed(4, bad_wire, true, false, capture_client, &captured);
    RUVIA_CHECK(bad_stream.scope_ == ruvia::http3_connection_error_scope::stream);
    RUVIA_CHECK(bad_stream.code_ == ruvia::http3_connection_error_code::message_error);
    RUVIA_CHECK_EQ(client.active_request_count(), 0U);
    RUVIA_CHECK(client.register_client_request(8, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    const auto good = response_wire(&resource, 200, "");
    RUVIA_CHECK(client.feed(8, good, true, false, capture_client, &captured).status_ ==
                ruvia::http3_connection_status::message_end);
}

RUVIA_TEST(http3_connection_client_qpack_errors_latch_connection_and_validate_ids_and_budget) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource, {.max_active_streams_ = 1});
    auto invalid_registration = client.register_client_request(1, ruvia::http_known_method::get);
    RUVIA_CHECK(invalid_registration.code_ == ruvia::http3_connection_error_code::stream_creation_error);
    RUVIA_CHECK(client.register_client_request(0, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(client.register_client_request(4, ruvia::http_known_method::get).code_ ==
                ruvia::http3_connection_error_code::excessive_load);
    RUVIA_CHECK(client.feed(1, {}, false, false, ignore_event, nullptr).code_ ==
                ruvia::http3_connection_error_code::stream_creation_error);

    ruvia::http3_connection broken(ruvia::http3_peer_role::client, &resource);
    RUVIA_CHECK(broken.register_client_request(0, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(broken.register_client_request(4, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    constexpr std::array<char, 4> dynamic_reference{0x01, 0x02, 0x01, 0x00};
    const auto error = broken.feed(0, dynamic_reference, false, false, ignore_event, nullptr);
    RUVIA_CHECK(error.scope_ == ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK(error.code_ == ruvia::http3_connection_error_code::qpack_decompression_failed);
    RUVIA_CHECK(broken.feed(0, {}, false, false, ignore_event, nullptr).code_ == error.code_);
    RUVIA_CHECK_EQ(broken.active_request_count(), 0U);
    RUVIA_CHECK(broken.register_client_request(8, ruvia::http_known_method::get).code_ == error.code_);
}

RUVIA_TEST(http3_connection_retirement_returns_all_protocol_storage_without_events) {
    counting_resource resource;
    for (const auto role : {ruvia::http3_peer_role::server, ruvia::http3_peer_role::client}) {
        ruvia::http3_connection connection(role, &resource);
        unsigned events_value = 0;
        const auto callback_value = [](void* opaque, const ruvia::http3_connection_event&) {
            ++*static_cast<unsigned*>(opaque);
        };
        const std::array<char, 3> settings{0, 4, 0};
        const auto control_id = role == ruvia::http3_peer_role::server ? 2U : 3U;
        RUVIA_CHECK(connection.feed(control_id, settings, false, false, callback_value, &events_value).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(connection.peer_settings().has_value());
        if (role == ruvia::http3_peer_role::client) {
            (void)connection.register_client_request(0, ruvia::http_known_method::get);
            (void)connection.register_client_request(4, ruvia::http_known_method::get);
        }
        const auto wire = role == ruvia::http3_peer_role::server
                              ? request_wire(&resource, "POST", "/items", "payload")
                              : response_wire(&resource, 200, "payload");
        RUVIA_CHECK(connection.feed(0, std::span(wire).first(3), false, false, callback_value, &events_value).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(connection.feed(4, wire, true, false, callback_value, &events_value).status_ == ruvia::http3_connection_status::message_end);
        RUVIA_CHECK_EQ(connection.active_request_count(), 1U);
        const auto previous_events = events_value;
        RUVIA_CHECK(connection.retire());
        RUVIA_CHECK_EQ(events_value, previous_events);
        RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
        RUVIA_CHECK(!connection.retire() && !connection.peer_settings() && !connection.peer_goaway_id());
        RUVIA_CHECK_EQ(connection.active_request_count(), 0U);
        RUVIA_CHECK(connection.feed(8, wire, true, false, callback_value, &events_value).scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(connection.register_client_request(8, ruvia::http_known_method::get).scope_ != ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK_EQ(events_value, previous_events);
        auto moved = std::move(connection);
        RUVIA_CHECK(!moved.retire() && !connection.retire());
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(http3_connection_locally_retires_server_parsers_without_peer_reset_events) {
    counting_resource resource;
    {
        ruvia::http3_connection server(ruvia::http3_peer_role::server, &resource, {.max_active_streams_ = 1});
        captured_events events;
        const auto wire = request_wire(&resource, "POST", "/items", "payload");
        std::size_t baseline = 0;
        for (std::uint64_t step = 0; step < 32; ++step) {
            const auto id = step * 4;
            const auto input = (step & 1U) == 0 ? std::span(wire).first(3) : std::span(wire);
            RUVIA_CHECK(server.feed(id, input, false, false, capture, &events).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK_EQ(server.active_request_count(), 1U);
            RUVIA_CHECK(!server.retire_client_request(id));
            const auto before = resource.deallocations_;
            RUVIA_CHECK(server.retire_server_request(id));
            RUVIA_CHECK(!server.retire_server_request(id));
            RUVIA_CHECK(resource.deallocations_ > before);
            RUVIA_CHECK_EQ(server.active_request_count(), 0U);
            if (step == 0) {
                baseline = resource.allocations_ - resource.deallocations_;
            } else {
                RUVIA_CHECK_EQ(resource.allocations_ - resource.deallocations_, baseline);
            }
        }
        RUVIA_CHECK(events.reset_.empty() && events.ended_.empty());
        RUVIA_CHECK(!server.retire_server_request(2));
        RUVIA_CHECK(server.feed(128, wire, true, false, capture, &events).status_ == ruvia::http3_connection_status::message_end);
        RUVIA_CHECK(events.ended_.size() == 1 && events.bodies_.back() == "payload");
        RUVIA_CHECK(!server.retire_server_request(128));
        ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource);
        (void)client.register_client_request(0, ruvia::http_known_method::get);
        RUVIA_CHECK(!client.retire_server_request(0));
        RUVIA_CHECK_EQ(client.active_request_count(), 1U);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(http3_connection_cannot_retire_server_parser_from_feed_callback) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection server(ruvia::http3_peer_role::server, &resource);
    struct attempt {
        ruvia::http3_connection* connection_;
        bool called_{};
        bool removed_{};
    } attempt_value{&server};
    const auto wire = request_wire(&resource, "POST", "/items", "payload");
    const auto result_value = server.feed(0, wire, true, false, [](void* opaque, const ruvia::http3_connection_event& event) {
            auto& state_value = *static_cast<attempt*>(opaque);
            if (event.kind_ == ruvia::http3_connection_event_kind::request_head) {
                state_value.called_ = true;
                state_value.removed_ = state_value.connection_->retire_server_request(event.stream_id_) || state_value.connection_->retire();
            } }, &attempt_value);
    RUVIA_CHECK(attempt_value.called_ && !attempt_value.removed_);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_connection_status::message_end);
}

RUVIA_TEST(http3_connection_retires_only_live_client_response_parser_state) {
    counting_resource resource;
    {
        ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource);
        client_captured events;
        RUVIA_CHECK(!client.retire_client_request(0));
        RUVIA_CHECK(client.register_client_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(client.register_client_request(4, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        const auto partial = response_wire(&resource, 200, "partial");
        RUVIA_CHECK(client.feed(0, std::span(partial).first(3), false, false, capture_client, &events).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK_EQ(client.active_request_count(), 2U);
        RUVIA_CHECK(!client.retire_client_request(2));
        RUVIA_CHECK(client.retire_client_request(0));
        RUVIA_CHECK(!client.retire_client_request(0));
        RUVIA_CHECK_EQ(client.active_request_count(), 1U);
        const auto completed = response_wire(&resource, 200, "ok");
        RUVIA_CHECK(client.feed(4, completed, true, false, capture_client, &events).status_ ==
                    ruvia::http3_connection_status::message_end);
        RUVIA_CHECK(events.bodies_[4] == "ok");
        RUVIA_CHECK_EQ(client.active_request_count(), 0U);
        RUVIA_CHECK(!client.retire_client_request(4));
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(http3_connection_cannot_retire_client_parser_from_its_feed_callback) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource);
    struct attempt final {
        ruvia::http3_connection* connection_{};
        bool called_{};
        bool removed_{};
    } attempt_value{&client};
    RUVIA_CHECK(client.register_client_request(0, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    const auto wire = response_wire(&resource, 200, "ok");
    const auto parsed_value = client.feed(0, wire, true, false, [](void* opaque, const ruvia::http3_connection_event& event) {
        auto& state_value = *static_cast<attempt*>(opaque);
        if (event.kind_ == ruvia::http3_connection_event_kind::final_head) {
            state_value.called_ = true;
            state_value.removed_ = state_value.connection_->retire_client_request(event.stream_id_) || state_value.connection_->retire();
        } }, &attempt_value);
    RUVIA_CHECK(attempt_value.called_ && !attempt_value.removed_);
    RUVIA_CHECK(parsed_value.status_ == ruvia::http3_connection_status::message_end);
    RUVIA_CHECK_EQ(client.active_request_count(), 0U);
}

RUVIA_TEST(http3_connection_unprocessed_evidence_excludes_observed_responses_and_unknown_requests) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource);
    const auto rejected = static_cast<std::uint64_t>(ruvia::http3_connection_error_code::request_rejected);
    for (const auto id : {0U, 4U, 8U, 12U}) {
        RUVIA_CHECK(client.register_client_request(id, ruvia::http_known_method::post).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(!client.peer_reports_unprocessed(id));
        RUVIA_CHECK(client.peer_reports_unprocessed(id, rejected));
        RUVIA_CHECK(!client.peer_reports_unprocessed(id, 0));
    }
    RUVIA_CHECK(!client.peer_reports_unprocessed(16, rejected));
    const auto final_head = response_wire(&resource, 200, "");
    RUVIA_CHECK(client.feed(8, final_head, false, false, ignore_event, nullptr).scope_ ==
                ruvia::http3_connection_error_scope::none);
    const std::array informational_fields{ruvia::http3_field_section_field_view{":status", "103"}};
    const auto section = ruvia::encode_http3_field_section(informational_fields, &resource);
    std::vector<char> informational(16);
    const auto prefix = ruvia::encode_http3_frame_header(informational, 1, std::get<0>(section).size());
    informational.resize(std::get<0>(prefix));
    informational.insert(informational.end(), std::get<0>(section).begin(), std::get<0>(section).end());
    RUVIA_CHECK(client.feed(12, informational, false, false, ignore_event, nullptr).scope_ ==
                ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(!client.peer_reports_unprocessed(8, rejected));
    RUVIA_CHECK(!client.peer_reports_unprocessed(12, rejected));
    constexpr std::array<char, 6> control{0x00, 0x04, 0x00, 0x07, 0x01, 0x04};
    RUVIA_CHECK(client.feed(3, control, false, false, ignore_event, nullptr).scope_ ==
                ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(!client.peer_reports_unprocessed(0));
    RUVIA_CHECK(client.peer_reports_unprocessed(4));
    RUVIA_CHECK(!client.peer_reports_unprocessed(8));
    RUVIA_CHECK(!client.peer_reports_unprocessed(12));
    RUVIA_CHECK(client.retire_client_request(4));
    RUVIA_CHECK(!client.peer_reports_unprocessed(4, rejected));
    RUVIA_CHECK(client.feed(3, {}, true, false, ignore_event, nullptr).scope_ ==
                ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK(!client.peer_reports_unprocessed(0, rejected));
}

RUVIA_TEST(http3_connection_client_does_not_admit_requests_past_peer_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource);
    RUVIA_CHECK(client.register_client_request(0, ruvia::http_known_method::get).scope_ ==
                ruvia::http3_connection_error_scope::none);
    constexpr std::array<char, 6> control{0x00, 0x04, 0x00, 0x07, 0x01, 0x04};
    RUVIA_CHECK(client.feed(3, control, false, false, ignore_event, nullptr).scope_ ==
                ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(client.peer_goaway_id() == 4);

    const auto rejected = client.register_client_request(4, ruvia::http_known_method::get);
    RUVIA_CHECK(rejected.scope_ == ruvia::http3_connection_error_scope::stream);
    RUVIA_CHECK(rejected.code_ == ruvia::http3_connection_error_code::request_rejected);
    RUVIA_CHECK_EQ(client.active_request_count(), 1U);
}

RUVIA_TEST(http3_connection_moved_from_metadata_is_empty) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection connection(ruvia::http3_peer_role::client, &resource);
    auto moved = std::move(connection);
    RUVIA_CHECK(!connection.peer_settings().has_value());
    RUVIA_CHECK(!connection.peer_goaway_id().has_value());
    RUVIA_CHECK_EQ(connection.active_request_count(), 0U);
    RUVIA_CHECK(!moved.peer_settings().has_value());
}

RUVIA_TEST(http3_connection_exposes_peer_goaway_after_settings_for_both_roles) {
    std::pmr::monotonic_buffer_resource resource;
    {
        ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource);
        RUVIA_CHECK(!client.peer_goaway_id().has_value());
        constexpr std::array<char, 9> control{0x00, 0x04, 0x00, 0x07, 0x01, 0x04, 0x07, 0x01, 0x00};
        for (std::size_t i = 0; i < control.size(); ++i) {
            const auto result_value = client.feed(3, std::span(control).subspan(i, 1), false, false, ignore_event, nullptr);
            RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(client.peer_goaway_id() == (i < 5 ? std::nullopt : i < 8 ? std::optional<std::uint64_t>{4}
                                                                                 : std::optional<std::uint64_t>{0}));
        }
        constexpr std::array<char, 3> increasing{0x07, 0x01, 0x04};
        const auto invalid = client.feed(3, increasing, false, false, ignore_event, nullptr);
        RUVIA_CHECK(invalid.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(invalid.code_ == ruvia::http3_connection_error_code::id_error);
        RUVIA_CHECK(client.peer_goaway_id() == 0);
    }
    {
        ruvia::http3_connection server(ruvia::http3_peer_role::server, &resource);
        RUVIA_CHECK(!server.peer_goaway_id().has_value());
        constexpr std::array<char, 9> control{0x00, 0x04, 0x00, 0x07, 0x01, 0x05, 0x07, 0x01, 0x03};
        for (std::size_t i = 0; i < control.size(); ++i) {
            const auto result_value = server.feed(2, std::span(control).subspan(i, 1), false, false, ignore_event, nullptr);
            RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(server.peer_goaway_id() == (i < 5 ? std::nullopt : i < 8 ? std::optional<std::uint64_t>{5}
                                                                                 : std::optional<std::uint64_t>{3}));
        }
        constexpr std::array<char, 3> increasing{0x07, 0x01, 0x04};
        const auto invalid = server.feed(2, increasing, false, false, ignore_event, nullptr);
        RUVIA_CHECK(invalid.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(invalid.code_ == ruvia::http3_connection_error_code::id_error);
        RUVIA_CHECK(server.peer_goaway_id() == 3);
    }
}

RUVIA_TEST(http3_connection_dynamic_headers_pause_at_frame_boundary_for_both_roles) {
    for (const auto role : {ruvia::http3_peer_role::server, ruvia::http3_peer_role::client}) {
        std::pmr::unsynchronized_pool_resource resource;
        ruvia::http3_connection connection(role, &resource,
            {.qpack_max_table_capacity_ = 512, .qpack_blocked_streams_ = 2});
        ruvia::http3_qpack_encoder encoder({.max_table_capacity_ = 512, .max_blocked_streams_ = 2}, &resource);
        const std::array request_fields{ruvia::http3_field_section_field_view{":method", "POST"},
            ruvia::http3_field_section_field_view{":scheme", "https"},
            ruvia::http3_field_section_field_view{":authority", "example.test"},
            ruvia::http3_field_section_field_view{":path", "/dynamic"},
            ruvia::http3_field_section_field_view{"content-length", "2"}};
        const std::array response_fields{ruvia::http3_field_section_field_view{":status", "200"},
            ruvia::http3_field_section_field_view{"content-length", "2"}};
        if (role == ruvia::http3_peer_role::client) {
            RUVIA_CHECK(connection.register_client_request(0, ruvia::http_known_method::post).scope_ ==
                        ruvia::http3_connection_error_scope::none);
        }
        auto fields_value = role == ruvia::http3_peer_role::server
                                ? std::span<const ruvia::http3_field_section_field_view>(request_fields)
                                : std::span<const ruvia::http3_field_section_field_view>(response_fields);
        auto section = encoder.encode(0, fields_value);
        RUVIA_CHECK((section.index() == 0));
        std::array<char, 16> header_value{};
        const auto header_size = ruvia::encode_http3_frame_header(header_value, 1, std::get<0>(section).size());
        std::vector<char> wire(header_value.begin(), header_value.begin() + std::get<0>(header_size));
        wire.insert(wire.end(), std::get<0>(section).begin(), std::get<0>(section).end());
        const auto boundary = wire.size();
        const auto data_size = ruvia::encode_http3_frame_header(header_value, 0, 2);
        wire.insert(wire.end(), header_value.begin(), header_value.begin() + std::get<0>(data_size));
        wire.insert(wire.end(), {'o', 'k'});
        client_captured captured;
        const auto blocked = connection.feed(0, wire, true, false, capture_client, &captured);
        RUVIA_CHECK(blocked.status_ == ruvia::http3_connection_status::qpack_blocked);
        RUVIA_CHECK_EQ(blocked.consumed_bytes_, boundary);
        RUVIA_CHECK(captured.kinds_.empty());
        const auto encoder_stream = role == ruvia::http3_peer_role::server ? 2u : 3u;
        std::vector<char> instructions{char(2)};
        const auto output = encoder.pending_encoder_output();
        instructions.insert(instructions.end(), output.begin(), output.end());
        RUVIA_CHECK(connection.feed(encoder_stream, instructions, false, false, capture_client, &captured).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(connection.feed(0, std::span(wire).subspan(boundary), true, false, capture_client, &captured).status_ ==
                    ruvia::http3_connection_status::message_end);
        RUVIA_CHECK(captured.bodies_[0] == "ok");
        RUVIA_CHECK_EQ(connection.active_request_count(), 0u);
        RUVIA_CHECK((encoder.consume_decoder(connection.pending_qpack_decoder_output()).index() == 0));
        RUVIA_CHECK(connection.consume_qpack_decoder_output(connection.pending_qpack_decoder_output().size()));
    }
}

namespace {
std::vector<char> control_settings(ruvia::http3_settings settings) {
    std::array<char, 128> bytes_value{};
    const auto size = ruvia::encode_http3_settings(std::span(bytes_value).subspan(16), settings);
    const auto frame = ruvia::encode_http3_frame_header(bytes_value, 4, std::get<0>(size));
    std::vector<char> wire{0};
    wire.insert(wire.end(), bytes_value.begin(), bytes_value.begin() + std::get<0>(frame));
    wire.insert(wire.end(), bytes_value.begin() + 16, bytes_value.begin() + 16 + std::get<0>(size));
    return wire;
}
std::vector<char> id_frame(std::uint64_t type, std::uint64_t id) {
    std::array<char, 24> bytes_value{};
    const auto frame = ruvia::encode_http3_frame_header(bytes_value, type, ruvia::http3_var_int_encoded_size(id));
    const auto encoded = ruvia::encode_http3_var_int(std::span(bytes_value).subspan(std::get<0>(frame)), id);
    return {bytes_value.begin(), bytes_value.begin() + std::get<0>(frame) + std::get<0>(encoded)};
}
}  // namespace
RUVIA_TEST(http3_push_stream_before_promise_resumes_without_buffering_body) {
    counting_resource resource;
    client_captured captured;
    {
        ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource, {.max_push_id_ = 0});
        ruvia::http3_connection server(ruvia::http3_peer_role::server, &resource);
        auto control = control_settings({});
        const auto maximum = id_frame(0xd, 0);
        control.insert(control.end(), maximum.begin(), maximum.end());
        RUVIA_CHECK(server.feed(2, control, false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
        auto promise = server.prepare_push_promise(0, 0, {.authority_ = "example.test", .path_ = "/asset"});
        RUVIA_CHECK((promise.index() == 0));
        RUVIA_CHECK(server.promised_request(0) != nullptr);
        RUVIA_CHECK_EQ(server.promised_request(0)->path_, "/asset");
        RUVIA_CHECK(server.promised_request(1) == nullptr);
        RUVIA_CHECK(client.register_client_request(0, ruvia::http_known_method::get).scope_ == ruvia::http3_connection_error_scope::none);
        auto pushed = response_wire(&resource, 200, "asset");
        pushed.insert(pushed.begin(), {1, 0});
        const auto blocked = client.feed(3, pushed, true, false, capture_client, &captured);
        RUVIA_CHECK(blocked.status_ == ruvia::http3_connection_status::push_promise_pending);
        RUVIA_CHECK_EQ(blocked.consumed_bytes_, 2u);
        RUVIA_CHECK(client.feed(0, std::get<0>(promise), false, false, capture_client, &captured).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(captured.kinds_[0].back() == ruvia::http3_connection_event_kind::push_promise);
        RUVIA_CHECK(client.feed(3, std::span(pushed).subspan(blocked.consumed_bytes_), true, false, capture_client, &captured).status_ == ruvia::http3_connection_status::message_end);
        RUVIA_CHECK_EQ(captured.bodies_[3], "asset");
        RUVIA_CHECK_EQ(client.active_request_count(), 1u);
        RUVIA_CHECK(client.feed(0, response_wire(&resource, 200, "root"), true, false, capture_client, &captured).status_ == ruvia::http3_connection_status::message_end);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}
RUVIA_TEST(http3_push_cancellation_can_precede_promise_on_client) {
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource, {.max_push_id_ = 0});
    auto control = control_settings({});
    const auto cancel = id_frame(3, 0);
    control.insert(control.end(), cancel.begin(), cancel.end());
    client_captured captured;
    RUVIA_CHECK(client.feed(3, control, false, false, capture_client, &captured).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(captured.kinds_[0].back() == ruvia::http3_connection_event_kind::push_canceled);
    const std::array<char, 2> prefix{1, 0};
    const auto result_value = client.feed(7, prefix, false, false, capture_client, &captured);
    RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::stream && result_value.code_ == ruvia::http3_connection_error_code::request_cancelled);
    RUVIA_CHECK_EQ(client.active_request_count(), 0u);
}
RUVIA_TEST(http3_connection_owned_qpack_encoder_accepts_decoder_acknowledgments) {
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http3_connection server(ruvia::http3_peer_role::server, &resource);
    ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource, {.qpack_max_table_capacity_ = 512, .qpack_blocked_streams_ = 2});
    auto settings = control_settings({.qpack_max_table_capacity_ = 512, .qpack_blocked_streams_ = 2});
    RUVIA_CHECK(server.feed(2, settings, false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
    const std::array fields_value{ruvia::http3_field_section_field_view{":status", "200"}, ruvia::http3_field_section_field_view{"x-reused", "value"}};
    const auto section = server.encode_field_section(0, fields_value);
    RUVIA_CHECK((section.index() == 0));
    std::array<char, 16> frame{};
    const auto size = ruvia::encode_http3_frame_header(frame, 1, std::get<0>(section).size());
    std::vector<char> wire(frame.begin(), frame.begin() + std::get<0>(size));
    wire.insert(wire.end(), std::get<0>(section).begin(), std::get<0>(section).end());
    RUVIA_CHECK(client.register_client_request(0, ruvia::http_known_method::get).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(client.feed(0, wire, true, false, ignore_event, nullptr).status_ == ruvia::http3_connection_status::qpack_blocked);
    const auto pending = server.pending_qpack_encoder_output();
    std::vector<char> instructions{2};
    instructions.insert(instructions.end(), pending.begin(), pending.end());
    RUVIA_CHECK(client.feed(7, instructions, false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(server.consume_qpack_encoder_output(pending.size()));
    RUVIA_CHECK(client.feed(0, {}, true, false, ignore_event, nullptr).status_ == ruvia::http3_connection_status::message_end);
    const auto acknowledgments = client.pending_qpack_decoder_output();
    instructions.assign(1, 3);
    instructions.insert(instructions.end(), acknowledgments.begin(), acknowledgments.end());
    RUVIA_CHECK(server.feed(6, instructions, false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(client.consume_qpack_decoder_output(acknowledgments.size()));
    RUVIA_CHECK((server.encode_field_section(4, fields_value).index() == 0));
}

RUVIA_TEST(http3_control_output_updates_push_authorization_cancellation_and_priorities) {
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource);
    ruvia::http3_connection server(ruvia::http3_peer_role::server, &resource);
    client_captured captured;
    auto control = control_settings({});
    RUVIA_CHECK(server.feed(2, control, false, false, capture_client, &captured).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(client.register_client_request(0, ruvia::http_known_method::get).scope_ == ruvia::http3_connection_error_scope::none);
    const auto maximum = client.prepare_max_push_id(2);
    RUVIA_CHECK((maximum.index() == 0));
    RUVIA_CHECK(server.feed(2, std::get<0>(maximum), false, false, capture_client, &captured).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(server.peer_max_push_id() == 2);
    RUVIA_CHECK((client.prepare_max_push_id(1).index() != 0));
    const auto promise = server.prepare_push_promise(0, 0, {.authority_ = "example.test", .path_ = "/asset"});
    RUVIA_CHECK((promise.index() == 0));
    RUVIA_CHECK(client.feed(0, std::get<0>(promise), false, false, capture_client, &captured).scope_ == ruvia::http3_connection_error_scope::none);
    const auto priority = client.prepare_priority_update({.element_id_ = 0, .push_ = true, .fields_ = {.urgency_ = 1}});
    RUVIA_CHECK((priority.index() == 0));
    RUVIA_CHECK(server.feed(2, std::get<0>(priority), false, false, capture_client, &captured).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(captured.kinds_[0].back() == ruvia::http3_connection_event_kind::priority_update);
    auto stream = server.prepare_push_stream(3, 0);
    RUVIA_CHECK((stream.index() == 0) && std::get<0>(stream).size() == 2);
    RUVIA_CHECK((server.prepare_push_stream(7, 0).index() != 0));
    const auto cancel = client.prepare_cancel_push(0);
    RUVIA_CHECK((cancel.index() == 0));
    RUVIA_CHECK(server.feed(2, std::get<0>(cancel), false, false, capture_client, &captured).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(captured.kinds_[3].back() == ruvia::http3_connection_event_kind::push_canceled);
    RUVIA_CHECK((server.prepare_push_promise(0, 0, {.authority_ = "example.test", .path_ = "/asset"}).index() != 0));
    RUVIA_CHECK((server.prepare_goaway(4)).index() == 0);
    RUVIA_CHECK((server.prepare_goaway(8).index() != 0));
    const auto result_value = server.feed(4, request_wire(&resource, "GET", "/", ""), true, false, ignore_event, nullptr);
    RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::request_rejected);
}
RUVIA_TEST(http3_priority_update_requires_client_control_stream_and_promised_push) {
    std::pmr::unsynchronized_pool_resource resource;
    for (const bool client_role : {false, true}) {
        ruvia::http3_connection receiver(client_role ? ruvia::http3_peer_role::client : ruvia::http3_peer_role::server, &resource);
        auto wire = control_settings({});
        const auto maximum = id_frame(0xd, 1);
        if (!client_role) {
            wire.insert(wire.end(), maximum.begin(), maximum.end());
        }
        std::array<char, 32> output{};
        const auto size = ruvia::encode_http3_priority_update(output, {.element_id_ = 0, .push_ = true, .fields_ = {.urgency_ = 0}});
        RUVIA_CHECK((size.index() == 0));
        wire.insert(wire.end(), output.begin(), output.begin() + std::get<0>(size));
        const auto result_value = receiver.feed(client_role ? 3 : 2, wire, false, false, ignore_event, nullptr);
        RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(result_value.code_ == (client_role ? ruvia::http3_connection_error_code::frame_unexpected : ruvia::http3_connection_error_code::id_error));
    }
}

RUVIA_TEST(http3_connection_ordinary_connect_uses_tunnel_events_without_extended_negotiation) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection server(ruvia::http3_peer_role::server, &resource);
    const std::array fields_value{ruvia::http3_field_section_field_view{":method", "CONNECT"},
        ruvia::http3_field_section_field_view{":authority", "example.test:443"}};
    const auto section = ruvia::encode_http3_field_section(fields_value, &resource);
    RUVIA_CHECK((section.index() == 0));
    std::array<char, 16> prefix{};
    const auto size = ruvia::encode_http3_frame_header(prefix, 1, std::get<0>(section).size());
    std::vector<char> bytes_value(prefix.begin(), prefix.begin() + std::get<0>(size));
    bytes_value.insert(bytes_value.end(), std::get<0>(section).begin(), std::get<0>(section).end());
    const auto data_size = ruvia::encode_http3_frame_header(prefix, 0, 6);
    bytes_value.insert(bytes_value.end(), prefix.begin(), prefix.begin() + std::get<0>(data_size));
    bytes_value.insert(bytes_value.end(), {'t', 'u', 'n', 'n', 'e', 'l'});
    struct tunnel_events final {
        std::size_t tunnel_{0};
        std::size_t body_{0};
        std::size_t end_{0};
    } events;
    const auto capture_events = [](void* opaque, const ruvia::http3_connection_event& event) {
        auto& result_value = *static_cast<tunnel_events*>(opaque);
        result_value.tunnel_ += event.kind_ == ruvia::http3_connection_event_kind::tunnel_data;
        result_value.body_ += event.kind_ == ruvia::http3_connection_event_kind::body;
        result_value.end_ += event.kind_ == ruvia::http3_connection_event_kind::message_end;
    };
    RUVIA_CHECK(server.feed(0, bytes_value, true, false, capture_events, &events).status_ == ruvia::http3_connection_status::message_end);
    RUVIA_CHECK_EQ(events.tunnel_, 1u);
    RUVIA_CHECK_EQ(events.body_, 0u);
    RUVIA_CHECK_EQ(events.end_, 1u);
}

RUVIA_TEST(http3_connection_origin_advertisement_is_incremental_and_requires_origin_context) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_connection server(ruvia::http3_peer_role::server, &resource);
    const auto frame = server.prepare_origin_advertisement(std::array<std::string_view, 1>{"https://example.test"});
    RUVIA_CHECK((frame.index() == 0));
    for (const bool enabled : {false, true}) {
        ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource, {.receive_origin_advertisements_ = enabled});
        const std::array<char, 3> prefix{0, 4, 0};
        RUVIA_CHECK(client.feed(3, prefix, false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
        std::vector<std::string> origins;
        const auto callback_value = [](void* opaque, const ruvia::http3_connection_event& event) {
            if (event.kind_ != ruvia::http3_connection_event_kind::origin_advertisement) {
                return;
            }
            auto& result_value = *static_cast<std::vector<std::string>*>(opaque);
            for (const auto& origin : event.origin_advertisement_->origins_) {
                result_value.emplace_back(origin);
            }
        };
        for (const auto& byte : std::get<0>(frame)) {
            RUVIA_CHECK(client.feed(3, {&byte, 1}, false, false, callback_value, &origins).scope_ == ruvia::http3_connection_error_scope::none);
        }
        RUVIA_CHECK_EQ(origins.size(), static_cast<std::size_t>(enabled));
        if (enabled) {
            RUVIA_CHECK_EQ(origins.front(), std::string("https://example.test"));
        }
    }
}

RUVIA_TEST(http3_push_promise_field_section_limit_excludes_its_varint_push_identifier) {
    std::pmr::monotonic_buffer_resource resource;
    const std::array fields_value{ruvia::http3_field_section_field_view{":method", "GET"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/"}};
    const auto section = ruvia::encode_http3_field_section(fields_value, &resource);
    RUVIA_CHECK((section.index() == 0));
    ruvia::http3_connection client(ruvia::http3_peer_role::client, &resource,
        {.max_encoded_field_section_bytes_ = std::get<0>(section).size(), .max_push_id_ = 0});
    RUVIA_CHECK(client.register_client_request(0, ruvia::http_known_method::get).scope_ == ruvia::http3_connection_error_scope::none);
    std::array<char, 16> prefix{};
    const auto size = ruvia::encode_http3_frame_header(prefix, 5, 8 + std::get<0>(section).size());
    std::vector<char> wire(prefix.begin(), prefix.begin() + std::get<0>(size));
    // The same Push ID zero may legally occupy eight bytes.
    wire.insert(wire.end(), {static_cast<char>(0xc0), 0, 0, 0, 0, 0, 0, 0});
    wire.insert(wire.end(), std::get<0>(section).begin(), std::get<0>(section).end());
    client_captured captured;
    RUVIA_CHECK(client.feed(0, wire, false, false, capture_client, &captured).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(captured.kinds_[0].back() == ruvia::http3_connection_event_kind::push_promise);
}
