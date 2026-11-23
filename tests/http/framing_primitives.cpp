#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "byte_view.h"
#include "http1_chunked_body.h"
#include "http1_server_response.h"
#include "http_connection_advertisement.h"
#include "http_io_buffer.h"
#include "http_priority.h"
#include "http_response_stream.h"
#include "http_server_sent_events.h"
#include "http_structured_field.h"
#include "outbound_bytes.h"
#include "quic_varint.h"
#include "websocket_frame.h"
#include "websocket_handshake.h"

#if defined(RUVIA_ENABLE_HTTP2)
#include "http2_connection.h"
#include "http2_frame.h"
#include "http2_hpack.h"
#endif

#if defined(RUVIA_ENABLE_HTTP3)
#include "http3_connection.h"
#include "http3_quic_connection.h"
#endif

TEST(RuviaHttpPrimitives, quic_varint_roundtrips_all_width_boundaries) {
    constexpr std::array<std::pair<std::uint64_t, std::size_t>, 8> cases{{
        {0ULL, 1},
        {63ULL, 1},
        {64ULL, 2},
        {16383ULL, 2},
        {16384ULL, 4},
        {(1ULL << 30) - 1, 4},
        {1ULL << 30, 8},
        {ruvia::quic_varint_max, 8},
    }};
    for (const auto& [val, expected_len] : cases) {
        EXPECT_EQ(ruvia::quic_varint_size(val), expected_len);
        std::array<std::byte, 8> buf{};
        for (std::size_t short_len = 0; short_len < expected_len; ++short_len) {
            const auto too_small = ruvia::encode_quic_varint(std::span<std::byte>(buf).first(short_len), val);
            ASSERT_TRUE(std::holds_alternative<ruvia::quic_varint_error>(too_small));
            EXPECT_EQ(std::get<ruvia::quic_varint_error>(too_small), ruvia::quic_varint_error::output_too_small);
        }
        const auto encoded = ruvia::encode_quic_varint(buf, val);
        ASSERT_TRUE(std::holds_alternative<std::size_t>(encoded));
        EXPECT_EQ(std::get<std::size_t>(encoded), expected_len);
        for (std::size_t trunc = 0; trunc < expected_len; ++trunc) {
            EXPECT_FALSE(ruvia::decode_quic_varint(std::span<const std::byte>(buf).first(trunc)).has_value());
        }
        const auto decoded = ruvia::decode_quic_varint(std::span<const std::byte>(buf).first(expected_len));
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded->value_, val);
        EXPECT_EQ(decoded->encoded_bytes_, expected_len);
    }
    std::array<std::byte, 8> buf{};
    const auto overflow = ruvia::encode_quic_varint(buf, ruvia::quic_varint_max + 1);
    ASSERT_TRUE(std::holds_alternative<ruvia::quic_varint_error>(overflow));
    EXPECT_EQ(std::get<ruvia::quic_varint_error>(overflow), ruvia::quic_varint_error::value_out_of_range);
}

TEST(RuviaHttpPrimitives, websocket_frame_codec_and_accept_key) {
    const auto accept = ruvia::compute_websocket_accept("dGhlIHNhbXBsZSBub25jZQ==");
    EXPECT_EQ(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    std::array<std::byte, ruvia::websocket_max_frame_header_bytes> header{};
    const std::string payload_text = "hello-masked-websocket-frame-payload-exceeding-16-bytes!";
    const std::array<std::byte, 4> mask_key{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
    const auto header_len = ruvia::encode_websocket_frame_header(header, static_cast<std::uint8_t>(ruvia::websocket_frame_kind::text), true, payload_text.size(), false, true);
    ASSERT_EQ(header_len, std::size_t{2});

    std::vector<std::byte> masked_payload(ruvia::as_bytes(payload_text).begin(), ruvia::as_bytes(payload_text).end());
    ruvia::mask_websocket_payload(masked_payload, mask_key);

    ruvia::http_io_buffer wire;
    wire.append(ruvia::byte_view(header).first(header_len));
    wire.append(ruvia::byte_view(mask_key));
    wire.append(ruvia::byte_view(masked_payload));

    std::size_t pending_consume = 0;
    const auto parsed = ruvia::websocket_try_read_frame(wire, pending_consume, ruvia::protocol_byte_limit::limited(1024), false, true);
    ASSERT_NE(parsed.frame(), nullptr);
    EXPECT_EQ(parsed.frame()->kind(), ruvia::websocket_frame_kind::text);
    EXPECT_TRUE(parsed.frame()->final());
    EXPECT_FALSE(parsed.frame()->compressed());
    EXPECT_EQ(parsed.frame()->payload(), payload_text);
    ruvia::consume_websocket_read_frame(wire, pending_consume);
    EXPECT_TRUE(wire.empty());

    const auto close_ok = ruvia::encode_websocket_close_payload(ruvia::websocket_close_code{1000}, "normal");
    ASSERT_NE(close_ok.encoded(), nullptr);
    EXPECT_FALSE(ruvia::websocket_close_payload_failure(close_ok.encoded()->bytes()).has_value());
}

TEST(RuviaHttpPrimitives, http1_chunk_sse_and_trailer_helpers) {
    std::string wire;
    ruvia::append_http1_chunk(wire, std::string_view("hello "));
    ruvia::append_http1_chunk(wire, ruvia::as_bytes("world"));
    ruvia::append_http1_chunk(wire, std::string_view{});  // Empty chunk is a no-op!
    const std::array<ruvia::http_header_view, 1> raw_trailers{{{"x-checksum", "abc123"}}};
    const auto validated_trailers = ruvia::validate_http_response_trailers(raw_trailers);
    ruvia::append_http1_final_chunk(wire, validated_trailers);

    ruvia::http1_chunked_body_decoder decoder({.trailer_message_ = ruvia::http1_trailer_message::response});
    std::string body;
    std::string_view cursor(wire);
    bool done = false;
    while (!done) {
        const auto step = decoder.decode(cursor);
        cursor.remove_prefix(step.consumed_bytes());
        if (const auto* chunk = step.get_if<ruvia::decode_output>()) {
            body.append(chunk->bytes());
        } else if (step.holds<ruvia::http1_chunk_decode_complete>()) {
            done = true;
        } else {
            FAIL() << "unexpected chunk decode state";
        }
    }
    EXPECT_EQ(body, "hello world");
    auto taken = decoder.take_trailers();
    ASSERT_EQ(taken.size(), std::size_t{1});
    EXPECT_EQ(taken[0].name(), "x-checksum");
    EXPECT_EQ(taken[0].value(), "abc123");

    std::string sse;
    ruvia::append_sse_message(sse, {.data_ = "line1\nline2", .event_ = "tick", .id_ = "42", .retry_ = std::chrono::milliseconds{1500}});
    EXPECT_EQ(sse, "event: tick\nid: 42\nretry: 1500\ndata: line1\ndata: line2\n\n");
}

TEST(RuviaHttpPrimitives, structured_fields_decimals_bytes_unescape_and_visitors) {
    const std::string_view input = R"(-12.345;scale=3, :YWJj:;enc="b64", "hello\\world\"!", ("a";x=1 42);listp=?1)";
    ruvia::http_structured_parser parser{ruvia::http_structured_text_input{input}};

    ruvia::http_structured_item item;
    std::vector<std::pair<std::string, std::int64_t>> params;
    ASSERT_TRUE(parser.item(item));
    ASSERT_TRUE(parser.parameters([&](std::string_view k, const ruvia::http_structured_item& v) {
        params.emplace_back(std::string(k), v.integer_);
        return true;
    }));
    EXPECT_EQ(item.kind_, ruvia::http_structured_item::kind_type::decimal);
    EXPECT_EQ(item.decimal_.milli_value_, -12345);
    EXPECT_DOUBLE_EQ(item.decimal_.to_double(), -12.345);
    ASSERT_EQ(params.size(), std::size_t{1});
    EXPECT_EQ(params[0].first, "scale");
    EXPECT_EQ(params[0].second, 3);

    ASSERT_TRUE(parser.consume(','));
    parser.ows();
    ASSERT_TRUE(parser.member(item));
    EXPECT_EQ(item.kind_, ruvia::http_structured_item::kind_type::byte_sequence);
    EXPECT_EQ(ruvia::as_chars(item.bytes()), "abc");

    ASSERT_TRUE(parser.consume(','));
    parser.ows();
    ASSERT_TRUE(parser.member(item));
    EXPECT_EQ(item.kind_, ruvia::http_structured_item::kind_type::string);
    EXPECT_TRUE(item.has_escapes_);
    EXPECT_EQ(item.unescaped_string(), "hello\\world\"!");
    std::string scratch;
    EXPECT_EQ(item.unescape(scratch), "hello\\world\"!");

    ASSERT_TRUE(parser.consume(','));
    parser.ows();
    std::vector<std::string> inner_Kinds;
    bool list_param_true = false;
    ASSERT_TRUE(parser.inner_list(
        [&](const ruvia::http_structured_item& elem) {
        if (elem.kind_ == ruvia::http_structured_item::kind_type::string) {
            inner_Kinds.emplace_back(elem.unescaped_string());
        } else if (elem.kind_ == ruvia::http_structured_item::kind_type::integer) {
            inner_Kinds.push_back(std::to_string(elem.integer_));
        }
        return true;
    },
        [&](std::string_view k, const ruvia::http_structured_item& v) {
        if (k == "listp" && v.kind_ == ruvia::http_structured_item::kind_type::boolean && v.boolean_) {
            list_param_true = true;
        }
        return true;
    }));
    ASSERT_EQ(inner_Kinds.size(), std::size_t{2});
    EXPECT_EQ(inner_Kinds[0], "a");
    EXPECT_EQ(inner_Kinds[1], "42");
    EXPECT_TRUE(list_param_true);
    EXPECT_TRUE(parser.empty());

    const auto prio = ruvia::format_http_priority({.urgency_ = 1, .incremental_ = true});
    EXPECT_EQ(prio, "u=1, i");

    const std::array<std::string_view, 1> origins{"https://api.example.com"};
    std::array<std::byte, 64> adv_buf{};
    const auto adv_len = ruvia::encode_http_origin_payload(adv_buf, origins);
    ASSERT_EQ(adv_len.index(), std::size_t{0});
    const auto decoded_adv = ruvia::decode_http_origin_advertisement(ruvia::byte_view(adv_buf).first(std::get<0>(adv_len)));
    ASSERT_EQ(decoded_adv.index(), std::size_t{0});
    ASSERT_EQ(std::get<0>(decoded_adv).origins_.size(), std::size_t{1});
    EXPECT_EQ(std::get<0>(decoded_adv).origins_[0], "https://api.example.com");
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(RuviaHttpPrimitives, stateful_hpack_fragment_decode_and_custom_http2_settings) {
    ruvia::hpack_encoder encoder(4096);
    std::string block;
    encoder.encode_status_field(block, ruvia::http_status::ok);
    const std::array<ruvia::http_header_view, 2> fields{{
        {"x-custom-a", "alpha-dynamic-value"},
        {"authorization", "Bearer secret-never-indexed"},
    }};
    encoder.encode_headers(block, fields);

    ruvia::hpack_decoder decoder;
    decoder.set_max_dynamic_table_size(4096);
    std::vector<std::pair<std::string, std::string>> decoded;
    const auto split = block.size() / 2;
    EXPECT_FALSE(decoder.decode(ruvia::as_bytes(block).first(split), false, [&](std::string_view n, std::string_view v) {
        decoded.emplace_back(std::string(n), std::string(v));
        return true;
    }).has_value());
    EXPECT_FALSE(decoder.decode(ruvia::as_bytes(block).subspan(split), true, [&](std::string_view n, std::string_view v) {
        decoded.emplace_back(std::string(n), std::string(v));
        return true;
    }).has_value());
    ASSERT_EQ(decoded.size(), std::size_t{3});
    EXPECT_EQ(decoded[0].first, ":status");
    EXPECT_EQ(decoded[0].second, "200");
    EXPECT_EQ(decoded[1].second, "alpha-dynamic-value");
    EXPECT_EQ(decoded[2].second, "Bearer secret-never-indexed");

    encoder.reset();
    decoder.reset();

    // Custom HTTP/2 local SETTINGS validation & wire advertisement
    EXPECT_THROW((void)ruvia::http2_connection::server({.max_frame_size_ = 1000}), std::invalid_argument);
    EXPECT_THROW((void)ruvia::http2_connection::server({.initial_window_size_ = 0x80000000U}), std::invalid_argument);

    auto server = ruvia::http2_connection::server({
        .max_concurrent_streams_ = 64,
        .initial_window_size_ = 131072,
        .max_frame_size_ = 32768,
        .enable_connect_protocol_ = false,
    });
    auto client = ruvia::http2_connection::client({});
    std::string c2s;
    (void)client.drain_output_batch(65536, c2s);
    EXPECT_EQ(server.feed(ruvia::as_bytes(c2s)), ruvia::http2_feed_status::accepted);
    std::string s2c;
    (void)server.drain_output_batch(65536, s2c);
    EXPECT_EQ(client.feed(ruvia::as_bytes(s2c)), ruvia::http2_feed_status::accepted);
    ASSERT_TRUE(client.peer_settings().has_value());
    EXPECT_EQ(client.peer_settings()->max_concurrent_streams_, 64U);
    EXPECT_EQ(client.peer_settings()->max_frame_size_, 32768U);
    EXPECT_FALSE(client.peer_settings()->enable_connect_protocol_);
    const auto req_id = client.submit_request_head({.authority_ = "example.test"});
    ASSERT_TRUE(std::holds_alternative<std::uint32_t>(req_id));
    ASSERT_TRUE(client.send_window_state(std::get<std::uint32_t>(req_id)).has_value());
    EXPECT_EQ(client.send_window_state(std::get<std::uint32_t>(req_id))->stream_window_, 131072);
}
#endif

#if defined(RUVIA_ENABLE_HTTP3)
TEST(RuviaHttpPrimitives, http3_expectation_gate_and_quic_packet_helpers) {
    auto client = ruvia::http3_connection::client({});
    ASSERT_EQ(client.bind_control_streams(2, 6, 10), ruvia::http3_transport_status::accepted);
    ASSERT_EQ(client.submit_request_head(0, {
                                                .method_ = "POST",
                                                .scheme_ = "https",
                                                .authority_ = "example.test",
                                                .target_ = "/upload",
                                                .content_ = ruvia::http_request_content_plan::known_length(4),
                                                .expectation_ = ruvia::http_client_request_expectation::continue_,
                                            }),
        ruvia::http_submit_status::accepted);

    const auto gated = client.submit_data(0, ruvia::as_bytes("data"));
    EXPECT_EQ(gated.status_, ruvia::http_data_submit_status::expectation_pending);
    EXPECT_EQ(gated.consumed_bytes_, std::size_t{0});

    EXPECT_EQ(client.release_request_content(0), ruvia::http_request_content_release_status::released);
    EXPECT_EQ(client.release_request_content(0), ruvia::http_request_content_release_status::not_pending);

    const auto accepted = client.submit_data(0, ruvia::as_bytes("data"));
    EXPECT_EQ(accepted.status_, ruvia::http_data_submit_status::complete);
    EXPECT_EQ(accepted.consumed_bytes_, std::size_t{4});

    // QUIC Version Negotiation & Stateless Reset helpers (PIMPL / no ngtcp2.h leak)
    const std::array<std::byte, 8> dcid{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    const std::array<std::byte, 4> scid{std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12}};
    const ruvia::quic_cid cid_obj(dcid);
    EXPECT_EQ(cid_obj.size(), std::size_t{8});
    EXPECT_EQ(cid_obj.bytes().size(), std::size_t{8});

    // Long Header packet with unsupported version 0xdeadbeef triggers version_negotiation
    std::vector<std::byte> unsupported_pkt{
        std::byte{0xC0}, std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF},
        std::byte{8}
    };
    unsupported_pkt.insert(unsupported_pkt.end(), dcid.begin(), dcid.end());
    unsupported_pkt.push_back(std::byte{4});
    unsupported_pkt.insert(unsupported_pkt.end(), scid.begin(), scid.end());
    unsupported_pkt.resize(1200, std::byte{0});
    const auto decoded_unsupported = ruvia::decode_quic_packet_version_cid(unsupported_pkt, 8);
    EXPECT_EQ(decoded_unsupported.status_, ruvia::quic_packet_version_status::version_negotiation);
    EXPECT_EQ(decoded_unsupported.version_, 0xDEADBEEFU);

    const std::array<std::uint32_t, 1> versions{ruvia::quic_version_v1};
    std::array<std::byte, 128> vn_buf{};
    const auto vn_len = ruvia::write_quic_version_negotiation_packet(vn_buf, 0xAB, decoded_unsupported.scid_, decoded_unsupported.dcid_, versions);
    ASSERT_GT(vn_len, std::size_t{0});
    const auto decoded_vn = ruvia::decode_quic_packet_version_cid(ruvia::byte_view(vn_buf).first(vn_len), 8);
    EXPECT_EQ(decoded_vn.status_, ruvia::quic_packet_version_status::ok);
    EXPECT_EQ(decoded_vn.version_, 0U);
    EXPECT_TRUE(std::ranges::equal(decoded_vn.dcid_, scid));
    EXPECT_TRUE(std::ranges::equal(decoded_vn.scid_, dcid));

    ruvia::http3_quic_stateless_reset_token token{};
    token.fill(std::byte{0xEE});
    std::array<std::byte, 64> rand_bytes{};
    rand_bytes.fill(std::byte{0x55});
    std::array<std::byte, 128> sr_buf{};
    const auto sr_len = ruvia::write_http3_quic_stateless_reset(token, rand_bytes, sr_buf, 48);
    ASSERT_GT(sr_len, std::size_t{20});
    EXPECT_LE(sr_len, std::size_t{48});
    EXPECT_TRUE(std::ranges::equal(ruvia::byte_view(sr_buf).subspan(sr_len - token.size(), token.size()), token));
}
#endif
