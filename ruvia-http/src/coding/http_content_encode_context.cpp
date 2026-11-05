#include <variant>

#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_content_codec.h"

#include "coding/http_brotli_stream.h"
#include "coding/http_content_codec.h"
#include "coding/http_zlib_stream.h"
#include "coding/http_zstd_stream.h"
#include "coding/pmr_codec_slot.h"

namespace ruvia {

// The kept state of a resettable coding. The native libraries retain its
// address, so it lives in one stable allocation from the context resource.
struct http_content_encode_context::impl_type final {
    using stream_type = std::variant<detail::zlib_deflate_stream, detail::zstd_encoder_stream>;

    impl_type(http_content_coding coding, std::pmr::memory_resource* resource)
        : stream_(make_stream(coding, resource)) {}

    [[nodiscard]] bool initialized() const noexcept {
        return std::visit([](const auto& stream) { return stream.initialized(); }, stream_);
    }

    [[nodiscard]] bool reset() noexcept {
        return std::visit([](auto& stream) { return stream.reset(); }, stream_);
    }

    [[nodiscard]] std::optional<std::pmr::string> encode(
        std::string_view input, http_content_encode_options options) {
        if (auto* deflater = std::get_if<detail::zlib_deflate_stream>(&stream_)) {
            return detail::encode_zlib_content(*deflater, input, options.max_encoded_bytes_, options.resource_);
        }
        return detail::encode_zstd_content(
            std::get<detail::zstd_encoder_stream>(stream_), input, options.max_encoded_bytes_, options.resource_);
    }

    stream_type stream_;

private:
    [[nodiscard]] static stream_type make_stream(http_content_coding coding, std::pmr::memory_resource* resource) {
        if (coding == http_content_coding::zstd) {
            return stream_type(std::in_place_type<detail::zstd_encoder_stream>, resource);
        }
        return stream_type(std::in_place_type<detail::zlib_deflate_stream>, resource,
            coding == http_content_coding::gzip ? detail::zlib_content_format::gzip
                                                : detail::zlib_content_format::deflate);
    }
};

http_content_encode_context::http_content_encode_context(
    http_content_coding coding, std::pmr::memory_resource* state_resource) noexcept
    : coding_(coding),
      resource_(detail::http_pmr_resource_or_default(state_resource)) {}

http_content_encode_context::~http_content_encode_context() {
    detail::destroy_http_pmr_object(detail::http_resolved_pmr_resource_tag{}, impl_, resource_);
}

std::optional<std::pmr::string> http_content_encode_context::encode(
    std::string_view input, http_content_encode_options options) {
    switch (coding_) {
        case http_content_coding::gzip:
        case http_content_coding::deflate:
        case http_content_coding::zstd:
            break;
        case http_content_coding::brotli: {
            detail::brotli_encoder_stream encoder(resource_);
            if (!encoder.initialized()) {
                throw http_content_codec_error("brotli encoder initialization failed");
            }
            return detail::encode_brotli_content(encoder, input, options.max_encoded_bytes_, options.resource_);
        }
        default:
            // identity and unknown values keep the one-shot facade's contract.
            return encode_http_content(coding_, input, options);
    }

    // Every exit, including a rethrown allocation failure, leaves the next
    // representation a fresh stream.
    if (detail::acquire_pmr_codec(impl_, resource_, coding_, resource_) == nullptr) {
        throw http_content_codec_error("content encoder initialization failed");
    }
    const detail::pmr_codec_reset_guard reset_guard(impl_, resource_);
    return impl_->encode(input, options);
}

}  // namespace ruvia
