#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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
        RUVIA_CHECK(parsed_value.has_value());
        if (parsed_value.has_value()) {
            RUVIA_CHECK(*parsed_value);
        }
    }
    const auto disabled = ruvia::parse_http_capsule_protocol("?0;bytes=:YQ=:");
    RUVIA_CHECK(disabled.has_value());
    if (disabled.has_value()) {
        RUVIA_CHECK(!*disabled);
    }
    // RFC 8941 4.2 discards only SP around a top-level Item.
    for (const auto value : {"?1;bytes=:Y:", "?1;bytes=:Y=:", "?1;bytes=:YWJj=:", "?1;bytes=:YWJ==:",
             "?1;bytes=:YQ===:", "?1;bytes=:Y=Q:", "?1;bytes=:YQ$:", "?1;bytes=:YQ\n:", "?1;bytes=:YQ", "?1\t",
             "?1 \t", "\t?1"}) {
        RUVIA_CHECK(!ruvia::parse_http_capsule_protocol(value).has_value());
    }
    const auto padded = ruvia::parse_http_capsule_protocol(" ?1  ");
    RUVIA_CHECK(padded.has_value() && *padded);
}

RUVIA_TEST(http_udp_datagram_context_prefix_roundtrips) {
    std::array<char, 8> prefix{};
    for (const auto id : {std::uint64_t{0}, std::uint64_t{63}, std::uint64_t{64}, std::uint64_t{16384}, (std::uint64_t{1} << 30), (std::uint64_t{1} << 62) - 1}) {
        const auto encoded = ruvia::encode_http_udp_datagram_prefix(prefix, id);
        RUVIA_CHECK(encoded.index() == 0);
        const auto decoded = ruvia::decode_http_udp_datagram(std::span<const char>(prefix).first(std::get<0>(encoded)));
        RUVIA_CHECK(decoded.has_value());
        RUVIA_CHECK_EQ(decoded->context_id_, id);
        RUVIA_CHECK(decoded->payload_.empty());
    }
    RUVIA_CHECK(!ruvia::decode_http_udp_datagram({}).has_value());
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

// RFC 9297 section 3.2: capsules the reader does not keep are skipped at any
// length; max_capsule_length_ bounds only kept capsules (for CONNECT-UDP, after
// the RFC 9298 Context ID shows the datagram is kept).
RUVIA_TEST(http_capsule_retention_skips_dropped_capsules_beyond_the_length_limit) {
    std::array<char, 16> header;
    const auto append_capsule = [&](std::string& wire, std::uint64_t type, std::string_view value) {
        const auto encoded = ruvia::encode_http_capsule_header(header, type, value.size());
        wire.append(header.data(), std::get<0>(encoded));
        wire.append(value);
    };
    std::string wire;
    append_capsule(wire, 0x17, std::string(200, 'u'));
    append_capsule(wire, 0, std::string("\x03", 1) + std::string(200, 'c'));
    append_capsule(wire, 0, std::string("\x40\x00", 2) + "two-byte-zero");
    append_capsule(wire, 0, std::string("\0", 1) + "after");
    append_capsule(wire, 0, std::string("\x41", 1));
    for (const std::size_t block : {std::size_t{1}, std::size_t{7}, wire.size()}) {
        ruvia::http_capsule_decoder decoder({.max_capsule_length_ = 100});
        capture captured;
        std::vector<std::string> kept;
        std::size_t offset{};
        ruvia::http_capsule_feed_result result_value{};
        while (offset != wire.size()) {
            const auto count = std::min(block, wire.size() - offset);
            result_value = decoder.feed_one(std::span<const char>(wire).subspan(offset, count), offset + count == wire.size(),
                capsule, &captured, ruvia::http_capsule_retention::udp_payloads);
            RUVIA_CHECK(result_value.status_ != ruvia::http_capsule_status::limit);
            offset += result_value.consumed_bytes_;
            if (result_value.capsule_complete_) {
                RUVIA_CHECK_EQ(captured.type_, std::uint64_t{0});
                kept.push_back(std::exchange(captured.body_, {}));
            }
        }
        RUVIA_CHECK(result_value.status_ == ruvia::http_capsule_status::end);
        // Context ID 0 in either encoding, and a value too short for its
        // Context ID (left to the reader as malformed), are kept.
        RUVIA_CHECK(kept == (std::vector<std::string>{std::string("\x40\x00", 2) + "two-byte-zero",
                                std::string("\0", 1) + "after", std::string("\x41", 1)}));
        RUVIA_CHECK_EQ(captured.ends_, std::size_t{3});
    }

    std::string datagrams;
    append_capsule(datagrams, 0x17, std::string(200, 'u'));
    append_capsule(datagrams, 0, "\x03kept");
    ruvia::http_capsule_decoder decoder({.max_capsule_length_ = 100});
    capture captured;
    RUVIA_CHECK(decoder.feed(datagrams, true, capsule, &captured, ruvia::http_capsule_retention::datagrams) ==
                ruvia::http_capsule_status::end);
    RUVIA_CHECK_EQ(captured.body_, std::string("\x03kept"));
    RUVIA_CHECK_EQ(captured.ends_, std::size_t{1});

    // Kept capsules over the limit stay terminal: every type for a generic
    // reader, and a CONNECT-UDP payload once its Context ID is known.
    for (const auto retention : {ruvia::http_capsule_retention::all, ruvia::http_capsule_retention::udp_payloads}) {
        std::string oversized;
        append_capsule(oversized, retention == ruvia::http_capsule_retention::all ? 0x17 : 0,
            std::string("\0", 1) + std::string(200, 'o'));
        ruvia::http_capsule_decoder limited({.max_capsule_length_ = 100});
        RUVIA_CHECK(limited.feed(oversized, false, nullptr, nullptr, retention) == ruvia::http_capsule_status::limit);
    }
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
    RUVIA_CHECK(received.has_value());
    RUVIA_CHECK(std::string_view(received->data(), received->size()) == opaque);
    const auto empty = session.prepare_datagram({});
    RUVIA_CHECK(empty.index() == 0 && std::get<0>(empty).prefix_size_ == 2);
    session.close_receive();
    RUVIA_CHECK(!session.receive_datagram(opaque).has_value());
    session.close_send();
    RUVIA_CHECK(session.prepare_datagram({}).index() != 0);
}
