#include "http_content_codec.h"

#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "decimal_text.h"
#include "http_compression_codec.h"
#include "http_content_coding.h"
#include "http_field_text.h"

namespace ruvia {

namespace {

using encoder_stream = std::variant<detail::zlib_deflate_stream, detail::brotli_encoder_stream, detail::zstd_encoder_stream>;

void emplace_encoder_stream(std::optional<encoder_stream>& target, http_content_coding coding, std::size_t max_memory_bytes) {
    switch (coding) {
        case http_content_coding::gzip:
        case http_content_coding::deflate:
            target.emplace(std::in_place_type<detail::zlib_deflate_stream>, coding == http_content_coding::gzip ? detail::zlib_content_format::gzip : detail::zlib_content_format::deflate, max_memory_bytes);
            return;
        case http_content_coding::brotli:
            target.emplace(std::in_place_type<detail::brotli_encoder_stream>, detail::brotli_stream_window_bits, max_memory_bytes);
            return;
        case http_content_coding::zstd:
            target.emplace(std::in_place_type<detail::zstd_encoder_stream>, max_memory_bytes);
            return;
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

[[nodiscard]] bool encode_stream(detail::zlib_deflate_stream& deflater, std::string_view input, std::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const int operation = finish_value ? Z_FINISH : (flush ? Z_SYNC_FLUSH : Z_NO_FLUSH);
    return deflater.encode(input, output, operation, max_encoded_bytes);
}

[[nodiscard]] bool encode_stream(detail::brotli_encoder_stream& encoder, std::string_view input, std::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const auto operation = finish_value ? BROTLI_OPERATION_FINISH : (flush ? BROTLI_OPERATION_FLUSH : BROTLI_OPERATION_PROCESS);
    return encoder.encode(input, output, operation, max_encoded_bytes);
}

[[nodiscard]] bool encode_stream(detail::zstd_encoder_stream& encoder, std::string_view input, std::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const auto operation = finish_value ? ZSTD_e_end : (flush ? ZSTD_e_flush : ZSTD_e_continue);
    return encoder.encode(input, output, operation, max_encoded_bytes);
}

}  // namespace

// One pinned codec stream per non-identity encoder. A failed initialization
// unwinds through the stream destructor, including an exception captured at
// the C ABI.
struct http_content_encoder::impl_type final {
    impl_type(http_content_coding coding, http_content_encoder_options options) :
        max_encoded_bytes_(options.max_encoded_bytes_),
        max_memory_bytes_(options.max_memory_bytes_),
        coding_(coding) {
        if (coding_ != http_content_coding::identity) {
            emplace_encoder_stream(stream_, coding_, max_memory_bytes_);
            std::visit([](const auto& stream) {
                if (!stream.initialized()) {
                    throw http_content_codec_error(initialization_failure(stream));
                }
            }, *stream_);
        }
    }

    void encode(std::string_view input, std::string& output, bool flush, bool finish_value) {
        const auto before_size = output.size();
        if (!stream_.has_value()) {
            if (!input.empty()) {
                if (input.size() > max_encoded_bytes_ - produced_bytes_) {
                    throw http_content_codec_error("encoded HTTP content size exceeded limit");
                }
                output.append(input.data(), input.size());
                produced_bytes_ += input.size();
            }
            return;
        }
        const auto remaining = max_encoded_bytes_ - produced_bytes_;
        const auto output_cap = saturating_add(before_size, remaining);
        const bool ok = std::visit([&](auto& stream) {
            return encode_stream(stream, input, output, flush, finish_value, output_cap);
        }, *stream_);
        if (!ok) {
            throw http_content_codec_error("encoded HTTP content size exceeded limit");
        }
        produced_bytes_ += output.size() - before_size;
    }

    // Reuse native working memory where supported. Brotli rebuilds.
    // A failed rebuild leaves the variant valueless, which the
    // owner never touches again until another explicit reset().
    void reset() {
        produced_bytes_ = 0;
        if (!stream_.has_value()) {
            return;
        }
        if (auto* deflater = std::get_if<detail::zlib_deflate_stream>(&*stream_)) {
            if (!deflater->reset()) {
                throw http_content_codec_error("failed to reset zlib content encoder");
            }
        } else if (auto* encoder = std::get_if<detail::zstd_encoder_stream>(&*stream_)) {
            if (!encoder->reset()) {
                throw http_content_codec_error("failed to reset Zstd content encoder");
            }
        } else {
            const auto& rebuilt = stream_->emplace<detail::brotli_encoder_stream>(detail::brotli_stream_window_bits, max_memory_bytes_);
            if (!rebuilt.initialized()) {
                throw http_content_codec_error(initialization_failure(rebuilt));
            }
        }
    }

    std::optional<encoder_stream> stream_;
    std::size_t max_encoded_bytes_;
    std::size_t max_memory_bytes_;
    std::size_t produced_bytes_{0};
    http_content_coding coding_;
};

http_content_encoder::http_content_encoder(http_content_coding coding, http_content_encoder_options options) :
    coding_(coding) {
    if (coding_ != http_content_coding::identity || options.max_encoded_bytes_ != (std::numeric_limits<std::size_t>::max)() || options.max_memory_bytes_ != (std::numeric_limits<std::size_t>::max)()) {
        if (http_content_coding_token(coding_).empty()) {
            throw std::invalid_argument("unsupported HTTP content coding");
        }
        impl_ = new impl_type(coding_, options);
    }
}

http_content_encoder::~http_content_encoder() {
    delete impl_;
}

void http_content_encoder::write(std::string_view input, std::string& output, bool flush) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP content encoder is no longer writable");
    }
    try {
        if (impl_ == nullptr) {
            if (!input.empty()) {
                output.append(input.data(), input.size());
            }
        } else if (!input.empty() || flush) {
            if (detail::http_text_aliases_string(input, output)) {
                // Appending codec output can reallocate the caller's string
                // before all input has been consumed. Own only this aliasing
                // case; independent input keeps the direct encoding path.
                const std::string owned_input(input);
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

void http_content_encoder::finish(std::string& output) {
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

void http_content_encoder::reset() {
    if (impl_ != nullptr) {
        try {
            impl_->reset();
        } catch (...) {
            phase_ = phase::failed;
            throw;
        }
    }
    phase_ = phase::active;
}
// The kept state of a resettable coding. The native libraries retain its
// address, so it lives in one stable allocation.
struct http_content_encode_context::impl_type final {
    using stream_type = std::variant<detail::zlib_deflate_stream, detail::zstd_encoder_stream>;

    explicit impl_type(http_content_coding coding) :
        stream_(make_stream(coding)) {}

    [[nodiscard]] bool initialized() const noexcept {
        return std::visit([](const auto& stream) {
            return stream.initialized();
        }, stream_);
    }

    [[nodiscard]] bool reset() noexcept {
        return std::visit([](auto& stream) {
            return stream.reset();
        }, stream_);
    }

    [[nodiscard]] std::optional<std::string> encode(std::string_view input, http_content_encode_options options) {
        if (auto* deflater = std::get_if<detail::zlib_deflate_stream>(&stream_)) {
            return detail::encode_zlib_content(*deflater, input, options.max_encoded_bytes_);
        }
        return detail::encode_zstd_content(std::get<detail::zstd_encoder_stream>(stream_), input, options.max_encoded_bytes_);
    }

    stream_type stream_;

private:
    [[nodiscard]] static stream_type make_stream(http_content_coding coding) {
        if (coding == http_content_coding::zstd) {
            return stream_type(std::in_place_type<detail::zstd_encoder_stream>);
        }
        return stream_type(std::in_place_type<detail::zlib_deflate_stream>, coding == http_content_coding::gzip ? detail::zlib_content_format::gzip : detail::zlib_content_format::deflate);
    }
};

http_content_encode_context::http_content_encode_context(http_content_coding coding) noexcept :
    coding_(coding) {}

http_content_encode_context::~http_content_encode_context() {
    delete impl_;
}

std::optional<std::string> http_content_encode_context::encode(std::string_view input, http_content_encode_options options) {
    switch (coding_) {
        case http_content_coding::gzip:
        case http_content_coding::deflate:
        case http_content_coding::zstd:
            break;
        case http_content_coding::brotli: {
            detail::brotli_encoder_stream encoder(detail::brotli_whole_buffer_window_bits);
            if (!encoder.initialized()) {
                throw http_content_codec_error("brotli encoder initialization failed");
            }
            return detail::encode_brotli_content(encoder, input, options.max_encoded_bytes_);
        }
        default:
            // identity and unknown values keep the one-shot facade's contract.
            return encode_http_content(coding_, input, options);
    }

    if (detail::acquire_codec(impl_, coding_) == nullptr) {
        throw http_content_codec_error("content encoder initialization failed");
    }
    const detail::codec_reset_guard reset_guard(impl_);
    return impl_->encode(input, options);
}

http_worker_codecs::http_worker_codecs() noexcept :
    gzip_(http_content_coding::gzip),
    deflate_(http_content_coding::deflate),
    brotli_(http_content_coding::brotli),
    zstd_(http_content_coding::zstd) {}

http_worker_codecs::~http_worker_codecs() {
    for (auto* pool : {&gzip_.streaming_, &deflate_.streaming_, &brotli_.streaming_, &zstd_.streaming_}) {
        for (std::size_t i = 0; i < pool->count_; ++i) {
            destroy_stream_encoder(*pool->free_[i]);
        }
        pool->count_ = 0;
    }
}

http_content_encode_context& http_worker_codecs::encoder(http_content_coding coding) {
    return encoders(coding).buffered_;
}

http_worker_codecs::coding_encoders_type& http_worker_codecs::encoders(http_content_coding coding) {
    switch (coding) {
        case http_content_coding::gzip:
            return gzip_;
        case http_content_coding::deflate:
            return deflate_;
        case http_content_coding::brotli:
            return brotli_;
        case http_content_coding::zstd:
            return zstd_;
        case http_content_coding::identity:
            break;
    }
    throw std::logic_error("identity content coding has no encoder");
}

http_stream_encoder_lease http_worker_codecs::acquire_stream_encoder(http_content_coding coding) {
    auto& pool = encoders(coding).streaming_;
    if (pool.count_ != 0) {
        return http_stream_encoder_lease(*this, *pool.free_[--pool.count_]);
    }
    return http_stream_encoder_lease(*this, *(new http_content_encoder(coding)));
}

void http_worker_codecs::release_stream_encoder(http_content_encoder& encoder) noexcept {
    auto& pool = encoders(encoder.coding()).streaming_;
    if (pool.count_ < pool.free_.size()) {
        try {
            encoder.reset();
            pool.free_[pool.count_++] = &encoder;
            return;
        } catch (...) {
        }
    }
    destroy_stream_encoder(encoder);
}

void http_worker_codecs::destroy_stream_encoder(http_content_encoder& encoder) noexcept {
    delete &encoder;
}

namespace {

[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_content_stage(http_content_coding coding, std::string_view input, http_content_decode_options options) {
    // A zero-length coded representation carries no content: user agents treat
    // it as empty rather than as a truncated coding stream. An out-of-range coding
    // has no token and stays unsupported even for empty input.
    if (input.empty() && !http_content_coding_token(coding).empty()) {
        return std::string{};
    }
    switch (coding) {
        case http_content_coding::gzip:
            return detail::decode_gzip_content(input, options.max_decoded_bytes_);
        case http_content_coding::deflate:
            return detail::decode_deflate_content(input, options.max_decoded_bytes_);
        case http_content_coding::brotli:
            return detail::decode_brotli_content(input, options.max_decoded_bytes_);
        case http_content_coding::zstd:
            return detail::decode_zstd_content(input, options.max_decoded_bytes_);
        case http_content_coding::identity:
            // Identity stages are skipped by decode_http_content.
            break;
    }
    return http_content_decode_error::unsupported_coding;
}

}  // namespace

std::optional<std::string> encode_http_content(http_content_coding coding, std::string_view input, http_content_encode_options options) {
    switch (coding) {
        case http_content_coding::brotli:
            return detail::encode_brotli_content(input, options.max_encoded_bytes_);
        case http_content_coding::zstd:
            return detail::encode_zstd_content(input, options.max_encoded_bytes_);
        case http_content_coding::gzip:
            return detail::encode_gzip_content(input, options.max_encoded_bytes_);
        case http_content_coding::deflate:
            return detail::encode_deflate_content(input, options.max_encoded_bytes_);
        case http_content_coding::identity:
            if (input.size() > options.max_encoded_bytes_) {
                return std::nullopt;
            }
            return std::string(input);
    }
    throw http_content_codec_error("unsupported content coding");
}

std::optional<std::string> encode_http_content(std::span<const http_content_coding> codings, std::string_view input, http_content_encode_options options) {
    // Nothing is allocated until a stage produces content.
    std::optional<std::string> current;
    for (const auto coding : codings) {
        // identity transforms nothing; only the size limit below applies to it.
        if (coding == http_content_coding::identity) {
            continue;
        }
        const std::string_view source_value = current.has_value() ? std::string_view(*current) : input;
        auto stage = encode_http_content(coding, source_value, options);
        if (!stage.has_value()) {
            return std::nullopt;
        }
        current = std::move(stage);
    }
    if (current.has_value()) {
        return current;
    }
    return encode_http_content(http_content_coding::identity, input, options);
}

std::variant<std::string, http_content_decode_error> decode_http_content(http_content_coding coding, std::string_view input, http_content_decode_options options) {
    return decode_http_content(std::span<const http_content_coding>(&coding, 1), input, options);
}

std::variant<std::string, http_content_decode_error> decode_http_content(std::span<const http_content_coding> codings, std::string_view input, http_content_decode_options options) {
    // Nothing is allocated until a stage produces content, so an oversized
    // identity body is rejected before any allocation.
    std::optional<std::string> current;
    for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
        // identity transforms nothing; only the size limit below applies to it.
        if (*coding == http_content_coding::identity) {
            continue;
        }
        const std::string_view source_value = current.has_value() ? std::string_view(*current) : input;
        auto stage = decode_content_stage(*coding, source_value, options);
        auto* content = std::get_if<std::string>(&stage);
        if (content == nullptr) {
            return std::get<http_content_decode_error>(stage);
        }
        current = std::move(*content);
    }
    if (current.has_value()) {
        return std::move(*current);
    }
    if (input.size() > options.max_decoded_bytes_) {
        return http_content_decode_error::decoded_size_exceeded;
    }
    return std::string(input);
}

}  // namespace ruvia
