#include "server/http_response_compression.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_accept_match.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_content_codec.h"

namespace ruvia::detail {
namespace {

// A coded representation is worth sending only when strictly smaller than
// the identity bytes; the encoder stops at this cap instead of finishing.
// When the client refuses identity (RFC 9110 §12.5.3), any acceptable coding
// beats a 406, so the cap is lifted.
[[nodiscard]] http_content_encode_options compressed_body_options(
    std::string_view plain, bool identity_accepted, std::pmr::memory_resource* resource) noexcept {
    if (!identity_accepted) {
        return {.max_encoded_bytes_ = (std::numeric_limits<std::size_t>::max)(), .resource_ = resource};
    }
    return {.max_encoded_bytes_ = plain.empty() ? 0 : plain.size() - 1, .resource_ = resource};
}

// Publishes an encoder result as the response representation. The commit
// stages every affected header before publishing the owned body, so a
// request-resource failure leaves the identity response usable for the typed
// 500 path instead of exposing a mixed body/metadata state.
[[nodiscard]] http_response_compression_status commit_compressed_body(
    http_response& response, http_content_coding coding, std::optional<std::pmr::string>&& encoding) noexcept {
    if (!encoding.has_value()) {
        return http_response_compression_status::not_applicable;
    }
    try {
        response.replace_body_with_content_encoding(std::move(*encoding), http_content_coding_token(coding));
    } catch (...) {
        ruvia::report_unhandled_failure("response compression", std::current_exception());
        return http_response_compression_status::failed;
    }
    return http_response_compression_status::compressed;
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

[[nodiscard]] bool has_response_integrity_header(const http_response& response) noexcept {
    for (const auto& header : response.headers()) {
        const auto name = header.name();
        if (name.size() != 11U && name.size() != 14U) {
            continue;
        }
        if (http_ascii_equals_ignore_case(name, "Content-Digest") ||
            http_ascii_equals_ignore_case(name, "Repr-Digest") ||
            http_ascii_equals_ignore_case(name, "Content-MD5")) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] cache_control response_cache_control(const http_response& response) noexcept {
    cache_control_field_parser parser;
    if (!response.header("Cache-Control").has_value()) {
        return parser.finish();
    }
    for (const auto& header : response.headers()) {
        if (http_ascii_equals_ignore_case(header.name(), "Cache-Control")) {
            parser.update(header.value());
        }
    }
    return parser.finish();
}

// The single decimal Content-Length a HEAD handler declared, or nullopt.
[[nodiscard]] std::optional<std::uint64_t> declared_content_length(const http_response& response) noexcept {
    const auto field = response.header("Content-Length");
    if (!field.has_value()) {
        return std::nullopt;
    }
    std::optional<std::uint64_t> declared;
    bool valid = true;
    http_visit_comma_separated_quoted_field_items(*field, [&](std::string_view item) noexcept {
        if (item.empty()) {
            valid = false;
            return false;
        }
        std::uint64_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(item.data(), item.data() + item.size(), parsed);
        if (ec != std::errc{} || ptr != item.data() + item.size() ||
            (declared.has_value() && *declared != parsed)) {
            valid = false;
            return false;
        }
        declared = parsed;
        return true;
    });
    return valid ? declared : std::nullopt;
}

// An empty HEAD body is a HEAD handler describing the GET representation
// without producing it. Encoding those zero bytes would publish the coded
// length of nothing (RFC 9110 §8.6). Apply the coding GET would select, sized
// by the declared Content-Length, and drop that length: the coded size is
// unknown and §8.6 allows omitting it.
[[nodiscard]] http_buffered_compression_step apply_head_content_encoding(
    const http_response_coding_selection& selection, http_response& response,
    const compression_config& options, std::optional<std::uint64_t> declared) {
    const bool encode = declared.has_value()
                            ? !(((*declared == 0 || *declared < options.min_bytes_) &&
                                    selection.identity_accepted()) ||
                                  *declared > options.max_bytes_)
                            : !selection.identity_accepted();
    if (!encode) {
        return {http_response_compression_status::not_applicable, false};
    }
    try {
        response.apply_content_encoding(http_content_coding_token(selection.coding()));
    } catch (...) {
        ruvia::report_unhandled_failure("response compression", std::current_exception());
        return {http_response_compression_status::failed, false};
    }
    return {http_response_compression_status::compressed, false};
}

}  // namespace

http_response_compression_decision prepare_response_compression(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, http_response_compression_source source_value,
    http_response_coding_availability availability) {
    // A file's bytes are served as the representation their validators and
    // HEAD response describe, whether the transport reads the file or the
    // context already carries them in memory.
    if (source_value == http_response_compression_source::buffered && response.has_file_representation()) {
        return http_response_compression_decision::fixed_representation;
    }
    if (!plan_http_response_body(request_method, response.status()).status_allows_body()) {
        return http_response_compression_decision::fixed_representation;
    }
    const auto status_code = response.status();
    // Integrity fields describe the content or the selected representation,
    // both of which include the content coding (RFC 9530 §1, §2, §3; legacy
    // Content-MD5 likewise covered the sent content). Encoding here would
    // leave them describing bytes that are not sent.
    if (availability == http_response_coding_availability::identity_only ||
        status_code == http_status::partial_content || status_code == http_status::reset_content ||
        response.header("Content-Encoding").has_value() || response.header("Content-Range").has_value() ||
        has_response_integrity_header(response) ||
        response_content_type_skips_compression(
            response.header("Content-Type").value_or(std::string_view{})) ||
        response_cache_control(response).has(cache_control_directive::no_transform)) {
        return http_response_compression_decision::fixed_representation;
    }

    response.add_vary_token("Accept-Encoding");
    if (selection.coding() == http_content_coding::identity) {
        return http_response_compression_decision::negotiated_identity;
    }
    return http_response_compression_decision::encode;
}

http_buffered_compression_step apply_response_compression(const http_response_coding_selection& selection,
    http_known_method request_method, http_response& response, const compression_config& options,
    worker_codecs& codecs, bool offload_available) {
    constexpr http_buffered_compression_step not_applicable{http_response_compression_status::not_applicable, false};
    const auto body = response.body_bytes();

    const auto decision = prepare_response_compression(selection, request_method, response,
        http_response_compression_source::buffered,
        http_response_coding_availability::identity_and_compression);
    if (decision == http_response_compression_decision::encode && request_method == http_known_method::head &&
        body.empty() && (response.has_head_metadata_body() || response.header("Content-Length").has_value())) {
        const auto declared = declared_content_length(response);
        if (declared != std::uint64_t{0}) {
            return apply_head_content_encoding(selection, response, options, declared);
        }
    }
    // min_bytes_ only says when encoding stops paying off; a client that
    // refuses identity is served a coding at any size instead of a 406.
    if (decision != http_response_compression_decision::encode ||
        (body.size() < options.min_bytes_ && selection.identity_accepted()) || body.size() > options.max_bytes_) {
        return not_applicable;
    }
    // Without a blocking pool there is no offload boundary, only the
    // configured policy: everything through max_bytes_ encodes in line.
    if (offload_available && body.size() > options.sync_bytes_) {
        return {http_response_compression_status::not_applicable, true};
    }

    const auto coding = selection.coding();
    std::optional<std::pmr::string> encoding;
    try {
        encoding = codecs.encoder(coding).encode(
            body, compressed_body_options(body, selection.identity_accepted(), response.memory_resource()));
    } catch (...) {
        ruvia::report_unhandled_failure("response compression", std::current_exception());
        return {http_response_compression_status::failed, false};
    }
    return {commit_compressed_body(response, coding, std::move(encoding)), false};
}

task<http_response_compression_status> compress_response_offloaded(http_response_coding_selection selection,
    http_response& response, blocking_pool& pool, const worker_handle& worker_value) {
    try {
        const auto coding = selection.coding();
        const bool identity_accepted = selection.identity_accepted();
        std::pmr::string plain(response.body_bytes(), process_resource());
        auto result_value =
            co_await run_blocking(pool, worker_value, [coding, identity_accepted, plain = std::move(plain)]() {
                return encode_http_content(
                    coding, plain, compressed_body_options(plain, identity_accepted, process_resource()));
            });
        if (result_value.failed()) {
            ruvia::report_unhandled_failure("response compression", result_value.error());
            co_return http_response_compression_status::failed;
        }
        if (!result_value.completed()) {
            // Queue saturation and shutdown are overload/lifecycle outcomes,
            // not broken encoders. Preserve the identity representation.
            co_return http_response_compression_status::not_applicable;
        }
        co_return commit_compressed_body(response, coding, std::move(result_value).value());
    } catch (...) {
        ruvia::report_unhandled_failure("response compression", std::current_exception());
        // Failure to allocate/copy the request-owned input or create the
        // one-shot transport leaves the original identity body intact.
        co_return http_response_compression_status::failed;
    }
}

}  // namespace ruvia::detail
