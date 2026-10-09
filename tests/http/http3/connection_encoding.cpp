#include <algorithm>
#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <variant>
#include <vector>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_settings.h"
#include "ruvia/http/http3_var_int.h"
#include "ruvia/http/http_response_stream.h"

#include "test_harness.h"

namespace {
void ignore_event(void*, const ruvia::http3_connection_event&) {}

std::vector<char> settings_wire(ruvia::http3_settings settings) {
    std::array<char, 128> bytes_value{};
    const auto payload_value = ruvia::encode_http3_settings(std::span(bytes_value).subspan(16), settings);
    const auto frame = ruvia::encode_http3_frame_header(bytes_value, 4, std::get<0>(payload_value));
    std::vector<char> wire{0};
    wire.insert(wire.end(), bytes_value.begin(), bytes_value.begin() + std::get<0>(frame));
    wire.insert(wire.end(), bytes_value.begin() + 16, bytes_value.begin() + 16 + std::get<0>(payload_value));
    return wire;
}

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t live_bytes_{};
    std::size_t allocations_{};
    std::size_t returns_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        auto* value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        live_bytes_ += bytes_value;
        ++allocations_;
        return value;
    }
    void do_deallocate(void* value, std::size_t bytes_value, std::size_t alignment) override {
        live_bytes_ -= bytes_value;
        ++returns_;
        std::pmr::new_delete_resource()->deallocate(value, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

enum class response_kind { buffered,
    streaming,
    interim,
    trailers,
    connect };

std::variant<std::size_t, ruvia::http3_response_head_failure> encode_response(
    ruvia::http3_connection& connection, response_kind kind, ruvia::http3_field_section_limits limits = {}) {
    if (kind == response_kind::trailers) {
        const std::array fields_value{ruvia::http3_field_section_field_view{"x-repeated", "value"}};
        auto result_value = connection.encode_response_trailers(0, fields_value, limits);
        if ((result_value.index() != 0)) {
            return std::get<1>(result_value);
        }
        return std::get<0>(result_value).decoded_field_section_size();
    }
    if (kind == response_kind::interim) {
        const std::array fields_value{ruvia::http_header_view{"x-repeated", "value"}};
        auto result_value = connection.encode_interim_response_head(0, ruvia::http_interim_response_head(ruvia::http_status::early_hints, fields_value), limits);
        if ((result_value.index() != 0)) {
            return std::get<1>(result_value);
        }
        return std::get<0>(result_value).field_section_.decoded_field_section_size();
    }
    ruvia::http_response response;
    response.header("date", "Thu, 01 Jan 1970 00:00:00 GMT");
    response.header("x-repeated", "value");
    if (kind == response_kind::streaming) {
        auto result_value = connection.encode_streaming_response_head(0, std::move(response), ruvia::http_known_method::get,
            ruvia::http_response_stream_kind::generic, ruvia::http_response_trailer_intent::none, limits);
        if ((result_value.index() != 0)) {
            return std::get<1>(result_value);
        }
        return std::get<0>(result_value).head_.field_section_.decoded_field_section_size();
    }
    auto result_value = kind == response_kind::connect
                            ? connection.encode_connect_response_head(0, response, limits)
                            : connection.encode_response_head(0, response, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response), limits);
    if ((result_value.index() != 0)) {
        return std::get<1>(result_value);
    }
    return std::get<0>(result_value).field_section_.decoded_field_section_size();
}
}  // namespace

RUVIA_TEST(http3_connection_response_encoding_applies_peer_decoded_limit_before_qpack) {
    for (const auto kind : {response_kind::buffered, response_kind::streaming, response_kind::interim, response_kind::trailers, response_kind::connect}) {
        ruvia::http3_connection baseline(ruvia::http3_peer_role::server, std::pmr::get_default_resource());
        const auto size = encode_response(baseline, kind);
        RUVIA_CHECK((size.index() == 0));
        if ((size.index() != 0)) {
            continue;
        }
        for (const auto peer_limit : {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0},
                 std::optional<std::uint64_t>{std::get<0>(size) - 1}, std::optional<std::uint64_t>{std::get<0>(size)},
                 std::optional<std::uint64_t>{std::get<0>(size) + 1}, std::optional<std::uint64_t>{ruvia::http3_var_int_max}}) {
            ruvia::http3_connection connection(ruvia::http3_peer_role::server, std::pmr::get_default_resource());
            const auto settings = settings_wire({.qpack_max_table_capacity_ = 512, .max_field_section_size_ = peer_limit, .qpack_blocked_streams_ = 2});
            RUVIA_CHECK(connection.feed(2, settings, false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
            const auto pending = connection.pending_qpack_encoder_output();
            const std::vector<char> before(pending.begin(), pending.end());
            const auto result_value = encode_response(connection, kind);
            const bool allowed = !peer_limit || *peer_limit >= std::get<0>(size);
            RUVIA_CHECK_EQ((result_value.index() == 0), allowed);
            if (!allowed && (result_value.index() != 0)) {
                RUVIA_CHECK(std::get<1>(result_value).kind_ == ruvia::http3_response_head_error::peer_field_section_limit);
                RUVIA_CHECK(std::get<1>(result_value).field_section_error_ == ruvia::http3_field_section_error::field_list_too_large);
                const auto after = connection.pending_qpack_encoder_output();
                RUVIA_CHECK(std::vector<char>(after.begin(), after.end()) == before);
                // A refused response must not poison the shared encoder.
                RUVIA_CHECK((connection.encode_field_section(4, {}).index() == 0));
            }
            if (allowed) {
                RUVIA_CHECK_EQ(std::get<0>(result_value), std::get<0>(size));
                ruvia::http3_field_section_limits local;
                local.max_decoded_bytes_ = std::get<0>(size) - 1;
                const auto refused = encode_response(connection, kind, local);
                RUVIA_CHECK((refused.index() != 0));
                if ((refused.index() != 0)) {
                    RUVIA_CHECK(std::get<1>(refused).kind_ == ruvia::http3_response_head_error::field_section_error);
                    RUVIA_CHECK(std::get<1>(refused).field_section_error_ == ruvia::http3_field_section_error::field_list_too_large);
                }
            }
        }
    }
}

RUVIA_TEST(http3_connection_response_encoding_applies_normalized_local_budgets) {
    const auto encode = [&ruvia_ctx](ruvia::http3_connection_config config, std::uint64_t peer_limit,
                            ruvia::http3_field_section_limits requested) {
        ruvia::http3_connection connection(ruvia::http3_peer_role::server, std::pmr::get_default_resource(), config);
        // Keep dynamic compression disabled so the encoded-byte budget test
        // measures the emitted literal, rather than a valid compact reference.
        const auto settings = settings_wire({.max_field_section_size_ = peer_limit});
        RUVIA_CHECK(connection.feed(2, settings, false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
        ruvia::http_response response;
        response.header("date", "Thu, 01 Jan 1970 00:00:00 GMT");
        response.header("x-pad", std::string(200, 'x'));
        return connection.encode_response_head(0, response,
            ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response), requested);
    };
    constexpr ruvia::http3_field_section_limits requested{65536, 65536, 256};
    const auto baseline = encode({}, 512, requested);
    RUVIA_CHECK((baseline.index() == 0));
    if ((baseline.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(baseline).field_section_.decoded_field_section_size(), std::size_t{391});
    }
    const ruvia::http3_connection_config decoded_limit{.max_field_section_size_ = 256};
    for (const auto peer_limit : {512, 384}) {
        const auto result_value = encode(decoded_limit, peer_limit, requested);
        RUVIA_CHECK((result_value.index() != 0));
        if ((result_value.index() != 0)) {
            RUVIA_CHECK(std::get<1>(result_value).kind_ == ruvia::http3_response_head_error::field_section_error);
            RUVIA_CHECK(std::get<1>(result_value).field_section_error_ == ruvia::http3_field_section_error::field_list_too_large);
        }
    }
    const auto peer_limited = encode(decoded_limit, 128, requested);
    RUVIA_CHECK((peer_limited.index() != 0));
    if ((peer_limited.index() != 0)) {
        RUVIA_CHECK(std::get<1>(peer_limited).kind_ == ruvia::http3_response_head_error::peer_field_section_limit);
        RUVIA_CHECK(std::get<1>(peer_limited).field_section_error_ == ruvia::http3_field_section_error::field_list_too_large);
    }

    const ruvia::http3_connection_config fields_limit{.max_fields_ = 1};
    const auto too_many_fields = encode(fields_limit, 65536, requested);
    RUVIA_CHECK((too_many_fields.index() != 0));
    if ((too_many_fields.index() != 0)) {
        RUVIA_CHECK(std::get<1>(too_many_fields).kind_ == ruvia::http3_response_head_error::field_section_error);
        RUVIA_CHECK(std::get<1>(too_many_fields).field_section_error_ == ruvia::http3_field_section_error::too_many_fields);
    }
    const ruvia::http3_connection_config encoded_limit{.max_encoded_field_section_bytes_ = 100};
    const auto too_many_encoded_bytes = encode(encoded_limit, 65536, requested);
    RUVIA_CHECK((too_many_encoded_bytes.index() != 0));
    if ((too_many_encoded_bytes.index() != 0)) {
        RUVIA_CHECK(std::get<1>(too_many_encoded_bytes).kind_ == ruvia::http3_response_head_error::field_section_error);
        RUVIA_CHECK(std::get<1>(too_many_encoded_bytes).field_section_error_ == ruvia::http3_field_section_error::field_section_too_large);
    }
}

RUVIA_TEST(http3_connection_client_request_and_push_promise_share_peer_limit) {
    // :method GET (42), :scheme https (44), :authority example.test (54),
    // and :path / (38), including the RFC 9114 per-field overhead.
    constexpr std::uint64_t decoded_size = 178;
    for (const auto peer_limit : {decoded_size - 1, decoded_size, decoded_size + 1}) {
        ruvia::http3_connection client(ruvia::http3_peer_role::client, std::pmr::get_default_resource());
        auto settings = settings_wire({.qpack_max_table_capacity_ = 512, .max_field_section_size_ = peer_limit, .qpack_blocked_streams_ = 2});
        RUVIA_CHECK(client.feed(3, settings, false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
        const auto output = client.pending_qpack_encoder_output();
        const std::vector<char> before(output.begin(), output.end());
        const auto request = client.encode_client_request_head(0,
            {.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .path_ = "/"});
        RUVIA_CHECK_EQ((request.index() == 0), peer_limit >= decoded_size);
        if ((request.index() != 0)) {
            const auto after = client.pending_qpack_encoder_output();
            RUVIA_CHECK(std::vector<char>(after.begin(), after.end()) == before);
        }

        ruvia::http3_connection server(ruvia::http3_peer_role::server, std::pmr::get_default_resource());
        // Authorize Push ID 0 on the same client control stream as SETTINGS.
        settings.push_back(static_cast<char>(0xd));
        settings.push_back(1);
        settings.push_back(0);
        RUVIA_CHECK(server.feed(2, settings, false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
        const auto promise = server.prepare_push_promise(0, 0, {.authority_ = "example.test"});
        RUVIA_CHECK_EQ((promise.index() == 0), peer_limit >= decoded_size);
        RUVIA_CHECK_EQ(server.promised_request(0) != nullptr, peer_limit >= decoded_size);
    }
}

RUVIA_TEST(http3_reset_before_headers_releases_unpublished_dynamic_references) {
    for (const bool goaway : {false, true}) {
        counting_resource memory;
        {
            ruvia::http3_connection client(ruvia::http3_peer_role::client, &memory);
            ruvia::http3_connection server(ruvia::http3_peer_role::server, &memory,
                {.qpack_max_table_capacity_ = 64, .qpack_blocked_streams_ = 1});
            if (goaway) {
                RUVIA_CHECK((server.prepare_goaway(0).index() == 0));
            }
            const auto settings = settings_wire({.qpack_max_table_capacity_ = 64, .qpack_blocked_streams_ = 1});
            RUVIA_CHECK(client.feed(3, settings, false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
            std::size_t warmed_live_bytes{};
            std::optional<std::pmr::vector<char>> retained;
            std::vector<char> retained_bytes;
            for (unsigned round = 0; round != 128; ++round) {
                const auto stream_id = 4 * round;
                {
                    const std::array fields_value{ruvia::http3_field_section_field_view{round % 2 == 0 ? "x-first" : "x-other", "value"}};
                    auto unsent = client.encode_field_section(stream_id, fields_value);
                    RUVIA_CHECK((unsent.index() == 0));
                    const auto instructions = client.pending_qpack_encoder_output();
                    // This table holds one field. Every new insertion requires
                    // release of the preceding stream's dynamic reference pin.
                    RUVIA_CHECK(!instructions.empty());
                    std::vector<char> encoder_wire;
                    if (round == 0) {
                        encoder_wire.push_back(2);
                    }
                    encoder_wire.insert(encoder_wire.end(), instructions.begin(), instructions.end());
                    RUVIA_CHECK(server.feed(6, encoder_wire, false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
                    RUVIA_CHECK(client.consume_qpack_encoder_output(instructions.size()));
                    RUVIA_CHECK_EQ(server.active_request_count(), std::size_t{0});

                    // Reset arrives without any request HEADERS/parser state.
                    RUVIA_CHECK(server.feed(stream_id, {}, false, true, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
                    const auto cancellation = server.pending_qpack_decoder_output();
                    std::vector<char> decoder_wire;
                    if (round == 0) {
                        decoder_wire.push_back(3);
                    }
                    decoder_wire.insert(decoder_wire.end(), cancellation.begin(), cancellation.end());
                    RUVIA_CHECK(client.feed(11, decoder_wire, false, false, ignore_event, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
                    RUVIA_CHECK(server.consume_qpack_decoder_output(cancellation.size()));
                    RUVIA_CHECK_EQ(server.active_request_count(), std::size_t{0});
                    if (round == 0) {
                        retained.emplace(std::move(std::get<0>(unsent)));
                        retained_bytes.assign(retained->begin(), retained->end());
                    }
                    RUVIA_CHECK(std::ranges::equal(*retained, retained_bytes));
                }
                if (round < 64) {
                    // Observe the bounded deque/buffer high-water mark rather than
                    // mistaking cached blocks or longer QPACK integers for a leak.
                    warmed_live_bytes = std::max(warmed_live_bytes, memory.live_bytes_);
                } else {
                    RUVIA_CHECK(memory.live_bytes_ <= warmed_live_bytes);
                }
            }
            if (goaway) {
                const auto rejected = server.feed(512, {}, false, false, ignore_event, nullptr);
                RUVIA_CHECK(rejected.status_ == ruvia::http3_connection_status::stream_error);
                RUVIA_CHECK(rejected.code_ == ruvia::http3_connection_error_code::request_rejected);
            }
        }
        RUVIA_CHECK_EQ(memory.live_bytes_, std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocations_, memory.returns_);
    }
}

RUVIA_TEST(http3_connection_request_and_generic_encoding_apply_peer_decoded_limit) {
    for (const auto role : {ruvia::http3_peer_role::client, ruvia::http3_peer_role::server}) {
        ruvia::http3_connection connection(role, std::pmr::get_default_resource());
        const auto settings = settings_wire({.qpack_max_table_capacity_ = 512, .max_field_section_size_ = 39, .qpack_blocked_streams_ = 2});
        RUVIA_CHECK(connection.feed(role == ruvia::http3_peer_role::client ? 3 : 2,
                                  settings, false, false, ignore_event, nullptr)
                        .scope_ == ruvia::http3_connection_error_scope::none);
        const std::array fields_value{ruvia::http3_field_section_field_view{":status", "200"}};
        RUVIA_CHECK((connection.encode_field_section(0, fields_value).index() != 0));  // 32 + 7 + 3 = 42
        const std::array fitting{ruvia::http3_field_section_field_view{"x", "value"}};
        RUVIA_CHECK((connection.encode_field_section(4, fitting).index() == 0));  // 38 decoded bytes
        if (role == ruvia::http3_peer_role::client) {
            RUVIA_CHECK((connection.encode_client_request_head(8, {.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .path_ = "/"}).index() != 0));
        }
    }
}
