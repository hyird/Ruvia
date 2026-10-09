#include <array>

#include "ruvia/http/http1_request_parser.h"

#include "content_decoding_fixture.h"

// Decoding a request body: what each coding accepts and what it refuses.

RUVIA_TEST(request_body_failures_own_cross_runtime_http_errors) {
    const auto too_large =
        ruvia::http_request_body_size_failure(5, protocol_byte_limit::limited(4));
    RUVIA_CHECK(too_large.has_value());
    if (too_large) {
        const auto error = too_large->protocol_error();
        RUVIA_CHECK_EQ(error.status(), ruvia::http_status::content_too_large);
        RUVIA_CHECK_EQ(
            std::string_view(error.what()), std::string_view("request body is too large"));
    }
    RUVIA_CHECK(
        !ruvia::http_request_body_addition_failure(2, 2, protocol_byte_limit::limited(4)));
    RUVIA_CHECK(ruvia::http_request_body_addition_failure(2, 3, protocol_byte_limit::limited(4))
            .has_value());

    const auto incomplete = ruvia::http_request_body_failure::incomplete().protocol_error();
    RUVIA_CHECK_EQ(incomplete.status(), ruvia::http_status::bad_request);
    RUVIA_CHECK_EQ(
        std::string_view(incomplete.what()), std::string_view("incomplete request body"));
}

RUVIA_TEST(http1_public_parser_classifies_cleartext_request_line_failures) {
    const ruvia::http1_request_parser parser;
    constexpr std::string_view invalid_request_line = "GET / NOT-HTTP\r\n\r\n";
    const auto parsed_line = parser.parse(invalid_request_line);
    const auto* line_failure = parsed_line.failure();
    RUVIA_CHECK(line_failure != nullptr);
    if (line_failure != nullptr) {
        RUVIA_CHECK(line_failure->source() ==
                    ruvia::http1_request_parse_failure_source::request_line);
        RUVIA_CHECK(ruvia::should_drop_invalid_cleartext_http1_input(
            invalid_request_line, line_failure->source()));
    }

    const auto malformed_header = parser.parse("GET / HTTP/1.1\r\nBad Header\r\n\r\n");
    const auto* message_failure = malformed_header.failure();
    RUVIA_CHECK(message_failure != nullptr);
    if (message_failure != nullptr) {
        RUVIA_CHECK(message_failure->source() ==
                    ruvia::http1_request_parse_failure_source::message);
        RUVIA_CHECK(!ruvia::should_drop_invalid_cleartext_http1_input(
            "GET / HTTP/1.1\r\nBad Header\r\n\r\n",
            message_failure->source()));
    }
}

RUVIA_TEST(http1_request_body_plan_has_one_framing_truth) {
    http1_server_request_parser parser;
    const auto none_state = parser.parse_message("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto& none = none_state.body_plan_;
    RUVIA_CHECK(none.without_body() != nullptr);
    RUVIA_CHECK(none.known_length() == nullptr);
    RUVIA_CHECK(none.chunked() == nullptr);
    RUVIA_CHECK(!none.requires_consumption());

    const auto empty_length_state = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\nContent-Length: 0\r\n\r\n");
    const auto& empty_length = empty_length_state.body_plan_;
    const auto* known_length = empty_length.known_length();
    RUVIA_CHECK(known_length != nullptr);
    RUVIA_CHECK(empty_length.without_body() == nullptr);
    RUVIA_CHECK(empty_length.chunked() == nullptr);
    if (known_length != nullptr) {
        RUVIA_CHECK_EQ(known_length->content_length(), std::size_t{0});
    }
    RUVIA_CHECK(!empty_length.requires_consumption());
    RUVIA_CHECK(empty_length.expectations().has_continue());
    const auto empty_expectation_plan =
        empty_length.expectation_plan(http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(empty_expectation_plan.no_action() != nullptr);

    const auto compressed_chunked_state = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n");
    const auto& compressed_chunked = compressed_chunked_state.body_plan_;
    const auto* chunked_body = compressed_chunked.chunked();
    RUVIA_CHECK(chunked_body != nullptr);
    RUVIA_CHECK(compressed_chunked.without_body() == nullptr);
    RUVIA_CHECK(compressed_chunked.known_length() == nullptr);
    RUVIA_CHECK(compressed_chunked.requires_consumption());
    const auto compressed_expectation_plan =
        compressed_chunked.expectation_plan(http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(compressed_expectation_plan.send_continue() != nullptr);
    if (chunked_body != nullptr) {
        RUVIA_CHECK_EQ(chunked_body->transfer_codings().values_.size(), std::size_t{1});
    }
}

RUVIA_TEST(request_body_gzip_round_trip) {
    const std::string plain = "The quick brown fox jumps over the lazy dog";
    const std::string gz = gzip_compress(plain);
    RUVIA_CHECK(!gz.empty());
    RUVIA_CHECK_EQ(decoded(http_content_coding::gzip, gz, decoded_body_limit), plain);
}

RUVIA_TEST(request_body_gzip_bomb_rejected) {
    const std::string big(1u << 20, 'a');  // 1 MiB, compresses to a tiny gzip
    const std::string gz = gzip_compress(big);
    // A small cap must stop the expansion, not decode the whole megabyte.
    RUVIA_CHECK(decode_error(http_content_coding::gzip, gz, 1024) ==
                http_content_decode_error::decoded_size_exceeded);
}

RUVIA_TEST(request_body_gzip_truncated_rejected) {
    const std::string plain(4096, 'q');
    std::string gz = gzip_compress(plain);
    RUVIA_CHECK(gz.size() > 6);
    gz.resize(gz.size() - 6);  // cut into the gzip trailer -> incomplete stream
    RUVIA_CHECK(
        decode_error(http_content_coding::gzip, gz) == http_content_decode_error::invalid_content);
}

RUVIA_TEST(request_body_gzip_decodes_every_rfc1952_member) {
    const std::string first = gzip_compress("first-");
    const std::string second = gzip_compress("second");
    RUVIA_CHECK(!first.empty());
    RUVIA_CHECK(!second.empty());
    RUVIA_CHECK_EQ(decoded(http_content_coding::gzip, first + second, decoded_body_limit),
        std::string("first-second"));
}

RUVIA_TEST(request_body_gzip_rejects_bytes_after_the_last_member) {
    std::string encoded = gzip_compress("complete");
    encoded.append("not-a-gzip-member");
    RUVIA_CHECK(
        decode_error(http_content_coding::gzip, encoded) == http_content_decode_error::invalid_content);
}

RUVIA_TEST(request_body_brotli_round_trip) {
    const std::string plain = "permessage brotli body content, repeated repeated repeated";
    const std::string br = brotli_compress(plain);
    RUVIA_CHECK(!br.empty());
    RUVIA_CHECK_EQ(decoded(http_content_coding::brotli, br, decoded_body_limit), plain);
}

RUVIA_TEST(request_body_brotli_bomb_rejected) {
    const std::string big(1u << 20, 'a');
    const std::string br = brotli_compress(big);
    RUVIA_CHECK(!br.empty());
    RUVIA_CHECK(decode_error(http_content_coding::brotli, br, 1024) ==
                http_content_decode_error::decoded_size_exceeded);
}

RUVIA_TEST(request_body_brotli_rejects_trailing_bytes) {
    std::string encoded = brotli_compress("complete");
    encoded.append("trailing");
    RUVIA_CHECK(decode_error(http_content_coding::brotli, encoded) ==
                http_content_decode_error::invalid_content);
}

RUVIA_TEST(request_body_zstd_round_trip) {
    const std::string plain = "zstd request body content, repeated repeated repeated repeated";
    const std::string zz = zstd_compress(plain);
    RUVIA_CHECK(!zz.empty());
    RUVIA_CHECK_EQ(decoded(http_content_coding::zstd, zz, decoded_body_limit), plain);
}

RUVIA_TEST(request_body_zstd_bomb_rejected) {
    const std::string big(1u << 20, 'a');  // 1 MiB, compresses to a tiny zstd frame
    const std::string zz = zstd_compress(big);
    RUVIA_CHECK(!zz.empty());
    // A small cap must stop the expansion mid-stream, not decode the whole megabyte.
    RUVIA_CHECK(decode_error(http_content_coding::zstd, zz, 1024) ==
                http_content_decode_error::decoded_size_exceeded);
}

RUVIA_TEST(request_body_zstd_decodes_every_rfc8878_frame) {
    const std::string first = zstd_compress("first-");
    const std::string second = zstd_compress("second");
    RUVIA_CHECK(!first.empty());
    RUVIA_CHECK(!second.empty());
    RUVIA_CHECK_EQ(decoded(http_content_coding::zstd, first + second, decoded_body_limit),
        std::string("first-second"));
}

RUVIA_TEST(request_body_zstd_rejects_bytes_after_the_last_frame) {
    std::string encoded = zstd_compress("complete");
    encoded.append("not-a-zstd-frame");
    RUVIA_CHECK(
        decode_error(http_content_coding::zstd, encoded) == http_content_decode_error::invalid_content);
}

RUVIA_TEST(http_request_content_decoder_owns_protocol_failure_status) {
    auto* resource = std::pmr::get_default_resource();

    constexpr std::array gzip{http_content_coding::gzip};
    constexpr std::array identity{http_content_coding::identity};
    constexpr std::array invalid_coding{static_cast<http_content_coding>(255)};
    const auto invalid = decode_http_request_content(
        gzip, "not-gzip", {.max_decoded_bytes_ = 1024, .resource_ = resource});
    RUVIA_CHECK(invalid.protocol_failure() != nullptr);
    RUVIA_CHECK(invalid.decoder_failure() == nullptr);
    RUVIA_CHECK_EQ(
        invalid.protocol_failure()->protocol_error().status(), ruvia::http_status::bad_request);

    const auto oversized = decode_http_request_content(
        identity, "too large", {.max_decoded_bytes_ = 4, .resource_ = resource});
    RUVIA_CHECK(oversized.protocol_failure() != nullptr);
    RUVIA_CHECK(oversized.decoder_failure() == nullptr);
    RUVIA_CHECK_EQ(oversized.protocol_failure()->protocol_error().status(),
        ruvia::http_status::content_too_large);

    const auto unsupported = decode_http_request_content(
        invalid_coding, {}, {.max_decoded_bytes_ = 1024, .resource_ = resource});
    RUVIA_CHECK(unsupported.protocol_failure() != nullptr);
    RUVIA_CHECK(unsupported.decoder_failure() == nullptr);
    RUVIA_CHECK_EQ(unsupported.protocol_failure()->protocol_error().status(),
        ruvia::http_status::unsupported_media_type);
}
