#pragma once

#include <brotli/encode.h>
#include <gtest/gtest.h>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"

namespace ruvia::test {

using ruvia::decode_http_content;
using ruvia::encode_http_content;
using ruvia::http1_chunked_body_decoder;
using ruvia::http1_request_body_plan;
using ruvia::http_content_coding;
using ruvia::http_content_decode_error;
using ruvia::http_transfer_coding;
using ruvia::http_transfer_coding_decode_failure;
using ruvia::http_transfer_coding_decode_need_input;
using ruvia::http_transfer_coding_decode_output_view;
using ruvia::http_transfer_coding_decode_result;
using ruvia::http_transfer_coding_decoder_failure;
using ruvia::http_transfer_coding_stack_decoder;
using ruvia::http_transfer_codings;
using ruvia::http_unsupported_expectation_policy;
using ruvia::parse_http_content_coding;
using ruvia::protocol_byte_limit;

inline constexpr std::size_t decoded_body_limit = 16 * 1024 * 1024;

inline std::string zlib_deflate_compress(std::string_view data) {
    z_stream stream{};
    if (deflateInit(&stream, Z_BEST_COMPRESSION) != Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    std::string output(data.size() + 256, '\0');
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    const int status = deflate(&stream, Z_FINISH);
    const auto written = stream.total_out;
    deflateEnd(&stream);
    if (status != Z_STREAM_END) {
        return {};
    }
    output.resize(written);
    return output;
}

inline std::string gzip_compress(std::string_view data) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    std::string out;
    char buffer[16384];
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        status = deflate(&stream, Z_FINISH);
        out.append(buffer, sizeof(buffer) - stream.avail_out);
    } while (status == Z_OK);
    (void)deflateEnd(&stream);
    return out;
}

struct transfer_decode_observation final {
    bool failed_{false};
    std::optional<ruvia::http_protocol_error> protocol_error_;
};

inline transfer_decode_observation append_transfer_decoded(
    http_transfer_coding_stack_decoder& decoder, std::string_view input, std::string& output) {
    std::array<char, std::size_t{8} * 1024> window{};
    for (;;) {
        const auto result_value = decoder.decode(input, window);
        input.remove_prefix(std::min(input.size(), result_value.consumed_bytes()));
        if (const auto* decoded = result_value.output()) {
            output.append(decoded->bytes());
            continue;
        }
        if (const auto* failure = result_value.failure()) {
            return {true, ruvia::http_request_transfer_coding_error(failure->error())};
        }
        if (result_value.decoder_failure() != nullptr) {
            return {true, std::nullopt};
        }
        return {};
    }
}

inline std::string brotli_compress(std::string_view data) {
    std::size_t bound = BrotliEncoderMaxCompressedSize(data.size());
    if (bound == 0) {
        bound = data.size() + 1024;
    }
    std::string out(bound, '\0');
    std::size_t out_size = bound;
    if (BrotliEncoderCompress(BROTLI_DEFAULT_QUALITY, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
            data.size(), reinterpret_cast<const std::uint8_t*>(data.data()), &out_size,
            reinterpret_cast<std::uint8_t*>(out.data())) != BROTLI_TRUE) {
        return {};
    }
    out.resize(out_size);
    return out;
}

inline std::string zstd_compress(std::string_view data) {
    const std::size_t bound = ZSTD_compressBound(data.size());
    std::string out(bound, '\0');
    const std::size_t size = ZSTD_compress(out.data(), bound, data.data(), data.size(), 3);
    if (ZSTD_isError(size)) {
        return {};
    }
    out.resize(size);
    return out;
}

inline std::string zstd_compress_with_window(std::string_view data, int window_log) {
    auto* context_value = ZSTD_createCCtx();
    if (context_value == nullptr) {
        return {};
    }
    struct guard final {
        ZSTD_CCtx* context_;
        ~guard() {
            ZSTD_freeCCtx(context_);
        }
    } guard_value{context_value};
    if (ZSTD_isError(ZSTD_CCtx_setParameter(context_value, ZSTD_c_windowLog, window_log)) != 0 ||
        ZSTD_isError(ZSTD_CCtx_setParameter(context_value, ZSTD_c_contentSizeFlag, 0)) != 0) {
        return {};
    }
    std::string output(ZSTD_compressBound(data.size()), '\0');
    const auto size =
        ZSTD_compress2(context_value, output.data(), output.size(), data.data(), data.size());
    if (ZSTD_isError(size) != 0) {
        return {};
    }
    output.resize(size);
    return output;
}

inline std::string decoded(http_content_coding coding, std::string_view input, std::size_t max_bytes) {
    auto result_value = decode_http_content(std::array{coding}, input,
        {.max_decoded_bytes_ = max_bytes});
    auto* content = std::get_if<std::string>(&result_value);
    if (content == nullptr) {
        throw std::runtime_error("test content decode failed");
    }
    return std::move(*content);
}

inline http_content_decode_error decode_error(
    http_content_coding coding, std::string_view input, std::size_t max_bytes = decoded_body_limit) {
    const auto result_value = decode_http_content(std::array{coding}, input,
        {.max_decoded_bytes_ = max_bytes});
    const auto* failure = std::get_if<http_content_decode_error>(&result_value);
    if (failure == nullptr) {
        throw std::runtime_error("test content decode unexpectedly succeeded");
    }
    return *failure;
}

inline std::string chunked(std::string_view body) {
    char size[2 * sizeof(std::size_t)];
    const auto [end, ec] = std::to_chars(size, size + sizeof(size), body.size(), 16);
    if (ec != std::errc{}) {
        return {};
    }
    std::string wire(size, end);
    wire.append("\r\n");
    wire.append(body);
    wire.append("\r\n0\r\n\r\n");
    return wire;
}

inline std::optional<std::string> zstd_round_trip(std::string_view plain, std::size_t truncate_by) {
    const std::size_t bound = ZSTD_compressBound(plain.size());
    std::string compressed(bound, '\0');
    const std::size_t written =
        ZSTD_compress(compressed.data(), compressed.size(), plain.data(), plain.size(), 3);
    if (ZSTD_isError(written) != 0 || written <= truncate_by) {
        return std::nullopt;
    }
    compressed.resize(written - truncate_by);

    auto result_value = ruvia::decode_http_content(std::array{http_content_coding::zstd}, compressed,
        {.max_decoded_bytes_ = ruvia::default_max_buffered_body_bytes});
    auto* decoded = std::get_if<std::string>(&result_value);
    if (decoded == nullptr) {
        return std::nullopt;
    }
    return std::move(*decoded);
}

}  // namespace ruvia::test

namespace ruvia::test {

// One-shot whole-message view of a buffered request over the public server
// contract: parse_head frames the head, and the body plan drives either a
// Content-Length slice or http1_chunked_body_decoder over the rest.
struct http1_request_message final {
    http1_server_request_parse_state state_;
    // Wire bytes of the whole message once it is complete (head, chunk lines
    // and trailers included).
    std::optional<std::size_t> message_bytes_{};
    // Decoded body of a complete message: the Content-Length bytes or the
    // concatenated chunk payloads.
    std::string body_{};
    std::optional<http1_chunk_decode_error> chunk_error_{};

    [[nodiscard]] bool complete() const noexcept {
        return message_bytes_.has_value();
    }
    // Head accepted, body still incomplete.
    [[nodiscard]] bool need_body() const noexcept {
        return state_.head_ready() != nullptr && !message_bytes_.has_value() && !chunk_error_.has_value();
    }
};

[[nodiscard]] inline http1_request_message parse_http1_request_message(
    std::string_view buffer) {
    http1_request_message message;
    message.state_.parse_head(buffer, 0);
    const auto* head = message.state_.head_ready();
    if (head == nullptr) {
        return message;
    }
    const auto header_bytes = head->header_bytes();
    if (const auto* known_length = message.state_.body_plan().known_length()) {
        if (buffer.size() - header_bytes < known_length->content_length()) {
            return message;
        }
        message.body_.assign(buffer.substr(header_bytes, known_length->content_length()));
        message.message_bytes_ = header_bytes + known_length->content_length();
        return message;
    }
    if (message.state_.body_plan().chunked() == nullptr) {
        message.message_bytes_ = header_bytes;
        return message;
    }
    http1_chunked_body_decoder decoder;
    auto consumed = header_bytes;
    for (;;) {
        const auto result_value = decoder.decode(buffer.substr(consumed));
        if (const auto* chunk = result_value.body_chunk()) {
            message.body_.append(chunk->bytes());
            consumed += chunk->consumed_bytes();
            continue;
        }
        if (const auto* done = result_value.complete()) {
            message.message_bytes_ = consumed + done->consumed_bytes();
        } else if (const auto* failure = result_value.failure()) {
            message.chunk_error_ = failure->error();
        }
        return message;
    }
}

}  // namespace ruvia::test

namespace ruvia::test {

using ruvia::http1_client_request_content_completion_status;
using ruvia::http1_client_request_wire_policy;
using ruvia::http1_client_response_parse_error;
using ruvia::http1_client_response_parse_result;
using ruvia::http1_client_response_parser;
using ruvia::http1_close_policy;
using ruvia::http1_parsed_client_response_head;
using ruvia::http_client_request_content_signal;
using ruvia::http_client_response_head;
using ruvia::http_protocol_version;

using http1_client_request_prepare_result = std::variant<ruvia::prepared_http1_client_request,
    ruvia::http1_client_request_buffer_too_small, ruvia::http1_client_request_prepare_error>;

// A prepared request feeds exactly one response parser, which takes its
// exchange state; a test that drives several parsers from the same request
// prepares it again for each of them.
inline http1_client_response_parser exchange_parser(
    http1_client_request_prepare_result prepared_result) {
    auto* prepared = std::get_if<ruvia::prepared_http1_client_request>(&prepared_result);
    if (prepared == nullptr) {
        throw std::runtime_error("test request could not be prepared");
    }
    return http1_client_response_parser(std::move(*prepared).take_exchange_state());
}

inline http1_client_response_parser exchange_parser(const ruvia::http_origin_view& origin,
    const ruvia::http_client_request_view& request, http1_client_request_wire_policy policy = {}) {
    std::array<char, 2048> request_head;
    return exchange_parser(
        ruvia::http1_client_request_writer().prepare(origin, request, request_head, policy));
}

inline http1_client_response_parser exchange_parser(const ruvia::http_origin_view& origin,
    const ruvia::http1_client_request_head_view& request, http1_client_request_wire_policy policy = {}) {
    std::array<char, 2048> request_head;
    return exchange_parser(
        ruvia::http1_client_request_writer().prepare_streaming(origin, request, request_head, policy));
}

inline http1_client_response_parse_result parse_wire(std::string_view method, std::string_view wire,
    http1_close_policy close_policy = http1_close_policy::allow_reuse,
    std::span<const ruvia::http_header_view> request_headers = {}) {
    const auto origin = ruvia::http_origin_view::https({.host_ = "example.test"});
    const http1_client_request_wire_policy policy{.close_policy_ = close_policy};
    if (method == "CONNECT") {
        std::array<char, 2048> request_head;
        return exchange_parser(ruvia::http1_client_request_writer().prepare_connect(
                                   "example.test:443", request_headers, request_head, policy))
            .parse(wire);
    }
    ruvia::http_client_request_view request;
    request.method_ = method;
    request.headers_ = request_headers;
    return exchange_parser(origin, request, policy).parse(wire);
}

inline http1_client_response_parse_result parse_result(std::string_view method,
    std::string_view header_section, http1_close_policy close_policy = http1_close_policy::allow_reuse,
    std::span<const ruvia::http_header_view> request_headers = {}) {
    std::string wire(header_section);
    wire.append("\r\n\r\n");
    return parse_wire(method, wire, close_policy, request_headers);
}

inline http1_parsed_client_response_head parse_head(std::string_view method,
    std::string_view header_section, http1_close_policy close_policy = http1_close_policy::allow_reuse,
    std::span<const ruvia::http_header_view> request_headers = {}) {
    auto result_value = parse_result(method, header_section, close_policy, request_headers);
    auto* parsed_value = result_value.parsed();
    if (parsed_value == nullptr) {
        throw std::runtime_error("test expected a parsed HTTP/1 response head");
    }
    return std::move(*parsed_value);
}

struct parsed_response final {
    http_client_response_head head_;
};

inline parsed_response parse_response(std::string_view method, std::string_view header_section) {
    auto head = parse_head(method, header_section);
    return parsed_response{std::move(head).take_head()};
}

inline bool parse_fails(std::string_view method, std::string_view header_section,
    http1_close_policy close_policy = http1_close_policy::allow_reuse,
    std::span<const ruvia::http_header_view> request_headers = {}) {
    const auto result_value = parse_result(method, header_section, close_policy, request_headers);
    return result_value.failure() != nullptr;
}

inline const ruvia::http1_client_known_length_response& require_known_length(
    const ruvia::http1_client_response_plan& plan) {
    const auto* known_length = plan.known_length();
    if (known_length == nullptr) {
        throw std::runtime_error("test expected exact-length response framing");
    }
    return *known_length;
}

inline const ruvia::http1_client_chunked_response& require_chunked(
    const ruvia::http1_client_response_plan& plan) {
    const auto* chunked = plan.chunked();
    if (chunked == nullptr) {
        throw std::runtime_error("test expected chunked response framing");
    }
    return *chunked;
}

inline const ruvia::http1_client_close_delimited_response& require_close_delimited(
    const ruvia::http1_client_response_plan& plan) {
    const auto* close_delimited = plan.close_delimited();
    if (close_delimited == nullptr) {
        throw std::runtime_error("test expected close-delimited response framing");
    }
    return *close_delimited;
}

inline const ruvia::http1_client_response_without_content& require_without_content(
    const ruvia::http1_client_response_plan& plan) {
    const auto* without_content = plan.without_content();
    if (without_content == nullptr) {
        throw std::runtime_error("test expected a final response without content");
    }
    return *without_content;
}

inline std::size_t active_plan_alternative_count(const ruvia::http1_client_response_plan& plan) noexcept {
    return static_cast<std::size_t>(plan.informational() != nullptr) +
           static_cast<std::size_t>(plan.without_content() != nullptr) +
           static_cast<std::size_t>(plan.known_length() != nullptr) +
           static_cast<std::size_t>(plan.chunked() != nullptr) +
           static_cast<std::size_t>(plan.close_delimited() != nullptr) +
           static_cast<std::size_t>(plan.connect_tunnel() != nullptr) +
           static_cast<std::size_t>(plan.protocol_upgrade() != nullptr);
}

inline http1_client_response_parse_error parse_failure_error(std::string_view method,
    std::string_view header_section, std::span<const ruvia::http_header_view> request_headers = {}) {
    const auto result_value =
        parse_result(method, header_section, http1_close_policy::allow_reuse, request_headers);
    const auto* failure = result_value.failure();
    if (failure == nullptr) {
        throw std::runtime_error("test expected an HTTP/1 response parse failure");
    }
    return *failure;
}

}  // namespace ruvia::test

namespace ruvia::test {

// A multipart/form-data Content-Type whose boundary parameter is `value` as a
// quoted-string, so every byte reaches the boundary rule unchanged.
[[nodiscard]] inline std::string multipart_boundary_content_type(std::string_view value) {
    std::string content_type("multipart/form-data; boundary=\"");
    for (const char byte : value) {
        if (byte == '"' || byte == '\\') {
            content_type.push_back('\\');
        }
        content_type.push_back(byte);
    }
    content_type.push_back('"');
    return content_type;
}

// Builds a boundary through parse_multipart_boundary, the Content-Type route.
[[nodiscard]] inline ruvia::multipart_boundary make_multipart_boundary(std::string_view value) {
    const auto parsed = ruvia::parse_multipart_boundary(multipart_boundary_content_type(value));
    if (const auto* boundary = parsed.boundary()) {
        return *boundary;
    }
    throw std::invalid_argument("invalid test multipart boundary");
}

}  // namespace ruvia::test

#if defined(RUVIA_ENABLE_HTTP2)

namespace ruvia::test {

// Replaces `into` with every pending outbound frame through the public batch
// drain. A single unbounded batch moves the whole buffer; on copy failure the
// connection keeps its bytes and the exception propagates.
inline void drain_output(ruvia::http2_connection& connection, std::string& into) {
    into.clear();
    (void)connection.take_output_batch(std::numeric_limits<std::size_t>::max(), into);
}

// Collects every pending outbound frame into an owning string so a test can
// feed, parse or compare the wire bytes.
inline std::string drain_output(ruvia::http2_connection& connection) {
    std::string batch;
    drain_output(connection, batch);
    return batch;
}

// Moves bytes between two connections. feed() input must outlive the events it
// produces, so every drained string is retained for the relay's lifetime.
class output_relay final {
public:
    [[nodiscard]] ruvia::http2_feed_result feed(
        ruvia::http2_connection& from, ruvia::http2_connection& to) {
        return to.feed(retained_.emplace_back(drain_output(from)));
    }

    void transfer(ruvia::http2_connection& from, ruvia::http2_connection& to) {
        const auto status = feed(from, to);
        if (status != ruvia::http2_feed_result::accepted) {
            throw std::runtime_error(
                "HTTP/2 transfer failed, status " + std::to_string(static_cast<int>(status)));
        }
    }

private:
    std::deque<std::string> retained_;
};

}  // namespace ruvia::test

#endif

#ifndef RUVIA_TEST_EXCEPTIONS_DEFINED
#define RUVIA_TEST_EXCEPTIONS_DEFINED

namespace ruvia::test {

template <typename fn_type>
[[nodiscard]] inline bool throws_on(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

}  // namespace ruvia::test

#endif
