#include <zstd_errors.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <variant>

#include "http_compression_codec.h"
#include "http_content_codec.h"
#include "http_field_text.h"

namespace ruvia {

namespace {

struct native_decode_step final {
    std::size_t consumed_;
    std::size_t produced_;
    bool complete_;
    bool invalid_;
};

[[nodiscard]] native_decode_step decode_native(detail::zlib_inflate_stream& decoder, std::string_view input, std::span<char> output) {
    auto& stream = decoder.native();
    const detail::zlib_borrow_scope borrow(stream);
    const auto input_bytes = (std::min)(input.size(), static_cast<std::size_t>((std::numeric_limits<uInt>::max)()));
    const auto output_bytes = (std::min)(output.size(), static_cast<std::size_t>((std::numeric_limits<uInt>::max)()));
    stream.next_in = input_bytes == 0 ? Z_NULL : reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input_bytes);
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output_bytes);
    const int status = inflate(&stream, Z_NO_FLUSH);
    decoder.rethrow_allocation_failure();
    if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END && status != Z_DATA_ERROR && status != Z_NEED_DICT) {
        throw http_content_codec_error("zlib content decoding failed");
    }
    return {input_bytes - stream.avail_in, output_bytes - stream.avail_out, status == Z_STREAM_END, status == Z_DATA_ERROR || status == Z_NEED_DICT};
}

[[nodiscard]] native_decode_step decode_native(detail::brotli_decoder_stream& decoder, std::string_view input, std::span<char> output) {
    std::size_t available_input = input.size();
    const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t available_output = output.size();
    auto* next_output = reinterpret_cast<std::uint8_t*>(output.data());
    const auto status = BrotliDecoderDecompressStream(decoder.native(), &available_input, &next_input, &available_output, &next_output, nullptr);
    // Custom allocator exceptions take precedence over the protective native
    // ring-buffer cap, and never escape through the C ABI.
    decoder.rethrow_allocation_failure();
    return {input.size() - available_input, output.size() - available_output, status == BROTLI_DECODER_RESULT_SUCCESS, status == BROTLI_DECODER_RESULT_ERROR};
}

[[nodiscard]] native_decode_step decode_native(detail::zstd_decoder_stream& decoder, std::string_view input, std::span<char> output) {
    ZSTD_inBuffer in{input.data(), input.size(), 0};
    ZSTD_outBuffer out{output.data(), output.size(), 0};
    const auto status = ZSTD_decompressStream(decoder.native(), &out, &in);
    decoder.rethrow_allocation_failure();
    if (ZSTD_isError(status) != 0 && ZSTD_getErrorCode(status) == ZSTD_error_memory_allocation) {
        throw http_content_codec_error(ZSTD_getErrorName(status));
    }
    return {in.pos, out.pos, status == 0, ZSTD_isError(status) != 0};
}

}  // namespace

namespace detail {

http_content_decoder_stream::http_content_decoder_stream(http_content_coding coding, http_content_decode_options options) :
    coding_(coding),
    options_(options) {
    if (http_content_coding_token(coding).empty()) {
        throw std::invalid_argument("unsupported HTTP content coding");
    }
    initialize();
}

void http_content_decoder_stream::initialize() {
    switch (coding_) {
        case http_content_coding::gzip:
        case http_content_coding::deflate:
            stream_.emplace(std::in_place_type<detail::zlib_inflate_stream>, coding_ == http_content_coding::gzip ? detail::zlib_gzip_window_bits : detail::zlib_deflate_window_bits, options_.max_memory_bytes_, options_.memory_);
            break;
        case http_content_coding::brotli:
            stream_.emplace(std::in_place_type<detail::brotli_decoder_stream>, options_.max_decoded_bytes_, options_.max_memory_bytes_, options_.memory_);
            break;
        case http_content_coding::zstd:
            stream_.emplace(std::in_place_type<detail::zstd_decoder_stream>, options_.max_memory_bytes_, options_.memory_);
            break;
        case http_content_coding::identity:
            return;
    }
    std::visit([](const auto& stream) {
        if (!stream.initialized()) {
            throw http_content_codec_error("failed to initialize HTTP content decoder");
        }
    }, *stream_);
}

http_content_decode_result http_content_decoder_stream::fail(std::size_t consumed, http_content_decode_error error) noexcept {
    phase_ = decoder_state::failed;
    failure_ = error;
    return {consumed, http_content_decode_failure(error)};
}

http_content_decode_result http_content_decoder_stream::decode_active(std::string_view input, std::span<char> output) {
    if (phase_ == decoder_state::failed) {
        return {0, http_content_decode_failure(failure_)};
    }
    if (phase_ == decoder_state::finished) {
        return {0, decode_complete{}};
    }
    if (coding_ == http_content_coding::identity) {
        const auto count = (std::min)({input.size(), output.size(), options_.max_decoded_bytes_ - decoded_bytes_});
        if (count != 0) {
            decoded_bytes_ += count;
            return {count, decode_output(input.substr(0, count))};
        }
        if (!input.empty()) {
            return fail(0, http_content_decode_error::decoded_size_exceeded);
        }
        return {0, decode_need_input{}};
    }
    std::size_t consumed = 0;
    for (;;) {
        if (boundary_) {
            if (coding_ != http_content_coding::gzip && coding_ != http_content_coding::zstd) {
                if (consumed != input.size()) {
                    return fail(consumed, http_content_decode_error::invalid_content);
                }
                phase_ = decoder_state::finished;
                return {consumed, decode_complete{}};
            }
            if (consumed == input.size()) {
                return {consumed, decode_need_input{}};
            }
            if (auto* stream = std::get_if<detail::zlib_inflate_stream>(&*stream_)) {
                if (!stream->reset()) {
                    throw http_content_codec_error("failed to reset gzip member decoder");
                }
            }
            // zstd starts the next concatenated frame automatically.
            boundary_ = false;
        }
        if (!received_input_ && input.empty()) {
            return {consumed, decode_need_input{}};
        }
        char probe;
        const auto room = options_.max_decoded_bytes_ - decoded_bytes_;
        const bool probing = room == 0;
        const auto writable = probing ? std::span<char>(&probe, 1) : output.first((std::min)(room, output.size()));
        const auto step = std::visit([&](auto& stream) {
            return decode_native(stream, input.substr(consumed), writable);
        }, *stream_);
        consumed += step.consumed_;
        received_input_ = received_input_ || step.consumed_ != 0;
        boundary_ = step.complete_;
        if (probing && step.produced_ != 0) {
            return fail(consumed, http_content_decode_error::decoded_size_exceeded);
        }
        bool invalid = step.invalid_;
        if (boundary_ && coding_ != http_content_coding::gzip && coding_ != http_content_coding::zstd && consumed != input.size()) {
            invalid = true;
        }
        auto error = http_content_decode_error::invalid_content;
        if (const auto* brotli = std::get_if<detail::brotli_decoder_stream>(&*stream_); brotli != nullptr && brotli->native_budget_exceeded()) {
            error = http_content_decode_error::decoded_size_exceeded;
            invalid = true;
        }
        decoded_bytes_ += step.produced_;
        if (invalid) {
            // Preserve valid output and exact input progress even when the
            // native library reports an error in the same call.
            phase_ = decoder_state::failed;
            failure_ = error;
        }
        if (step.produced_ != 0) {
            return {consumed, decode_output(std::string_view(output.data(), step.produced_))};
        }
        if (invalid) {
            return fail(consumed, error);
        }
        if (boundary_) {
            continue;
        }
        if (consumed != input.size()) {
            if (step.consumed_ != 0) {
                continue;
            }
            return fail(consumed, http_content_decode_error::invalid_content);
        }
        return {consumed, decode_need_input{}};
    }
}

http_content_decode_result http_content_decoder_stream::decode(std::string_view input, std::span<char> output) {
    try {
        return decode_active(input, output);
    } catch (...) {
        phase_ = decoder_state::failed;
        throw;
    }
}

http_content_decode_result http_content_decoder_stream::finish_input() noexcept {
    if (phase_ == decoder_state::failed) {
        return {0, http_content_decode_failure(failure_)};
    }
    if (phase_ == decoder_state::finished || coding_ == http_content_coding::identity || boundary_ || !received_input_) {
        phase_ = decoder_state::finished;
        return {0, decode_complete{}};
    }
    return fail(0, http_content_decode_error::invalid_content);
}

void http_content_decoder_stream::reset() {
    phase_ = decoder_state::failed;
    if (stream_.has_value() && !stream_->valueless_by_exception()) {
        if (auto* stream = std::get_if<zlib_inflate_stream>(&*stream_)) {
            if (!stream->reset()) {
                throw http_content_codec_error("failed to reset HTTP content decoder");
            }
        } else if (auto* decoder = std::get_if<zstd_decoder_stream>(&*stream_)) {
            if (!decoder->reset()) {
                throw http_content_codec_error("failed to reset HTTP content decoder");
            }
        } else {
            initialize();
        }
    } else {
        initialize();
    }
    decoded_bytes_ = 0;
    received_input_ = false;
    boundary_ = false;
    failure_ = http_content_decode_error::invalid_content;
    phase_ = decoder_state::active;
}

}  // namespace detail

struct http_content_decoder::impl_type final {
    impl_type(http_content_coding coding, http_content_decode_options options) :
        stream_(coding, options) {}
    detail::http_content_decoder_stream stream_;
};

http_content_decoder::http_content_decoder(http_content_coding coding, http_content_decode_options options) :
    impl_(new impl_type(coding, options)) {}

http_content_decoder::~http_content_decoder() {
    delete impl_;
}

http_content_decode_result http_content_decoder::decode(std::string_view input, std::span<char> output) {
    auto& stream = impl_->stream_;
    if (stream.state() == decoder_state::active) {
        const char* error = output.empty() ? "content decoding needs output space" : nullptr;
        if (error == nullptr && stream.coding_ != http_content_coding::identity && detail::http_text_overlaps(input, std::string_view(output.data(), output.size()))) {
            error = "compressed content input overlaps output";
        }
        if (error != nullptr) {
            (void)stream.fail(0, http_content_decode_error::invalid_content);
            throw std::invalid_argument(error);
        }
    }
    return stream.decode(input, output);
}

http_content_decode_result http_content_decoder::finish_input() noexcept {
    return impl_->stream_.finish_input();
}

decoder_state http_content_decoder::state() const noexcept {
    return impl_->stream_.state();
}

void http_content_decoder::reset() {
    impl_->stream_.reset();
}

}  // namespace ruvia
