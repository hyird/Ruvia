#include <algorithm>
#include <array>
#include <cstddef>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http3_client_response.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_var_int.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

namespace {
struct client_response_events final {
    std::vector<ruvia::http3_client_response_event_kind> kinds_;
    std::string body_;
    std::vector<std::string> trailers_;
    std::vector<std::string> trailer_values_;
    std::vector<bool> never_indexed_;
    std::vector<std::optional<ruvia::http_response_body_plan>> response_body_plans_;
    std::vector<std::optional<std::uint64_t>> content_lengths_;
    std::vector<std::optional<ruvia::http_client_request_content_signal>> request_content_signals_;
};
struct counting_resource final : std::pmr::memory_resource {
    std::size_t outstanding_{};
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++outstanding_;
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* p, std::size_t bytes_value, std::size_t alignment) override {
        --outstanding_;
        std::pmr::new_delete_resource()->deallocate(p, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
struct reentry_state final {
    ruvia::http3_client_response* response_;
};
void reenter(void* opaque, const ruvia::http3_client_response_event&) {
    auto& state_value = *static_cast<reentry_state*>(opaque);
    static_cast<void>(state_value.response_->feed({}, false, false, reenter, opaque));
}
void collect(void* p, const ruvia::http3_client_response_event& event) {
    auto& out = *static_cast<client_response_events*>(p);
    out.kinds_.push_back(event.kind_);
    out.request_content_signals_.push_back(event.request_content_signal_);
    if (!event.body_.empty()) {
        out.body_.append(event.body_.data(), event.body_.size());
    }
    out.response_body_plans_.emplace_back(event.response_body_plan_);
    out.content_lengths_.emplace_back(event.head_ == nullptr ? std::nullopt : event.head_->content_length_);
    if (event.kind_ == ruvia::http3_client_response_event_kind::trailer_field) {
        out.trailers_.emplace_back(event.trailer_.name_);
        out.trailer_values_.emplace_back(event.trailer_.value_);
        out.never_indexed_.push_back(event.trailer_.never_indexed_);
    }
}
std::vector<char> frame(std::uint64_t type, std::span<const char> payload_value) {
    std::vector<char> result_value(16 + payload_value.size());
    auto type_size = ruvia::encode_http3_var_int(result_value, type);
    auto length_size = ruvia::encode_http3_var_int(std::span<char>(result_value).subspan(std::get<0>(type_size)), payload_value.size());
    result_value.resize(std::get<0>(type_size) + std::get<0>(length_size));
    result_value.insert(result_value.end(), payload_value.begin(), payload_value.end());
    return result_value;
}
std::vector<char> field_section(std::span<const ruvia::http3_field_section_field_view> fields_value) {
    std::pmr::monotonic_buffer_resource resource;
    auto encoded = ruvia::encode_http3_field_section(fields_value, &resource);
    return {std::get<0>(encoded).begin(), std::get<0>(encoded).end()};
}
std::vector<char> response_head(std::uint16_t status, std::optional<std::uint64_t> content_length = {}) {
    const auto status_text = std::to_string(status);
    const auto length_text = content_length ? std::to_string(*content_length) : std::string{};
    const std::array<ruvia::http3_field_section_field_view, 2> fields_value{{{":status", status_text},
        {"content-length", length_text}}};
    const auto section = field_section(std::span(fields_value).first(content_length ? 2 : 1));
    return frame(1, section);
}
std::vector<char> response_data(std::string_view body) {
    return frame(0, body);
}
const ruvia::http_response_body_plan* response_plan(const client_response_events& events_value, std::size_t index) {
    return index < events_value.response_body_plans_.size() && events_value.response_body_plans_[index]
               ? &*events_value.response_body_plans_[index]
               : nullptr;
}
bool same_plan(const ruvia::http_response_body_plan* left, const ruvia::http_response_body_plan* right) {
    return left != nullptr && right != nullptr && left->request_method() == right->request_method() &&
           left->response_status() == right->response_status() && left->content_semantics() == right->content_semantics() &&
           left->status_allows_body() == right->status_allows_body() && left->body_suppressed() == right->body_suppressed();
}
}  // namespace

RUVIA_TEST(http3_client_response_handles_final_body_and_fragmentation) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::http3_client_response response(0, ruvia::http_known_method::get, &memory);
    client_response_events events;
    constexpr std::array<char, 10> wire{1, 3, 0, 0, static_cast<char>(0xd9),
        0, 3, 'a', 'b', 'c'};
    auto result_value = response.feed(std::span<const char>(wire).first(2), false, false, collect, &events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::need_more_data);
    result_value = response.feed(std::span<const char>(wire).subspan(2, 6), false, false, collect, &events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::need_more_data);
    result_value = response.feed(std::span<const char>(wire).subspan(8), true, false, collect, &events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(events.body_, std::string("abc"));
    RUVIA_CHECK(events.kinds_.size() >= 3);
    RUVIA_CHECK(events.kinds_.front() == ruvia::http3_client_response_event_kind::final_head);
    RUVIA_CHECK(events.kinds_.back() == ruvia::http3_client_response_event_kind::message_end);
    for (std::size_t i = 1; i + 1 < events.kinds_.size(); ++i) {
        RUVIA_CHECK(events.kinds_[i] == ruvia::http3_client_response_event_kind::body);
        RUVIA_CHECK(response_plan(events, i) == nullptr);
    }
    const auto* final_plan = response_plan(events, 0);
    const auto* end_plan = response_plan(events, events.kinds_.size() - 1);
    RUVIA_CHECK(final_plan != nullptr && final_plan->request_method() == ruvia::http_known_method::get);
    RUVIA_CHECK(final_plan != nullptr && final_plan->response_status() == ruvia::http_status::ok);
    RUVIA_CHECK(final_plan != nullptr && final_plan->content_semantics() == ruvia::http_response_content_semantics::with_content);
    RUVIA_CHECK(final_plan != nullptr && !final_plan->body_suppressed());
    RUVIA_CHECK(same_plan(final_plan, end_plan));
}

RUVIA_TEST(http3_client_response_rejects_data_before_headers_and_bad_stream_ids) {
    std::pmr::monotonic_buffer_resource memory;
    bool rejected = false;
    try {
        ruvia::http3_client_response invalid(1, ruvia::http_known_method::get, &memory);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    rejected = false;
    try {
        ruvia::http3_client_response invalid(ruvia::http3_var_int_max + 1, ruvia::http_known_method::get, &memory);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    ruvia::http3_client_response response(0, ruvia::http_known_method::get, &memory);
    client_response_events events;
    constexpr std::array<char, 2> data{0, 0};
    const auto result_value = response.feed(data, false, false, collect, &events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::connection_error);
    RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::frame_unexpected);
}

RUVIA_TEST(http3_client_response_rejects_unadvertised_push_promise_with_id_error) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_client_response response(0, ruvia::http_known_method::get, &resource);
    client_response_events events;
    constexpr std::array<ruvia::http3_field_section_field_view, 1> status{{{":status", "200"}}};
    const auto headers = frame(1, field_section(status));
    RUVIA_CHECK(response.feed(headers, false, false, collect, &events).status_ ==
                ruvia::http3_client_response_status::need_more_data);
    constexpr std::array<char, 1> push_id{0};
    const auto promise = frame(5, push_id);
    const auto result_value = response.feed(promise, false, false, collect, &events);
    RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::id_error);
    RUVIA_CHECK(events.kinds_.size() == 1);
}

RUVIA_TEST(http3_client_response_head_rejects_payload_but_accepts_empty_fin) {
    std::pmr::monotonic_buffer_resource memory;
    client_response_events events;
    constexpr std::array<char, 5> headers{1, 3, 0, 0, static_cast<char>(0xd9)};
    {
        ruvia::http3_client_response response(0, ruvia::http_known_method::head, &memory);
        auto result_value = response.feed(headers, false, false, collect, &events);
        RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::need_more_data);
        result_value = response.feed({}, true, false, collect, &events);
        RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::message_end);
    }
    {
        ruvia::http3_client_response response(4, ruvia::http_known_method::head, &memory);
        constexpr std::array<char, 8> payload_value{1, 3, 0, 0, static_cast<char>(0xd9), 0, 1, 'x'};
        const auto result_value = response.feed(payload_value, true, false, collect, &events);
        RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::stream_error);
        RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::message_error);
    }
}

RUVIA_TEST(http3_client_response_validates_all_trailers_before_callback) {
    std::pmr::monotonic_buffer_resource memory;
    const std::array<char, 3> head{0, 0, static_cast<char>(0xd9)};
    const std::array<ruvia::http3_field_section_field_view, 2> fields_value{{{"x-one", "1", false}, {"x-two", "2", false}}};
    auto encoded = field_section(fields_value);
    auto head_frame = frame(1, head);
    auto trailer_frame = frame(1, encoded);
    head_frame.insert(head_frame.end(), trailer_frame.begin(), trailer_frame.end());
    client_response_events events;
    ruvia::http3_client_response response(0, ruvia::http_known_method::get, &memory);
    auto result_value = response.feed(head_frame, false, false, collect, &events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::need_more_data);
    RUVIA_CHECK_EQ(events.trailers_.size(), std::size_t{2});
    RUVIA_CHECK_EQ(events.trailers_[0], std::string("x-one"));
    RUVIA_CHECK_EQ(events.trailers_[1], std::string("x-two"));

    ruvia::http3_client_response_limits limits{};
    limits.max_fields_ = 1;
    ruvia::http3_client_response limited(8, ruvia::http_known_method::get, &memory, limits);
    client_response_events limited_events;
    result_value = limited.feed(head_frame, false, false, collect, &limited_events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::connection_error);
    RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::excessive_load);
    RUVIA_CHECK(limited_events.trailers_.empty());

    constexpr std::array<std::string_view, 6> invalid_names{
        "host", "X-Test", "x bad", ":status", std::string_view("x\0bad", 5), "\x80-name"};
    for (const auto name : invalid_names) {
        const std::array<ruvia::http3_field_section_field_view, 2> invalid_fields{{{"x-good", "ok", false}, {name, "bad", false}}};
        const auto invalid_encoded = field_section(invalid_fields);
        auto invalid_frame = frame(1, head);
        const auto invalid_trailer_frame = frame(1, invalid_encoded);
        invalid_frame.insert(invalid_frame.end(), invalid_trailer_frame.begin(), invalid_trailer_frame.end());
        client_response_events rejected_events;
        ruvia::http3_client_response rejected(4, ruvia::http_known_method::get, &memory);
        result_value = rejected.feed(invalid_frame, false, false, collect, &rejected_events);
        RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::stream_error);
        RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::message_error);
        RUVIA_CHECK(rejected_events.trailers_.empty());
    }
}

RUVIA_TEST(http3_client_response_trailer_storage_is_atomic_and_reclaimed_on_allocation_failure) {
    const std::string name = "x-" + std::string(80, 'n');
    const std::string value(100, 'v');
    const std::array fields_value{ruvia::http3_field_section_field_view{name, value, true},
        ruvia::http3_field_section_field_view{"x-final", "done", false}};
    const auto trailer_wire = frame(1, field_section(fields_value));
    const auto head_wire = response_head(200);
    bool succeeded = false;
    std::size_t failures = 0;
    for (std::size_t allowance = 0; allowance != 64 && !succeeded; ++allowance) {
        failing_memory_resource memory;
        {
            ruvia::http3_client_response response(0, ruvia::http_known_method::get, &memory);
            client_response_events events;
            RUVIA_CHECK(response.feed(head_wire, false, false, collect, &events).status_ ==
                        ruvia::http3_client_response_status::need_more_data);
            memory.fail_after(allowance);
            try {
                const auto result_value = response.feed(trailer_wire, true, false, collect, &events);
                RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::message_end);
                succeeded = true;
                RUVIA_CHECK_EQ(events.trailers_.size(), std::size_t{2});
                if (events.trailers_.size() == 2) {
                    RUVIA_CHECK_EQ(events.trailers_.front(), name);
                    RUVIA_CHECK_EQ(events.trailer_values_.front(), value);
                    RUVIA_CHECK(events.never_indexed_.front());
                    RUVIA_CHECK(!events.never_indexed_.back());
                }
            } catch (const std::bad_alloc&) {
                ++failures;
                RUVIA_CHECK(events.trailers_.empty());
            }
            memory.allow_allocations();
        }
        RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK(succeeded);
    RUVIA_CHECK(failures >= 2);
}

RUVIA_TEST(http3_client_response_rejects_forbidden_response_trailer_fields) {
    std::pmr::monotonic_buffer_resource memory;
    const std::array<char, 3> head{0, 0, static_cast<char>(0xd9)};
    for (const auto field : {ruvia::http3_field_section_field_view{"set-cookie", "sid=secret"},
             ruvia::http3_field_section_field_view{"content-encoding", "gzip"}}) {
        const std::array fields_value{field};
        const auto trailer = field_section(fields_value);
        auto wire = frame(1, head);
        const auto trailing_frame = frame(1, trailer);
        wire.insert(wire.end(), trailing_frame.begin(), trailing_frame.end());
        client_response_events events;
        ruvia::http3_client_response response(0, ruvia::http_known_method::get, &memory);
        const auto result_value = response.feed(wire, true, false, collect, &events);
        RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::stream_error);
        RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::message_error);
        RUVIA_CHECK(events.trailers_.empty());
    }
    const std::array valid_fields{ruvia::http3_field_section_field_view{"etag", "\"tag\""}};
    const auto trailer = field_section(valid_fields);
    auto wire = frame(1, head);
    const auto trailing_frame = frame(1, trailer);
    wire.insert(wire.end(), trailing_frame.begin(), trailing_frame.end());
    client_response_events events;
    ruvia::http3_client_response valid(0, ruvia::http_known_method::get, &memory);
    const auto result_value = valid.feed(wire, true, false, collect, &events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(events.trailers_.size(), std::size_t{1});
}

RUVIA_TEST(http3_client_response_preserves_final_plan_after_informational_response) {
    std::pmr::monotonic_buffer_resource memory;
    constexpr std::array<char, 3> informational_section{0, 0, static_cast<char>(0xd8)};
    auto wire = frame(1, informational_section);
    auto final_head = response_head(200, 3);
    auto data = response_data("abc");
    wire.insert(wire.end(), final_head.begin(), final_head.end());
    wire.insert(wire.end(), data.begin(), data.end());

    client_response_events events;
    ruvia::http3_client_response response(0, ruvia::http_known_method::get, &memory);
    const auto result_value = response.feed(wire, true, false, collect, &events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(events.body_, std::string("abc"));
    RUVIA_CHECK_EQ(events.kinds_.size(), std::size_t{4});
    RUVIA_CHECK(events.kinds_[0] == ruvia::http3_client_response_event_kind::informational_head);
    RUVIA_CHECK(events.kinds_[1] == ruvia::http3_client_response_event_kind::final_head);
    RUVIA_CHECK(events.kinds_[2] == ruvia::http3_client_response_event_kind::body);
    RUVIA_CHECK(events.kinds_[3] == ruvia::http3_client_response_event_kind::message_end);
    RUVIA_CHECK(response_plan(events, 0) == nullptr);
    RUVIA_CHECK(response_plan(events, 2) == nullptr);
    const auto* final_plan = response_plan(events, 1);
    RUVIA_CHECK(final_plan != nullptr && final_plan->response_status() == ruvia::http_status::ok);
    RUVIA_CHECK(final_plan != nullptr && final_plan->content_semantics() == ruvia::http_response_content_semantics::with_content);
    RUVIA_CHECK(same_plan(final_plan, response_plan(events, 3)));
}

RUVIA_TEST(http3_client_response_preserves_body_suppression_and_head_representation_length) {
    std::pmr::monotonic_buffer_resource memory;
    struct case_value final {
        std::uint64_t stream_id_;
        ruvia::http_known_method method_;
        std::uint16_t status_;
        std::optional<std::uint64_t> content_length_;
    };
    constexpr std::array cases{
        case_value{0, ruvia::http_known_method::head, 200, 123},
        case_value{4, ruvia::http_known_method::get, 204, std::nullopt},
        case_value{8, ruvia::http_known_method::get, 304, 123},
    };
    for (const auto& test : cases) {
        auto wire = response_head(test.status_, test.content_length_);
        ruvia::http3_client_response response(test.stream_id_, test.method_, &memory);
        client_response_events events;
        const auto result_value = response.feed(wire, true, false, collect, &events);
        RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::message_end);
        RUVIA_CHECK(events.body_.empty());
        RUVIA_CHECK_EQ(events.kinds_.size(), std::size_t{2});
        RUVIA_CHECK(events.kinds_[0] == ruvia::http3_client_response_event_kind::final_head);
        RUVIA_CHECK(events.kinds_[1] == ruvia::http3_client_response_event_kind::message_end);
        RUVIA_CHECK(events.content_lengths_[0] == test.content_length_);
        const auto* final_plan = response_plan(events, 0);
        RUVIA_CHECK(final_plan != nullptr && final_plan->request_method() == test.method_);
        RUVIA_CHECK(final_plan != nullptr && final_plan->response_status().value() == test.status_);
        RUVIA_CHECK(final_plan != nullptr && final_plan->content_semantics() == ruvia::http_response_content_semantics::without_content);
        RUVIA_CHECK(final_plan != nullptr && final_plan->body_suppressed());
        RUVIA_CHECK(same_plan(final_plan, response_plan(events, 1)));
    }
}

RUVIA_TEST(http3_client_response_accepts_get_with_legal_empty_body_and_fin) {
    std::pmr::monotonic_buffer_resource memory;
    auto wire = response_head(200, 0);
    const auto data = response_data({});
    wire.insert(wire.end(), data.begin(), data.end());
    ruvia::http3_client_response response(12, ruvia::http_known_method::get, &memory);
    client_response_events events;
    const auto result_value = response.feed(wire, true, false, collect, &events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK(events.body_.empty());
    RUVIA_CHECK_EQ(events.kinds_.size(), std::size_t{2});
    RUVIA_CHECK(events.kinds_[0] == ruvia::http3_client_response_event_kind::final_head);
    RUVIA_CHECK(events.kinds_[1] == ruvia::http3_client_response_event_kind::message_end);
    const auto* final_plan = response_plan(events, 0);
    RUVIA_CHECK(final_plan != nullptr && final_plan->content_semantics() == ruvia::http_response_content_semantics::with_content);
    RUVIA_CHECK(final_plan != nullptr && !final_plan->body_suppressed());
    RUVIA_CHECK(same_plan(final_plan, response_plan(events, 1)));
}

RUVIA_TEST(http3_client_response_rejects_101_and_qpack_trailer_atomically) {
    std::pmr::monotonic_buffer_resource memory;
    const std::array<ruvia::http3_field_section_field_view, 1> switching{{{":status", "101", false}}};
    auto section = field_section(switching);
    auto wire = frame(1, section);
    client_response_events events;
    ruvia::http3_client_response response(0, ruvia::http_known_method::get, &memory);
    auto result_value = response.feed(wire, false, false, collect, &events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::stream_error);

    constexpr std::array<char, 3> head{0, 0, static_cast<char>(0xd9)};
    auto combined = frame(1, head);
    constexpr std::array<char, 3> broken_qpack{0, 0, static_cast<char>(0xff)};
    auto bad_trailer = frame(1, broken_qpack);
    combined.insert(combined.end(), bad_trailer.begin(), bad_trailer.end());
    ruvia::http3_client_response with_bad_trailer(4, ruvia::http_known_method::get, &memory);
    client_response_events trailer_events;
    result_value = with_bad_trailer.feed(combined, false, false, collect, &trailer_events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::connection_error);
    RUVIA_CHECK(result_value.code_ == ruvia::http3_connection_error_code::qpack_decompression_failed);
    RUVIA_CHECK(trailer_events.trailers_.empty());
}

RUVIA_TEST(http3_client_response_connect_success_delivers_tunnel_data) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::http3_client_response response(0, ruvia::http_known_method::connect, &memory);
    client_response_events events;
    // Feed successful CONNECT headers and tunnel bytes over separate frames.
    const auto headers = response_head(200);
    auto result_value = response.feed(headers, false, false, collect, &events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::need_more_data);
    constexpr std::array<char, 5> data{0, 3, 'a', 'b', 'c'};
    result_value = response.feed(data, true, false, collect, &events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(events.body_, std::string("abc"));
    RUVIA_CHECK_EQ(events.kinds_.size(), std::size_t{3});
    RUVIA_CHECK(events.kinds_[0] == ruvia::http3_client_response_event_kind::final_head);
    RUVIA_CHECK(events.kinds_[1] == ruvia::http3_client_response_event_kind::tunnel_data);
    RUVIA_CHECK(events.kinds_[2] == ruvia::http3_client_response_event_kind::message_end);
    const auto* final_plan = response_plan(events, 0);
    RUVIA_CHECK(final_plan != nullptr && final_plan->content_semantics() == ruvia::http_response_content_semantics::connect_tunnel);
    RUVIA_CHECK(final_plan != nullptr && final_plan->body_suppressed());
    RUVIA_CHECK(response_plan(events, 1) == nullptr);
    RUVIA_CHECK(same_plan(final_plan, response_plan(events, 2)));
}

RUVIA_TEST(http3_client_response_callback_reentrancy_is_terminal) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::http3_client_response response(0, ruvia::http_known_method::get, &memory);
    reentry_state state_value{&response};
    constexpr std::array<char, 5> headers{1, 3, 0, 0, static_cast<char>(0xd9)};
    bool threw = false;
    try {
        static_cast<void>(response.feed(headers, false, false, reenter, &state_value));
    } catch (const std::logic_error&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    const auto result_value = response.feed({}, false, false, collect, nullptr);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::stream_error);
}

RUVIA_TEST(http3_client_response_pmr_lifetime_covers_terminal_and_unstarted_paths) {
    counting_resource memory;
    constexpr std::array<char, 10> wire{1, 3, 0, 0, static_cast<char>(0xd9), 0, 3, 'a', 'b', 'c'};
    std::string retained;
    for (std::uint64_t id = 0; id < 16; id += 4) {
        ruvia::http3_client_response response(id, ruvia::http_known_method::get, &memory);
        client_response_events events;
        const auto result_value = response.feed(wire, true, false, collect, &events);
        RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::message_end);
        retained = events.body_;
    }
    {
        ruvia::http3_client_response reset(0, ruvia::http_known_method::get, &memory);
        client_response_events events;
        static_cast<void>(reset.feed({}, false, true, collect, &events));
    }
    {
        ruvia::http3_client_response failed(0, ruvia::http_known_method::get, &memory);
        constexpr std::array<char, 2> data_first{0, 0};
        client_response_events events;
        static_cast<void>(failed.feed(data_first, false, false, collect, &events));
    }
    {
        ruvia::http3_client_response never_started(0, ruvia::http_known_method::get, &memory);
    }
    RUVIA_CHECK_EQ(retained, std::string("abc"));
    RUVIA_CHECK_EQ(memory.outstanding_, std::size_t{0});
}

RUVIA_TEST(http3_client_response_reset_is_terminal) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::http3_client_response response(0, ruvia::http_known_method::head, &memory);
    client_response_events events;
    const auto result_value = response.feed({}, false, true, collect, &events);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_client_response_status::reset);
    RUVIA_CHECK_EQ(events.kinds_.size(), std::size_t{1});
}

RUVIA_TEST(http3_client_response_signals_continue_only_for100_and_stops_content_at_final_head) {
    std::pmr::monotonic_buffer_resource resource;
    client_response_events events;
    ruvia::http3_client_response response(0, ruvia::http_known_method::post, &resource);
    const std::array<ruvia::http3_field_section_field_view, 1> early{{{":status", "103"}}};
    const std::array<ruvia::http3_field_section_field_view, 1> continued{{{":status", "100"}}};
    const std::array<ruvia::http3_field_section_field_view, 2> final{{{":status", "204"}, {"x-final", "retained"}}};
    auto make_head = [&](auto fields_value) {
        const auto encoded = ruvia::encode_http3_field_section(fields_value, &resource);
        std::string wire;
        std::array<char, 16> prefix{};
        auto type = ruvia::encode_http3_var_int(prefix, 1);
        wire.append(prefix.data(), std::get<0>(type));
        auto length = ruvia::encode_http3_var_int(prefix, std::get<0>(encoded).size());
        wire.append(prefix.data(), std::get<0>(length));
        wire.append(std::get<0>(encoded).data(), std::get<0>(encoded).size());
        return wire;
    };
    RUVIA_CHECK(response.feed(make_head(early), false, false, collect, &events).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(response.feed(make_head(continued), false, false, collect, &events).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(response.feed(make_head(final), true, false, collect, &events).status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(events.request_content_signals_.size(), std::size_t{4});
    if (events.request_content_signals_.size() == 4) {
        RUVIA_CHECK(!events.request_content_signals_[0]);
        RUVIA_CHECK(events.request_content_signals_[1] == ruvia::http_client_request_content_signal::continue_value);
        RUVIA_CHECK(events.request_content_signals_[2] == ruvia::http_client_request_content_signal::exchange_complete);
        RUVIA_CHECK(!events.request_content_signals_[3]);
    }
}
