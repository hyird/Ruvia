#include <array>
#include <string>
#include <variant>

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

RUVIA_TEST(http_udp_datagram_context_prefix_roundtrips) {
    std::array<char, 8> prefix{};
    for (const auto id : {std::uint64_t{0}, std::uint64_t{63}, std::uint64_t{64}, std::uint64_t{16384}, (std::uint64_t{1} << 30), (std::uint64_t{1} << 62) - 1}) {
        const auto encoded = ruvia::encode_http_udp_datagram_prefix(prefix, id);
        RUVIA_CHECK(encoded.index() == 0);
        const auto decoded = ruvia::decode_http_udp_datagram(std::span<const char>(prefix).first(std::get<0>(encoded)));
        RUVIA_CHECK(decoded.index() == 0);
        RUVIA_CHECK_EQ(std::get<0>(decoded).context_id_, id);
        RUVIA_CHECK(std::get<0>(decoded).payload_.empty());
    }
    RUVIA_CHECK(ruvia::decode_http_udp_datagram({}).index() != 0);
    RUVIA_CHECK(ruvia::encode_http_udp_datagram_prefix(prefix, std::uint64_t{1} << 62).index() != 0);
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

RUVIA_TEST(http_datagram_capsule_channel_frames_opaque_payloads_and_obeys_half_close) {
    ruvia::http_datagram_session session;
    const std::string_view opaque("\1\0\xff", 3);
    const auto plan = session.prepare_datagram(std::span(opaque.data(), opaque.size()));
    RUVIA_CHECK(plan.index() == 0);
    RUVIA_CHECK_EQ(std::get<0>(plan).prefix_size_, std::size_t{2});
    RUVIA_CHECK(std::get<0>(plan).prefix_[0] == 0 && std::get<0>(plan).prefix_[1] == 3);
    RUVIA_CHECK(std::get<0>(plan).payload_.data() == opaque.data());
    const auto received = session.receive_datagram(std::get<0>(plan).payload_);
    RUVIA_CHECK(received.index() == 0 && std::get<0>(received));
    RUVIA_CHECK(std::string_view(std::get<0>(received)->data(), std::get<0>(received)->size()) == opaque);
    const auto empty = session.prepare_datagram({});
    RUVIA_CHECK(empty.index() == 0 && std::get<0>(empty).prefix_size_ == 2);
    session.close_receive();
    RUVIA_CHECK(!std::get<0>(session.receive_datagram(opaque)));
    session.close_send();
    RUVIA_CHECK(session.prepare_datagram({}).index() != 0);
}
