#include "ruvia/http/http_content_encoder.h"

#include <zlib.h>
#include <zstd.h>

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/http/detail/util/http_address_overlap.h"
#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/pmr_resource.h"

#include "coding/http_brotli_stream.h"
#include "coding/http_zlib_stream.h"
#include "coding/http_zstd_stream.h"

namespace ruvia {
namespace {

using encoder_stream = std::variant<detail::zlib_deflate_stream, detail::brotli_encoder_stream, detail::zstd_encoder_stream>;

inline constexpr auto unbounded_stream_output = (std::numeric_limits<std::size_t>::max)();

[[nodiscard]] encoder_stream make_encoder_stream(http_content_coding coding, std::pmr::memory_resource* resource) {
    switch (coding) {
        case http_content_coding::gzip:
        case http_content_coding::deflate:
            return encoder_stream(std::in_place_type<detail::zlib_deflate_stream>, resource,
                coding == http_content_coding::gzip ? detail::zlib_content_format::gzip
                                                    : detail::zlib_content_format::deflate);
        case http_content_coding::brotli:
            return encoder_stream(std::in_place_type<detail::brotli_encoder_stream>, resource,
                detail::brotli_stream_window_bits);
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
    static_cast<void>(deflater.encode(input, output, operation, unbounded_stream_output));
}

void encode_stream(detail::brotli_encoder_stream& encoder, std::string_view input, std::pmr::string& output,
    bool flush, bool finish_value) {
    const auto operation = finish_value ? BROTLI_OPERATION_FINISH : flush ? BROTLI_OPERATION_FLUSH
                                                                          : BROTLI_OPERATION_PROCESS;
    static_cast<void>(encoder.encode(input, output, operation, unbounded_stream_output));
}

void encode_stream(detail::zstd_encoder_stream& encoder, std::string_view input, std::pmr::string& output,
    bool flush, bool finish_value) {
    const auto operation = finish_value ? ZSTD_e_end : flush ? ZSTD_e_flush
                                                             : ZSTD_e_continue;
    static_cast<void>(encoder.encode(input, output, operation, unbounded_stream_output));
}

}  // namespace

// One pinned codec stream per non-identity encoder. A failed initialization
// unwinds through the stream destructor, including an exception captured at
// the C ABI.
struct http_content_encoder::impl_type final {
    impl_type(http_content_coding coding, std::pmr::memory_resource* resource)
        : stream_(make_encoder_stream(coding, resource)),
          resource_(resource) {
        std::visit([](const auto& stream) {
            if (!stream.initialized()) {
                throw http_content_codec_error(initialization_failure(stream));
            }
        },
            stream_);
    }

    void encode(std::string_view input, std::pmr::string& output, bool flush, bool finish_value) {
        std::visit([&](auto& stream) { encode_stream(stream, input, output, flush, finish_value); }, stream_);
    }

    // Returns the codec to a fresh stream, keeping its working memory. Brotli
    // has no reset: the encoder is rebuilt in place from the same resource. A
    // failed rebuild leaves the variant valueless, which the owner never
    // touches again because it stays failed.
    void reset() {
        if (auto* deflater = std::get_if<detail::zlib_deflate_stream>(&stream_)) {
            if (!deflater->reset()) {
                throw http_content_codec_error("failed to reset zlib content encoder");
            }
        } else if (auto* encoder = std::get_if<detail::zstd_encoder_stream>(&stream_)) {
            if (!encoder->reset()) {
                throw http_content_codec_error("failed to reset Zstd content encoder");
            }
        } else {
            const auto& rebuilt =
                stream_.emplace<detail::brotli_encoder_stream>(resource_, detail::brotli_stream_window_bits);
            if (!rebuilt.initialized()) {
                throw http_content_codec_error(initialization_failure(rebuilt));
            }
        }
    }

    encoder_stream stream_;
    std::pmr::memory_resource* resource_;
};

http_content_encoder::http_content_encoder(http_content_coding coding, std::pmr::memory_resource* resource)
    : coding_(coding),
      resource_(detail::http_pmr_resource_or_default(resource)) {
    if (coding_ != http_content_coding::identity) {
        impl_ = detail::construct_http_pmr_object<impl_type>(resource_, coding_, resource_);
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
            if (detail::http_text_aliases_string(input, output)) {
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

}  // namespace ruvia
