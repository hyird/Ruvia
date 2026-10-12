#include "ruvia/http/http_content_encoder.h"

#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/pmr_resource.h"

#include "coding/http_brotli_stream.h"
#include "coding/http_zlib_stream.h"
#include "coding/http_zstd_stream.h"
#include "util/http_text_overlap.h"

namespace ruvia {
namespace {

using encoder_stream = std::variant<detail::zlib_deflate_stream, detail::brotli_encoder_stream, detail::zstd_encoder_stream>;

template <typename stream_type>
[[noreturn]] void fail_encoding(const stream_type& stream, const char* message) {
    stream.rethrow_allocation_failure();
    throw http_content_encoder_error(message);
}

[[nodiscard]] encoder_stream make_encoder_stream(http_content_coding coding, std::pmr::memory_resource* resource) {
    switch (coding) {
        case http_content_coding::gzip:
        case http_content_coding::deflate:
            return encoder_stream(std::in_place_type<detail::zlib_deflate_stream>, resource, Z_DEFAULT_COMPRESSION,
                coding == http_content_coding::gzip ? 15 + 16 : 15, detail::zlib_default_mem_level, Z_DEFAULT_STRATEGY);
        case http_content_coding::brotli:
            return encoder_stream(std::in_place_type<detail::brotli_encoder_stream>, resource);
        case http_content_coding::zstd:
            return encoder_stream(std::in_place_type<detail::zstd_encoder_stream>, resource);
        case http_content_coding::identity:
            break;
    }
    throw std::invalid_argument("unsupported HTTP content coding");
}

[[nodiscard]] const char* initialization_failure(const detail::zlib_deflate_stream&) noexcept {
    return "failed to initialize zlib content encoder";
}

[[nodiscard]] const char* initialization_failure(const detail::brotli_encoder_stream&) noexcept {
    return "failed to initialize Brotli content encoder";
}

[[nodiscard]] const char* initialization_failure(const detail::zstd_encoder_stream&) noexcept {
    return "failed to initialize Zstd content encoder";
}

void encode_stream(detail::zlib_deflate_stream& deflater, std::string_view input, std::pmr::string& output,
    bool flush, bool finish_value) {
    const int operation = finish_value ? Z_FINISH : flush ? Z_SYNC_FLUSH
                                                          : Z_NO_FLUSH;
    auto& stream = deflater.native();
    struct clear_borrows final {
        z_stream& stream_;
        ~clear_borrows() {
            stream_.next_in = nullptr;
            stream_.avail_in = 0;
            stream_.next_out = nullptr;
            stream_.avail_out = 0;
        }
    } clear{stream};
    std::size_t supplied = 0;
    std::array<char, 16384> buffer;
    for (;;) {
        if (stream.avail_in == 0 && supplied < input.size()) {
            const auto count = static_cast<uInt>(
                std::min<std::size_t>(input.size() - supplied, (std::numeric_limits<uInt>::max)()));
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data() + supplied));
            stream.avail_in = count;
            supplied += count;
        }
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        const auto status = deflate(&stream, operation);
        // Repeating a completed flush has no work left; zlib reports
        // Z_BUF_ERROR without invalidating the stream.
        if (status == Z_BUF_ERROR && operation != Z_FINISH &&
            stream.avail_in == 0 && supplied == input.size()) {
            return;
        }
        if (status != Z_OK && status != Z_STREAM_END) {
            fail_encoding(deflater, "zlib content encoding failed");
        }
        const auto produced = buffer.size() - stream.avail_out;
        output.append(buffer.data(), produced);
        if (operation == Z_FINISH && status == Z_STREAM_END) {
            return;
        }
        if (operation != Z_FINISH && stream.avail_in == 0 && supplied == input.size() && stream.avail_out != 0) {
            return;
        }
        if (produced == 0 && stream.avail_in == before_input) {
            fail_encoding(deflater, "zlib content encoder made no progress");
        }
    }
}

void encode_stream(detail::brotli_encoder_stream& encoder, std::string_view input, std::pmr::string& output,
    bool flush, bool finish_value) {
    const auto operation = finish_value ? BROTLI_OPERATION_FINISH : flush ? BROTLI_OPERATION_FLUSH
                                                                          : BROTLI_OPERATION_PROCESS;
    auto* state_value = encoder.native();
    std::size_t available_input = input.size();
    const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
    for (;;) {
        std::size_t available_output = 0;
        const auto before_input = available_input;
        if (BrotliEncoderCompressStream(state_value, operation, &available_input, &next_input,
                &available_output, nullptr, nullptr) != BROTLI_TRUE) {
            fail_encoding(encoder, "Brotli content encoding failed");
        }
        std::size_t produced = 0;
        const auto* bytes_value = BrotliEncoderTakeOutput(state_value, &produced);
        // Consume the borrowed block before the next encoder call.
        if (produced != 0) {
            output.append(reinterpret_cast<const char*>(bytes_value), produced);
        }
        if (operation == BROTLI_OPERATION_FINISH && BrotliEncoderIsFinished(state_value) == BROTLI_TRUE) {
            return;
        }
        if (available_input == 0 && BrotliEncoderHasMoreOutput(state_value) != BROTLI_TRUE) {
            if (operation == BROTLI_OPERATION_FINISH) {
                fail_encoding(encoder, "Brotli content encoder did not finish");
            }
            return;
        }
        if (produced == 0 && available_input == before_input) {
            fail_encoding(encoder, "Brotli content encoder made no progress");
        }
    }
}

void encode_stream(detail::zstd_encoder_stream& encoder, std::string_view input, std::pmr::string& output,
    bool flush, bool finish_value) {
    const auto operation = finish_value ? ZSTD_e_end : flush ? ZSTD_e_flush
                                                             : ZSTD_e_continue;
    auto* context_value = encoder.native();
    ZSTD_inBuffer input_buffer{input.data(), input.size(), 0};
    std::array<char, 16384> buffer;
    for (;;) {
        ZSTD_outBuffer output_buffer{buffer.data(), buffer.size(), 0};
        const auto before_input = input_buffer.pos;
        const auto remaining = ZSTD_compressStream2(context_value, &output_buffer, &input_buffer, operation);
        if (ZSTD_isError(remaining) != 0) {
            fail_encoding(encoder, "Zstd content encoding failed");
        }
        output.append(buffer.data(), output_buffer.pos);
        if (operation == ZSTD_e_end && remaining == 0 && input_buffer.pos == input_buffer.size) {
            return;
        }
        if (operation != ZSTD_e_end && input_buffer.pos == input_buffer.size &&
            output_buffer.pos < buffer.size() && (operation == ZSTD_e_continue || remaining == 0)) {
            return;
        }
        if (output_buffer.pos == 0 && input_buffer.pos == before_input) {
            fail_encoding(encoder, "Zstd content encoder made no progress");
        }
    }
}

}  // namespace

// One pinned codec stream per non-identity encoder. A failed initialization
// unwinds through the stream destructor, including an exception captured at
// the C ABI.
struct http_content_encoder::impl final {
    impl(http_content_coding coding, std::pmr::memory_resource* resource)
        : stream_(make_encoder_stream(coding, resource)) {
        std::visit([](const auto& stream) {
            if (!stream.initialized()) {
                throw http_content_encoder_error(initialization_failure(stream));
            }
        },
            stream_);
    }

    void encode(std::string_view input, std::pmr::string& output, bool flush, bool finish_value) {
        std::visit([&](auto& stream) { encode_stream(stream, input, output, flush, finish_value); }, stream_);
    }

    encoder_stream stream_;
};

http_content_encoder::http_content_encoder(http_content_coding coding, std::pmr::memory_resource* resource)
    : coding_(coding),
      resource_(detail::http_pmr_resource_or_default(resource)) {
    if (coding_ != http_content_coding::identity) {
        impl_ = detail::construct_http_pmr_object<impl>(resource_, coding_, resource_);
    }
}

http_content_encoder::~http_content_encoder() {
    detail::destroy_http_pmr_object(impl_, resource_);
}

void http_content_encoder::write(std::string_view input, std::pmr::string& output, bool flush) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP content encoder is no longer writable");
    }
    try {
        if (impl_ == nullptr) {
            if (!input.empty()) {
                output.append(input.data(), input.size());
            }
        } else if (!input.empty() || flush) {
            if (detail::http_text_overlaps(input, {output.data(), output.size() + 1})) {
                // Appending codec output can reallocate the caller's string
                // before all input has been consumed. Own only this aliasing
                // case; independent input keeps the direct encoding path.
                const std::pmr::string owned_input(input, resource_);
                impl_->encode(owned_input, output, flush, false);
            } else {
                impl_->encode(input, output, flush, false);
            }
        }
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_content_encoder::finish(std::pmr::string& output) {
    if (phase_ == phase::finished) {
        return;
    }
    if (phase_ == phase::failed) {
        throw std::logic_error("HTTP content encoder has failed");
    }
    try {
        if (impl_ != nullptr) {
            impl_->encode({}, output, false, true);
        }
        phase_ = phase::finished;
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

}  // namespace ruvia
