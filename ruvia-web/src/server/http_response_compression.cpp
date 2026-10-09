#include "server/http_response_compression.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/process_resource.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_media_type.h"

namespace ruvia::detail {
namespace {

enum class buffered_compression_attempt_status : std::uint8_t {
    compressed,
    not_smaller,
    failed,
};

struct buffered_compression_attempt final {
    explicit buffered_compression_attempt(buffered_compression_attempt_status status) noexcept
        : status_(status),
          bytes_(process_resource()) {}

    buffered_compression_attempt(
        buffered_compression_attempt_status status, std::pmr::string bytes_value) noexcept
        : status_(status),
          bytes_(std::move(bytes_value)) {}

    buffered_compression_attempt_status status_;
    std::pmr::string bytes_;
};

[[nodiscard]] buffered_compression_attempt encode_buffered_body(
    http_content_coding coding, std::pmr::string plain) {
    const auto max_encoded_bytes = plain.empty() ? 0 : plain.size() - 1;
    auto encoding = encode_http_content(
        coding, plain, {.max_encoded_bytes_ = max_encoded_bytes, .resource_ = process_resource()});
    if (auto* encoded = encoding.encoded(); encoded != nullptr) {
        return buffered_compression_attempt(
            buffered_compression_attempt_status::compressed, std::move(*encoded).take_bytes());
    }
    const auto* failure = encoding.failure();
    if (failure != nullptr && failure->error() == http_content_encode_error::encoded_size_exceeded) {
        return buffered_compression_attempt(buffered_compression_attempt_status::not_smaller);
    }
    return buffered_compression_attempt(buffered_compression_attempt_status::failed);
}

[[nodiscard]] bool media_type_starts_with(
    std::string_view media_type, std::string_view prefix) noexcept {
    return media_type.size() >= prefix.size() &&
           http_ascii_equals_ignore_case(media_type.substr(0, prefix.size()), prefix);
}

[[nodiscard]] bool response_content_type_skips_compression(std::string_view content_type_value) noexcept {
    if (content_type_value.empty()) {
        return false;
    }
    const auto media_type = ::ruvia::http_media_type_only(content_type_value);
    if (media_type.empty()) {
        return false;
    }
    // Dominant compressible types short-circuit the skip list below.
    if (media_type_starts_with(media_type, "text/") ||
        http_ascii_equals_ignore_case(media_type, "application/json")) {
        return false;
    }
    if (http_ascii_equals_ignore_case(media_type, "image/svg+xml")) {
        return false;
    }
    return media_type_starts_with(media_type, "image/") || media_type_starts_with(media_type, "video/") ||
           media_type_starts_with(media_type, "audio/") ||
           http_ascii_equals_ignore_case(media_type, "application/gzip") ||
           http_ascii_equals_ignore_case(media_type, "application/x-gzip") ||
           http_ascii_equals_ignore_case(media_type, "application/zip") ||
           http_ascii_equals_ignore_case(media_type, "application/zstd") ||
           http_ascii_equals_ignore_case(media_type, "application/pdf") ||
           http_ascii_equals_ignore_case(media_type, "application/octet-stream");
}

[[nodiscard]] cache_control response_cache_control(const http_response& response) noexcept {
    cache_control_field_parser parser;
    for (const auto& header : response.headers()) {
        if (http_ascii_equals_ignore_case(header.name(), "Cache-Control")) {
            parser.update(header.value());
        }
    }
    return parser.finish();
}

}  // namespace

http_response_compression_decision prepare_response_compression(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, http_response_compression_source source_value,
    http_response_coding_availability availability) {
    switch (source_value) {
        case http_response_compression_source::buffered:
        case http_response_compression_source::stream:
        case http_response_compression_source::sse:
            break;
        default:
            throw std::invalid_argument("invalid response compression source");
    }
    switch (availability) {
        case http_response_coding_availability::identity_only:
        case http_response_coding_availability::identity_and_compression:
            break;
        default:
            throw std::invalid_argument("invalid response coding availability");
    }

    if (source_value == http_response_compression_source::buffered && response.file_body().has_value()) {
        return http_response_compression_decision::fixed_representation;
    }
    if (!plan_http_response_body(request_method, response.status()).status_allows_body()) {
        return http_response_compression_decision::fixed_representation;
    }
    const auto status_code = response.status();
    if (status_code == http_status::partial_content || status_code == http_status::reset_content ||
        response_has_header_name(response, "Content-Encoding") ||
        response_has_header_name(response, "Content-Range") ||
        (source_value != http_response_compression_source::sse &&
            response_content_type_skips_compression(
                response.header("Content-Type").value_or(std::string_view{}))) ||
        response_cache_control(response).has(cache_control_directive::no_transform) ||
        availability == http_response_coding_availability::identity_only) {
        return http_response_compression_decision::fixed_representation;
    }

    response.add_vary_token("Accept-Encoding");
    if (selection.coding() == http_content_coding::identity) {
        return http_response_compression_decision::negotiated_identity;
    }
    return http_response_compression_decision::encode;
}

http_response_compression_result apply_response_compression(const http_response_coding_selection& selection,
    http_known_method request_method, http_response& response, const compression_config& options) {
    const auto response_content = response.body_bytes();

    const auto decision = prepare_response_compression(selection, request_method, response,
        http_response_compression_source::buffered,
        http_response_coding_availability::identity_and_compression);
    if (decision != http_response_compression_decision::encode) {
        return http_response_compression_result::make_not_applicable();
    }

    const auto coding = selection.coding();
    if (response_content.size() < options.min_bytes_ ||
        response_content.size() > options.max_bytes_ || response_content.size() > options.sync_bytes_) {
        return http_response_compression_result::make_not_applicable();
    }
    const auto body = response_content;
    const auto max_encoded_bytes = body.empty() ? 0 : body.size() - 1;
    std::optional<http_content_encode_result> encoding;
    try {
        encoding.emplace(encode_http_content(coding, body,
            {.max_encoded_bytes_ = max_encoded_bytes, .resource_ = response.memory_resource()}));
    } catch (...) {
        return http_response_compression_result::make_failed();
    }
    auto* encoded = encoding->encoded();
    if (encoded == nullptr) {
        const auto* failure = encoding->failure();
        if (failure != nullptr &&
            failure->error() == http_content_encode_error::encoded_size_exceeded) {
            return http_response_compression_result::make_not_applicable();
        }
        return http_response_compression_result::make_failed();
    }

    try {
        response.replace_body_with_content_encoding(
            std::move(*encoded).take_bytes(), http_content_coding_token(coding));
    } catch (...) {
        // The representation commit stages every affected header before
        // publishing the owned body. A request-resource failure therefore
        // leaves the identity response usable for the typed 500 path instead
        // of exposing a mixed body/metadata state.
        return http_response_compression_result::make_failed();
    }
    return http_response_compression_result::make_compressed();
}

task<http_response_compression_result> apply_response_compression_async(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, compression_config options, blocking_pool* pool,
    const worker_handle& worker_value) {
    const auto response_content = response.body_bytes();
    const auto coding = selection.coding();
    const auto size = response_content.size();
    if (size <= options.sync_bytes_) {
        co_return apply_response_compression(selection, request_method, response, options);
    }
    if (pool == nullptr) {
        // An explicitly disabled pool removes the offload boundary, not the
        // configured compression policy. Extend the synchronous range through
        // max_bytes and keep the same eligibility/commit behavior.
        options.sync_bytes_ = options.max_bytes_;
        co_return apply_response_compression(selection, request_method, response, options);
    }
    const auto decision = prepare_response_compression(selection, request_method, response,
        http_response_compression_source::buffered,
        http_response_coding_availability::identity_and_compression);
    if (decision != http_response_compression_decision::encode || size < options.min_bytes_ ||
        size > options.max_bytes_) {
        co_return http_response_compression_result::make_not_applicable();
    }

    try {
        std::pmr::string plain(response_content, process_resource());
        auto result_value =
            co_await try_run_blocking(*pool, worker_value, [coding, plain = std::move(plain)]() mutable {
                return encode_buffered_body(coding, std::move(plain));
            });
        if (result_value.failed()) {
            co_return http_response_compression_result::make_failed();
        }
        if (!result_value.completed()) {
            // Queue saturation and shutdown are overload/lifecycle outcomes,
            // not broken encoders. Preserve the identity representation.
            co_return http_response_compression_result::make_not_applicable();
        }
        auto attempt_value = std::move(result_value).value();
        if (attempt_value.status_ == buffered_compression_attempt_status::not_smaller) {
            co_return http_response_compression_result::make_not_applicable();
        }
        if (attempt_value.status_ != buffered_compression_attempt_status::compressed ||
            attempt_value.bytes_.empty()) {
            co_return http_response_compression_result::make_failed();
        }
        try {
            response.replace_body_with_content_encoding(
                std::move(attempt_value.bytes_), http_content_coding_token(coding));
        } catch (...) {
            co_return http_response_compression_result::make_failed();
        }
        co_return http_response_compression_result::make_compressed();
    } catch (...) {
        // Failure to allocate/copy the request-owned input or create the
        // one-shot transport leaves the original identity body intact.
        co_return http_response_compression_result::make_failed();
    }
}

}  // namespace ruvia::detail
