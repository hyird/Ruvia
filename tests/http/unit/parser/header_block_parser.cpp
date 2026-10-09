#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_parse_error.h"

#include "failing_memory_resource.h"
#include "parser/http_header_block_parser.h"
#include "test_harness.h"

namespace {

using ruvia::http_parse_error;
using ruvia::http_transfer_coding;
using ruvia::detail::find_http_header_end;
using ruvia::detail::http_content_length_parse_status;
using ruvia::detail::http_content_length_state;
using ruvia::detail::http_transfer_encoding_parse_status;
using ruvia::detail::http_transfer_encoding_state;

using ruvia::detail::parse_http_header_block;
using ruvia::detail::parsed_request_header_block;

struct parsed final {
    std::optional<http_parse_error> error_;
    bool has_host_;
    bool saw_chunked_;
    bool transfer_coding_unsupported_;
    bool has_content_length_;
    std::size_t content_length_;
    std::size_t transfer_coding_count_;
    ruvia::http_transfer_coding first_transfer_coding_;
    bool has_te_;
};

parsed parse(std::string_view head) {
    parsed_request_header_block block{};
    const auto header_bytes = find_http_header_end(head, 0);
    const auto error = parse_http_header_block(head, header_bytes, block);
    const auto content_length = block.content_length_.value();
    const auto transfer_encoding = block.transfer_encoding_.value();
    const auto* final_chunked =
        transfer_encoding.has_value() ? transfer_encoding->final_chunked() : nullptr;
    const auto* non_chunked =
        transfer_encoding.has_value() ? transfer_encoding->non_chunked() : nullptr;
    ruvia::http_transfer_codings transfer_codings;
    if (final_chunked != nullptr) {
        transfer_codings = final_chunked->transfer_codings();
    } else if (non_chunked != nullptr) {
        transfer_codings = non_chunked->transfer_codings();
    }
    return {error, block.host_header_index_ >= 0, final_chunked != nullptr,
        block.transfer_encoding_.unsupported(), content_length.has_value(),
        content_length.value_or(0), transfer_codings.values_.size(),
        transfer_codings.empty() ? http_transfer_coding::gzip : transfer_codings.values_[0],
        block.te_header_present_};
}

RUVIA_TEST(content_length_field_updates_are_transactional) {
    http_content_length_state<> state;
    RUVIA_CHECK(state.parse_field("5") == http_content_length_parse_status::ok);
    RUVIA_CHECK(state.value() == std::optional<std::size_t>(5));

    RUVIA_CHECK(state.parse_field("5, invalid") == http_content_length_parse_status::invalid);
    RUVIA_CHECK(state.value() == std::optional<std::size_t>(5));

    RUVIA_CHECK(state.parse_field("6, 6") == http_content_length_parse_status::conflicting);
    RUVIA_CHECK(state.value() == std::optional<std::size_t>(5));

    http_content_length_state<std::uint64_t> wide;
    RUVIA_CHECK(wide.parse_field(" \t18446744073709551615, 018446744073709551615\t ") ==
                http_content_length_parse_status::ok);
    RUVIA_CHECK(wide.value() == std::optional<std::uint64_t>(UINT64_MAX));
    RUVIA_CHECK(wide.parse_single_value("18446744073709551615") == http_content_length_parse_status::ok);
    for (const std::string_view invalid : {"", " 18446744073709551615", "18446744073709551615 ",
             "18446744073709551615,18446744073709551615", "18446744073709551616"}) {
        RUVIA_CHECK(wide.parse_single_value(invalid) == http_content_length_parse_status::invalid);
        RUVIA_CHECK(wide.value() == std::optional<std::uint64_t>(UINT64_MAX));
    }
    RUVIA_CHECK(wide.parse_single_value("0") == http_content_length_parse_status::conflicting);
    RUVIA_CHECK(wide.value() == std::optional<std::uint64_t>(UINT64_MAX));
}

RUVIA_TEST(transfer_encoding_field_allocation_failures_preserve_committed_value) {
    for (const bool populated : {false, true}) {
        bool saw_failure = false;
        bool saw_success = false;
        for (std::size_t fail_after = 0; fail_after < 32; ++fail_after) {
            failing_memory_resource resource;
            {
                http_transfer_encoding_state state_value(&resource);
                if (populated) {
                    RUVIA_CHECK(state_value.parse_field("gzip") == http_transfer_encoding_parse_status::ok);
                }
                const auto baseline = resource.live_allocations();
                resource.fail_after(fail_after);
                bool allocation_failed = false;
                try {
                    RUVIA_CHECK(state_value.parse_field("custom, deflate, chunked") == http_transfer_encoding_parse_status::unsupported);
                } catch (const std::bad_alloc&) {
                    allocation_failed = true;
                }
                resource.allow_allocations();
                if (allocation_failed) {
                    saw_failure = true;
                    RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
                    RUVIA_CHECK(!state_value.unsupported());
                    RUVIA_CHECK_EQ(state_value.value().has_value(), populated);
                    if (populated && state_value.value()) {
                        const auto* old_value = state_value.value()->non_chunked();
                        RUVIA_CHECK(old_value != nullptr);
                        if (old_value != nullptr) {
                            const auto& codings = old_value->transfer_codings().values_;
                            RUVIA_CHECK_EQ(codings.size(), std::size_t{1});
                            RUVIA_CHECK(codings[0] == http_transfer_coding::gzip);
                            RUVIA_CHECK(codings.get_allocator().resource() == &resource);
                        }
                    }
                    RUVIA_CHECK(state_value.parse_field("custom, deflate, chunked") == http_transfer_encoding_parse_status::unsupported);
                } else {
                    saw_success = true;
                }
                RUVIA_CHECK(state_value.unsupported());
                const auto* value = state_value.value()->final_chunked();
                RUVIA_CHECK(value != nullptr);
                if (value != nullptr) {
                    const auto& codings = value->transfer_codings().values_;
                    RUVIA_CHECK_EQ(codings.size(), populated ? std::size_t{2} : std::size_t{1});
                    RUVIA_CHECK(codings[codings.size() - 1] == http_transfer_coding::deflate);
                    RUVIA_CHECK(codings.get_allocator().resource() == &resource);
                }
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
            if (saw_success) {
                break;
            }
        }
        RUVIA_CHECK(saw_failure);
        RUVIA_CHECK(saw_success);
    }
}

RUVIA_TEST(transfer_coding_sequence_preserves_resource_and_ownership) {
    failing_memory_resource source_resource;
    failing_memory_resource target_resource;
    {
        ruvia::http_transfer_codings source_value(&source_resource);
        source_value.values_.push_back(http_transfer_coding::gzip);
        auto copy = source_value;
        RUVIA_CHECK(copy.values_.get_allocator().resource() == &source_resource);
        RUVIA_CHECK(copy.values_.data() != source_value.values_.data());
        const auto* storage = source_value.values_.data();
        const auto baseline = source_resource.live_allocations();
        source_resource.fail_after(0);
        auto moved = std::move(source_value);
        RUVIA_CHECK(moved.values_.data() == storage);
        RUVIA_CHECK(source_value.empty());
        RUVIA_CHECK_EQ(source_resource.live_allocations(), baseline);
        bool allocation_failed = false;
        try {
            source_value.values_.push_back(http_transfer_coding::deflate);
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }
        RUVIA_CHECK(allocation_failed);
        source_resource.allow_allocations();
        source_value.values_.push_back(http_transfer_coding::deflate);

        ruvia::http_transfer_codings target(&target_resource);
        target_resource.fail_after(0);
        allocation_failed = false;
        try {
            target = std::move(moved);
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }
        RUVIA_CHECK(allocation_failed);
        RUVIA_CHECK(target.empty());
        RUVIA_CHECK(moved.values_.data() == storage);
        RUVIA_CHECK(moved.values_[0] == http_transfer_coding::gzip);
        target_resource.allow_allocations();
        target = std::move(moved);
        RUVIA_CHECK(target.values_.get_allocator().resource() == &target_resource);
        RUVIA_CHECK(target.values_[0] == http_transfer_coding::gzip);
        RUVIA_CHECK(moved.empty());
        RUVIA_CHECK(copy.values_[0] == http_transfer_coding::gzip);
    }
    RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(target_resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(transfer_coding_sequence_same_resource_move_is_allocation_free_and_bounded) {
    failing_memory_resource resource;
    {
        ruvia::http_transfer_codings source_value(&resource);
        ruvia::http_transfer_codings target(&resource);
        source_value.values_.push_back(http_transfer_coding::gzip);
        target.values_.push_back(http_transfer_coding::deflate);
        const auto* storage = source_value.values_.data();
        resource.fail_after(0);
        target = std::move(source_value);
        RUVIA_CHECK(source_value.empty());
        RUVIA_CHECK(target.values_.data() == storage);
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{1});
        for (std::size_t count = 1; count < ruvia::max_transfer_codings; ++count) {
            target.values_.push_back(http_transfer_coding::deflate);
        }
        bool rejected = false;
        try {
            target.values_.push_back(http_transfer_coding::gzip);
        } catch (const std::length_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        RUVIA_CHECK_EQ(target.values_.size(), ruvia::max_transfer_codings);
        RUVIA_CHECK(target.values_.data() == storage);
        RUVIA_CHECK(target.values_[0] == http_transfer_coding::gzip);
        resource.allow_allocations();
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(transfer_encoding_field_updates_are_transactional_and_discriminated) {
    http_transfer_encoding_state state;
    RUVIA_CHECK(!state.value().has_value());
    RUVIA_CHECK(state.parse_field("gzip") == http_transfer_encoding_parse_status::ok);

    auto value = state.value();
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK(value->non_chunked() != nullptr);
    RUVIA_CHECK(value->final_chunked() == nullptr);
    if (const auto* non_chunked = value->non_chunked()) {
        RUVIA_CHECK_EQ(non_chunked->transfer_codings().values_.size(), std::size_t{1});
        RUVIA_CHECK(non_chunked->transfer_codings().values_[0] == http_transfer_coding::gzip);
    }

    RUVIA_CHECK(
        state.parse_field("chunked, deflate") == http_transfer_encoding_parse_status::malformed);
    value = state.value();
    RUVIA_CHECK(value->non_chunked() != nullptr);
    RUVIA_CHECK(value->final_chunked() == nullptr);

    RUVIA_CHECK(state.parse_field("g@zip") == http_transfer_encoding_parse_status::malformed);
    RUVIA_CHECK(state.parse_field(R"(custom; level="a\"b")") ==
                http_transfer_encoding_parse_status::unsupported);
    RUVIA_CHECK(state.parse_field("custom; level") == http_transfer_encoding_parse_status::malformed);
    value = state.value();
    RUVIA_CHECK(value->non_chunked() != nullptr);
    RUVIA_CHECK(value->final_chunked() == nullptr);

    // Unsupported state is sticky across later fields, so verify a valid
    // final-chunked update on a separate state without unknown codings.
    http_transfer_encoding_state final_state;
    RUVIA_CHECK(final_state.parse_field("gzip") == http_transfer_encoding_parse_status::ok);
    RUVIA_CHECK(final_state.parse_field("chunked") == http_transfer_encoding_parse_status::ok);
    value = final_state.value();
    RUVIA_CHECK(value->non_chunked() == nullptr);
    RUVIA_CHECK(value->final_chunked() != nullptr);
    if (const auto* final_chunked = value->final_chunked()) {
        RUVIA_CHECK_EQ(final_chunked->transfer_codings().values_.size(), std::size_t{1});
    }
}

}  // namespace

RUVIA_TEST(header_block_parses_valid_request) {
    const auto result_value = parse("GET / HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\n\r\n");
    RUVIA_CHECK(!result_value.error_.has_value());
    RUVIA_CHECK(result_value.has_host_);
    RUVIA_CHECK(result_value.has_content_length_);
    RUVIA_CHECK_EQ(result_value.content_length_, std::size_t{5});
}

RUVIA_TEST(header_block_parser_preserves_short_and_long_field_values) {
    for (const std::size_t length : {0U, 1U, 7U, 8U, 15U, 16U, 31U, 32U, 4096U}) {
        std::string value(length, 'a');
        for (std::size_t index = 1; index + 1 < length; ++index) {
            value[index] = index % 3 == 0 ? '\t' : index % 3 == 1 ? ' '
                                                                  : '\xe9';
        }
        std::string input = "GET / HTTP/1.1\r\nHost: example.test\r\nX-Value: \t";
        input += value;
        input += " \t\r\nX-Next: done\r\n\r\n";
        const auto expected_header_bytes = input.size();
        input.push_back('\0');
        input.push_back('\x7f');
        parsed_request_header_block block{};
        const auto header_bytes = find_http_header_end(input, 0);
        RUVIA_CHECK_EQ(header_bytes, expected_header_bytes);
        RUVIA_CHECK(!parse_http_header_block(input, header_bytes, block).has_value());
        RUVIA_CHECK_EQ(block.header_count_, std::size_t{3});
        RUVIA_CHECK_EQ(block.headers_[1].name_.bind(input), std::string_view("X-Value"));
        RUVIA_CHECK_EQ(block.headers_[1].value_.bind(input), std::string_view(value));
        RUVIA_CHECK_EQ(block.headers_[2].value_.bind(input), std::string_view("done"));
    }
}

RUVIA_TEST(header_block_parser_rejects_controls_at_field_value_word_boundaries) {
    for (const unsigned invalid : {0U, 10U, 13U, 31U, 127U}) {
        for (const std::size_t position : {0U, 1U, 6U, 7U, 8U, 9U, 14U, 15U, 16U, 31U,
                 32U, 63U, 64U, 4095U}) {
            std::string value(4096, 'a');
            value[position] = static_cast<char>(invalid);
            const std::string input = "GET / HTTP/1.1\r\nHost: example.test\r\nX-Value: " + value + "\r\n\r\n";
            parsed_request_header_block block{};
            const auto error = parse_http_header_block(input, find_http_header_end(input, 0), block);
            RUVIA_CHECK(error == http_parse_error::invalid_header);
        }
    }
}

RUVIA_TEST(header_block_parser_handles_deterministic_arbitrary_header_bytes) {
    std::uint64_t state_value = 0x7a13f045c2d98e31ULL;
    constexpr std::string_view field_value_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 !#$%&'*+-.^_`|~";
    const auto next_byte = [&]() noexcept {
        state_value = state_value * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<char>(state_value >> 56U);
    };

    for (std::size_t sample = 0; sample < 4096; ++sample) {
        std::string buffer = "GET / HTTP/1.1\r\n";
        const auto random_bytes = static_cast<std::size_t>(static_cast<unsigned char>(next_byte())) +
                                  (static_cast<std::size_t>(
                                       static_cast<unsigned char>(next_byte()) & 1U)
                                      << 8U);
        for (std::size_t i = 0; i < random_bytes; ++i) {
            buffer.push_back(next_byte());
        }
        buffer.append("\r\n\r\n");

        const auto header_bytes = find_http_header_end(buffer, 0);
        RUVIA_CHECK(header_bytes != std::string_view::npos);
        parsed_request_header_block block{};
        const auto error = parse_http_header_block(buffer, header_bytes, block);
        if (error.has_value()) {
            continue;
        }

        RUVIA_CHECK_EQ(block.method_.bind(buffer), std::string_view("GET"));
        RUVIA_CHECK_EQ(block.target_.bind(buffer), std::string_view("/"));
        RUVIA_CHECK_EQ(block.version_.bind(buffer), std::string_view("HTTP/1.1"));
        RUVIA_CHECK(block.header_count_ <= block.headers_.size());
        for (std::size_t i = 0; i < block.header_count_; ++i) {
            const auto& header_value = block.headers_[i];
            RUVIA_CHECK(header_value.name_.length_ > 0);
            RUVIA_CHECK(static_cast<std::size_t>(header_value.name_.offset_) + header_value.name_.length_ <=
                        header_bytes);
            RUVIA_CHECK(static_cast<std::size_t>(header_value.value_.offset_) + header_value.value_.length_ <=
                        header_bytes);
        }

        std::string accepted = "GET / HTTP/1.1\r\nHost: example.test\r\nX-Fuzz: ";
        const auto value_bytes =
            static_cast<std::size_t>(static_cast<unsigned char>(next_byte()) & 0x7FU);
        for (std::size_t i = 0; i < value_bytes; ++i) {
            const auto index =
                static_cast<std::size_t>(static_cast<unsigned char>(next_byte())) %
                field_value_chars.size();
            accepted.push_back(field_value_chars[index]);
        }
        accepted.append("\r\n\r\n");

        const auto accepted_header_bytes = find_http_header_end(accepted, 0);
        RUVIA_CHECK(accepted_header_bytes != std::string_view::npos);
        parsed_request_header_block accepted_block{};
        RUVIA_CHECK(!parse_http_header_block(accepted, accepted_header_bytes, accepted_block).has_value());
        RUVIA_CHECK_EQ(accepted_block.method_.bind(accepted), std::string_view("GET"));
        RUVIA_CHECK_EQ(accepted_block.target_.bind(accepted), std::string_view("/"));
        RUVIA_CHECK_EQ(accepted_block.version_.bind(accepted), std::string_view("HTTP/1.1"));
        RUVIA_CHECK_EQ(accepted_block.header_count_, std::size_t{2});
        RUVIA_CHECK_EQ(accepted_block.headers_[0].name_.bind(accepted), std::string_view("Host"));
        RUVIA_CHECK_EQ(
            accepted_block.headers_[0].value_.bind(accepted), std::string_view("example.test"));
        RUVIA_CHECK_EQ(accepted_block.headers_[1].name_.bind(accepted), std::string_view("X-Fuzz"));
        RUVIA_CHECK(static_cast<std::size_t>(accepted_block.headers_[1].value_.offset_) +
                        accepted_block.headers_[1].value_.length_ <=
                    accepted_header_bytes);
    }
}

RUVIA_TEST(header_block_rejects_conflicting_content_length) {
    // Two Content-Length values that disagree is a classic request-smuggling
    // vector and must be rejected.
    const auto result_value =
        parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n");
    RUVIA_CHECK(result_value.error_ == http_parse_error::conflicting_content_length);
}

RUVIA_TEST(header_block_allows_repeated_equal_content_length) {
    const auto result_value =
        parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n");
    RUVIA_CHECK(!result_value.error_.has_value());
    RUVIA_CHECK_EQ(result_value.content_length_, std::size_t{5});
}

RUVIA_TEST(header_block_allows_combined_equal_content_length) {
    const auto result_value = parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5, 5\r\n\r\n");
    RUVIA_CHECK(!result_value.error_.has_value());
    RUVIA_CHECK(result_value.has_content_length_);
    RUVIA_CHECK_EQ(result_value.content_length_, std::size_t{5});

    const auto conflict = parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5, 6\r\n\r\n");
    RUVIA_CHECK(conflict.error_ == http_parse_error::conflicting_content_length);
}

RUVIA_TEST(header_block_rejects_invalid_content_length) {
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n").error_ ==
                http_parse_error::invalid_content_length);
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: -5\r\n\r\n").error_ ==
                http_parse_error::invalid_content_length);
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5x\r\n\r\n").error_ ==
                http_parse_error::invalid_content_length);
}

RUVIA_TEST(header_block_rejects_duplicate_host) {
    // A duplicate Host header is ambiguous (host confusion) and must be rejected.
    const auto result_value = parse("GET / HTTP/1.1\r\nHost: a.com\r\nHost: b.com\r\n\r\n");
    RUVIA_CHECK(result_value.error_ == http_parse_error::invalid_host);
}

RUVIA_TEST(header_block_uses_recipient_connection_and_upgrade_list_rules) {
    const auto tolerant = parse(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Connection: , keep-alive,\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: , custom/1, websocket,\r\n\r\n");
    RUVIA_CHECK(!tolerant.error_.has_value());

    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n"
                      "Connection: close;invalid\r\n\r\n")
                    .error_ == http_parse_error::invalid_connection);
    for (const std::string_view option : {"Authorization", "Cookie", "Range"}) {
        const auto result_value = parse(std::string("GET / HTTP/1.1\r\nHost: x\r\nConnection: ") +
                                        std::string(option) + "\r\n\r\n");
        RUVIA_CHECK(result_value.error_ == http_parse_error::invalid_connection);
    }
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n"
                      "Upgrade: websocket/\r\n\r\n")
                    .error_ == http_parse_error::invalid_upgrade);
}

RUVIA_TEST(header_block_validates_te_field_values) {
    for (const std::string_view valid : {"", "trailers", "gzip", "deflate;q=0.5", "deflate;Q=0.5",
             "x-gzip ; q=1.000", "custom;level=\"a,b\";q=0"}) {
        const auto result_value = parse(
            std::string("GET / HTTP/1.1\r\nHost: x\r\nTE: ") + std::string(valid) + "\r\n\r\n");
        RUVIA_CHECK(!result_value.error_.has_value());
        RUVIA_CHECK(result_value.has_te_);
    }

    for (const std::string_view invalid :
        {",trailers", "trailers,", "trailers,,gzip", "chunked", "trailers;q=0.5", "gzip;q=1.001",
            "gzip;q=\"0.5\"", "gzip;q =0.5", "gzip;q= 0.5", "gzip; q = 0.5", "gzip;level=1",
            "gzip;q=0.5;level=1", "gzip; q", "gzip:q=0.5", "bad token"}) {
        const auto result_value = parse(
            std::string("GET / HTTP/1.1\r\nHost: x\r\nTE: ") + std::string(invalid) + "\r\n\r\n");
        RUVIA_CHECK(result_value.error_ == http_parse_error::invalid_header);
    }
}

RUVIA_TEST(header_block_rejects_duplicate_content_type) {
    // Content-Type is a singleton representation header; accepting duplicates
    // lets body helpers observe only the cached last value.
    const auto result_value = parse(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Type: application/json\r\n\r\n");
    RUVIA_CHECK(result_value.error_ == http_parse_error::invalid_header);
}

RUVIA_TEST(header_block_rejects_duplicate_range) {
    // Ruvia file responses only support a single byte-range set. Repeated Range
    // fields must not degrade to cached last-value behavior.
    const auto result_value = parse(
        "GET /file HTTP/1.1\r\nHost: x\r\n"
        "Range: bytes=0-99\r\n"
        "Range: bytes=200-299\r\n\r\n");
    RUVIA_CHECK(result_value.error_ == http_parse_error::invalid_header);
}

RUVIA_TEST(header_block_validates_trailer_field_names) {
    RUVIA_CHECK(
        !parse("POST / HTTP/1.1\r\n"
               "Host: x\r\n"
               "Trailer: X-Checksum, X-Signature\r\n\r\n")
            .error_.has_value());
    RUVIA_CHECK(
        !parse("POST / HTTP/1.1\r\n"
               "Host: x\r\n"
               "Trailer: ,\r\n\r\n")
            .error_.has_value());
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\n"
                      "Host: x\r\n"
                      "Trailer: X-Checksum, bad field\r\n\r\n")
                    .error_ == http_parse_error::invalid_header);
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\n"
                      "Host: x\r\n"
                      "Trailer: Content-Length\r\n\r\n")
                    .error_ == http_parse_error::invalid_header);
}

RUVIA_TEST(header_block_accepts_repeated_etag_list_fields) {
    // If-None-Match is a list field, so repeated lines are equivalent to a
    // comma-joined value (RFC 9110 §5.3). The execution layer folds all lines.
    const auto result_value = parse(
        "GET /file HTTP/1.1\r\nHost: x\r\n"
        "If-None-Match: \"old\"\r\n"
        "If-None-Match: \"new\"\r\n\r\n");
    RUVIA_CHECK(!result_value.error_.has_value());
}

RUVIA_TEST(header_block_rejects_duplicate_auth_and_cors_singletons) {
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n"
                      "Authorization: Bearer first\r\n"
                      "Authorization: Bearer second\r\n\r\n")
                    .error_ == http_parse_error::invalid_header);
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n"
                      "Origin: https://a.example\r\n"
                      "Origin: https://b.example\r\n\r\n")
                    .error_ == http_parse_error::invalid_header);
    RUVIA_CHECK(parse("OPTIONS / HTTP/1.1\r\nHost: x\r\n"
                      "Access-Control-Request-Method: GET\r\n"
                      "Access-Control-Request-Method: POST\r\n\r\n")
                    .error_ == http_parse_error::invalid_header);
}

RUVIA_TEST(header_block_enforces_cors_request_field_grammar) {
    for (const auto value :
        {std::string_view(""), std::string_view("POST, DELETE"), std::string_view("POST /admin")}) {
        const auto result_value =
            parse(std::string("OPTIONS / HTTP/1.1\r\nHost: x\r\n") +
                  "Access-Control-Request-Method: " + std::string(value) + "\r\n\r\n");
        RUVIA_CHECK(result_value.error_ == http_parse_error::invalid_header);
    }

    for (const auto value :
        {std::string_view(""), std::string_view(", ,"), std::string_view("X-Good, X Bad")}) {
        const auto result_value =
            parse(std::string("OPTIONS / HTTP/1.1\r\nHost: x\r\n") +
                  "Access-Control-Request-Headers: " + std::string(value) + "\r\n\r\n");
        RUVIA_CHECK(result_value.error_ == http_parse_error::invalid_header);
    }

    for (const auto value : {std::string_view("*"), std::string_view("https://app.example/"),
             std::string_view("https://APP.example")}) {
        const auto result_value = parse(
            std::string("GET / HTTP/1.1\r\nHost: x\r\nOrigin: ") + std::string(value) + "\r\n\r\n");
        RUVIA_CHECK(result_value.error_ == http_parse_error::invalid_header);
    }

    RUVIA_CHECK(
        !parse("OPTIONS / HTTP/1.1\r\nHost: x\r\n"
               "Origin: https://first.example https://second.example\r\n"
               "Access-Control-Request-Method: PATCH\r\n"
               "Access-Control-Request-Headers: , X-One,, X-Two,\r\n\r\n")
            .error_.has_value());
    RUVIA_CHECK(!parse("GET / HTTP/1.1\r\nHost: x\r\nOrigin: null\r\n\r\n").error_.has_value());
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n"
                      "Origin: https://app.example:65536\r\n\r\n")
                    .error_ == http_parse_error::invalid_header);
}

RUVIA_TEST(header_block_rejects_invalid_bracketed_host_literal) {
    RUVIA_CHECK(!parse("GET / HTTP/1.1\r\nHost: [::1]\r\n\r\n").error_.has_value());
    RUVIA_CHECK(
        parse("GET / HTTP/1.1\r\nHost: [::::]\r\n\r\n").error_ == http_parse_error::invalid_host);
}

RUVIA_TEST(header_block_accepts_transfer_encoding_chunked) {
    const auto result_value = parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(!result_value.error_.has_value());
    RUVIA_CHECK(result_value.saw_chunked_);
}

RUVIA_TEST(header_block_rejects_transfer_coding_parameters) {
    for (const auto value :
        {std::string_view("chunked;note=\"a,b\""), std::string_view("gzip;level=9, chunked")}) {
        const auto result_value = parse(std::string("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ") +
                                        std::string(value) + "\r\n\r\n");
        RUVIA_CHECK(result_value.error_ == http_parse_error::invalid_transfer_encoding);
    }
}

RUVIA_TEST(header_block_accepts_one_transfer_coding_before_final_chunked) {
    // RFC 9112 §6.1 explicitly defines gzip, chunked: the content is compressed
    // first and chunked last for message framing.
    const auto gzip_chunked = parse(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n");
    RUVIA_CHECK(!gzip_chunked.error_.has_value());
    RUVIA_CHECK(gzip_chunked.saw_chunked_);
    RUVIA_CHECK_EQ(gzip_chunked.transfer_coding_count_, std::size_t{1});
    RUVIA_CHECK(gzip_chunked.first_transfer_coding_ == ruvia::http_transfer_coding::gzip);

    const auto split = parse(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: deflate\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(!split.error_.has_value());
    RUVIA_CHECK(split.saw_chunked_);
    RUVIA_CHECK_EQ(split.transfer_coding_count_, std::size_t{1});
    RUVIA_CHECK(split.first_transfer_coding_ == ruvia::http_transfer_coding::deflate);

    // A lone non-chunked coding is recorded here, then rejected by the request-level
    // body planner because request Transfer-Encoding requires final chunked framing.
    const auto lone = parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n");
    RUVIA_CHECK(!lone.error_.has_value());
    RUVIA_CHECK(!lone.saw_chunked_);
    RUVIA_CHECK_EQ(lone.transfer_coding_count_, std::size_t{1});
    RUVIA_CHECK(lone.first_transfer_coding_ == ruvia::http_transfer_coding::gzip);
}

RUVIA_TEST(header_block_rejects_smuggling_transfer_encodings) {
    // "chunked" must be the final coding.
    RUVIA_CHECK(
        parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, gzip\r\n\r\n").error_ ==
        http_parse_error::invalid_transfer_encoding);
    // A second coding after chunked (here a repeated header) is malformed.
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
                      "Transfer-Encoding: chunked\r\n\r\n")
                    .error_ == http_parse_error::invalid_transfer_encoding);
    // Unknown codings remain recorded for request-level framing precedence.
    const auto unknown =
        parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: bogus\r\n\r\n");
    RUVIA_CHECK(!unknown.error_.has_value());
    RUVIA_CHECK(unknown.transfer_coding_unsupported_);
    const auto parameterized_unknown = parse(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: bogus; level=1\r\n\r\n");
    RUVIA_CHECK(!parameterized_unknown.error_.has_value());
    RUVIA_CHECK(parameterized_unknown.transfer_coding_unsupported_);
    // Invalid transfer-coding grammar is a malformed request, not an unknown
    // extension that merits 501.
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: g@zip\r\n\r\n").error_ ==
                http_parse_error::invalid_transfer_encoding);
    // Multiple supported codings before final chunked framing preserve order.
    const auto stacked = parse(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, deflate, chunked\r\n\r\n");
    RUVIA_CHECK(!stacked.error_.has_value());
    RUVIA_CHECK(stacked.saw_chunked_);
    RUVIA_CHECK_EQ(stacked.transfer_coding_count_, std::size_t{2});
    RUVIA_CHECK(stacked.first_transfer_coding_ == http_transfer_coding::gzip);
    // Empty transfer-coding list items are malformed in this framing-sensitive header.
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ,chunked\r\n\r\n").error_ ==
                http_parse_error::invalid_transfer_encoding);
}

RUVIA_TEST(transfer_encoding_fields_preserve_duplicate_order) {
    http_transfer_encoding_state state;
    RUVIA_CHECK(state.parse_field("gzip") == http_transfer_encoding_parse_status::ok);
    RUVIA_CHECK(state.parse_field("gzip, deflate, chunked") == http_transfer_encoding_parse_status::ok);
    const auto& value = state.value();
    RUVIA_CHECK(value.has_value());
    if (value.has_value() && value->final_chunked() != nullptr) {
        const auto& codings = value->final_chunked()->transfer_codings().values_;
        RUVIA_CHECK_EQ(codings.size(), std::size_t{3});
        RUVIA_CHECK(codings[0] == http_transfer_coding::gzip);
        RUVIA_CHECK(codings[1] == http_transfer_coding::gzip);
        RUVIA_CHECK(codings[2] == http_transfer_coding::deflate);
    }
}

RUVIA_TEST(transfer_encoding_layer_limit_is_bounded_and_field_update_is_transactional) {
    http_transfer_encoding_state state;
    for (std::size_t index = 0; index < ruvia::max_transfer_codings; ++index) {
        RUVIA_CHECK(state.parse_field("gzip") == http_transfer_encoding_parse_status::ok);
    }
    RUVIA_CHECK(state.parse_field("gzip, chunked,") == http_transfer_encoding_parse_status::malformed);
    const auto& value = state.value();
    RUVIA_CHECK(value.has_value());
    if (value.has_value() && value->non_chunked() != nullptr) {
        RUVIA_CHECK_EQ(value->non_chunked()->transfer_codings().values_.size(),
            ruvia::max_transfer_codings);
    }
}

RUVIA_TEST(transfer_encoding_unknown_then_malformed_across_fields_is_malformed) {
    const auto result_value = parse(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: unknown; level=1\r\n"
        "Transfer-Encoding: deflate, chunked, gzip\r\n\r\n");
    RUVIA_CHECK(result_value.error_ == http_parse_error::invalid_transfer_encoding);
}

RUVIA_TEST(header_block_content_length_edge_cases) {
    // OWS around the value is trimmed.
    const auto ows = parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length:   42  \r\n\r\n");
    RUVIA_CHECK(!ows.error_.has_value());
    RUVIA_CHECK_EQ(ows.content_length_, std::size_t{42});
    // Leading zeros parse to the same numeric value (no desync).
    const auto zeros = parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 007\r\n\r\n");
    RUVIA_CHECK(!zeros.error_.has_value());
    RUVIA_CHECK_EQ(zeros.content_length_, std::size_t{7});
    // A '+' sign, a hex form, and overflow are all rejected rather than wrapped.
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\n").error_ ==
                http_parse_error::invalid_content_length);
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0x10\r\n\r\n").error_ ==
                http_parse_error::invalid_content_length);
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n"
                      "Content-Length: 99999999999999999999999999\r\n\r\n")
                    .error_ == http_parse_error::invalid_content_length);
}

RUVIA_TEST(find_http_header_end_incremental_search) {
    const std::string_view req = "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n";
    // Total size is 38 bytes. Delimiter \r\n\r\n is at indices 34, 35, 36, 37.
    // If the server read previousBufferLength bytes (anywhere from 0 up to 37),
    // and then more bytes arrive so the buffer now contains all 38 bytes,
    // find_http_header_end(req, previousBufferLength) MUST find the header end at 38!
    for (std::size_t offset = 0; offset < req.size(); ++offset) {
        RUVIA_CHECK_EQ(find_http_header_end(req, offset), req.size());
    }
}

RUVIA_TEST(find_http_header_end_respects_fragment_bounds) {
    RUVIA_CHECK_EQ(find_http_header_end({}, 0), std::string_view::npos);
    for (std::size_t length = 0; length < 65; ++length) {
        std::string wire(length, 'x');
        if (length > 3) {
            wire[length / 2] = '\n';
        }
        wire.push_back('\0');
        wire.append("\r\n\r\n");
        for (std::size_t split = 0; split < wire.size(); ++split) {
            RUVIA_CHECK_EQ(find_http_header_end(std::string_view(wire).substr(0, split), 0), std::string_view::npos);
            RUVIA_CHECK_EQ(find_http_header_end(wire, split), wire.size());
        }
    }
}

RUVIA_TEST(find_http_header_end_selects_next_pipeline_message) {
    const std::string_view request = "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n";
    const std::string wire = std::string(request) + std::string(request);
    for (std::size_t offset = 0; offset < wire.size(); ++offset) {
        RUVIA_CHECK_EQ(find_http_header_end(wire, offset), offset < request.size() ? request.size() : wire.size());
    }
    RUVIA_CHECK_EQ(find_http_header_end(wire, wire.size()), std::string_view::npos);
    RUVIA_CHECK_EQ(find_http_header_end(wire, std::numeric_limits<std::size_t>::max()), std::string_view::npos);
}

RUVIA_TEST(find_http_header_end_enforces_header_byte_limit) {
    const std::string within_limit = std::string(ruvia::max_http_header_bytes - 4, 'x') + "\r\n\r\n";
    RUVIA_CHECK_EQ(find_http_header_end(within_limit, 0), ruvia::max_http_header_bytes);
    RUVIA_CHECK_EQ(find_http_header_end(within_limit, within_limit.size() - 1), ruvia::max_http_header_bytes);
    const std::string with_body = within_limit + "body\r\n\r\n";
    RUVIA_CHECK_EQ(find_http_header_end(with_body, 0), ruvia::max_http_header_bytes);
    RUVIA_CHECK_EQ(find_http_header_end(with_body, within_limit.size()), std::string_view::npos);
    const std::string over_limit = "x" + within_limit;
    RUVIA_CHECK_EQ(find_http_header_end(over_limit, 0), std::string_view::npos);
}
