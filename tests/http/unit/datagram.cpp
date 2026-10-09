#include <array>
#include <string>
#include <variant>

#include "ruvia/http/http3_settings.h"
#include "ruvia/http/http_datagram.h"

#include "test_harness.h"

namespace {
struct capture {
    std::string body_;
    std::uint64_t type_{0};
    std::size_t ends_{0};
};
void capsule(void* context_value, ruvia::http_capsule_event event) {
    auto& capture_value = *static_cast<capture*>(context_value);
    capture_value.type_ = event.type_;
    if (!event.payload_.empty()) {
        capture_value.body_.append(event.payload_.data(), event.payload_.size());
    }
    capture_value.ends_ += event.end_capsule_;
}
}  // namespace
RUVIA_TEST(http_capsule_protocol_byte_parameters_accept_optional_padding_and_validate_quartets) {
    for (const auto value : {"?1;bytes=:YQ=:", "?1;bytes=:YQ==:", "?1;bytes=:YQ:", "?1;bytes=:YWJj:",
             "?1;bytes=:YWJ:", "?1;bytes=:YWJ=:", "?1;bytes=:YR==:", "?1;bytes=::"}) {
        const auto parsed_value = ruvia::parse_http_capsule_protocol(value);
        RUVIA_CHECK((parsed_value.index() == 0));
        if ((parsed_value.index() == 0)) {
            RUVIA_CHECK(std::get<0>(parsed_value));
        }
    }
    const auto disabled = ruvia::parse_http_capsule_protocol("?0;bytes=:YQ=:");
    RUVIA_CHECK((disabled.index() == 0));
    if ((disabled.index() == 0)) {
        RUVIA_CHECK(!std::get<0>(disabled));
    }
    for (const auto value : {"?1;bytes=:Y:", "?1;bytes=:Y=:", "?1;bytes=:YWJj=:", "?1;bytes=:YWJ==:",
             "?1;bytes=:YQ===:", "?1;bytes=:Y=Q:", "?1;bytes=:YQ$:", "?1;bytes=:YQ\n:", "?1;bytes=:YQ"}) {
        RUVIA_CHECK((ruvia::parse_http_capsule_protocol(value).index() != 0));
    }
}

RUVIA_TEST(http_datagram_quarter_stream_id_and_udp_context) {
    std::array<char, 16> prefix{};
    auto count = ruvia::encode_http3_datagram_prefix(prefix, 4096);
    RUVIA_CHECK((count.index() == 0));
    auto decoded = ruvia::decode_http3_datagram(std::span<const char>(prefix).first(std::get<0>(count)));
    RUVIA_CHECK((decoded.index() == 0));
    RUVIA_CHECK_EQ(std::get<0>(decoded).stream_id_, 4096u);
    RUVIA_CHECK(std::get<0>(decoded).payload_.empty());
    RUVIA_CHECK(!(ruvia::encode_http3_datagram_prefix(prefix, 1).index() == 0));
    auto oversized = ruvia::encode_http3_var_int(prefix, std::uint64_t{1} << 60);
    RUVIA_CHECK(!(ruvia::decode_http3_datagram(std::span<const char>(prefix).first(std::get<0>(oversized))).index() == 0));
    RUVIA_CHECK(!(ruvia::decode_http3_datagram({}).index() == 0));
    auto context_value = ruvia::encode_http_udp_datagram_prefix(prefix);
    auto udp = ruvia::decode_http_udp_datagram(std::span<const char>(prefix).first(std::get<0>(context_value)));
    RUVIA_CHECK((udp.index() == 0));
    RUVIA_CHECK_EQ(std::get<0>(udp).context_id_, 0u);
}
RUVIA_TEST(http_capsule_incremental_unknown_empty_and_datagram) {
    std::array<char, 16> prefix{};
    auto count = ruvia::encode_http_capsule_header(prefix, 0x1234, 5);
    std::string wire(prefix.data(), std::get<0>(count));
    wire += "hello";
    auto empty = ruvia::encode_http_capsule_header(prefix, 0, 0);
    wire.append(prefix.data(), std::get<0>(empty));
    ruvia::http_capsule_decoder decoder;
    capture capture;
    for (const char& byte : wire) {
        RUVIA_CHECK(decoder.feed({&byte, 1}, false, capsule, &capture) == ruvia::http_capsule_status::need_more_data);
    }
    RUVIA_CHECK_EQ(capture.body_, std::string("hello"));
    RUVIA_CHECK_EQ(capture.ends_, 2u);
    RUVIA_CHECK(decoder.feed({}, true, capsule, &capture) == ruvia::http_capsule_status::end);
}
RUVIA_TEST(http_capsule_truncated_and_length_limit_are_terminal) {
    const std::array prefix{char(0), char(3)};
    ruvia::http_capsule_decoder limit({.max_capsule_length_ = 2});
    RUVIA_CHECK(limit.feed(prefix, false, nullptr, nullptr) == ruvia::http_capsule_status::limit);
    ruvia::http_capsule_decoder truncated;
    RUVIA_CHECK(truncated.feed(prefix, true, nullptr, nullptr) == ruvia::http_capsule_status::truncated);
}
RUVIA_TEST(http3_datagram_settings_validate_boolean_and_round_trip) {
    std::array<char, 128> bytes_value{};
    auto size = ruvia::encode_http3_settings(bytes_value, {.h3_datagram_ = true});
    RUVIA_CHECK((size.index() == 0));
    auto decoded = ruvia::decode_http3_settings(std::span<const char>(bytes_value).first(std::get<0>(size)));
    RUVIA_CHECK((decoded.index() == 0));
    RUVIA_CHECK(std::get<0>(decoded).h3_datagram_);
    const std::array invalid{char(0x33), char(2)};
    RUVIA_CHECK(!(ruvia::decode_http3_settings(invalid).index() == 0));
}

RUVIA_TEST(http_capsule_pull_decoder_stops_at_each_capsule_and_commits_fin_after_the_suffix) {
    std::array<char, 16> header;
    std::string wire;
    for (const auto& item : std::array<std::pair<std::uint64_t, std::string_view>, 3>{{{0x123456789ULL, "first"}, {0, ""}, {7, "last"}}}) {
        const auto encoded = ruvia::encode_http_capsule_header(header, item.first, item.second.size());
        wire.append(header.data(), std::get<0>(encoded));
        wire.append(item.second);
    }
    for (std::size_t block : {std::size_t{1}, std::size_t{3}, wire.size()}) {
        ruvia::http_capsule_decoder decoder;
        capture captured;
        std::size_t offset{}, complete_value{};
        std::string all_payload;
        while (offset != wire.size()) {
            const auto count = std::min(block, wire.size() - offset);
            const auto result_value = decoder.feed_one(std::span<const char>(wire).subspan(offset, count), offset + count == wire.size(), capsule, &captured);
            RUVIA_CHECK(result_value.consumed_bytes_ > 0 && result_value.consumed_bytes_ <= count);
            offset += result_value.consumed_bytes_;
            if (result_value.capsule_complete_) {
                const std::array<std::uint64_t, 3> expected_types{0x123456789ULL, 0, 7};
                const std::array<std::string_view, 3> expected_payloads{"first", "", "last"};
                RUVIA_CHECK(captured.type_ == expected_types[complete_value]);
                RUVIA_CHECK(captured.body_ == expected_payloads[complete_value]);
                captured.body_.clear();
                ++complete_value;
            }
            RUVIA_CHECK(result_value.status_ == (offset == wire.size() ? ruvia::http_capsule_status::end : ruvia::http_capsule_status::need_more_data));
        }
        RUVIA_CHECK_EQ(complete_value, std::size_t{3});
        RUVIA_CHECK_EQ(captured.ends_, std::size_t{3});
        const auto ended = decoder.feed_one({}, true, capsule, &captured);
        RUVIA_CHECK(ended.status_ == ruvia::http_capsule_status::end && !ended.capsule_complete_ && ended.consumed_bytes_ == 0);
    }
    ruvia::http_capsule_decoder truncated;
    const std::array partial{char(0), char(3), char('a')};
    const auto result_value = truncated.feed_one(partial, true, nullptr, nullptr);
    RUVIA_CHECK(result_value.status_ == ruvia::http_capsule_status::truncated && !result_value.capsule_complete_ && result_value.consumed_bytes_ == partial.size());
}

RUVIA_TEST(http_datagram_generic_channel_frames_opaque_payloads_and_plans_receive_failures) {
    using namespace ruvia;
    http_datagram_session session_value({.http3_stream_id_ = 12, .local_h3_datagram_ = true, .peer_h3_datagram_ = true, .quic_datagram_ = true, .max_quic_payload_bytes_ = 8});
    const std::string_view opaque("\1\0\xff", 3);
    const auto native = session_value.prepare_datagram(std::span(opaque.data(), opaque.size()), http_datagram_transport::quic);
    RUVIA_CHECK((native.index() == 0) && std::get<0>(native).prefix_size_ == 1 && std::get<0>(native).prefix_[0] == 3);
    const std::string packet = std::string(std::get<0>(native).prefix_.data(), std::get<0>(native).prefix_size_) + std::string(opaque);
    const auto received_value = session_value.receive_datagram(std::span(packet.data(), packet.size()), http_datagram_transport::quic);
    RUVIA_CHECK((received_value.index() == 0) && std::get<0>(received_value) && std::string_view((*std::get<0>(received_value)).data(), (*std::get<0>(received_value)).size()) == opaque);
    const auto capsule = session_value.prepare_datagram({}, http_datagram_transport::capsule);
    RUVIA_CHECK((capsule.index() == 0) && std::get<0>(capsule).prefix_size_ == 2 && std::get<0>(capsule).prefix_[0] == 0 && std::get<0>(capsule).prefix_[1] == 0);
    RUVIA_CHECK((session_value.prepare_datagram(std::span<const char>("12345678", 8), http_datagram_transport::quic).index() != 0));
    session_value.close_receive();
    RUVIA_CHECK(!std::get<0>(session_value.receive_datagram(std::span(packet.data(), packet.size()), http_datagram_transport::quic)));
    session_value.close_send();
    RUVIA_CHECK((session_value.prepare_datagram({}, http_datagram_transport::capsule).index() != 0));
    const http3_datagram_view view{12, {}};
    RUVIA_CHECK(plan_http3_datagram_receive(view, {true, true, true, true}) == http3_datagram_receive_status::deliver);
    RUVIA_CHECK(plan_http3_datagram_receive(view, {true, false, true, false}) == http3_datagram_receive_status::drop);
    RUVIA_CHECK(plan_http3_datagram_receive(view, {true, true, false, false}) == http3_datagram_receive_status::drop);
    RUVIA_CHECK(plan_http3_datagram_receive(view, {true, true, true, false}) == http3_datagram_receive_status::stream_error);
    RUVIA_CHECK(plan_http3_datagram_receive(view, {false, true, true, true}) == http3_datagram_receive_status::connection_error);
}
