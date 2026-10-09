#pragma once

#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http1_request_body_plan.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_body_failure.h"
#include "ruvia/http/http_request_content_decoding.h"
#include "ruvia/http/http_transfer_coding.h"
#include "ruvia/http/http_transfer_coding_decoder.h"
#include "ruvia/http/protocol_byte_limit.h"

#include "test_harness.h"

namespace content_decoding_test {

using ruvia::decode_http_content;
using ruvia::decode_http_request_content;
using ruvia::encode_http_content;
using ruvia::http1_chunked_body_decoder;
using ruvia::http1_request_body_plan;
using ruvia::http1_server_request_parser;
using ruvia::http_content_coding;
using ruvia::http_content_decode_error;
using ruvia::http_content_decode_failure;
using ruvia::http_content_decode_result;
using ruvia::http_content_encode_error;
using ruvia::http_content_encode_failure;
using ruvia::http_content_encode_options;
using ruvia::http_content_encode_result;
using ruvia::http_decoded_content;
using ruvia::http_encoded_content;
using ruvia::http_request_content_decode_protocol_failure;
using ruvia::http_request_content_decode_result;
using ruvia::http_request_content_decoder_failure;
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

class reject_large_allocation_resource final : public std::pmr::memory_resource {
public:
    explicit reject_large_allocation_resource(std::size_t maximum_block_bytes)
        : maximum_block_bytes_(maximum_block_bytes) {}

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (bytes_value > maximum_block_bytes_) {
            throw std::bad_alloc();
        }
        return std::pmr::get_default_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::get_default_resource()->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t maximum_block_bytes_;
};

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
    http_transfer_coding_stack_decoder& decoder, std::string_view input, std::pmr::string& output) {
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
    auto result_value = decode_http_content(
        coding, input, {.max_decoded_bytes_ = max_bytes, .resource_ = std::pmr::get_default_resource()});
    const auto* content = result_value.decoded();
    if (content == nullptr) {
        throw std::runtime_error("test content decode failed");
    }
    return std::string(content->bytes());
}

inline http_content_decode_error decode_error(
    http_content_coding coding, std::string_view input, std::size_t max_bytes = decoded_body_limit) {
    const auto result_value = decode_http_content(
        coding, input, {.max_decoded_bytes_ = max_bytes, .resource_ = std::pmr::get_default_resource()});
    const auto* failure = result_value.failure();
    if (failure == nullptr) {
        throw std::runtime_error("test content decode unexpectedly succeeded");
    }
    return failure->error();
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

    const auto result_value = ruvia::decode_http_content(http_content_coding::zstd, compressed,
        {.max_decoded_bytes_ = ruvia::default_max_buffered_body_bytes,
            .resource_ = std::pmr::get_default_resource()});
    const auto* decoded = result_value.decoded();
    if (decoded == nullptr) {
        return std::nullopt;
    }
    return std::string(decoded->bytes());
}

}  // namespace content_decoding_test

using namespace content_decoding_test;  // NOLINT(google-build-using-namespace)
