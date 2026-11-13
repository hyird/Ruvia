#include "ruvia/http/http_content_coding.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"

#include "http_connection_fields.h"
// The Content-Encoding field itself (RFC 9110 sections 8.4 and 5.6.1): the token
// each coding is spelled with, the list-grammar accumulator across field lines,
// and which codec a decode or encode of that coding runs on.

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <zlib.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>
#undef ZSTD_STATIC_LINKING_ONLY
#include <bit>
#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
namespace ruvia {

std::string_view http_content_coding_token(http_content_coding coding) noexcept {
    switch (coding) {
        case http_content_coding::brotli:
            return "br";
        case http_content_coding::zstd:
            return "zstd";
        case http_content_coding::gzip:
            return "gzip";
        case http_content_coding::deflate:
            return "deflate";
        case http_content_coding::identity:
            return "identity";
    }
    return {};
}

std::optional<http_content_coding> http_content_coding_from_token(std::string_view token) noexcept {
    switch (token.size()) {
        case 2:
            if (http_ascii_equals_ignore_case(token, "br")) {
                return http_content_coding::brotli;
            }
            break;
        case 4:
            if (http_ascii_equals_ignore_case(token, "gzip")) {
                return http_content_coding::gzip;
            }
            if (http_ascii_equals_ignore_case(token, "zstd")) {
                return http_content_coding::zstd;
            }
            break;
        case 6:
            if (http_ascii_equals_ignore_case(token, "x-gzip")) {
                return http_content_coding::gzip;
            }
            break;
        case 7:
            if (http_ascii_equals_ignore_case(token, "deflate")) {
                return http_content_coding::deflate;
            }
            break;
        case 8:
            if (http_ascii_equals_ignore_case(token, "identity")) {
                return http_content_coding::identity;
            }
            break;
        default:
            break;
    }
    return std::nullopt;
}

}  // namespace ruvia

namespace ruvia::detail {

void http_content_coding_field_parser::update(std::string_view value) noexcept {
    if (invalid_) {
        return;
    }
    // Content-Encoding members are tokens (RFC 9110 section 8.4.1).
    invalid_ = visit_http_content_coding_field(value, http_field_list_role::recipient,
                   [this](std::string_view, std::optional<http_content_coding> coding) noexcept {
                       if (coding.has_value() && *coding == http_content_coding::identity) {
                           return true;
                       }
                       if (codings_.size_ == max_content_codings) {
                           // Keep scanning: later malformed members still invalidate the field.
                           depth_exceeded_ = true;
                       } else if (coding.has_value()) {
                           codings_.values_[codings_.size_++] = *coding;
                       } else {
                           ++codings_.size_;
                       }
                       if (!coding.has_value()) {
                           unknown_coding_ = true;
                       }
                       return true;
                   }) != http_field_list_parse_status::ok;
}

http_content_coding_field_result http_content_coding_field_parser::finish() const noexcept {
    if (invalid_) {
        return http_content_coding_field_result_access::invalid();
    }
    if (depth_exceeded_) {
        return http_content_coding_field_result_access::unsupported(true);
    }
    if (unknown_coding_) {
        return http_content_coding_field_result_access::unsupported(false);
    }
    return http_content_coding_field_result_access::coding(codings_);
}

bool is_valid_http_content_encoding_field_value(std::string_view value, http_field_list_role role) noexcept {
    return http_visit_token_list(value, role, [](std::string_view) noexcept { return true; }) ==
           http_field_list_parse_status::ok;
}

}  // namespace ruvia::detail

namespace ruvia {

http_content_coding_field_result parse_http_content_coding(
    std::string_view value) noexcept {
    detail::http_content_coding_field_parser parser;
    parser.update(value);
    return parser.finish();
}

http_content_coding_field_result parse_http_content_coding_headers(
    std::span<const http_header> headers) noexcept {
    return detail::http_content_coding_from_headers(headers);
}

http_content_coding_field_result parse_http_content_coding_headers(
    const http_response_headers& headers) noexcept {
    detail::http_content_coding_field_parser parser;
    const auto* const first = detail::http_response_header_state_access::known_header(
        headers, detail::response_header_content_encoding);
    if (first == nullptr) {
        return parser.finish();
    }
    if (!detail::http_response_header_access::append(*first)) {
        parser.update(first->value());
        return parser.finish();
    }
    for (const auto& header : headers) {
        if (detail::http_response_header_access::known_bit(header) ==
            detail::response_header_content_encoding) {
            parser.update(header.value());
        }
    }
    return parser.finish();
}

http_content_coding_field_result request_content_coding(const http_request& request) noexcept {
    detail::http_content_coding_field_parser parser;
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::content_encoding, [&parser](std::string_view value) noexcept {
            parser.update(value);
            return true;
        });
    return parser.finish();
}

namespace {

[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_content_stage(
    http_content_coding coding, std::string_view input, http_content_decode_options options) {
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

std::optional<std::string> encode_http_content(
    http_content_coding coding, std::string_view input, http_content_encode_options options) {
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

std::optional<std::string> encode_http_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_encode_options options) {
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

std::variant<std::string, http_content_decode_error> decode_http_content(
    http_content_coding coding, std::string_view input, http_content_decode_options options) {
    return decode_http_content(std::span<const http_content_coding>(&coding, 1), input, options);
}

std::variant<std::string, http_content_decode_error> decode_http_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_decode_options options) {
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

namespace {

[[nodiscard]] bool is_compress_coding_token(std::string_view token) noexcept {
    return http_ascii_equals_ignore_case(token, "compress") ||
           http_ascii_equals_ignore_case(token, "x-compress");
}

[[nodiscard]] int& explicit_quality(http_response_coding_qualities& qualities, http_content_coding coding) noexcept {
    switch (coding) {
        case http_content_coding::gzip:
            return qualities.gzip_.explicit_quality_;
        case http_content_coding::deflate:
            return qualities.deflate_.explicit_quality_;
        case http_content_coding::brotli:
            return qualities.brotli_.explicit_quality_;
        case http_content_coding::zstd:
            return qualities.zstd_.explicit_quality_;
        case http_content_coding::identity:
            break;
    }
    return qualities.identity_.explicit_quality_;
}

// Hands every nonempty Accept-Encoding member's coding token and weight to
// `visitor`. A repeated coding keeps its highest weight.
template <typename visitor_type>
void visit_accept_encoding_members(std::string_view accept_encoding, visitor_type&& visitor) noexcept {
    detail::http_visit_comma_separated_quoted(accept_encoding, [&visitor](std::string_view item) noexcept {
        visitor(detail::http_header_token_before_parameters(item), detail::http_weight_parameter(item));
        return true;
    });
}

}  // namespace

void http_accepted_encoding_quality::update(
    std::string_view accept_encoding, std::string_view coding) noexcept {
    const bool gzip_coding = http_is_gzip_coding_token(coding);
    const bool compress_coding = is_compress_coding_token(coding);
    visit_accept_encoding_members(accept_encoding,
        [coding, gzip_coding, compress_coding, this](std::string_view token, int weight) noexcept {
            const bool explicit_match = gzip_coding       ? http_is_gzip_coding_token(token)
                                        : compress_coding ? is_compress_coding_token(token)
                                                          : http_ascii_equals_ignore_case(token, coding);
            if (explicit_match) {
                explicit_quality_ = std::max(explicit_quality_, weight);
            } else if (token == "*") {
                wildcard_quality_ = std::max(wildcard_quality_, weight);
            }
        });
}

bool http_accepts_encoding(std::string_view accept_encoding, std::string_view coding) noexcept {
    if (accept_encoding.empty()) {
        return http_ascii_equals_ignore_case(coding, "identity");
    }
    http_accepted_encoding_quality quality;
    quality.update(accept_encoding, coding);
    return quality.accepts(http_ascii_equals_ignore_case(coding, "identity"));
}

void http_response_coding_qualities::update(std::string_view accept_encoding) noexcept {
    field_present_ = true;
    visit_accept_encoding_members(accept_encoding, [this](std::string_view token, int weight) noexcept {
        has_non_empty_item_ = true;
        if (const auto coding = http_content_coding_from_token(token)) {
            auto& quality = explicit_quality(*this, *coding);
            quality = std::max(quality, weight);
        } else if (token == "*") {
            for (auto* coding_quality : {&gzip_, &deflate_, &brotli_, &zstd_, &identity_}) {
                coding_quality->wildcard_quality_ = std::max(coding_quality->wildcard_quality_, weight);
            }
        }
    });
}

bool http_accepts_encoding(const http_request& request, std::string_view coding) noexcept {
    http_accepted_encoding_quality quality;
    bool present = false;
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::accept_encoding, [&](std::string_view value) noexcept {
            present = true;
            quality.update(value, coding);
            return true;
        });
    return !present || quality.accepts(http_ascii_equals_ignore_case(coding, "identity"));
}

bool http_response_coding_selection::accepts(std::string_view coding, const http_request& request) const noexcept {
    if (const auto known = http_content_coding_from_token(coding)) {
        return accepts(*known);
    }
    return http_accepts_encoding(request, coding);
}

http_response_content_coding_status evaluate_http_response_content_coding(
    const http_response_coding_selection& selection, const http_response& response,
    const http_request* request) {
    if (!detail::response_has_known_header(response, detail::response_header_content_encoding)) {
        return http_response_content_coding_status::absent;
    }
    const bool evaluate_extensions = request != nullptr &&
                                     detail::http_request_access::has_known_header(*request, detail::http_field_kind::accept_encoding);
    struct extension_quality final {
        std::string_view token_;
        http_accepted_encoding_quality quality_;
    };
    std::array<extension_quality, 8> local{};
    std::size_t local_size = 0;
    std::vector<extension_quality> overflow;
    const auto extensions = [&]() noexcept -> std::span<extension_quality> {
        return overflow.empty() ? std::span(local.data(), local_size) : std::span(overflow);
    };
    bool accepted = true;
    bool invalid = false;
    const auto remember_extension = [&](std::string_view token) {
        for (const auto& extension : extensions()) {
            if (http_ascii_equals_ignore_case(extension.token_, token) ||
                (is_compress_coding_token(extension.token_) && is_compress_coding_token(token))) {
                return;
            }
        }
        if (overflow.empty() && local_size < local.size()) {
            local[local_size++] = {token, {}};
        } else {
            if (overflow.empty()) {
                overflow.reserve(local.size() * 2);
                overflow.insert(overflow.end(), local.begin(), local.end());
            }
            overflow.push_back({token, {}});
        }
    };
    for (const auto& header : response.headers()) {
        if (detail::http_response_header_access::known_bit(header) != detail::response_header_content_encoding) {
            continue;
        }
        const auto status = detail::visit_http_content_coding_field(header.value(), detail::http_field_list_role::recipient,
            [&](std::string_view token, std::optional<http_content_coding> coding) {
                if (coding.has_value()) {
                    accepted = accepted && selection.accepts(*coding);
                } else if (evaluate_extensions) {
                    remember_extension(token);
                }
                return true;
            });
        invalid = invalid || status != detail::http_field_list_parse_status::ok;
    }
    if (invalid) {
        return http_response_content_coding_status::invalid;
    }
    if (!accepted) {
        return http_response_content_coding_status::not_acceptable;
    }
    if (!extensions().empty()) {
        bool field_present = false;
        (void)detail::http_request_access::visit_known_headers(
            *request, detail::http_field_kind::accept_encoding, [&](std::string_view value) noexcept {
                field_present = true;
                visit_accept_encoding_members(value, [&](std::string_view token, int weight) noexcept {
                    for (auto& extension : extensions()) {
                        const bool matches = http_ascii_equals_ignore_case(extension.token_, token) ||
                                             (is_compress_coding_token(extension.token_) && is_compress_coding_token(token));
                        if (matches) {
                            extension.quality_.explicit_quality_ = std::max(extension.quality_.explicit_quality_, weight);
                        } else if (token == "*") {
                            extension.quality_.wildcard_quality_ = std::max(extension.quality_.wildcard_quality_, weight);
                        }
                    }
                });
                return true;
            });
        if (field_present) {
            for (const auto& extension : extensions()) {
                if (!extension.quality_.accepts()) {
                    return http_response_content_coding_status::not_acceptable;
                }
            }
        }
    }
    return http_response_content_coding_status::accepted;
}

http_response_coding_selection select_http_accept_encoding(
    std::string_view accept_encoding, std::span<const http_content_coding> candidates) noexcept {
    http_response_coding_qualities qualities;
    qualities.update(accept_encoding);
    return http_response_coding_selection::select(qualities, candidates);
}

http_response_coding_selection select_http_accept_encoding(
    std::span<const http_header> request_headers,
    std::span<const http_content_coding> candidates) noexcept {
    http_response_coding_qualities qualities;
    for (const auto& header : request_headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Accept-Encoding")) {
            qualities.update(header.value());
        }
    }
    return http_response_coding_selection::select(qualities, candidates);
}

http_response_coding_selection select_http_accept_encoding(
    std::span<const http_header_view> request_headers,
    std::span<const http_content_coding> candidates) noexcept {
    http_response_coding_qualities qualities;
    for (const auto& header : request_headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Accept-Encoding")) {
            qualities.update(header.value());
        }
    }
    return http_response_coding_selection::select(qualities, candidates);
}

http_response_coding_selection select_http_accept_encoding(
    const http_request& request, std::span<const http_content_coding> candidates) noexcept {
    return http_response_coding_selection_for(request).select(candidates);
}

std::optional<http_content_coding> negotiate_http_content_coding(
    std::string_view accept_encoding, std::span<const http_content_coding> candidates) noexcept {
    const auto selection = select_http_accept_encoding(accept_encoding, candidates);
    return selection.acceptable() ? std::optional(selection.coding()) : std::nullopt;
}

std::optional<http_content_coding> negotiate_http_content_coding(
    std::span<const http_header> request_headers,
    std::span<const http_content_coding> candidates) noexcept {
    const auto selection = select_http_accept_encoding(request_headers, candidates);
    return selection.acceptable() ? std::optional(selection.coding()) : std::nullopt;
}

std::optional<http_content_coding> negotiate_http_content_coding(
    std::span<const http_header_view> request_headers,
    std::span<const http_content_coding> candidates) noexcept {
    const auto selection = select_http_accept_encoding(request_headers, candidates);
    return selection.acceptable() ? std::optional(selection.coding()) : std::nullopt;
}

std::optional<http_content_coding> negotiate_http_content_coding(
    const http_request& request, std::span<const http_content_coding> candidates) noexcept {
    const auto selection = select_http_accept_encoding(request, candidates);
    return selection.acceptable() ? std::optional(selection.coding()) : std::nullopt;
}

bool is_http_media_type_compressible(std::string_view content_type_value) noexcept {
    const auto media_type = http_media_type_only(content_type_value);
    if (media_type.empty()) {
        return false;
    }
    const auto starts_with = [&](std::string_view prefix) noexcept {
        return media_type.size() >= prefix.size() &&
               http_ascii_equals_ignore_case(media_type.substr(0, prefix.size()), prefix);
    };
    const auto ends_with = [&](std::string_view suffix) noexcept {
        return media_type.size() >= suffix.size() &&
               http_ascii_equals_ignore_case(
                   media_type.substr(media_type.size() - suffix.size()), suffix);
    };
    if (ends_with("zip")) {
        return false;
    }
    if (starts_with("text/") || ends_with("+json") || ends_with("+xml")) {
        return true;
    }
    static constexpr std::array<std::string_view, 14> compressible_types{
        "application/json",
        "application/javascript",
        "application/x-javascript",
        "application/ecmascript",
        "application/wasm",
        "application/xml",
        "application/x-ndjson",
        "application/vnd.ms-fontobject",
        "font/ttf",
        "font/otf",
        "application/x-font-ttf",
        "application/x-font-otf",
        "image/x-icon",
        "image/vnd.microsoft.icon",
    };
    for (const auto type : compressible_types) {
        if (http_ascii_equals_ignore_case(media_type, type)) {
            return true;
        }
    }
    return false;
}
}  // namespace ruvia

namespace ruvia::detail {

namespace {

// zlib requests `items * size` bytes. Zero-sized and overflowing requests are
// refused with nullptr, which is how zlib expects allocation failure.
voidpf zlib_allocate(voidpf opaque, uInt items, uInt size) noexcept {
    if (items == 0 || size == 0) {
        return nullptr;
    }
    const auto item_bytes = static_cast<std::size_t>(items);
    const auto size_bytes = static_cast<std::size_t>(size);
    if (item_bytes > (std::numeric_limits<std::size_t>::max)() / size_bytes) {
        return nullptr;
    }
    return http_c_library_memory::allocate(item_bytes * size_bytes, opaque);
}

[[nodiscard]] z_stream make_zlib_stream(http_c_library_memory& allocation) noexcept {
    z_stream stream{};
    stream.zalloc = &zlib_allocate;
    stream.zfree = &http_c_library_memory::deallocate_opaque_first;
    stream.opaque = &allocation;
    return stream;
}

}  // namespace

zlib_inflate_stream::zlib_inflate_stream(int window_bits, std::size_t max_memory_bytes)
    : allocation_(max_memory_bytes, max_memory_bytes != (std::numeric_limits<std::size_t>::max)()),
      stream_(make_zlib_stream(allocation_)) {
    if (inflateInit2(&stream_, window_bits) != Z_OK) {
        allocation_.rethrow_if_failed();
    }
}

zlib_inflate_stream::~zlib_inflate_stream() {
    if (initialized()) {
        (void)inflateEnd(&stream_);
    }
}

bool zlib_inflate_stream::reset() noexcept {
    allocation_.clear_allocation_failure();
    return initialized() && inflateReset(&stream_) == Z_OK;
}

zlib_deflate_stream::zlib_deflate_stream(
    int level, int window_bits, int mem_level, int strategy, std::size_t max_memory_bytes)
    : allocation_(max_memory_bytes, max_memory_bytes != (std::numeric_limits<std::size_t>::max)()),
      stream_(make_zlib_stream(allocation_)) {
    if (deflateInit2(&stream_, level, Z_DEFLATED, window_bits, mem_level, strategy) != Z_OK) {
        allocation_.rethrow_if_failed();
    }
}

zlib_deflate_stream::zlib_deflate_stream(zlib_content_format format, std::size_t max_memory_bytes)
    : zlib_deflate_stream(Z_DEFAULT_COMPRESSION,
          format == zlib_content_format::gzip ? zlib_gzip_window_bits : zlib_deflate_window_bits,
          zlib_default_mem_level, Z_DEFAULT_STRATEGY, max_memory_bytes) {}

zlib_deflate_stream::~zlib_deflate_stream() {
    if (initialized()) {
        (void)deflateEnd(&stream_);
    }
}

bool zlib_deflate_stream::reset() noexcept {
    allocation_.clear_allocation_failure();
    return initialized() && deflateReset(&stream_) == Z_OK;
}

bool zlib_deflate_stream::encode(std::string_view input, std::string& output, int flush,
    std::size_t max_encoded_bytes) {
    if (output.size() > max_encoded_bytes) {
        return false;
    }
    const zlib_borrow_scope borrow(stream_);
    std::size_t supplied = 0;
    // Uninitialized scratch: only the bytes zlib produced are copied out.
    std::array<char, 16384> buffer;
    for (;;) {
        zlib_refill_input(stream_, input, supplied);
        // The requested flush applies once the last input window is handed
        // over: after Z_FINISH zlib accepts no further input.
        const bool last_window = supplied == input.size();
        const int operation = last_window ? flush : Z_NO_FLUSH;
        // At the cap, a one-byte probe distinguishes "already complete" from
        // "more output pending" without letting a byte past the cap through.
        const auto room = max_encoded_bytes - output.size();
        const bool probing = room == 0;
        const auto writable = probing ? std::size_t{1} : (std::min)(buffer.size(), room);
        const auto before_input = stream_.avail_in;
        stream_.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream_.avail_out = static_cast<uInt>(writable);
        const int status = deflate(&stream_, operation);
        const bool input_consumed = last_window && stream_.avail_in == 0;
        // Repeating a completed flush has no work left; zlib reports
        // Z_BUF_ERROR without invalidating the stream.
        if (status == Z_BUF_ERROR && operation != Z_FINISH && input_consumed) {
            return true;
        }
        if (status != Z_OK && status != Z_STREAM_END) {
            allocation_.rethrow_if_failed();
            throw http_content_codec_error("zlib content encoding failed");
        }
        const auto produced = writable - stream_.avail_out;
        if (probing && produced != 0) {
            return false;
        }
        output.append(buffer.data(), produced);
        if (status == Z_STREAM_END ||
            (operation != Z_FINISH && input_consumed && stream_.avail_out != 0)) {
            return true;
        }
        if (probing) {
            return false;
        }
        if (produced == 0 && stream_.avail_in == before_input) {
            throw http_content_codec_error("zlib content encoder made no progress");
        }
    }
}

}  // namespace ruvia::detail

// gzip (RFC 1952) and zlib-wrapped deflate through zlib, with tracked working memory.

namespace ruvia::detail {

static std::variant<std::string, http_content_decode_error> decode_zlib_content(std::string_view input,
    std::size_t max_decoded_bytes, int window_bits,
    bool allow_concatenated_members) {
    std::string output;
    zlib_inflate_stream inflater(window_bits);
    if (!inflater.initialized()) {
        throw http_content_codec_error("zlib decoder initialization failed");
    }
    auto& stream = inflater.native();

    std::size_t supplied = 0;
    char buffer[16384];
    for (;;) {
        zlib_refill_input(stream, input, supplied);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = static_cast<uInt>(sizeof(buffer));
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) {
            inflater.rethrow_allocation_failure();
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (!append_decoded_bytes(output, buffer, produced, max_decoded_bytes)) {
            return http_content_decode_error::decoded_size_exceeded;
        }

        if (status == Z_STREAM_END) {
            // RFC 1952 gzip data is a series of members. Preserve any input
            // already supplied to zlib, reset only the member state, and keep
            // decoding until the exact HTTP content boundary is consumed.
            zlib_refill_input(stream, input, supplied);
            if (stream.avail_in == 0 && supplied == input.size()) {
                return output;
            }
            if (!allow_concatenated_members) {
                return http_content_decode_error::invalid_content;
            }
            auto* next_input = stream.next_in;
            const auto available_input = stream.avail_in;
            const int reset = inflateReset2(&stream, window_bits);
            if (reset != Z_OK) {
                throw http_content_codec_error("failed to reset zlib content decoder");
            }
            stream.next_in = next_input;
            stream.avail_in = available_input;
            continue;
        }
        if (status == Z_MEM_ERROR) {
            throw http_content_codec_error("zlib decoder ran out of memory");
        }
        if (status != Z_OK && status != Z_BUF_ERROR) {
            return http_content_decode_error::invalid_content;
        }

        const bool progressed = produced != 0 || stream.avail_in != before_input;
        if (!progressed) {
            if (stream.avail_in == 0 && supplied < input.size()) {
                continue;
            }
            return http_content_decode_error::invalid_content;
        }
    }
}

std::optional<std::string> encode_zlib_content(zlib_deflate_stream& deflater,
    std::string_view input, std::size_t max_encoded_bytes) {
    return encode_whole_content_with_stream(deflater, Z_FINISH, input, max_encoded_bytes);
}
std::variant<std::string, http_content_decode_error> decode_gzip_content(
    std::string_view input, std::size_t max_decoded_bytes) {
    return decode_zlib_content(input, max_decoded_bytes, zlib_gzip_window_bits, true);
}

std::variant<std::string, http_content_decode_error> decode_deflate_content(
    std::string_view input, std::size_t max_decoded_bytes) {
    return decode_zlib_content(input, max_decoded_bytes, zlib_deflate_window_bits, false);
}

std::optional<std::string> encode_gzip_content(
    std::string_view input, std::size_t max_encoded_bytes) {
    return encode_whole_content<zlib_deflate_stream>(
        "zlib encoder initialization failed", Z_FINISH, input, max_encoded_bytes, zlib_content_format::gzip);
}

std::optional<std::string> encode_deflate_content(
    std::string_view input, std::size_t max_encoded_bytes) {
    return encode_whole_content<zlib_deflate_stream>("zlib encoder initialization failed", Z_FINISH, input,
        max_encoded_bytes, zlib_content_format::deflate);
}

}  // namespace ruvia::detail

// Brotli (RFC 7932) state with tracked working memory.
// Brotli retains the allocator context as its opaque pointer, so these types are
// pinned: owners embed them in storage whose address never changes.
//
// Construction rethrows an allocation failure captured during initialization.
// Any other Brotli refusal leaves the stream uninitialized so each owner maps it
// into its own error contract.

namespace ruvia::detail {

// Decoder whose native working memory is bounded by the caller's decoded-size
// cap rather than by the window and metablock sizes the peer declares: a
// declared ring buffer that the cap could never fill is refused, and
// native_budget_exceeded() then reports a cap violation instead of a codec
// failure.
class brotli_decoder_stream final {
public:
    explicit brotli_decoder_stream(
        std::size_t max_decoded_bytes,
        std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
    ~brotli_decoder_stream();
    brotli_decoder_stream(const brotli_decoder_stream&) = delete;
    brotli_decoder_stream& operator=(const brotli_decoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return state_ != nullptr;
    }
    [[nodiscard]] BrotliDecoderState* native() const noexcept {
        return state_;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_if_failed();
    }
    [[nodiscard]] bool native_budget_exceeded() const noexcept {
        return allocation_.budget_exceeded();
    }

private:
    http_c_library_memory allocation_;
    BrotliDecoderState* state_;
};

// LZ77 window exponents. Whole-buffer encoding is bounded by its input and
// released at once, so it keeps the library default; a stream encoder lives as
// long as its response (SSE), so it uses Pingora's 512 KiB window.
inline constexpr int brotli_whole_buffer_window_bits = BROTLI_DEFAULT_WINDOW;
inline constexpr int brotli_stream_window_bits = 19;

// Encoder at quality 5, shared by whole-buffer and streaming HTTP content coding.
class brotli_encoder_stream final {
public:
    explicit brotli_encoder_stream(
        int window_bits,
        std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
    ~brotli_encoder_stream();
    brotli_encoder_stream(const brotli_encoder_stream&) = delete;
    brotli_encoder_stream& operator=(const brotli_encoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return state_ != nullptr;
    }
    // Consumes the input and drains the requested operation into output. False
    // means the total output budget was exceeded before appending a block.
    // After false or an exception, the owner must discard/rebuild this stream.
    [[nodiscard]] bool encode(std::string_view input, std::string& output,
        BrotliEncoderOperation operation, std::size_t max_encoded_bytes);

private:
    http_c_library_memory allocation_;
    BrotliEncoderState* state_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

namespace {

inline constexpr std::uint32_t http_brotli_quality = 5;

// Upper bound of the per-metablock Huffman groups (256 trees each of the
// literal, command and 520-symbol distance alphabets at alphabet + 376
// four-byte entries, about 2.7 MB in the reference decoder) plus context maps,
// block-type trees and the state itself. It does not depend on the window.
inline constexpr std::size_t brotli_decoder_table_allowance = 3U * 1024U * 1024U;

// The reference decoder sizes its ring buffer as the smallest power of two
// covering the bytes already decoded plus the declared metablock length, and
// grows it by doubling, holding the old ring during the copy. Content that
// fits `max_decoded_bytes` therefore never needs more than three times that
// cap (1 KiB floor) plus write-ahead slack; a larger ring can only serve
// content the cap rejects anyway.
[[nodiscard]] std::size_t brotli_decoder_native_budget(std::size_t max_decoded_bytes) noexcept {
    constexpr auto unlimited = (std::numeric_limits<std::size_t>::max)();
    constexpr std::size_t ring_floor = 1024;
    constexpr std::size_t ring_slack = 2 * 1024;
    const auto ring_bytes = (std::max)(max_decoded_bytes, ring_floor);
    if (ring_bytes > (unlimited - ring_slack - brotli_decoder_table_allowance) / 3) {
        return unlimited;
    }
    return 3 * ring_bytes + ring_slack + brotli_decoder_table_allowance;
}

[[nodiscard]] bool brotli_allocation_failure(BrotliDecoderErrorCode error) noexcept {
    switch (error) {
        case BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES:
        case BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS:
        case BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP:
        case BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1:
        case BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2:
        case BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES:
            return true;
        default:
            return false;
    }
}
}  // namespace

brotli_decoder_stream::brotli_decoder_stream(
    std::size_t max_decoded_bytes, std::size_t max_memory_bytes)
    : allocation_(
          (std::min)(brotli_decoder_native_budget(max_decoded_bytes), max_memory_bytes),
          max_memory_bytes < brotli_decoder_native_budget(max_decoded_bytes)),
      state_(BrotliDecoderCreateInstance(
          &http_c_library_memory::allocate_opaque_first, &http_c_library_memory::deallocate_opaque_first, &allocation_)) {
    if (state_ == nullptr) {
        allocation_.rethrow_if_failed();
    }
}

brotli_decoder_stream::~brotli_decoder_stream() {
    if (state_ != nullptr) {
        BrotliDecoderDestroyInstance(state_);
    }
}

brotli_encoder_stream::brotli_encoder_stream(int window_bits, std::size_t max_memory_bytes)
    : allocation_(max_memory_bytes, max_memory_bytes != (std::numeric_limits<std::size_t>::max)()),
      state_(BrotliEncoderCreateInstance(
          &http_c_library_memory::allocate_opaque_first, &http_c_library_memory::deallocate_opaque_first, &allocation_)) {
    if (state_ != nullptr &&
        (BrotliEncoderSetParameter(state_, BROTLI_PARAM_QUALITY, http_brotli_quality) != BROTLI_TRUE ||
            BrotliEncoderSetParameter(state_, BROTLI_PARAM_LGWIN, static_cast<std::uint32_t>(window_bits)) != BROTLI_TRUE)) {
        BrotliEncoderDestroyInstance(state_);
        state_ = nullptr;
    }
    if (state_ == nullptr) {
        allocation_.rethrow_if_failed();
    }
}

brotli_encoder_stream::~brotli_encoder_stream() {
    if (state_ != nullptr) {
        BrotliEncoderDestroyInstance(state_);
    }
}

bool brotli_encoder_stream::encode(std::string_view input, std::string& output,
    BrotliEncoderOperation operation, std::size_t max_encoded_bytes) {
    std::size_t available_input = input.size();
    const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
    for (;;) {
        const auto before_input = available_input;
        std::size_t available_output = 0;
        const auto compressed = BrotliEncoderCompressStream(state_, operation,
            &available_input, &next_input, &available_output, nullptr, nullptr);
        allocation_.rethrow_if_failed();
        if (compressed != BROTLI_TRUE) {
            throw http_content_codec_error("brotli encoding failed");
        }
        std::size_t produced = 0;
        const auto* bytes_value = BrotliEncoderTakeOutput(state_, &produced);
        if (output.size() > max_encoded_bytes || produced > max_encoded_bytes - output.size()) {
            return false;
        }
        // Consume the borrowed block directly into the caller's output before
        // any subsequent encoder call can invalidate it.
        if (produced != 0) {
            output.append(reinterpret_cast<const char*>(bytes_value), produced);
        }
        if (operation == BROTLI_OPERATION_FINISH) {
            if (BrotliEncoderIsFinished(state_) == BROTLI_TRUE) {
                return true;
            }
        } else if (available_input == 0 && BrotliEncoderHasMoreOutput(state_) != BROTLI_TRUE) {
            return true;
        }
        if (produced == 0 && available_input == before_input) {
            throw http_content_codec_error("brotli encoder made no progress");
        }
    }
}

std::variant<std::string, http_content_decode_error> decode_brotli_content(
    std::string_view input, std::size_t max_decoded_bytes) {
    std::string output;
    brotli_decoder_stream decoder(max_decoded_bytes);
    if (!decoder.initialized()) {
        throw http_content_codec_error("brotli decoder initialization failed");
    }
    auto* state_value = decoder.native();

    const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t available_input = input.size();
    for (;;) {
        const auto before_input = available_input;
        std::size_t available_output = 0;
        const auto result_value = BrotliDecoderDecompressStream(
            state_value, &available_input, &next_input, &available_output, nullptr, nullptr);
        bool produced_output = false;
        // The ring buffer can expose more than one contiguous block. Consume
        // each borrow before the next decoder call invalidates it.
        while (BrotliDecoderHasMoreOutput(state_value) == BROTLI_TRUE) {
            std::size_t produced = 0;
            const auto* bytes_value = BrotliDecoderTakeOutput(state_value, &produced);
            if (!append_decoded_bytes(
                    output, reinterpret_cast<const char*>(bytes_value), produced, max_decoded_bytes)) {
                return http_content_decode_error::decoded_size_exceeded;
            }
            produced_output = produced_output || produced != 0;
        }
        if (result_value == BROTLI_DECODER_RESULT_SUCCESS) {
            // RFC 7932 defines one Brotli stream. Its decoder deliberately does
            // not over-consume, so remaining bytes are not part of this content.
            if (available_input == 0) {
                return output;
            }
            return http_content_decode_error::invalid_content;
        }
        if (result_value == BROTLI_DECODER_RESULT_ERROR) {
            // A ring buffer refused by the native budget was declared larger
            // than the cap allows the content to be.
            if (decoder.native_budget_exceeded()) {
                return http_content_decode_error::decoded_size_exceeded;
            }
            decoder.rethrow_allocation_failure();
            if (brotli_allocation_failure(BrotliDecoderGetErrorCode(state_value))) {
                throw std::bad_alloc();
            }
            return http_content_decode_error::invalid_content;
        }
        const bool progressed = produced_output || available_input != before_input;
        if (!progressed ||
            (result_value == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && available_input == 0)) {
            return http_content_decode_error::invalid_content;
        }
    }
}

std::optional<std::string> encode_brotli_content(
    brotli_encoder_stream& encoder, std::string_view input,
    std::size_t max_encoded_bytes) {
    return encode_whole_content_with_stream(
        encoder, BROTLI_OPERATION_FINISH, input, max_encoded_bytes);
}

std::optional<std::string> encode_brotli_content(
    std::string_view input, std::size_t max_encoded_bytes) {
    return encode_whole_content<brotli_encoder_stream>("brotli encoder initialization failed",
        BROTLI_OPERATION_FINISH, input, max_encoded_bytes, brotli_whole_buffer_window_bits);
}

}  // namespace ruvia::detail

// zstd (RFC 8878) state with tracked working memory and
// whose window never exceeds the 8 MiB HTTP limit of RFC 9659. zstd
// retains the allocator context as its opaque pointer, so these types are
// pinned: owners embed them in storage whose address never changes.
//
// Construction rethrows an allocation failure captured during initialization.
// Any other zstd refusal leaves the stream uninitialized so each owner maps it
// into its own error contract.

namespace ruvia::detail {

class zstd_decoder_stream final {
public:
    explicit zstd_decoder_stream(
        std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
    ~zstd_decoder_stream();
    zstd_decoder_stream(const zstd_decoder_stream&) = delete;
    zstd_decoder_stream& operator=(const zstd_decoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return stream_ != nullptr;
    }
    [[nodiscard]] ZSTD_DStream* native() const noexcept {
        return stream_;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_if_failed();
    }

private:
    http_c_library_memory allocation_;
    ZSTD_DStream* stream_;
};

// Encoder at the default compression level, with the level's own window.
// reset() ends any frame in progress and keeps both the parameters and the
// working memory, so the next frame is identical to one from a new encoder.
// false means the owner must destroy it.
class zstd_encoder_stream final {
public:
    explicit zstd_encoder_stream(
        std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
    ~zstd_encoder_stream();
    zstd_encoder_stream(const zstd_encoder_stream&) = delete;
    zstd_encoder_stream& operator=(const zstd_encoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return context_ != nullptr;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_if_failed();
    }
    [[nodiscard]] bool reset() noexcept;
    // The single zstd encode loop, shared by whole-buffer and streaming
    // coding: consumes the input and drains `operation` into output. False
    // means output would exceed `max_encoded_bytes` in total; no byte past
    // that cap is appended. After false or an exception, the owner must reset
    // or discard this stream.
    [[nodiscard]] bool encode(std::string_view input, std::string& output,
        ZSTD_EndDirective operation, std::size_t max_encoded_bytes);

private:
    http_c_library_memory allocation_;
    ZSTD_CCtx* context_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

namespace {

// RFC 9659 §3: HTTP zstd windows never exceed 8 MiB. Decoders must accept
// that window, so it is also the decoder's floor.
inline constexpr int http_zstd_window_log_max = 23;

[[noreturn]] void fail_zstd_encoding(const zstd_encoder_stream& encoder, std::size_t result_value) {
    encoder.rethrow_allocation_failure();
    throw http_content_codec_error(ZSTD_getErrorName(result_value));
}

}  // namespace

zstd_decoder_stream::zstd_decoder_stream(std::size_t max_memory_bytes)
    : allocation_(max_memory_bytes, max_memory_bytes != (std::numeric_limits<std::size_t>::max)()),
      stream_(ZSTD_createDStream_advanced(ZSTD_customMem{
          &http_c_library_memory::allocate_opaque_first, &http_c_library_memory::deallocate_opaque_first, &allocation_})) {
    if (stream_ != nullptr &&
        (ZSTD_isError(ZSTD_initDStream(stream_)) != 0 ||
            ZSTD_isError(ZSTD_DCtx_setParameter(stream_, ZSTD_d_windowLogMax, http_zstd_window_log_max)) != 0)) {
        (void)ZSTD_freeDStream(stream_);
        stream_ = nullptr;
    }
    if (stream_ == nullptr) {
        allocation_.rethrow_if_failed();
    }
}

zstd_decoder_stream::~zstd_decoder_stream() {
    if (stream_ != nullptr) {
        (void)ZSTD_freeDStream(stream_);
    }
}

zstd_encoder_stream::zstd_encoder_stream(std::size_t max_memory_bytes)
    : allocation_(max_memory_bytes, max_memory_bytes != (std::numeric_limits<std::size_t>::max)()),
      context_(ZSTD_createCCtx_advanced(ZSTD_customMem{
          &http_c_library_memory::allocate_opaque_first, &http_c_library_memory::deallocate_opaque_first, &allocation_})) {
    // The level picks its own window (and adapts it to a known source size).
    // RFC 9659's 8 MiB is only a ceiling; pinning it would also fix the
    // streaming input buffer at the window size for every pooled encoder.
    const bool window_exceeds_http_limit =
        ZSTD_getCParams(ZSTD_CLEVEL_DEFAULT, ZSTD_CONTENTSIZE_UNKNOWN, 0).windowLog >
        static_cast<unsigned>(http_zstd_window_log_max);
    if (context_ != nullptr &&
        (ZSTD_isError(ZSTD_CCtx_setParameter(context_, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT)) != 0 ||
            (window_exceeds_http_limit &&
                ZSTD_isError(ZSTD_CCtx_setParameter(context_, ZSTD_c_windowLog, http_zstd_window_log_max)) != 0))) {
        (void)ZSTD_freeCCtx(context_);
        context_ = nullptr;
    }
    if (context_ == nullptr) {
        allocation_.rethrow_if_failed();
    }
}

zstd_encoder_stream::~zstd_encoder_stream() {
    if (context_ != nullptr) {
        (void)ZSTD_freeCCtx(context_);
    }
}

bool zstd_encoder_stream::reset() noexcept {
    allocation_.clear_allocation_failure();
    return context_ != nullptr && ZSTD_isError(ZSTD_CCtx_reset(context_, ZSTD_reset_session_only)) == 0;
}

bool zstd_encoder_stream::encode(std::string_view input, std::string& output,
    ZSTD_EndDirective operation, std::size_t max_encoded_bytes) {
    if (output.size() > max_encoded_bytes) {
        return false;
    }
    ZSTD_inBuffer input_buffer{input.data(), input.size(), 0};
    // Uninitialized scratch: only the bytes zstd produced are copied out.
    std::array<char, 16384> buffer;
    for (;;) {
        // At the cap, a one-byte probe distinguishes "already complete" from
        // "more output pending" without letting a byte past the cap through.
        const auto room = max_encoded_bytes - output.size();
        const bool probing = room == 0;
        const auto writable = probing ? std::size_t{1} : (std::min)(buffer.size(), room);
        ZSTD_outBuffer output_buffer{buffer.data(), writable, 0};
        const auto before_input = input_buffer.pos;
        const auto remaining = ZSTD_compressStream2(context_, &output_buffer, &input_buffer, operation);
        if (ZSTD_isError(remaining) != 0) {
            fail_zstd_encoding(*this, remaining);
        }
        if (probing && output_buffer.pos != 0) {
            return false;
        }
        output.append(buffer.data(), output_buffer.pos);
        const bool input_consumed = input_buffer.pos == input_buffer.size;
        if (input_consumed && (operation == ZSTD_e_continue ? output_buffer.pos < writable : remaining == 0)) {
            return true;
        }
        if (probing) {
            return false;
        }
        if (output_buffer.pos == 0 && input_buffer.pos == before_input) {
            throw http_content_codec_error("Zstd content encoder made no progress");
        }
    }
}

std::variant<std::string, http_content_decode_error> decode_zstd_content(
    std::string_view input, std::size_t max_decoded_bytes) {
    std::string output;
    zstd_decoder_stream decoder;
    if (!decoder.initialized()) {
        throw http_content_codec_error("zstd decoder initialization failed");
    }
    auto* stream = decoder.native();

    ZSTD_inBuffer in{input.data(), input.size(), 0};
    char buffer[16384];
    for (;;) {
        const auto before_input = in.pos;
        ZSTD_outBuffer out{buffer, sizeof(buffer), 0};
        const auto result_value = ZSTD_decompressStream(stream, &out, &in);
        if (ZSTD_isError(result_value) != 0) {
            decoder.rethrow_allocation_failure();
            if (ZSTD_getErrorCode(result_value) == ZSTD_error_memory_allocation) {
                throw http_content_codec_error(ZSTD_getErrorName(result_value));
            }
            return http_content_decode_error::invalid_content;
        }
        if (!append_decoded_bytes(output, buffer, out.pos, max_decoded_bytes)) {
            return http_content_decode_error::decoded_size_exceeded;
        }
        if (result_value == 0 && in.pos == in.size) {
            return output;
        }
        // A zero result with remaining input completed one RFC 8878 frame;
        // ZSTD_decompressStream is ready to consume the next concatenated frame.
        if (out.pos == 0 && in.pos == before_input) {
            return http_content_decode_error::invalid_content;
        }
    }
}

std::optional<std::string> encode_zstd_content(
    zstd_encoder_stream& encoder, std::string_view input,
    std::size_t max_encoded_bytes) {
    return encode_whole_content_with_stream(encoder, ZSTD_e_end, input, max_encoded_bytes);
}

std::optional<std::string> encode_zstd_content(
    std::string_view input, std::size_t max_encoded_bytes) {
    return encode_whole_content<zstd_encoder_stream>(
        "zstd encoder initialization failed", ZSTD_e_end, input, max_encoded_bytes);
}

}  // namespace ruvia::detail

namespace ruvia {

namespace {

using encoder_stream = std::variant<detail::zlib_deflate_stream, detail::brotli_encoder_stream, detail::zstd_encoder_stream>;

void emplace_encoder_stream(std::optional<encoder_stream>& target, http_content_coding coding,
    std::size_t max_memory_bytes) {
    switch (coding) {
        case http_content_coding::gzip:
        case http_content_coding::deflate:
            target.emplace(std::in_place_type<detail::zlib_deflate_stream>,
                coding == http_content_coding::gzip ? detail::zlib_content_format::gzip
                                                    : detail::zlib_content_format::deflate,
                max_memory_bytes);
            return;
        case http_content_coding::brotli:
            target.emplace(std::in_place_type<detail::brotli_encoder_stream>,
                detail::brotli_stream_window_bits, max_memory_bytes);
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

[[nodiscard]] bool encode_stream(detail::zlib_deflate_stream& deflater, std::string_view input,
    std::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const int operation = finish_value ? Z_FINISH : flush ? Z_SYNC_FLUSH
                                                          : Z_NO_FLUSH;
    return deflater.encode(input, output, operation, max_encoded_bytes);
}

[[nodiscard]] bool encode_stream(detail::brotli_encoder_stream& encoder, std::string_view input,
    std::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const auto operation = finish_value ? BROTLI_OPERATION_FINISH : flush ? BROTLI_OPERATION_FLUSH
                                                                          : BROTLI_OPERATION_PROCESS;
    return encoder.encode(input, output, operation, max_encoded_bytes);
}

[[nodiscard]] bool encode_stream(detail::zstd_encoder_stream& encoder, std::string_view input,
    std::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const auto operation = finish_value ? ZSTD_e_end : flush ? ZSTD_e_flush
                                                             : ZSTD_e_continue;
    return encoder.encode(input, output, operation, max_encoded_bytes);
}

}  // namespace

// One pinned codec stream per non-identity encoder. A failed initialization
// unwinds through the stream destructor, including an exception captured at
// the C ABI.
struct http_content_encoder::impl_type final {
    impl_type(http_content_coding coding, http_content_encoder_options options)
        : max_encoded_bytes_(options.max_encoded_bytes_),
          max_memory_bytes_(options.max_memory_bytes_),
          coding_(coding) {
        if (coding_ != http_content_coding::identity) {
            emplace_encoder_stream(stream_, coding_, max_memory_bytes_);
            std::visit(
                [](const auto& stream) {
                    if (!stream.initialized()) {
                        throw http_content_codec_error(initialization_failure(stream));
                    }
                },
                *stream_);
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
        const auto output_cap = remaining > (std::numeric_limits<std::size_t>::max)() - before_size
                                    ? (std::numeric_limits<std::size_t>::max)()
                                    : before_size + remaining;
        const bool ok = std::visit(
            [&](auto& stream) {
                return encode_stream(stream, input, output, flush, finish_value, output_cap);
            },
            *stream_);
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
            const auto& rebuilt = stream_->emplace<detail::brotli_encoder_stream>(
                detail::brotli_stream_window_bits, max_memory_bytes_);
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

http_content_encoder::http_content_encoder(
    http_content_coding coding, http_content_encoder_options options)
    : coding_(coding) {
    if (coding_ != http_content_coding::identity ||
        options.max_encoded_bytes_ != (std::numeric_limits<std::size_t>::max)() ||
        options.max_memory_bytes_ != (std::numeric_limits<std::size_t>::max)()) {
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

    explicit impl_type(http_content_coding coding)
        : stream_(make_stream(coding)) {}

    [[nodiscard]] bool initialized() const noexcept {
        return std::visit([](const auto& stream) { return stream.initialized(); }, stream_);
    }

    [[nodiscard]] bool reset() noexcept {
        return std::visit([](auto& stream) { return stream.reset(); }, stream_);
    }

    [[nodiscard]] std::optional<std::string> encode(
        std::string_view input, http_content_encode_options options) {
        if (auto* deflater = std::get_if<detail::zlib_deflate_stream>(&stream_)) {
            return detail::encode_zlib_content(*deflater, input, options.max_encoded_bytes_);
        }
        return detail::encode_zstd_content(
            std::get<detail::zstd_encoder_stream>(stream_), input, options.max_encoded_bytes_);
    }

    stream_type stream_;

private:
    [[nodiscard]] static stream_type make_stream(http_content_coding coding) {
        if (coding == http_content_coding::zstd) {
            return stream_type(std::in_place_type<detail::zstd_encoder_stream>);
        }
        return stream_type(std::in_place_type<detail::zlib_deflate_stream>,
            coding == http_content_coding::gzip ? detail::zlib_content_format::gzip
                                                : detail::zlib_content_format::deflate);
    }
};

http_content_encode_context::http_content_encode_context(
    http_content_coding coding) noexcept
    : coding_(coding) {}

http_content_encode_context::~http_content_encode_context() {
    delete impl_;
}

std::optional<std::string> http_content_encode_context::encode(
    std::string_view input, http_content_encode_options options) {
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

namespace {

class streaming_decode_stage final {
public:
    streaming_decode_stage(http_content_coding coding,
        std::size_t max_decoded_bytes, std::size_t max_memory_bytes)
        : coding_(coding),
          max_decoded_bytes_(max_decoded_bytes) {
        emplace_decoder_stream(coding, max_decoded_bytes, max_memory_bytes);
    }
    streaming_decode_stage(const streaming_decode_stage&) = delete;
    streaming_decode_stage& operator=(const streaming_decode_stage&) = delete;

    void write(std::string_view input, std::string& output) {
        if (input.empty()) {
            return;
        }
        saw_input_ = true;
        if (auto* zlib_decoder = std::get_if<detail::zlib_inflate_stream>(&*stream_)) {
            write_zlib(*zlib_decoder, input, output);
        } else if (auto* brotli_decoder = std::get_if<detail::brotli_decoder_stream>(&*stream_)) {
            write_brotli(*brotli_decoder, input, output);
        } else if (auto* zstd_decoder = std::get_if<detail::zstd_decoder_stream>(&*stream_)) {
            write_zstd(*zstd_decoder, input, output);
        }
    }

    void finish() const {
        if (saw_input_ && !at_frame_end_) {
            throw http_content_transcode_error(
                http_content_decode_error::invalid_content, "truncated compressed HTTP content");
        }
    }

private:
    using decoder_variant = std::variant<detail::zlib_inflate_stream, detail::brotli_decoder_stream,
        detail::zstd_decoder_stream>;

    void emplace_decoder_stream(http_content_coding coding,
        std::size_t max_decoded_bytes, std::size_t max_memory_bytes) {
        switch (coding) {
            case http_content_coding::gzip:
            case http_content_coding::deflate: {
                auto& var = stream_.emplace(std::in_place_type<detail::zlib_inflate_stream>,
                    coding == http_content_coding::gzip ? detail::zlib_gzip_window_bits
                                                        : detail::zlib_deflate_window_bits,
                    max_memory_bytes);
                if (!std::get<detail::zlib_inflate_stream>(var).initialized()) {
                    throw http_content_codec_error("failed to initialize zlib content decoder");
                }
                return;
            }
            case http_content_coding::brotli: {
                auto& var = stream_.emplace(std::in_place_type<detail::brotli_decoder_stream>,
                    max_decoded_bytes, max_memory_bytes);
                const auto& decoder = std::get<detail::brotli_decoder_stream>(var);
                if (!decoder.initialized()) {
                    if (decoder.native_budget_exceeded()) {
                        throw http_content_transcode_error(
                            http_content_decode_error::decoded_size_exceeded,
                            "Brotli decoder memory budget exceeded");
                    }
                    throw http_content_codec_error("failed to initialize Brotli content decoder");
                }
                return;
            }
            case http_content_coding::zstd: {
                auto& var =
                    stream_.emplace(std::in_place_type<detail::zstd_decoder_stream>, max_memory_bytes);
                if (!std::get<detail::zstd_decoder_stream>(var).initialized()) {
                    throw http_content_codec_error("failed to initialize Zstd content decoder");
                }
                return;
            }
            case http_content_coding::identity:
                break;
        }
        throw std::invalid_argument("unsupported HTTP content coding for decode stage");
    }

    void append_checked(std::string& output, const char* data, std::size_t size) {
        if (size > max_decoded_bytes_ - decoded_bytes_) {
            throw http_content_transcode_error(
                http_content_decode_error::decoded_size_exceeded,
                "decoded HTTP content size exceeded limit");
        }
        output.append(data, size);
        decoded_bytes_ += size;
    }

    void write_zlib(
        detail::zlib_inflate_stream& inflater, std::string_view input, std::string& output) {
        const bool allow_concatenated = coding_ == http_content_coding::gzip;
        const int window_bits =
            allow_concatenated ? detail::zlib_gzip_window_bits : detail::zlib_deflate_window_bits;
        auto& stream = inflater.native();
        const detail::zlib_borrow_scope borrow(stream);
        if (at_frame_end_) {
            if (!allow_concatenated) {
                throw http_content_transcode_error(http_content_decode_error::invalid_content,
                    "trailing bytes after deflate stream");
            }
            if (inflateReset2(&stream, window_bits) != Z_OK) {
                throw http_content_codec_error("failed to reset zlib content decoder");
            }
            at_frame_end_ = false;
        }
        std::size_t supplied = 0;
        std::array<char, 16384> buffer;
        for (;;) {
            detail::zlib_refill_input(stream, input, supplied);
            const auto before_input = stream.avail_in;
            stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());
            const int status = inflate(&stream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) {
                inflater.rethrow_allocation_failure();
            }
            const auto produced = buffer.size() - stream.avail_out;
            if (produced != 0) {
                append_checked(output, buffer.data(), produced);
            }
            if (status == Z_STREAM_END) {
                at_frame_end_ = true;
                detail::zlib_refill_input(stream, input, supplied);
                if (stream.avail_in == 0 && supplied == input.size()) {
                    return;
                }
                if (!allow_concatenated) {
                    throw http_content_transcode_error(http_content_decode_error::invalid_content,
                        "trailing bytes after deflate stream");
                }
                auto* const next_input = stream.next_in;
                const auto available_input = stream.avail_in;
                if (inflateReset2(&stream, window_bits) != Z_OK) {
                    throw http_content_codec_error("failed to reset zlib content decoder");
                }
                stream.next_in = next_input;
                stream.avail_in = available_input;
                at_frame_end_ = false;
                continue;
            }
            if (status == Z_MEM_ERROR) {
                throw http_content_codec_error("zlib decoder ran out of memory");
            }
            if (status != Z_OK && status != Z_BUF_ERROR) {
                throw http_content_transcode_error(
                    http_content_decode_error::invalid_content, "invalid zlib compressed content");
            }
            const bool progressed = produced != 0 || stream.avail_in != before_input;
            if (!progressed) {
                if (stream.avail_in == 0 && supplied < input.size()) {
                    continue;
                }
                if (stream.avail_in == 0 && supplied == input.size()) {
                    return;
                }
                throw http_content_transcode_error(
                    http_content_decode_error::invalid_content, "invalid zlib compressed content");
            }
        }
    }

    void write_brotli(
        detail::brotli_decoder_stream& decoder, std::string_view input, std::string& output) {
        if (at_frame_end_) {
            throw http_content_transcode_error(
                http_content_decode_error::invalid_content, "trailing bytes after Brotli stream");
        }
        auto* const state_value = decoder.native();
        const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
        std::size_t available_input = input.size();
        for (;;) {
            const auto before_input = available_input;
            std::size_t available_output = 0;
            const auto result_value = BrotliDecoderDecompressStream(
                state_value, &available_input, &next_input, &available_output, nullptr, nullptr);
            bool produced_output = false;
            while (BrotliDecoderHasMoreOutput(state_value) == BROTLI_TRUE) {
                std::size_t produced = 0;
                const auto* const bytes_value = BrotliDecoderTakeOutput(state_value, &produced);
                if (produced != 0) {
                    append_checked(output, reinterpret_cast<const char*>(bytes_value), produced);
                    produced_output = true;
                }
            }
            if (result_value == BROTLI_DECODER_RESULT_SUCCESS) {
                if (available_input != 0) {
                    throw http_content_transcode_error(http_content_decode_error::invalid_content,
                        "trailing bytes after Brotli stream");
                }
                at_frame_end_ = true;
                return;
            }
            if (result_value == BROTLI_DECODER_RESULT_ERROR) {
                if (decoder.native_budget_exceeded()) {
                    throw http_content_transcode_error(
                        http_content_decode_error::decoded_size_exceeded,
                        "Brotli decoder memory budget exceeded");
                }
                decoder.rethrow_allocation_failure();
                throw http_content_transcode_error(http_content_decode_error::invalid_content,
                    "invalid Brotli compressed content");
            }
            if (result_value == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && available_input == 0) {
                return;
            }
            if (!produced_output && available_input == before_input) {
                throw http_content_transcode_error(http_content_decode_error::invalid_content,
                    "invalid Brotli compressed content");
            }
        }
    }

    void write_zstd(
        detail::zstd_decoder_stream& decoder, std::string_view input, std::string& output) {
        auto* const stream = decoder.native();
        ZSTD_inBuffer in{input.data(), input.size(), 0};
        std::array<char, 16384> buffer;
        at_frame_end_ = false;
        for (;;) {
            const auto before_input = in.pos;
            ZSTD_outBuffer out{buffer.data(), buffer.size(), 0};
            const auto result_value = ZSTD_decompressStream(stream, &out, &in);
            if (ZSTD_isError(result_value) != 0) {
                decoder.rethrow_allocation_failure();
                throw http_content_transcode_error(
                    http_content_decode_error::invalid_content, "invalid Zstd compressed content");
            }
            if (out.pos != 0) {
                append_checked(output, buffer.data(), out.pos);
            }
            if (result_value == 0) {
                at_frame_end_ = true;
                if (in.pos == in.size) {
                    return;
                }
                at_frame_end_ = false;
                continue;
            }
            if (in.pos == in.size && out.pos < buffer.size()) {
                return;
            }
            if (out.pos == 0 && in.pos == before_input) {
                throw http_content_transcode_error(
                    http_content_decode_error::invalid_content, "invalid Zstd compressed content");
            }
        }
    }

    http_content_coding coding_;
    std::size_t max_decoded_bytes_;
    std::size_t decoded_bytes_{0};
    bool saw_input_{false};
    bool at_frame_end_{false};
    std::optional<decoder_variant> stream_;
};

}  // namespace

struct http_content_transcoder::impl_type final {
    using stage_ptr = std::unique_ptr<streaming_decode_stage>;

    impl_type(std::span<const http_content_coding> source_codings,
        http_content_coding target_coding, http_content_transcoder_options options)
        : options_(options),
          target_coding_(target_coding),
          encoder_(target_coding,
              http_content_encoder_options{
                  .max_encoded_bytes_ = options.max_encoded_bytes_,
                  .max_memory_bytes_ = options.max_memory_bytes_}) {
        if (http_content_coding_token(target_coding).empty()) {
            throw std::invalid_argument("unsupported target HTTP content coding");
        }
        for (auto it = source_codings.rbegin(); it != source_codings.rend(); ++it) {
            if (http_content_coding_token(*it).empty()) {
                throw std::invalid_argument("unsupported source HTTP content coding");
            }
            if (*it != http_content_coding::identity) {
                source_codings_.push_back(*it);
            }
        }
        build_stages();
    }

    void build_stages() {
        stages_.clear();
        stages_.reserve(source_codings_.size());
        for (const auto coding : source_codings_) {
            stages_.push_back(std::make_unique<streaming_decode_stage>(
                coding, options_.max_decoded_bytes_, options_.max_memory_bytes_));
        }
        identity_decoded_bytes_ = 0;
    }

    void write(std::string_view input, std::string& output, bool flush) {
        if (stages_.empty()) {
            if (input.size() > options_.max_decoded_bytes_ - identity_decoded_bytes_) {
                throw http_content_transcode_error(
                    http_content_decode_error::decoded_size_exceeded,
                    "decoded HTTP content size exceeded limit");
            }
            identity_decoded_bytes_ += input.size();
            encoder_.write(input, output, flush);
            return;
        }
        if (stages_.size() == 1 && target_coding_ == http_content_coding::identity &&
            options_.max_encoded_bytes_ == (std::numeric_limits<std::size_t>::max)() &&
            !detail::http_text_aliases_string(input, output)) {
            stages_.front()->write(input, output);
            return;
        }
        scratch_a_.clear();
        stages_.front()->write(input, scratch_a_);
        std::string* current = &scratch_a_;
        std::string* next = &scratch_b_;
        for (std::size_t index = 1; index < stages_.size(); ++index) {
            next->clear();
            stages_[index]->write(*current, *next);
            std::swap(current, next);
        }
        if (!current->empty() || flush) {
            encoder_.write(*current, output, flush);
        }
    }

    void finish(std::string& output) {
        for (const auto& stage : stages_) {
            stage->finish();
        }
        encoder_.finish(output);
    }

    void reset() {
        build_stages();
        scratch_a_.clear();
        scratch_b_.clear();
        encoder_.reset();
    }

    std::vector<http_content_coding> source_codings_;
    std::vector<stage_ptr> stages_;
    std::string scratch_a_;
    std::string scratch_b_;
    http_content_transcoder_options options_;
    std::size_t identity_decoded_bytes_{0};
    http_content_coding target_coding_;
    http_content_encoder encoder_;
};

http_content_transcoder::http_content_transcoder(http_content_coding source_coding,
    http_content_coding target_coding, http_content_transcoder_options options)
    : http_content_transcoder(
          std::span<const http_content_coding>(&source_coding, 1), target_coding, options) {}

http_content_transcoder::http_content_transcoder(
    std::span<const http_content_coding> source_codings, http_content_coding target_coding,
    http_content_transcoder_options options)
    : target_coding_(target_coding),
      impl_(new impl_type(source_codings, target_coding, options)) {}

http_content_transcoder::~http_content_transcoder() {
    delete impl_;
}

void http_content_transcoder::write(
    std::string_view input, std::string& output, bool flush) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP content transcoder is no longer writable");
    }
    try {
        impl_->write(input, output, flush);
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_content_transcoder::finish(std::string& output) {
    if (phase_ == phase::finished) {
        return;
    }
    if (phase_ == phase::failed) {
        throw std::logic_error("HTTP content transcoder has failed");
    }
    try {
        impl_->finish(output);
        phase_ = phase::finished;
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_content_transcoder::reset() {
    try {
        impl_->reset();
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
    phase_ = phase::active;
}

struct http_staged_content_transcoder::impl_type final {
    impl_type(std::span<const http_content_coding> source_codings,
        http_content_coding target_coding, http_staged_transcode_options options)
        : min_bytes_(options.min_bytes_),
          max_bytes_(options.max_bytes_),
          max_memory_bytes_(options.max_memory_bytes_),
          target_coding_(target_coding),
          require_smaller_(options.require_smaller_than_source_) {
        if (min_bytes_ > max_bytes_) {
            throw std::invalid_argument(
                "staged transcoder minimum size must not exceed maximum size");
        }
        if (http_content_coding_token(target_coding_).empty()) {
            throw std::invalid_argument("unsupported HTTP target content coding");
        }
        for (const auto coding : source_codings) {
            if (http_content_coding_token(coding).empty()) {
                throw std::invalid_argument("unsupported HTTP source content coding");
            }
            if (coding != http_content_coding::identity) {
                source_codings_.push_back(coding);
            }
        }
        if (!source_codings_.empty() && max_bytes_ > 0) {
            const std::size_t decode_cap =
                max_bytes_ == (std::numeric_limits<std::size_t>::max)() ? max_bytes_ : max_bytes_ + 1U;
            decoder_.emplace(source_codings_, http_content_coding::identity,
                http_content_transcoder_options{
                    .max_decoded_bytes_ = decode_cap,
                    .max_memory_bytes_ = max_memory_bytes_,
                });
        }
    }

    void switch_to_passthrough() noexcept {
        passthrough_ = true;
        decoder_.reset();
        std::string().swap(staged_decoded_);
    }

    bool write(std::string_view input, std::string* passthrough_out) {
        if (passthrough_) {
            if (!input.empty()) {
                if (passthrough_out != nullptr) {
                    if (!staged_raw_.empty()) {
                        passthrough_out->append(staged_raw_);
                        std::string().swap(staged_raw_);
                    }
                    passthrough_out->append(input);
                } else {
                    staged_raw_.append(input);
                }
            } else if (passthrough_out != nullptr && !staged_raw_.empty()) {
                passthrough_out->append(staged_raw_);
                std::string().swap(staged_raw_);
            }
            return false;
        }

        if (input.size() > max_bytes_ || staged_raw_.size() > max_bytes_ - input.size()) {
            switch_to_passthrough();
            if (passthrough_out != nullptr) {
                if (!staged_raw_.empty()) {
                    passthrough_out->append(staged_raw_);
                    std::string().swap(staged_raw_);
                }
                passthrough_out->append(input);
            } else {
                staged_raw_.append(input);
            }
            return false;
        }

        if (input.size() > max_memory_bytes_ - staged_raw_.size()) {
            throw std::bad_alloc();
        }
        staged_raw_.append(input);
        if (source_codings_.empty()) {
            decoded_bytes_ = staged_raw_.size();
            return true;
        }

        try {
            decoder_->write(input, staged_decoded_, false);
            decoded_bytes_ = staged_decoded_.size();
            if (decoded_bytes_ > max_bytes_) {
                switch_to_passthrough();
                if (passthrough_out != nullptr && !staged_raw_.empty()) {
                    passthrough_out->append(staged_raw_);
                    std::string().swap(staged_raw_);
                }
                return false;
            }
        } catch (const http_content_transcode_error& err) {
            if (err.decode_error() == http_content_decode_error::decoded_size_exceeded) {
                switch_to_passthrough();
                if (passthrough_out != nullptr && !staged_raw_.empty()) {
                    passthrough_out->append(staged_raw_);
                    std::string().swap(staged_raw_);
                }
                return false;
            }
            throw;
        }
        return true;
    }

    http_staged_transcode_result finish() {
        const http_content_coding fallback_coding =
            source_codings_.empty() ? http_content_coding::identity : source_codings_.back();
        if (passthrough_) {
            return http_staged_transcode_result(
                false, fallback_coding, decoded_bytes_, std::move(staged_raw_));
        }
        if (!source_codings_.empty()) {
            try {
                decoder_->finish(staged_decoded_);
                decoded_bytes_ = staged_decoded_.size();
                if (decoded_bytes_ > max_bytes_) {
                    switch_to_passthrough();
                    return http_staged_transcode_result(
                        false, fallback_coding, decoded_bytes_, std::move(staged_raw_));
                }
            } catch (const http_content_transcode_error& err) {
                if (err.decode_error() == http_content_decode_error::decoded_size_exceeded) {
                    switch_to_passthrough();
                    return http_staged_transcode_result(
                        false, fallback_coding, decoded_bytes_, std::move(staged_raw_));
                }
                throw;
            }
        } else {
            decoded_bytes_ = staged_raw_.size();
        }

        const std::string_view plain_view =
            source_codings_.empty() ? std::string_view(staged_raw_) : std::string_view(staged_decoded_);
        if (plain_view.size() < min_bytes_ || plain_view.size() > max_bytes_ ||
            (source_codings_.empty() && target_coding_ == http_content_coding::identity)) {
            return http_staged_transcode_result(
                false, fallback_coding, plain_view.size(), std::move(staged_raw_));
        }
        if (target_coding_ == http_content_coding::identity) {
            return http_staged_transcode_result(true, http_content_coding::identity,
                plain_view.size(), std::move(staged_decoded_));
        }

        const std::size_t max_encoded =
            require_smaller_
                ? (staged_raw_.empty() ? 0U : staged_raw_.size() - 1U)
                : (std::numeric_limits<std::size_t>::max)();
        std::optional<std::string> encoded;
        if (max_memory_bytes_ == (std::numeric_limits<std::size_t>::max)()) {
            encoded = encode_http_content(target_coding_, plain_view,
                http_content_encode_options{
                    .max_encoded_bytes_ = max_encoded,
                });
        } else {
            switch (target_coding_) {
                case http_content_coding::gzip:
                case http_content_coding::deflate: {
                    detail::zlib_deflate_stream deflater(
                        target_coding_ == http_content_coding::gzip
                            ? detail::zlib_content_format::gzip
                            : detail::zlib_content_format::deflate,
                        max_memory_bytes_);
                    if (!deflater.initialized()) {
                        throw http_content_codec_error("zlib encoder initialization failed");
                    }
                    encoded = detail::encode_zlib_content(deflater, plain_view, max_encoded);
                    break;
                }
                case http_content_coding::brotli: {
                    detail::brotli_encoder_stream encoder(
                        detail::brotli_whole_buffer_window_bits, max_memory_bytes_);
                    if (!encoder.initialized()) {
                        throw http_content_codec_error("brotli encoder initialization failed");
                    }
                    encoded = detail::encode_brotli_content(encoder, plain_view, max_encoded);
                    break;
                }
                case http_content_coding::zstd: {
                    detail::zstd_encoder_stream encoder(max_memory_bytes_);
                    if (!encoder.initialized()) {
                        throw http_content_codec_error("zstd encoder initialization failed");
                    }
                    encoded = detail::encode_zstd_content(encoder, plain_view, max_encoded);
                    break;
                }
                case http_content_coding::identity:
                    break;
            }
        }
        if (!encoded.has_value()) {
            return http_staged_transcode_result(
                false, fallback_coding, plain_view.size(), std::move(staged_raw_));
        }
        return http_staged_transcode_result(
            true, target_coding_, plain_view.size(), std::move(*encoded));
    }

    void reset() {
        staged_raw_.clear();
        staged_decoded_.clear();
        decoded_bytes_ = 0;
        passthrough_ = false;
        if (!source_codings_.empty() && max_bytes_ > 0) {
            const std::size_t decode_cap =
                max_bytes_ == (std::numeric_limits<std::size_t>::max)() ? max_bytes_ : max_bytes_ + 1U;
            if (decoder_.has_value()) {
                decoder_->reset();
            } else {
                decoder_.emplace(source_codings_, http_content_coding::identity,
                    http_content_transcoder_options{
                        .max_decoded_bytes_ = decode_cap,
                        .max_memory_bytes_ = max_memory_bytes_,
                    });
            }
        } else {
            decoder_.reset();
        }
    }

    std::vector<http_content_coding> source_codings_;
    std::optional<http_content_transcoder> decoder_;
    std::string staged_raw_;
    std::string staged_decoded_;
    std::size_t min_bytes_;
    std::size_t max_bytes_;
    std::size_t max_memory_bytes_;
    std::size_t decoded_bytes_{0};
    http_content_coding target_coding_;
    bool require_smaller_{false};
    bool passthrough_{false};
};

http_staged_content_transcoder::http_staged_content_transcoder(
    http_content_coding source_coding, http_content_coding target_coding,
    http_staged_transcode_options options)
    : http_staged_content_transcoder(
          std::span<const http_content_coding>(&source_coding, 1), target_coding, options) {}

http_staged_content_transcoder::http_staged_content_transcoder(
    std::span<const http_content_coding> source_codings, http_content_coding target_coding,
    http_staged_transcode_options options)
    : target_coding_(target_coding),
      impl_(new impl_type(source_codings, target_coding, options)) {}

http_staged_content_transcoder::~http_staged_content_transcoder() {
    delete impl_;
}

bool http_staged_content_transcoder::passthrough() const noexcept {
    return impl_ != nullptr && impl_->passthrough_;
}

std::size_t http_staged_content_transcoder::staged_raw_bytes() const noexcept {
    return impl_ != nullptr ? impl_->staged_raw_.size() : 0;
}

std::size_t http_staged_content_transcoder::decoded_bytes() const noexcept {
    return impl_ != nullptr ? impl_->decoded_bytes_ : 0;
}

bool http_staged_content_transcoder::write(std::string_view input) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP staged content transcoder is no longer writable");
    }
    try {
        return impl_->write(input, nullptr);
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

bool http_staged_content_transcoder::write(
    std::string_view input, std::string& passthrough_output) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP staged content transcoder is no longer writable");
    }
    try {
        if (detail::http_text_aliases_string(input, passthrough_output)) {
            const std::string owned(input);
            return impl_->write(owned, &passthrough_output);
        }
        return impl_->write(input, &passthrough_output);
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

http_staged_transcode_result http_staged_content_transcoder::finish() {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP staged content transcoder cannot finish in current state");
    }
    try {
        auto result = impl_->finish();
        phase_ = phase::finished;
        return result;
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_staged_content_transcoder::reset() {
    try {
        impl_->reset();
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
    phase_ = phase::active;
}

http_worker_codecs::http_worker_codecs() noexcept
    : gzip_(http_content_coding::gzip),
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
}  // namespace ruvia

// The MIME entity grammar multipart bodies are validated against (RFC 2045
// sections 5.1 and 5.3): which characters form a token, what a field name, field
// body, parameter and media type may contain, and the duplicate-parameter guard
// a media type needs.

namespace ruvia::detail {

[[nodiscard]] inline bool http_mime_token_char(char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    if (byte <= 0x20 || byte >= 0x7F) {
        return false;
    }
    switch (value) {
        case '(':
        case ')':
        case '<':
        case '>':
        case '@':
        case ',':
        case ';':
        case ':':
        case '\\':
        case '"':
        case '/':
        case '[':
        case ']':
        case '?':
        case '=':
            return false;
        default:
            return true;
    }
}

[[nodiscard]] inline bool http_valid_mime_field_name(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    return std::ranges::all_of(value, [](char byte) {
        const auto character = static_cast<unsigned char>(byte);
        return character >= 33 && character <= 126 && byte != ':';
    });
}

[[nodiscard]] inline bool http_valid_mime_parameter_value(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() != '"') {
        return std::ranges::all_of(value, http_mime_token_char);
    }
    return http_is_quoted_string(value);
}

[[nodiscard]] inline bool http_parse_mime_parameter(std::string_view parameter, std::string_view& name,
    std::string_view& value, bool strict_equals = true) noexcept {
    const auto equals = parameter.find('=');
    if (parameter.empty() || equals == std::string_view::npos) {
        return false;
    }

    const auto raw_name = parameter.substr(0, equals);
    const auto raw_value = parameter.substr(equals + 1);
    name = http_trim_ows(raw_name);
    value = http_trim_ows(raw_value);
    // Top-level HTTP media-type parameters forbid OWS around '=' (RFC 9110
    // section 5.6.6). MIME body-part structured fields retain RFC 822's
    // separator whitespace and opt out while sharing the remaining checks.
    return (!strict_equals || (name.size() == raw_name.size() && value.size() == raw_value.size())) &&
           !name.empty() && std::ranges::all_of(name, http_mime_token_char) &&
           http_valid_mime_parameter_value(value);
}

template <http_temporary_owning_char_string parameter_type>
bool http_parse_mime_parameter(
    parameter_type&&, std::string_view&, std::string_view&, bool = true) = delete;

class http_mime_parameter_names final {
public:
    [[nodiscard]] bool record(std::string_view name) noexcept {
        for (std::size_t index = 0; index < size_; ++index) {
            if (http_ascii_equals_ignore_case(names_[index], name)) {
                return false;
            }
        }
        // Parameter-heavy input is hostile in practice. A fixed bound keeps
        // duplicate detection allocation-free and its worst-case work constant.
        if (size_ == names_.size()) {
            return false;
        }
        names_[size_++] = name;
        return true;
    }

private:
    std::array<std::string_view, 64> names_;
    std::size_t size_ = 0;
};

[[nodiscard]] inline bool http_valid_mime_media_type(std::string_view value) noexcept {
    const auto parameters = http_find_unquoted_delimiter(value, 0, ';');
    const auto media_type = http_trim_ows(value.substr(0, parameters));
    const auto slash = media_type.find('/');
    if (slash == std::string_view::npos ||
        media_type.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }
    const auto type = media_type.substr(0, slash);
    const auto subtype = media_type.substr(slash + 1);
    if (type == "*" || subtype == "*" || !std::ranges::all_of(type, http_mime_token_char) ||
        !std::ranges::all_of(subtype, http_mime_token_char) || type.empty() || subtype.empty()) {
        return false;
    }
    http_mime_parameter_names parameter_names;
    return http_all_parameters(value, [&parameter_names](std::string_view parameter) noexcept {
        std::string_view name;
        std::string_view parameter_value;
        return http_parse_mime_parameter(parameter, name, parameter_value, false) &&
               parameter_names.record(name);
    });
}

}  // namespace ruvia::detail

// The multipart state machine: find the next delimiter, read one part's header
// block, then hand out that part's body in chunks -- driven entirely by what is
// currently buffered, so a caller may feed the body in any pieces.

namespace ruvia::detail {

enum class http_multipart_delimiter_kind : std::uint8_t {
    no_match,
    invalid,
    need_input,
    part,
    close,
};

// Delimiter scanning distinguishes absence, a malformed delimiter line, an
// input-boundary ambiguity, a regular part delimiter, and the terminal close
// delimiter. Candidate outcomes expose their offset; complete delimiter lines
// expose their line length.
struct http_multipart_delimiter_result final {
    http_multipart_delimiter_kind kind_{http_multipart_delimiter_kind::no_match};
    std::size_t offset_{0};
    std::size_t line_bytes_{0};

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_no_match() noexcept {
        return {.kind_ = http_multipart_delimiter_kind::no_match};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_invalid() noexcept {
        return {.kind_ = http_multipart_delimiter_kind::invalid};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_need_input(
        std::size_t offset = 0) noexcept {
        return {.kind_ = http_multipart_delimiter_kind::need_input, .offset_ = offset};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_part(
        std::size_t offset, std::size_t line_bytes) noexcept {
        return {.kind_ = http_multipart_delimiter_kind::part,
            .offset_ = offset,
            .line_bytes_ = line_bytes};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_close(
        std::size_t offset, std::size_t line_bytes) noexcept {
        return {.kind_ = http_multipart_delimiter_kind::close,
            .offset_ = offset,
            .line_bytes_ = line_bytes};
    }

    [[nodiscard]] constexpr http_multipart_delimiter_result rebased(std::size_t base) const noexcept {
        if (kind_ == http_multipart_delimiter_kind::need_input ||
            kind_ == http_multipart_delimiter_kind::part ||
            kind_ == http_multipart_delimiter_kind::close) {
            return {.kind_ = kind_, .offset_ = base + offset_, .line_bytes_ = line_bytes_};
        }
        return *this;
    }
};

[[nodiscard]] inline bool http_multipart_marker_prefix_matches(
    std::string_view input, std::string_view boundary) noexcept {
    const auto expected_size = boundary.size() + 2;
    const auto compared = std::min(input.size(), expected_size);
    for (std::size_t index = 0; index < compared; ++index) {
        const char expected = index < 2 ? '-' : boundary[index - 2];
        if (input[index] != expected) {
            return false;
        }
    }
    return true;
}

// Matches one delimiter line beginning at its leading "--". RFC 2046
// transport-padding is accepted after a regular delimiter or after the closing
// "--". A closing delimiter ending exactly at the current buffer boundary is
// complete only when the I/O owner has signalled end-of-input.
[[nodiscard]] inline http_multipart_delimiter_result http_match_multipart_delimiter_line(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    std::size_t& padding_cursor) noexcept {
    const auto value = boundary.value();
    const auto marker_size = value.size() + 2;
    if (!http_multipart_marker_prefix_matches(input, value)) {
        return http_multipart_delimiter_result::make_no_match();
    }
    if (input.size() < marker_size) {
        return input_finished ? http_multipart_delimiter_result::make_no_match()
                              : http_multipart_delimiter_result::make_need_input();
    }

    std::size_t cursor_value = marker_size;
    bool close = false;
    if (cursor_value < input.size() && input[cursor_value] == '-') {
        if (cursor_value + 1 >= input.size()) {
            return input_finished ? http_multipart_delimiter_result::make_no_match()
                                  : http_multipart_delimiter_result::make_need_input();
        }
        if (input[cursor_value + 1] != '-') {
            return http_multipart_delimiter_result::make_invalid();
        }
        close = true;
        cursor_value += 2;
    }

    cursor_value = std::max(cursor_value, padding_cursor);
    // transport-padding is *LWSP-char (RFC 2046 section 5.1.1): SP / HTAB.
    while (cursor_value < input.size() && is_http_ows_char(input[cursor_value])) {
        ++cursor_value;
    }
    padding_cursor = cursor_value;
    if (cursor_value == input.size()) {
        if (close && input_finished) {
            return http_multipart_delimiter_result::make_close(0, cursor_value);
        }
        return input_finished ? http_multipart_delimiter_result::make_no_match()
                              : http_multipart_delimiter_result::make_need_input();
    }
    if (input[cursor_value] != '\r') {
        return http_multipart_delimiter_result::make_invalid();
    }
    if (cursor_value + 1 >= input.size()) {
        return input_finished ? http_multipart_delimiter_result::make_no_match()
                              : http_multipart_delimiter_result::make_need_input();
    }
    if (input[cursor_value + 1] != '\n') {
        return http_multipart_delimiter_result::make_invalid();
    }
    return close ? http_multipart_delimiter_result::make_close(0, cursor_value + 2)
                 : http_multipart_delimiter_result::make_part(0, cursor_value + 2);
}

// Cursors belong to one retained input prefix and must be reset after consume.
// Searches keep at most the three-byte delimiter prefix overlap; a candidate's
// transport padding resumes at its last inspected byte.
[[nodiscard]] inline http_multipart_delimiter_result http_find_multipart_delimiter_from_cursor(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    bool check_leading_dash, std::size_t rebase_crlf_offset,
    std::size_t& search_cursor, std::size_t& padding_cursor) noexcept {
    if (check_leading_dash && search_cursor == 0 && !input.empty() && input.front() == '-') {
        auto match = http_match_multipart_delimiter_line(input, boundary, input_finished, padding_cursor);
        if (match.kind_ != http_multipart_delimiter_kind::no_match) {
            return match;
        }
        padding_cursor = 0;
    }
    for (auto prefix = input.find("\r\n--", search_cursor); prefix != std::string_view::npos;
        prefix = input.find("\r\n--", prefix + 1)) {
        auto match = http_match_multipart_delimiter_line(
            input.substr(prefix + 2), boundary, input_finished, padding_cursor);
        if (match.kind_ == http_multipart_delimiter_kind::no_match) {
            padding_cursor = 0;
            continue;
        }
        search_cursor = prefix;
        return match.rebased(prefix + rebase_crlf_offset);
    }
    search_cursor = input.size() > 3 ? input.size() - 3 : 0;
    return http_multipart_delimiter_result::make_no_match();
}

[[nodiscard]] inline http_multipart_delimiter_result http_find_initial_multipart_delimiter(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    std::size_t& search_cursor, std::size_t& padding_cursor) noexcept {
    auto match = http_find_multipart_delimiter_from_cursor(
        input, boundary, input_finished, true, 2, search_cursor, padding_cursor);
    if (match.kind_ != http_multipart_delimiter_kind::no_match) {
        return match;
    }
    // The CRLF belongs to the preamble, but the first marker byte may still
    // become a delimiter. Keep it out of the preamble quota until the next
    // byte resolves the candidate or EOF makes the prefix incomplete.
    if (!input_finished && input.ends_with("\r\n-")) {
        return http_multipart_delimiter_result::make_need_input(input.size() - 1);
    }
    return match;
}

// RFC 2046 section 5.1.1: body-part = MIME-part-headers [CRLF *OCTET], so a part
// may end right after its header block and the next delimiter's CRLF is the one
// that closed the headers. Body lines cannot start with the dash-boundary, so at
// the body start a delimiter line at offset 0 is a delimiter, not content.
[[nodiscard]] inline http_multipart_delimiter_result http_find_multipart_body_delimiter(
    std::string_view input, const multipart_boundary& boundary, bool input_finished, bool at_body_start,
    std::size_t& search_cursor, std::size_t& padding_cursor) noexcept {
    return http_find_multipart_delimiter_from_cursor(
        input, boundary, input_finished, at_body_start, 0, search_cursor, padding_cursor);
}

[[nodiscard]] inline bool http_is_form_data_disposition(std::string_view disposition) noexcept {
    return http_ascii_equals_ignore_case(http_header_token_before_parameters(disposition), "form-data");
}

struct http_multipart_part_headers final {
    std::string_view name_;
    std::string_view filename_;
    std::string_view content_type_;
    bool filename_present_{false};

    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] constexpr std::string_view filename() const noexcept {
        return filename_;
    }

    [[nodiscard]] constexpr bool has_filename() const noexcept {
        return filename_present_;
    }

    [[nodiscard]] constexpr std::string_view content_type() const noexcept {
        return content_type_;
    }
};

using http_multipart_part_header_parse_result =
    std::variant<http_multipart_part_headers, multipart_parse_error>;

[[nodiscard]] inline http_multipart_part_header_parse_result http_parse_multipart_part_headers(
    std::string_view headers) noexcept {
    std::optional<std::string_view> disposition;
    std::optional<std::string_view> content_type;
    auto remaining_headers = headers;
    while (!remaining_headers.empty()) {
        const auto line_end = remaining_headers.find("\r\n");
        const auto line = line_end == std::string_view::npos ? remaining_headers
                                                             : remaining_headers.substr(0, line_end);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || !http_valid_mime_field_name(line.substr(0, colon)) ||
            !is_valid_http_field_value_bytes(line.substr(colon + 1))) {
            return multipart_parse_error::invalid_part_headers;
        }
        const auto key = line.substr(0, colon);
        const auto value = http_trim_ows(line.substr(colon + 1));
        if (http_ascii_equals_ignore_case(key, "Content-Disposition")) {
            if (disposition) {
                return multipart_parse_error::invalid_content_disposition;
            }
            disposition = value;
        } else if (http_ascii_equals_ignore_case(key, "Content-Type")) {
            if (content_type || !http_valid_mime_media_type(value)) {
                return multipart_parse_error::invalid_part_headers;
            }
            content_type = value;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        remaining_headers.remove_prefix(line_end + 2);
    }

    if (!disposition || !http_is_form_data_disposition(*disposition)) {
        return multipart_parse_error::invalid_content_disposition;
    }

    // name and filename keep their wire spelling (token or validated
    // quoted-string); the parser decodes quoted-pairs only when it must own them.
    std::optional<std::string_view> name;
    std::optional<std::string_view> filename;
    http_mime_parameter_names parameter_names;
    const bool valid = http_all_parameters(*disposition, [&](std::string_view parameter) noexcept {
        std::string_view key;
        std::string_view value;
        if (!http_parse_mime_parameter(parameter, key, value, false) || !parameter_names.record(key)) {
            return false;
        }
        // RFC 7578 section 4.2 forbids senders to use filename*; recipients get no
        // reject rule, so it falls through with the other unknown parameters.
        if (http_ascii_equals_ignore_case(key, "name")) {
            name = value;
        } else if (http_ascii_equals_ignore_case(key, "filename")) {
            filename = value;
        }
        return true;
    });
    if (!valid) {
        return multipart_parse_error::invalid_content_disposition;
    }
    if (!name) {
        return multipart_parse_error::missing_field_name;
    }

    return http_multipart_part_headers{
        .name_ = *name,
        .filename_ = filename.value_or(std::string_view{}),
        .content_type_ = content_type.value_or(std::string_view{}),
        .filename_present_ = filename.has_value(),
    };
}

struct multipart_part_access final {
    using text_storage_type = multipart_part::text_storage_type;

    // Unmodified fields borrow the caller-owned body; unfolded or escaped fields
    // own string storage.
    [[nodiscard]] static multipart_part make_decoded(multipart_part::text_storage_type name,
        multipart_part::text_storage_type filename,
        multipart_part::text_storage_type content_type_value, std::string_view body,
        bool filename_present) noexcept {
        return multipart_part(std::move(name), std::move(filename),
            std::move(content_type_value), body, filename_present);
    }

    template <http_temporary_owning_char_string body_type>
    static multipart_part make_decoded(multipart_part::text_storage_type,
        multipart_part::text_storage_type, multipart_part::text_storage_type, body_type&&, bool) = delete;
};

struct multipart_stream_part_access final {
    [[nodiscard]] static constexpr multipart_stream_part make(std::string_view name,
        std::string_view filename, std::string_view content_type_value, std::string_view body,
        multipart_chunk_phase phase, bool filename_present) noexcept {
        return multipart_stream_part(name, filename, content_type_value, body, phase, filename_present);
    }

    template <http_temporary_owning_char_string name_type>
    static multipart_stream_part make(name_type&&, std::string_view, std::string_view, std::string_view,
        multipart_chunk_phase, bool) = delete;

    template <http_temporary_owning_char_string filename_type>
    static multipart_stream_part make(std::string_view, filename_type&&, std::string_view,
        std::string_view, multipart_chunk_phase, bool) = delete;

    template <http_temporary_owning_char_string content_type>
    static multipart_stream_part make(std::string_view, std::string_view, content_type&&,
        std::string_view, multipart_chunk_phase, bool) = delete;

    template <http_temporary_owning_char_string body_type>
    static multipart_stream_part make(std::string_view, std::string_view, std::string_view, body_type&&,
        multipart_chunk_phase, bool) = delete;
};

}  // namespace ruvia::detail

namespace ruvia {

namespace {

// Decodes a validated boundary parameter (token or quoted-string) into
// `decoded`; the boundary rule itself is checked by multipart_boundary.
[[nodiscard]] std::optional<std::string_view> decode_multipart_boundary_parameter(
    std::string_view parameter, std::array<char, 70>& decoded) noexcept {
    std::size_t size = 0;
    if (parameter.empty() || parameter.front() != '"') {
        if (parameter.size() > decoded.size()) {
            return std::nullopt;
        }
        return parameter;
    }
    http_quoted_string_cursor cursor(parameter);
    unsigned char byte = 0;
    while (cursor.next(byte)) {
        if (size >= decoded.size()) {
            return std::nullopt;
        }
        decoded[size++] = static_cast<char>(byte);
    }
    return std::string_view(decoded.data(), size);
}

}  // namespace

multipart_boundary_parse_result parse_multipart_boundary(std::string_view content_type_value) {
    if (!http_ascii_equals_ignore_case(
            detail::http_header_token_before_parameters(content_type_value), "multipart/form-data")) {
        return multipart_boundary_parse_result::make_not_applicable();
    }
    std::optional<multipart_boundary> boundary;
    std::array<char, 70> decoded{};
    detail::http_mime_parameter_names parameter_names;
    const bool valid = detail::http_all_parameters(content_type_value, [&](std::string_view parameter) {
        if (parameter.empty()) {
            return true;
        }
        std::string_view key;
        std::string_view value;
        if (!detail::http_parse_mime_parameter(parameter, key, value) || !parameter_names.record(key)) {
            return false;
        }
        if (http_ascii_equals_ignore_case(key, "boundary")) {
            const auto decoded_value = decode_multipart_boundary_parameter(value, decoded);
            boundary = decoded_value.has_value() ? multipart_boundary::try_create(*decoded_value)
                                                 : std::nullopt;
            if (!boundary) {
                return false;
            }
        }
        return true;
    });
    if (!valid || !boundary) {
        return multipart_boundary_parse_result::make_failure();
    }
    return multipart_boundary_parse_result(std::move(*boundary));
}

namespace {

constexpr std::size_t max_multipart_preamble_bytes = std::size_t{64} * 1024;
constexpr std::size_t max_multipart_header_bytes = std::size_t{64} * 1024;
constexpr std::size_t max_multipart_delimiter_line_bytes = std::size_t{64} * 1024;

// Appends a validated token or quoted-string parameter value with its
// quoted-pairs (RFC 9110 section 5.6.4) decoded.
void append_mime_parameter_value(std::string& out, std::string_view value) {
    if (value.empty() || value.front() != '"') {
        out.append(value);
        return;
    }
    http_quoted_string_cursor cursor(value);
    unsigned char byte = 0;
    while (cursor.next(byte)) {
        out.push_back(static_cast<char>(byte));
    }
}

[[nodiscard]] std::string_view multipart_text_view(
    const std::variant<std::string_view, std::string>& storage) noexcept {
    if (const auto* borrowed = std::get_if<std::string_view>(&storage)) {
        return *borrowed;
    }
    return std::get<std::string>(storage);
}

std::string& emplace_owned_multipart_text(
    std::variant<std::string_view, std::string>& storage) {
    if (auto* owned = std::get_if<std::string>(&storage)) {
        owned->clear();
        return *owned;
    }
    return storage.emplace<std::string>();
}

[[nodiscard]] http_protocol_error multipart_protocol_error(multipart_parse_error error) noexcept {
    switch (error) {
        case multipart_parse_error::incomplete_body:
            return http_protocol_error(http_status::bad_request, "multipart_incomplete_body",
                "incomplete multipart body");
        case multipart_parse_error::invalid_delimiter:
            return http_protocol_error(http_status::bad_request, "multipart_invalid_delimiter",
                "invalid multipart delimiter");
        case multipart_parse_error::preamble_too_large:
            return http_protocol_error(http_status::content_too_large,
                "multipart_preamble_too_large", "multipart preamble exceeds limit");
        case multipart_parse_error::part_headers_too_large:
            return http_protocol_error(http_status::content_too_large,
                "multipart_part_headers_too_large", "multipart part headers exceed limit");
        case multipart_parse_error::invalid_part_headers:
            return http_protocol_error(http_status::bad_request, "multipart_invalid_part_headers",
                "invalid multipart part headers");
        case multipart_parse_error::invalid_content_disposition:
            return http_protocol_error(http_status::bad_request,
                "multipart_invalid_content_disposition", "invalid multipart content disposition");
        case multipart_parse_error::missing_field_name:
            return http_protocol_error(http_status::bad_request, "multipart_missing_field_name",
                "invalid multipart field name");
        case multipart_parse_error::delimiter_line_too_large:
            return http_protocol_error(http_status::content_too_large,
                "multipart_delimiter_line_too_large", "multipart delimiter line exceeds limit");
        case multipart_parse_error::too_many_parts:
            return http_protocol_error(http_status::content_too_large, "multipart_too_many_parts",
                "multipart part count exceeds limit");
        case multipart_parse_error::metadata_too_large:
            return http_protocol_error(http_status::content_too_large,
                "multipart_metadata_too_large", "multipart metadata exceeds limit");
    }
    return http_protocol_error(
        http_status::bad_request, "multipart_invalid_body", "invalid multipart body");
}

}  // namespace

http_protocol_error multipart_poll_failure::protocol_error() const noexcept {
    return multipart_protocol_error(error_);
}

multipart_parser::multipart_parser(multipart_parse_options options)
    : boundary_(std::move(options.boundary_)),
      input_(std::in_place_type<http_io_buffer>),
      remaining_parts_(options.max_parts_),
      remaining_metadata_bytes_(options.max_metadata_bytes_) {}

multipart_parser::multipart_parser(
    std::string_view complete_body, multipart_parse_options options, complete_input_tag_type)
    : boundary_(std::move(options.boundary_)),
      input_(complete_body),
      remaining_parts_(options.max_parts_),
      remaining_metadata_bytes_(options.max_metadata_bytes_),
      input_eof_(true) {}

std::vector<multipart_part> parse_multipart_body(
    std::string_view body, multipart_parse_options options) {
    multipart_parser parser(body, std::move(options), multipart_parser::complete_input_tag_type{});
    std::vector<multipart_part> parts;
    for (;;) {
        auto result_value = parser.poll();
        if (const auto* part = result_value.part()) {
            parts.push_back(detail::multipart_part_access::make_decoded(
                std::exchange(parser.current_name_, std::string_view{}),
                std::exchange(parser.current_filename_, std::string_view{}),
                std::exchange(parser.current_content_type_, std::string_view{}), part->body(),
                part->has_filename()));
            continue;
        }
        if (result_value.done() != nullptr) {
            return parts;
        }
        if (const auto* failure = result_value.failure()) {
            throw failure->protocol_error();
        }
        throw multipart_protocol_error(multipart_parse_error::incomplete_body);
    }
}

std::string_view multipart_parser::buffer_view() const noexcept {
    if (const auto* borrowed = std::get_if<std::string_view>(&input_)) {
        return *borrowed;
    }
    return std::get<http_io_buffer>(input_).view();
}

void multipart_parser::consume(std::size_t bytes_value) noexcept {
    if (auto* borrowed = std::get_if<std::string_view>(&input_)) {
        borrowed->remove_prefix(std::min(bytes_value, borrowed->size()));
    } else {
        std::get<http_io_buffer>(input_).consume(bytes_value);
    }
    header_scan_offset_ = header_scan_offset_ >= bytes_value ? header_scan_offset_ - bytes_value : 0;
    delimiter_scan_offset_ =
        delimiter_scan_offset_ >= bytes_value ? delimiter_scan_offset_ - bytes_value : 0;
}

void multipart_parser::reset_scan_cursors() noexcept {
    header_scan_offset_ = 0;
    delimiter_scan_offset_ = 0;
    delimiter_padding_offset_ = 0;
}

void multipart_parser::compact_pending() {
    if (pending_erase_bytes_ == 0) {
        return;
    }
    consume(pending_erase_bytes_);
    pending_erase_bytes_ = 0;
}

void multipart_parser::feed(std::string_view chunk) {
    auto* owned = std::get_if<http_io_buffer>(&input_);
    if (owned == nullptr || input_eof_ || (state_.index() != 0) ||
        std::get<0>(state_) == progress_state_type::done) {
        throw std::logic_error("multipart parser cannot accept input in a terminal state");
    }
    owned->append(chunk);
}

void multipart_parser::finish_input() noexcept {
    input_eof_ = true;
}

multipart_poll_result multipart_parser::fail(multipart_parse_error error) noexcept {
    auto result_value = multipart_poll_result::make_failure(error);
    state_ = error;
    return result_value;
}

multipart_poll_result multipart_parser::poll() {
    if ((state_.index() != 0)) {
        return multipart_poll_result::make_failure(std::get<1>(state_));
    }
    for (;;) {
        compact_pending();
        switch (std::get<0>(state_)) {
            case progress_state_type::boundary: {
                const auto step = process_boundary();
                if ((step.index() != 0)) {
                    return fail(std::get<1>(step));
                }
                const auto progress_value = std::get<0>(step);
                if (progress_value == step_progress_type::need_input) {
                    if (input_eof_) {
                        return fail(multipart_parse_error::incomplete_body);
                    }
                    return multipart_poll_result::make_need_input();
                }
                if (progress_value == step_progress_type::done) {
                    return multipart_poll_result::make_done();
                }
                break;
            }
            case progress_state_type::headers: {
                const auto step = process_headers();
                if ((step.index() != 0)) {
                    return fail(std::get<1>(step));
                }
                const auto progress_value = std::get<0>(step);
                if (progress_value == step_progress_type::need_input) {
                    if (input_eof_) {
                        return fail(multipart_parse_error::incomplete_body);
                    }
                    return multipart_poll_result::make_need_input();
                }
                break;
            }
            case progress_state_type::body: {
                auto result_value = read_body_chunk();
                if (result_value.need_input() != nullptr && input_eof_) {
                    return fail(multipart_parse_error::incomplete_body);
                }
                return result_value;
            }
            case progress_state_type::done:
                return multipart_poll_result::make_done();
        }
    }
}

multipart_parser::step_result_type multipart_parser::process_boundary() {
    // RFC 2046 section 5.1.1: the first boundary may be preceded by a preamble that
    // is ignored. Skip it once, reusing the buffered parser's boundary finder so
    // the streaming and buffered paths accept exactly the same bodies.
    if (first_boundary_) {
        const auto delimiter =
            detail::http_find_initial_multipart_delimiter(buffer_view(), boundary_, input_eof_,
                delimiter_scan_offset_, delimiter_padding_offset_);
        if (delimiter.kind_ == detail::http_multipart_delimiter_kind::invalid) {
            return multipart_parse_error::invalid_delimiter;
        }
        if (delimiter.kind_ == detail::http_multipart_delimiter_kind::no_match) {
            if (buffer_view().size() > max_multipart_preamble_bytes) {
                return multipart_parse_error::preamble_too_large;
            }
            return step_progress_type::need_input;
        }
        if (delimiter.kind_ == detail::http_multipart_delimiter_kind::need_input) {
            const auto buffer_bytes = buffer_view().size();
            if (delimiter.offset_ > max_multipart_preamble_bytes) {
                return multipart_parse_error::preamble_too_large;
            }
            if (buffer_bytes - delimiter.offset_ > max_multipart_delimiter_line_bytes) {
                return multipart_parse_error::delimiter_line_too_large;
            }
            return step_progress_type::need_input;
        }
        if (delimiter.offset_ > max_multipart_preamble_bytes) {
            return multipart_parse_error::preamble_too_large;
        }
        if (delimiter.line_bytes_ > max_multipart_delimiter_line_bytes) {
            return multipart_parse_error::delimiter_line_too_large;
        }
        consume(delimiter.offset_ + delimiter.line_bytes_);
        reset_scan_cursors();
        first_boundary_ = false;
        if (delimiter.kind_ == detail::http_multipart_delimiter_kind::part) {
            state_ = progress_state_type::headers;
            return step_progress_type::continue_value;
        }
        state_ = progress_state_type::done;
        return step_progress_type::done;
    }

    if (buffer_view().starts_with("\r\n")) {
        consume(2);
        reset_scan_cursors();
    }

    const auto delimiter =
        detail::http_match_multipart_delimiter_line(buffer_view(), boundary_, input_eof_,
            delimiter_padding_offset_);
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::need_input) {
        if (buffer_view().size() > max_multipart_delimiter_line_bytes) {
            return multipart_parse_error::delimiter_line_too_large;
        }
        return step_progress_type::need_input;
    }
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::part ||
        delimiter.kind_ == detail::http_multipart_delimiter_kind::close) {
        if (delimiter.line_bytes_ > max_multipart_delimiter_line_bytes) {
            return multipart_parse_error::delimiter_line_too_large;
        }
        consume(delimiter.line_bytes_);
        reset_scan_cursors();
        if (delimiter.kind_ == detail::http_multipart_delimiter_kind::part) {
            state_ = progress_state_type::headers;
            return step_progress_type::continue_value;
        }
        state_ = progress_state_type::done;
        return step_progress_type::done;
    }
    return multipart_parse_error::invalid_delimiter;
}

multipart_parser::step_result_type multipart_parser::process_headers() {
    // Cap on a single part's header block, mirroring the 64KB request-header limit.
    const auto buffer = buffer_view();
    const auto headers_end = buffer.find("\r\n\r\n", header_scan_offset_);
    if (headers_end == std::string_view::npos) {
        header_scan_offset_ = buffer.size() > 3 ? buffer.size() - 3 : 0;
        if (buffer.size() > max_multipart_header_bytes) {
            return multipart_parse_error::part_headers_too_large;
        }
        return step_progress_type::need_input;
    }
    // Include the terminating CRLF CRLF in the same byte cap. Checking only
    // the incomplete path allowed an oversized but already-terminated block
    // delivered in one feed() to bypass the limit entirely.
    if (headers_end > max_multipart_header_bytes - 4) {
        return multipart_parse_error::part_headers_too_large;
    }

    if (remaining_parts_ == 0) {
        return multipart_parse_error::too_many_parts;
    }
    if (headers_end + 4 > remaining_metadata_bytes_) {
        return multipart_parse_error::metadata_too_large;
    }
    auto headers = buffer.substr(0, headers_end);
    const auto first_fold = detail::find_field_line_fold(headers);
    const bool unfolded = first_fold != std::string_view::npos;
    if (unfolded) {
        detail::copy_unfolded_field_lines(
            headers, first_fold, detail::field_line_folding_policy::mime, current_headers_);
        headers = current_headers_;
    }
    const auto parsed_headers = detail::http_parse_multipart_part_headers(headers);
    if (const auto* error = std::get_if<multipart_parse_error>(&parsed_headers)) {
        return *error;
    }
    const auto* part_headers = std::get_if<detail::http_multipart_part_headers>(&parsed_headers);

    const bool borrowed_headers = std::holds_alternative<std::string_view>(input_) && !unfolded;
    if (borrowed_headers && part_headers->name().find('\\') == std::string_view::npos) {
        current_name_ = http_trim_quoted_field_value(part_headers->name());
    } else {
        append_mime_parameter_value(
            emplace_owned_multipart_text(current_name_), part_headers->name());
    }
    has_current_filename_ = part_headers->has_filename();
    if (has_current_filename_ && !http_trim_quoted_field_value(part_headers->filename()).empty()) {
        if (borrowed_headers && part_headers->filename().find('\\') == std::string_view::npos) {
            current_filename_ = http_trim_quoted_field_value(part_headers->filename());
        } else {
            append_mime_parameter_value(
                emplace_owned_multipart_text(current_filename_), part_headers->filename());
        }
    } else {
        current_filename_ = std::string_view{};
    }
    if (part_headers->content_type().empty()) {
        current_content_type_ = std::string_view{};
    } else if (borrowed_headers) {
        current_content_type_ = part_headers->content_type();
    } else {
        emplace_owned_multipart_text(current_content_type_)
            .assign(part_headers->content_type().data(), part_headers->content_type().size());
    }
    --remaining_parts_;
    remaining_metadata_bytes_ -= headers_end + 4;
    consume(headers_end + 4);
    reset_scan_cursors();
    next_chunk_is_first_ = true;
    state_ = progress_state_type::body;
    return step_progress_type::continue_value;
}

multipart_stream_part multipart_parser::make_part(std::string_view body, bool part_end) {
    const auto phase =
        next_chunk_is_first_ ? (part_end ? multipart_chunk_phase::complete : multipart_chunk_phase::first)
                             : (part_end ? multipart_chunk_phase::last : multipart_chunk_phase::middle);
    auto part = detail::multipart_stream_part_access::make(multipart_text_view(current_name_),
        multipart_text_view(current_filename_), multipart_text_view(current_content_type_), body,
        phase, has_current_filename_);
    next_chunk_is_first_ = false;
    return part;
}

multipart_poll_result multipart_parser::read_body_chunk() {
    const auto buffer = buffer_view();
    const auto delimiter =
        detail::http_find_multipart_body_delimiter(buffer, boundary_, input_eof_, next_chunk_is_first_,
            delimiter_scan_offset_, delimiter_padding_offset_);
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::invalid) {
        return fail(multipart_parse_error::invalid_delimiter);
    }
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::part ||
        delimiter.kind_ == detail::http_multipart_delimiter_kind::close) {
        if (delimiter.line_bytes_ > max_multipart_delimiter_line_bytes) {
            return fail(multipart_parse_error::delimiter_line_too_large);
        }
        const auto crlf_bytes =
            buffer.substr(delimiter.offset_).starts_with("\r\n") ? std::size_t{2} : std::size_t{0};
        auto part = make_part(buffer.substr(0, delimiter.offset_), true);
        pending_erase_bytes_ = delimiter.offset_ + crlf_bytes + delimiter.line_bytes_;
        reset_scan_cursors();
        state_ = delimiter.kind_ == detail::http_multipart_delimiter_kind::part
                     ? progress_state_type::headers
                     : progress_state_type::done;
        return multipart_poll_result::make_part(part);
    }
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::need_input) {
        if (buffer.size() - delimiter.offset_ > max_multipart_delimiter_line_bytes + 2) {
            return fail(multipart_parse_error::delimiter_line_too_large);
        }
        if (delimiter.offset_ > 0) {
            auto part = make_part(buffer.substr(0, delimiter.offset_), false);
            pending_erase_bytes_ = delimiter.offset_;
            return multipart_poll_result::make_part(part);
        }
        return multipart_poll_result::make_need_input();
    }

    const auto keep_tail = boundary_.value().size() + 8;
    if (!input_eof_ && buffer.size() > keep_tail) {
        const auto bytes_value = buffer.size() - keep_tail;
        auto part = make_part(buffer.substr(0, bytes_value), false);
        pending_erase_bytes_ = bytes_value;
        return multipart_poll_result::make_part(part);
    }

    return multipart_poll_result::make_need_input();
}

}  // namespace ruvia

namespace ruvia {
namespace {

// Keeps every offset and size representable as a pointer difference.
constexpr std::size_t max_io_buffer_bytes = static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());

}  // namespace

http_io_buffer::http_io_buffer(http_io_buffer&& other) noexcept
    : storage_(std::exchange(other.storage_, nullptr)),
      capacity_(std::exchange(other.capacity_, 0)),
      begin_(std::exchange(other.begin_, 0)),
      end_(std::exchange(other.end_, 0)) {}

http_io_buffer::~http_io_buffer() {
    release();
}

std::span<char> http_io_buffer::prepare(std::size_t bytes) & {
    if (capacity_ - end_ < bytes) {
        make_room(bytes);
    }
    return std::span<char>(storage_ + end_, bytes);
}

std::span<char> http_io_buffer::prepare_spare() & noexcept {
    if (end_ == capacity_ && begin_ >= size()) {
        move_window_to_front();
    }
    return std::span<char>(storage_ + end_, capacity_ - end_);
}

void http_io_buffer::commit(std::size_t bytes) {
    if (bytes > capacity_ - end_) {
        throw std::length_error("http_io_buffer commit exceeds the prepared space");
    }
    end_ += bytes;
}

void http_io_buffer::append(std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }
    if (capacity_ - end_ >= bytes.size()) {
        // The source may be readable bytes of this buffer, which never
        // overlap the writable space but may share its storage.
        std::memmove(storage_ + end_, bytes.data(), bytes.size());
        end_ += bytes.size();
        return;
    }
    const auto capacity = room_capacity(bytes.size());
    if (capacity > capacity_ || detail::http_text_overlaps(bytes, std::string_view(storage_, capacity_))) {
        // Publish the appended bytes together with the replacement storage.
        reallocate(capacity, bytes);
        return;
    }
    move_window_to_front();
    std::memcpy(storage_ + end_, bytes.data(), bytes.size());
    end_ += bytes.size();
}

void http_io_buffer::consume(std::size_t bytes) noexcept {
    begin_ += std::min(bytes, size());
    if (begin_ == end_) {
        clear();
    }
}

void http_io_buffer::reserve(std::size_t bytes) {
    if (bytes > max_io_buffer_bytes) {
        throw std::length_error("http_io_buffer size exceeds the addressable range");
    }
    if (bytes > capacity_) {
        reallocate(bytes);
    }
}

void http_io_buffer::shrink_to(std::size_t bytes) {
    const auto target = std::max(size(), bytes);
    if (capacity_ > target) {
        reallocate(target);
    }
}

void http_io_buffer::clear_retaining_small(std::size_t retained_bytes) noexcept {
    clear();
    if (capacity_ > retained_bytes) {
        release();
    }
}

std::size_t http_io_buffer::room_capacity(std::size_t bytes) const {
    if (bytes > max_io_buffer_bytes - size()) {
        throw std::length_error("http_io_buffer size exceeds the addressable range");
    }
    // Charge compaction to the consumed prefix: moving the live bytes must
    // recover at least as many bytes. Otherwise grow once for future writes.
    if (capacity_ - size() >= bytes && begin_ >= size()) {
        return capacity_;
    }
    const auto doubled = capacity_ > max_io_buffer_bytes / 2 ? max_io_buffer_bytes : capacity_ * 2;
    return std::max(size() + bytes, doubled);
}

void http_io_buffer::make_room(std::size_t bytes) {
    const auto capacity = room_capacity(bytes);
    if (capacity > capacity_) {
        reallocate(capacity);
        return;
    }
    move_window_to_front();
}

void http_io_buffer::move_window_to_front() noexcept {
    if (begin_ == 0) {
        return;
    }
    const auto live = size();
    std::memmove(storage_, storage_ + begin_, live);
    begin_ = 0;
    end_ = live;
}

void http_io_buffer::reallocate(std::size_t capacity, std::string_view tail) {
    const auto live = size();
    char* storage = capacity == 0 ? nullptr : std::allocator<char>{}.allocate(capacity);
    if (live != 0) {
        std::memcpy(storage, storage_ + begin_, live);
    }
    if (!tail.empty()) {
        std::memcpy(storage + live, tail.data(), tail.size());
    }
    // Publish the complete window before old storage can reenter its owner.
    http_io_buffer retired(std::move(*this));
    storage_ = storage;
    capacity_ = capacity;
    begin_ = 0;
    end_ = live + tail.size();
}

void http_io_buffer::release() noexcept {
    auto* const storage = std::exchange(storage_, nullptr);
    const auto capacity = std::exchange(capacity_, 0);
    begin_ = 0;
    end_ = 0;
    if (storage != nullptr) {
        std::allocator<char>{}.deallocate(storage, capacity);
    }
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

constexpr std::size_t allocation_alignment = alignof(std::max_align_t);
// The prefix keeps the payload max-aligned while recording the block size.
constexpr std::size_t allocation_prefix_bytes =
    ((sizeof(std::size_t) + allocation_alignment - 1) / allocation_alignment) * allocation_alignment;

[[nodiscard]] std::byte* allocation_base(void* pointer) noexcept {
    return static_cast<std::byte*>(pointer) - allocation_prefix_bytes;
}

[[nodiscard]] std::size_t allocation_payload_size(void* pointer) noexcept {
    std::size_t total = 0;
    std::memcpy(&total, allocation_base(pointer), sizeof(total));
    return total - allocation_prefix_bytes;
}

}  // namespace

http_c_library_memory::http_c_library_memory(
    std::size_t native_budget, bool throw_on_budget_exceeded) noexcept
    : native_budget_(native_budget),
      throw_on_budget_exceeded_(throw_on_budget_exceeded) {}

void http_c_library_memory::rethrow_if_failed() const {
    if (auto failure = std::exchange(failure_, nullptr)) {
        std::rethrow_exception(failure);
    }
}

void http_c_library_memory::rethrow_allocation_failure() const {
    rethrow_if_failed();
    throw std::bad_alloc();
}

void* http_c_library_memory::allocate(std::size_t size, void* context) noexcept {
    if (context == nullptr) {
        return nullptr;
    }
    auto& self = *static_cast<http_c_library_memory*>(context);
    if (size > self.native_budget_ - self.live_bytes_) {
        self.budget_exceeded_ = true;
        if (self.throw_on_budget_exceeded_ && !self.failure_) {
            self.failure_ = std::make_exception_ptr(std::bad_alloc{});
        }
        return nullptr;
    }
    if (size > (std::numeric_limits<std::size_t>::max)() - allocation_prefix_bytes) {
        return nullptr;
    }
    const auto total = size + allocation_prefix_bytes;
    try {
        auto* base = static_cast<std::byte*>(::operator new(total, std::align_val_t{allocation_alignment}));
        std::memcpy(base, &total, sizeof(total));
        self.live_bytes_ += size;
        return base + allocation_prefix_bytes;
    } catch (...) {
        if (!self.failure_) {
            self.failure_ = std::current_exception();
        }
        return nullptr;
    }
}

void http_c_library_memory::deallocate(void* pointer, void* context) noexcept {
    if (pointer == nullptr) {
        return;
    }
    auto& self = *static_cast<http_c_library_memory*>(context);
    std::size_t total = 0;
    std::memcpy(&total, allocation_base(pointer), sizeof(total));
    ::operator delete(allocation_base(pointer), total, std::align_val_t{allocation_alignment});
    self.live_bytes_ -= total - allocation_prefix_bytes;
}

void* http_c_library_memory::allocate_zeroed(std::size_t count, std::size_t size, void* context) noexcept {
    if (size != 0 && count > (std::numeric_limits<std::size_t>::max)() / size) {
        return nullptr;
    }
    auto* pointer = allocate(count * size, context);
    if (pointer != nullptr) {
        std::memset(pointer, 0, count * size);
    }
    return pointer;
}

void* http_c_library_memory::reallocate(void* pointer, std::size_t size, void* context) noexcept {
    if (pointer == nullptr) {
        return allocate(size, context);
    }
    if (size == 0) {
        deallocate(pointer, context);
        return nullptr;
    }
    auto* replacement = allocate(size, context);
    if (replacement == nullptr) {
        // C realloc semantics: the original block stays valid on failure.
        return nullptr;
    }
    std::memcpy(replacement, pointer, (std::min)(size, allocation_payload_size(pointer)));
    deallocate(pointer, context);
    return replacement;
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

// Splits `data` at CR, LF or CRLF and passes each line to `visitor`; the
// sizing pass and the encoder share this one line rule.
template <typename visitor_type>
void visit_sse_data_lines(std::string_view data, visitor_type&& visitor) {
    while (true) {
        const auto next_value = data.find_first_of("\r\n");
        visitor(next_value == std::string_view::npos ? data : data.substr(0, next_value));
        if (next_value == std::string_view::npos) {
            return;
        }
        auto advance = next_value + 1U;
        if (data[next_value] == '\r' && next_value + 1U < data.size() && data[next_value + 1U] == '\n') {
            advance = next_value + 2U;
        }
        data.remove_prefix(advance);
    }
}

constexpr std::string_view sse_utf8_bom = "\xEF\xBB\xBF";
constexpr std::string_view sse_replacement_character = "\xEF\xBF\xBD";
constexpr std::string_view sse_default_event_type = "message";

// The event stream is decoded with WHATWG UTF-8 decode, which replaces each
// maximal subpart of an ill-formed sequence with one U+FFFD. The decoder never
// consumes an ASCII byte, so repairing each field value after the line split
// matches decoding the stream first.
void replace_ill_formed_utf8(std::string& value) {
    const std::string_view bytes(value);
    std::size_t index = 0;
    while (index < bytes.size()) {
        if (static_cast<unsigned char>(bytes[index]) < 0x80) {
            ++index;
            continue;
        }
        const auto sequence = scan_utf8_sequence(bytes, index);
        if (!sequence.well_formed_) {
            break;
        }
        index += sequence.length_;
    }
    if (index == bytes.size()) {
        return;
    }
    std::string repaired;
    repaired.reserve(bytes.size());
    repaired.append(bytes.substr(0, index));
    while (index < bytes.size()) {
        if (static_cast<unsigned char>(bytes[index]) < 0x80) {
            repaired.push_back(bytes[index]);
            ++index;
            continue;
        }
        const auto sequence = scan_utf8_sequence(bytes, index);
        repaired.append(sequence.well_formed_ ? bytes.substr(index, sequence.length_) : sse_replacement_character);
        index += sequence.length_;
    }
    value.swap(repaired);
}

}  // namespace

sse_parser::sse_parser(sse_parser_options options)
    : event_limit_(options.event_limit_) {}

sse_parse_result sse_parser::parse(std::string_view input) & {
    if (state_ != sse_parser_state::active) {
        throw std::logic_error("SSE parser is not active");
    }
    release_event_buffers();
    std::size_t position = 0;
    if (!bom_settled_) {
        while (position < input.size() && bom_matched_ < sse_utf8_bom.size() &&
               input[position] == sse_utf8_bom[bom_matched_]) {
            ++bom_matched_;
            ++position;
        }
        if (bom_matched_ < sse_utf8_bom.size() && position == input.size()) {
            return {.consumed_bytes_ = position};
        }
        bom_settled_ = true;
        if (bom_matched_ < sse_utf8_bom.size()) {
            // Not a BOM: the bytes matched so far begin the first field name.
            append_name(sse_utf8_bom.substr(0, bom_matched_));
        }
    }
    while (position < input.size()) {
        if (std::exchange(skip_line_feed_, false) && input[position] == '\n') {
            ++position;
            continue;
        }
        const auto rest = input.substr(position);
        if (phase_ == line_phase::value_start) {
            phase_ = line_phase::value;
            if (rest.front() == ' ') {
                ++position;
            }
            continue;
        }
        if (phase_ == line_phase::name) {
            const auto stop = rest.find_first_of(":\r\n");
            append_name(rest.substr(0, stop));
            if (stop == std::string_view::npos) {
                return {.consumed_bytes_ = input.size()};
            }
            position += stop + 1;
            const bool unnamed = name_size_ == 0 && !name_overflow_;
            if (!unnamed) {
                classify_field();
            }
            if (rest[stop] == ':') {
                phase_ = unnamed ? line_phase::comment : line_phase::value_start;
                continue;
            }
            skip_line_feed_ = rest[stop] == '\r';
            if (unnamed) {
                if (dispatch()) {
                    return {.consumed_bytes_ = position,
                        .event_ = sse_event(event_type_.empty() ? sse_default_event_type : std::string_view(event_type_),
                            data_, last_event_id_)};
                }
                continue;
            }
            // A line without a colon names a field with an empty value.
            if (!end_line()) {
                fail();
                return {.consumed_bytes_ = position};
            }
            continue;
        }
        const auto stop = rest.find_first_of("\r\n");
        const auto bytes = rest.substr(0, stop);
        if (phase_ == line_phase::value && !append_value(bytes)) {
            fail();
            return {.consumed_bytes_ = position};
        }
        if (stop == std::string_view::npos) {
            return {.consumed_bytes_ = input.size()};
        }
        position += stop + 1;
        skip_line_feed_ = rest[stop] == '\r';
        if (phase_ == line_phase::comment) {
            reset_line();
        } else if (!end_line()) {
            fail();
            return {.consumed_bytes_ = position};
        }
    }
    return {.consumed_bytes_ = position};
}

void sse_parser::finish() noexcept {
    if (state_ != sse_parser_state::active) {
        return;
    }
    release_event_buffers();
    reset_line();
    http_clear_string_retaining_small(data_);
    http_clear_string_retaining_small(event_type_);
    http_clear_string_retaining_small(value_);
    state_ = sse_parser_state::finished;
}

void sse_parser::reset() noexcept {
    event_published_ = false;
    reset_line();
    http_clear_string_retaining_small(data_);
    http_clear_string_retaining_small(event_type_);
    http_clear_string_retaining_small(value_);
    http_clear_string_retaining_small(last_event_id_buffer_);
    http_clear_string_retaining_small(last_event_id_);
    reconnection_time_.reset();
    skip_line_feed_ = false;
    bom_matched_ = 0;
    bom_settled_ = false;
    state_ = sse_parser_state::active;
}

std::size_t sse_parser::buffered_bytes() const noexcept {
    return data_.size() + event_type_.size() + value_.size();
}

void sse_parser::append_name(std::string_view bytes) noexcept {
    if (name_overflow_ || bytes.size() > name_.size() - name_size_) {
        name_overflow_ = name_overflow_ || !bytes.empty();
        return;
    }
    bytes.copy(name_.data() + name_size_, bytes.size());
    name_size_ = static_cast<std::uint8_t>(name_size_ + bytes.size());
}

void sse_parser::classify_field() noexcept {
    const std::string_view name(name_.data(), name_overflow_ ? 0 : name_size_);
    field_ = name == "data"    ? field_kind::data
             : name == "event" ? field_kind::event
             : name == "id"    ? field_kind::id
             : name == "retry" ? field_kind::retry
                               : field_kind::ignored;
}

bool sse_parser::append_value(std::string_view bytes) {
    if (field_ == field_kind::retry) {
        // Only ASCII digits make a valid reconnection time; a value beyond
        // the duration range saturates.
        constexpr auto max_retry = static_cast<std::uint64_t>((std::numeric_limits<std::chrono::milliseconds::rep>::max)());
        for (const char byte : bytes) {
            if (byte < '0' || byte > '9') {
                retry_valid_ = false;
                break;
            }
            const auto digit = static_cast<std::uint64_t>(byte - '0');
            retry_digits_ = true;
            retry_value_ = retry_value_ > (max_retry - digit) / 10 ? max_retry : retry_value_ * 10 + digit;
        }
        return true;
    }
    if (field_ == field_kind::ignored || bytes.empty()) {
        return true;
    }
    if (event_limit_.addition_exceeds(buffered_bytes(), bytes.size())) {
        return false;
    }
    (field_ == field_kind::data ? data_ : value_).append(bytes);
    return true;
}

bool sse_parser::end_line() {
    switch (field_) {
        case field_kind::data:
            if (event_limit_.addition_exceeds(buffered_bytes(), 1)) {
                return false;
            }
            data_.push_back('\n');
            break;
        case field_kind::event:
            replace_ill_formed_utf8(value_);
            event_type_.swap(value_);
            break;
        case field_kind::id:
            // An id containing NUL is ignored.
            if (value_.find('\0') == std::string::npos) {
                replace_ill_formed_utf8(value_);
                last_event_id_buffer_.swap(value_);
            }
            break;
        case field_kind::retry:
            if (retry_digits_ && retry_valid_) {
                reconnection_time_ = std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(retry_value_));
            }
            break;
        case field_kind::ignored:
            break;
    }
    reset_line();
    return true;
}

bool sse_parser::dispatch() {
    last_event_id_ = last_event_id_buffer_;
    if (data_.empty()) {
        http_clear_string_retaining_small(event_type_);
        return false;
    }
    data_.pop_back();
    replace_ill_formed_utf8(data_);
    event_published_ = true;
    return true;
}

void sse_parser::reset_line() noexcept {
    phase_ = line_phase::name;
    field_ = field_kind::ignored;
    name_size_ = 0;
    name_overflow_ = false;
    value_.clear();
    retry_value_ = 0;
    retry_digits_ = false;
    retry_valid_ = true;
}

void sse_parser::release_event_buffers() noexcept {
    if (std::exchange(event_published_, false)) {
        http_clear_string_retaining_small(data_);
        http_clear_string_retaining_small(event_type_);
    }
}

void sse_parser::fail() noexcept {
    reset_line();
    http_clear_string_retaining_small(data_);
    http_clear_string_retaining_small(event_type_);
    http_clear_string_retaining_small(value_);
    state_ = sse_parser_state::failed;
}

std::string format_sse_message(const sse_message& message) {
    if (message.event_.view().find_first_of("\r\n") != std::string_view::npos ||
        (message.id_.has_value() &&
            message.id_->view().find_first_of("\r\n") != std::string_view::npos)) {
        throw std::invalid_argument("SSE event and id must not contain CR or LF");
    }
    if (message.id_.has_value() && (message.id_->view().find('\0') != std::string_view::npos)) {
        throw std::invalid_argument("SSE id must not contain a NUL character");
    }
    if (message.retry_.has_value() && message.retry_->count() < 0) {
        throw std::invalid_argument("SSE retry delay must not be negative");
    }
    // An event stream is UTF-8 (WHATWG HTML, "Parsing an event stream").
    if (!is_valid_utf8(message.event_.view()) ||
        (message.id_.has_value() && !is_valid_utf8(message.id_->view())) ||
        (message.data_.has_value() && !is_valid_utf8(message.data_->view()))) {
        throw std::invalid_argument("SSE fields must be valid UTF-8");
    }

    std::string frame;
    std::array<char, http_max_unsigned_decimal_size> retry_buffer{};
    std::string_view retry_digits{};
    std::size_t total_size = 1U;
    const auto add_size = [&frame, &total_size](std::size_t bytes) {
        if (bytes > frame.max_size() - total_size) {
            throw std::length_error("SSE frame exceeds string capacity");
        }
        total_size += bytes;
    };
    if (!message.event_.empty()) {
        add_size(7U);
        add_size(message.event_.size());
        add_size(1U);
    }
    if (message.id_.has_value()) {
        add_size(3U);
        if (!message.id_->empty()) {
            add_size(1U);
            add_size(message.id_->size());
        }
        add_size(1U);
    }
    if (message.retry_.has_value()) {
        retry_digits = http_format_unsigned(
            retry_buffer, static_cast<std::uint64_t>(message.retry_->count()));
        add_size(7U);
        add_size(retry_digits.size());
        add_size(1U);
    }
    if (message.data_.has_value()) {
        visit_sse_data_lines(message.data_->view(), [&add_size](std::string_view line) {
            add_size(6U);
            add_size(line.size());
            add_size(1U);
        });
    }

    frame.reserve(total_size);
    if (!message.event_.empty()) {
        frame.append("event: ");
        frame.append(message.event_.data(), message.event_.size());
        frame.push_back('\n');
    }
    if (message.id_.has_value()) {
        frame.append("id:");
        if (!message.id_->empty()) {
            frame.push_back(' ');
            frame.append(message.id_->data(), message.id_->size());
        }
        frame.push_back('\n');
    }
    if (message.retry_.has_value()) {
        frame.append("retry: ");
        frame.append(retry_digits);
        frame.push_back('\n');
    }
    if (message.data_.has_value()) {
        visit_sse_data_lines(message.data_->view(), [&frame](std::string_view line) {
            frame.append("data: ");
            frame.append(line.data(), line.size());
            frame.push_back('\n');
        });
    }
    frame.push_back('\n');
    return frame;
}

}  // namespace ruvia

// What a caller-supplied request header section must satisfy before the writer
// will encode it, and the facts the encoder then needs from it: its field
// section budget, which singleton fields it already carries, and the
// connection and upgrade options it expresses.

namespace ruvia::detail {

struct http1_client_request_field_facts final {
    // The caller's fields, charged by the shared field-section rule.
    http_field_section_budget section_;
    detail::request_field_state fields_{detail::request_field_framing::http1};
    detail::http_connection_options connection_options_;
    detail::http_upgrade_protocols upgrade_protocols_;
    std::string_view first_upgrade_value_{};
    std::size_t upgrade_value_bytes_{0};
    std::size_t upgrade_field_count_{0};
    bool has_te_{false};
    // The caller declared trailer fields; the writer requires chunked content.
    bool has_trailer_{false};
};

[[nodiscard]] bool analyze_http1_client_request_field(http_header_view header, detail::http_field_kind kind,
    http1_client_request_field_facts& facts, http1_client_request_prepare_error& error,
    http_request_field_source source = http_request_field_source::generated) noexcept;

[[nodiscard]] bool finish_http1_client_request_fields(
    const http1_client_request_field_facts& facts, http1_client_request_prepare_error& error) noexcept;

// Validate every field and collect the facts the encoder needs. Returns false
// with `error` set on the first field that cannot be sent.
[[nodiscard]] bool analyze_http1_client_request_fields(std::span<const http_header_view> headers,
    http1_client_request_field_facts& facts, http1_client_request_prepare_error& error,
    http_request_field_source source) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

struct http1_client_request_prepare_result_access final {
    using result_type = std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>;

    [[nodiscard]] static http1_client_exchange_state exchange_state(http_known_method method,
        http1_close_policy close_policy, http1_client_initial_content_state content_state,
        std::string offered_upgrade_protocols) noexcept {
        return http1_client_exchange_state(
            method, close_policy, content_state, std::move(offered_upgrade_protocols));
    }

    [[nodiscard]] static constexpr result_type buffer_too_small(
        std::size_t required_head_bytes) noexcept {
        return result_type(http1_client_request_buffer_too_small(required_head_bytes));
    }

    [[nodiscard]] static constexpr result_type failure(
        http1_client_request_prepare_error error) noexcept {
        return result_type(error);
    }

    [[nodiscard]] static result_type prepared_streaming_content(
        std::string_view head, std::optional<std::uint64_t> length, bool gated,
        http1_client_exchange_state state_value) noexcept {
        return result_type(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_streaming_request_content(length, gated)), std::move(state_value)));
    }

    [[nodiscard]] static result_type prepared_without_content(
        std::string_view head, http1_client_exchange_state exchange_state) noexcept {
        return result_type(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_request_without_content()),
            std::move(exchange_state)));
    }

    [[nodiscard]] static result_type prepared_immediate_content(
        std::string_view head, std::string_view content_bytes,
        http1_client_exchange_state exchange_state) noexcept {
        return result_type(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_immediate_request_content(content_bytes)),
            std::move(exchange_state)));
    }

    [[nodiscard]] static result_type prepared_continue_gated_content(
        std::string_view head, std::string_view content_bytes,
        http1_client_exchange_state exchange_state) noexcept {
        return result_type(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_continue_gated_request_content(content_bytes)),
            std::move(exchange_state)));
    }
};

}  // namespace ruvia::detail

namespace ruvia {
namespace {

constexpr std::string_view crlf = "\r\n";
constexpr std::string_view http11_request_line_suffix = " HTTP/1.1\r\n";
constexpr std::string_view host_prefix = "Host: ";
constexpr std::string_view content_length_prefix = "Content-Length: ";
constexpr std::string_view expect_prefix = "Expect: ";
constexpr std::string_view connection_close = "Connection: close\r\n";
constexpr std::string_view chunked_header = "Transfer-Encoding: chunked\r\n";

void append_authority(detail::http_raw_wire_sink& sink, std::string_view host, std::string_view port) noexcept {
    sink.append(host);
    if (!port.empty()) {
        sink.append(':');
        sink.append(port);
    }
}

struct request_head_write_plan final {
    std::string_view host_;
    std::string_view port_;
    std::string_view method_;
    // "http://" or "https://" ahead of the authority for absolute-form;
    // empty for origin-form.
    std::string_view absolute_scheme_;
    std::string_view target_;
    std::span<const http_header_view> headers_;
    std::string_view content_length_;
    bool connect_;
    bool explicit_content_;
    bool expect_continue_;
    bool generate_connection_close_;
};

[[nodiscard]] char* write_request_head(char* output, const request_head_write_plan& plan) noexcept {
    detail::http_raw_wire_sink sink(output);
    sink.append(plan.method_);
    sink.append(' ');
    if (plan.connect_) {
        append_authority(sink, plan.host_, plan.port_);
    } else {
        if (!plan.absolute_scheme_.empty()) {
            sink.append(plan.absolute_scheme_);
            append_authority(sink, plan.host_, plan.port_);
        }
        sink.append(plan.target_);
    }
    sink.append(http11_request_line_suffix);
    sink.append(host_prefix);
    append_authority(sink, plan.host_, plan.port_);
    sink.append(crlf);
    for (const auto& header : plan.headers_) {
        detail::emit_http1_field_line(sink, header.name(), header.value());
    }
    if (plan.explicit_content_ && plan.content_length_.empty()) {
        sink.append(chunked_header);
    } else if (plan.explicit_content_) {
        sink.append(content_length_prefix);
        sink.append(plan.content_length_);
        sink.append(crlf);
    }
    if (plan.expect_continue_) {
        sink.append(expect_prefix);
        sink.append(detail::http_continue_expectation_token);
        sink.append(crlf);
    }
    if (plan.generate_connection_close_) {
        sink.append(connection_close);
    }
    sink.append(crlf);
    return sink.position();
}

[[nodiscard]] bool is_valid_http1_close_policy(http1_close_policy policy) noexcept {
    switch (policy) {
        case http1_close_policy::allow_reuse:
        case http1_close_policy::close_after_response:
            return true;
    }
    return false;
}

[[nodiscard]] bool is_valid_http_client_request_expectation(
    http_client_request_expectation expectation) noexcept {
    switch (expectation) {
        case http_client_request_expectation::none:
        case http_client_request_expectation::continue_value:
            return true;
    }
    return false;
}

[[nodiscard]] std::string own_offered_upgrade_protocols(
    std::span<const http_header_view> headers, const detail::http1_client_request_field_facts& facts) {
    if (facts.upgrade_field_count_ == 0) {
        return {};
    }
    if (facts.upgrade_field_count_ == 1) {
        return std::string(facts.first_upgrade_value_);
    }
    std::string result;
    result.reserve(facts.upgrade_value_bytes_);
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "Upgrade")) {
            continue;
        }
        if (!result.empty()) {
            result.push_back(',');
        }
        result.append(header.value());
    }
    return result;
}

// Every request line, CONNECT included, rejects these wire policies first.
// CONNECT is always authority-form (RFC 9112 section 3.2.3).
[[nodiscard]] std::optional<http1_client_request_prepare_error> validate_wire_policy(
    http1_client_request_wire_policy policy, bool connect) noexcept {
    if (!is_valid_http1_close_policy(policy.close_policy_)) {
        return http1_client_request_prepare_error::invalid_close_policy;
    }
    if (!is_valid_http_client_request_expectation(policy.expectation_)) {
        return http1_client_request_prepare_error::invalid_expectation;
    }
    switch (policy.target_form_) {
        case http1_request_target_form::origin:
            return std::nullopt;
        case http1_request_target_form::absolute:
            if (!connect) {
                return std::nullopt;
            }
            break;
    }
    return http1_client_request_prepare_error::invalid_target_form;
}

[[nodiscard]] std::optional<http1_client_request_prepare_error> validate_non_connect_request_line(
    std::string_view method, std::string_view target, http1_client_request_wire_policy policy,
    http_known_method& known_method) noexcept {
    if (const auto error = validate_wire_policy(policy, false)) {
        return error;
    }
    if (!is_valid_http_method_token(method)) {
        return http1_client_request_prepare_error::invalid_method;
    }
    known_method = classify_http_method(method);
    if (known_method == http_known_method::connect) {
        return http1_client_request_prepare_error::connect_requires_dedicated_entry;
    }
    if (!detail::is_valid_origin_or_asterisk_form_target(known_method, target)) {
        return http1_client_request_prepare_error::invalid_target;
    }
    return std::nullopt;
}

[[nodiscard]] detail::http1_client_request_prepare_result_access::result_type prepare_request(const http_origin_view& origin,
    std::string_view method, http_known_method known_method, std::string_view target,
    std::span<const http_header_view> headers, http_client_request_content_view content,
    std::span<char> head_buffer, http1_client_request_wire_policy policy,
    const http1_client_request_head_view* streaming = nullptr,
    std::span<const http_header_view> connect_udp_headers = {}) {
    if (!detail::is_valid_http_request_field_source(policy.field_source_)) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_header);
    }
    const bool connect = known_method == http_known_method::connect;
    detail::http1_client_request_field_facts header_facts;
    http1_client_request_prepare_error error = http1_client_request_prepare_error::invalid_header;
    const auto* content_bytes = content.borrowed_bytes();
    const bool explicit_content = content_bytes != nullptr || streaming != nullptr;
    const auto length = streaming       ? (streaming->content_length_
                                                  ? std::optional<std::uint64_t>(streaming->content_length_->value())
                                                  : std::nullopt)
                        : content_bytes ? std::optional<std::uint64_t>(content_bytes->value().size())
                                        : std::nullopt;
    const bool chunked = streaming && !length;
    if (!detail::analyze_http1_client_request_fields(headers, header_facts, error, policy.field_source_)) {
        return detail::http1_client_request_prepare_result_access::failure(error);
    }
    // A sender that will generate trailer fields SHOULD declare them (RFC 9110
    // section 6.6.2); only chunked content has a trailer section.
    if (header_facts.has_trailer_ && !chunked) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::trailer_requires_chunked_content);
    }
    const bool expect_continue = policy.expectation_ == http_client_request_expectation::continue_value;
    const auto content_indication = explicit_content && (!length || *length != 0)
                                        ? http_request_content_indication::will_follow
                                        : http_request_content_indication::no_content;
    if (!http_client_expectation_is_valid(expect_continue, content_indication)) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::expectation_without_content);
    }
    if (content_indication == http_request_content_indication::will_follow) {
        const auto content_semantics = classify_http_request_content_semantics(method);
        if (content_semantics == http_request_content_semantics::forbidden) {
            return detail::http1_client_request_prepare_result_access::failure(
                http1_client_request_prepare_error::content_forbidden_for_method);
        }
        // RFC 9110 section 9.3.7 requires the media description when generating
        // OPTIONS content, not when forwarding an already received request.
        if (policy.field_source_ == http_request_field_source::generated &&
            content_semantics == http_request_content_semantics::content_type_required &&
            !header_facts.fields_.seen(detail::http_field_kind::content_type)) {
            return detail::http1_client_request_prepare_result_access::failure(
                http1_client_request_prepare_error::options_content_type_required);
        }
    }

    const bool generate_connection_close =
        policy.close_policy_ == http1_close_policy::close_after_response &&
        !header_facts.connection_options_.close();
    const auto effective_close_policy =
        header_facts.connection_options_.close() || generate_connection_close
            ? http1_close_policy::close_after_response
            : http1_close_policy::allow_reuse;

    // Reuse each decimal token in measurement and emission. CONNECT uses the
    // same port token for both its authority target and generated Host field.
    std::array<char, http_max_unsigned_decimal_size> port_buffer;
    const auto port_value = connect || origin.port() != (origin.scheme() == http_scheme::https ? std::uint16_t{443} : std::uint16_t{80})
                                ? http_format_unsigned(port_buffer, origin.port())
                                : std::string_view{};
    const auto authority_bytes = origin.host().size() + (port_value.empty() ? 0 : 1 + port_value.size());
    std::array<char, http_max_unsigned_decimal_size> content_length_buffer;
    const auto content_length_value = explicit_content && !chunked
                                          ? http_format_unsigned(content_length_buffer, *length)
                                          : std::string_view{};
    // absolute-form names the origin as the generated Host does, so the two
    // authorities agree (RFC 9112 section 3.2); OPTIONS * keeps only the
    // authority, with an empty path (section 3.2.4).
    const auto absolute_scheme = connect || policy.target_form_ != http1_request_target_form::absolute
                                     ? std::string_view{}
                                 : origin.scheme() == http_scheme::https ? std::string_view{"https://"}
                                                                         : std::string_view{"http://"};
    const auto emitted_target = !absolute_scheme.empty() && target == "*" ? std::string_view{} : target;
    const std::size_t target_bytes =
        connect ? authority_bytes
                : (absolute_scheme.empty() ? 0 : absolute_scheme.size() + authority_bytes) + emitted_target.size();
    // The caller's fields are already charged; add the request line and the
    // generated fields to the same section budget.
    auto head_budget = header_facts.section_;
    if (!head_budget.add(method.size()) || !head_budget.add(1) || !head_budget.add(target_bytes) ||
        !head_budget.add(http11_request_line_suffix.size()) ||
        !head_budget.add(host_prefix.size()) || !head_budget.add(authority_bytes) ||
        !head_budget.add(crlf.size()) || (chunked && !head_budget.add(chunked_header.size())) ||
        (explicit_content && !chunked &&
            (!head_budget.add(content_length_prefix.size()) ||
                !head_budget.add(content_length_value.size()) || !head_budget.add(crlf.size()))) ||
        (expect_continue &&
            (!head_budget.add(expect_prefix.size()) ||
                !head_budget.add(detail::http_continue_expectation_token.size()) ||
                !head_budget.add(crlf.size()))) ||
        (generate_connection_close && !head_budget.add(connection_close.size())) ||
        !head_budget.add(crlf.size())) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::header_too_large);
    }
    if (!connect_udp_headers.empty() &&
        validate_http_connect_udp_request_to_send({
                                                      .version_ = http_protocol_version::http11,
                                                      .method_ = "GET",
                                                      .authority_ = origin.host(),
                                                      .path_ = target,
                                                      .headers_ = connect_udp_headers,
                                                  })
            .has_value()) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_header);
    }
    const auto head_bytes = head_budget.bytes();
    if (head_buffer.size() < head_bytes) {
        return detail::http1_client_request_prepare_result_access::buffer_too_small(head_bytes);
    }

    // Upgrade is the only response-planning fact that cannot be reduced to a
    // fixed-size value. Own it before mutating the caller's output buffer so an
    // allocation failure leaves request preparation transactional.
    auto offered_upgrade_protocols = header_facts.upgrade_protocols_.has_protocol()
                                         ? own_offered_upgrade_protocols(headers, header_facts)
                                         : std::string();

    const auto content_state = expect_continue
                                   ? detail::http1_client_initial_content_state::awaiting_continue
                                   : (explicit_content && (!length || *length != 0)
                                             ? detail::http1_client_initial_content_state::pending
                                             : detail::http1_client_initial_content_state::complete);
    auto exchange_state = detail::http1_client_request_prepare_result_access::exchange_state(
        known_method, effective_close_policy, content_state,
        std::move(offered_upgrade_protocols));
    const request_head_write_plan write_plan{origin.host(), port_value, method, absolute_scheme, emitted_target, headers, content_length_value,
        connect, explicit_content, expect_continue, generate_connection_close};
    const auto output = head_buffer.first(head_bytes);
    // Descriptors and every borrowed string are consumed before the output is
    // overwritten when the caller's request storage aliases the head buffer.
    detail::write_http_wire(output,
        detail::http_wire_inputs_overlap(std::string_view(output.data(), output.size()), headers,
            {method, target, origin.host()}),
        [&write_plan](char* destination) noexcept { return write_request_head(destination, write_plan); });
    const auto head = std::string_view(head_buffer.data(), head_bytes);
    if (streaming) {
        return detail::http1_client_request_prepare_result_access::prepared_streaming_content(head, length, expect_continue, std::move(exchange_state));
    }
    if (!explicit_content) {
        return detail::http1_client_request_prepare_result_access::prepared_without_content(
            head, std::move(exchange_state));
    }
    if (expect_continue) {
        return detail::http1_client_request_prepare_result_access::prepared_continue_gated_content(
            head, content_bytes->value(), std::move(exchange_state));
    }
    return detail::http1_client_request_prepare_result_access::prepared_immediate_content(
        head, content_bytes->value(), std::move(exchange_state));
}

// The one CONNECT rule set behind both prepare_connect() entries: wire-policy
// checks first, then the tunnel target (null when the caller's authority is
// invalid; port 0 is never a tunnel target), emitted in authority-form.
[[nodiscard]] detail::http1_client_request_prepare_result_access::result_type prepare_connect_request(
    const http_origin_view* tunnel_origin, std::span<const http_header_view> headers, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) {
    if (const auto error = validate_wire_policy(policy, true)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    if (tunnel_origin == nullptr || tunnel_origin->port() == 0) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_connect_origin);
    }
    return prepare_request(*tunnel_origin, "CONNECT", http_known_method::connect, {}, headers,
        http_client_request_content_view::none(), head_buffer, policy);
}

}  // namespace

std::string_view http1_client_request_prepare_error_message(
    http1_client_request_prepare_error error) noexcept {
    switch (error) {
        case http1_client_request_prepare_error::invalid_method:
            return "invalid HTTP/1 client request method";
        case http1_client_request_prepare_error::invalid_target:
            return "invalid HTTP/1 client request target";
        case http1_client_request_prepare_error::connect_requires_dedicated_entry:
            return "CONNECT requires the dedicated HTTP/1 client entry";
        case http1_client_request_prepare_error::invalid_connect_origin:
            return "invalid HTTP/1 CONNECT origin";
        case http1_client_request_prepare_error::invalid_header:
            return "invalid HTTP/1 client request header";
        case http1_client_request_prepare_error::host_header_managed_by_writer:
            return "HTTP/1 client Host is managed by the writer";
        case http1_client_request_prepare_error::content_length_managed_by_writer:
            return "HTTP/1 client Content-Length is managed by the writer";
        case http1_client_request_prepare_error::transfer_encoding_unsupported:
            return "HTTP/1 client Transfer-Encoding is unsupported";
        case http1_client_request_prepare_error::trailer_requires_chunked_content:
            return "HTTP/1 client Trailer requires chunked request content";
        case http1_client_request_prepare_error::expect_header_managed_by_writer:
            return "HTTP/1 client Expect is managed by the writer";
        case http1_client_request_prepare_error::invalid_connection:
            return "invalid HTTP/1 client Connection header";
        case http1_client_request_prepare_error::invalid_upgrade:
            return "invalid HTTP/1 client Upgrade header";
        case http1_client_request_prepare_error::upgrade_connection_option_required:
            return "HTTP/1 Upgrade requires Connection: Upgrade";
        case http1_client_request_prepare_error::te_connection_option_required:
            return "HTTP/1 TE requires Connection: TE";
        case http1_client_request_prepare_error::expectation_without_content:
            return "Continue expectation requires non-empty request content";
        case http1_client_request_prepare_error::content_forbidden_for_method:
            return "request method forbids content";
        case http1_client_request_prepare_error::options_content_type_required:
            return "OPTIONS content requires Content-Type";
        case http1_client_request_prepare_error::header_too_large:
            return "HTTP/1 client request header is too large";
        case http1_client_request_prepare_error::invalid_close_policy:
            return "invalid HTTP/1 client close policy";
        case http1_client_request_prepare_error::invalid_expectation:
            return "invalid HTTP/1 client request expectation";
        case http1_client_request_prepare_error::invalid_target_form:
            return "invalid HTTP/1 client request target form";
    }
    return "invalid HTTP/1 client request";
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare(const http_origin_view& origin,
    const http_client_request_view& request, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) const {
    http_known_method known_method = http_known_method::unknown;
    if (const auto error = validate_non_connect_request_line(
            request.method_, request.target_, policy, known_method)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    return prepare_request(origin, request.method_, known_method, request.target_,
        static_cast<std::span<const http_header_view>>(request.headers_), request.content_, head_buffer,
        policy);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare_streaming(const http_origin_view& origin,
    const http1_client_request_head_view& request, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) const {
    http_known_method known_method = http_known_method::unknown;
    if (const auto error = validate_non_connect_request_line(
            request.method_, request.target_, policy, known_method)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    return prepare_request(origin, request.method_, known_method, request.target_, request.headers_,
        http_client_request_content_view::none(), head_buffer, policy, &request);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare_connect_udp(const http_origin_view& origin,
    borrowed_text target, std::span<const http_header_view> headers, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) const {
    http_known_method known_method = http_known_method::unknown;
    if (const auto error = validate_non_connect_request_line("GET", target.view(), policy, known_method)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "connection")) {
            return detail::http1_client_request_prepare_result_access::failure(
                http1_client_request_prepare_error::invalid_connection);
        }
        if (http_ascii_equals_ignore_case(field.name(), "upgrade")) {
            return detail::http1_client_request_prepare_result_access::failure(
                http1_client_request_prepare_error::invalid_upgrade);
        }
    }
    detail::http_header_descriptor_storage<http_header_view> staged_fields;
    staged_fields.reserve(headers.size() + 3U);
    for (const auto& field : headers) {
        staged_fields.append(field);
    }
    staged_fields.append(
        http_header_view{detail::http_connection_field_name, detail::http_connection_upgrade_option});
    staged_fields.append(http_header_view{"Upgrade", "connect-udp"});
    staged_fields.append(http_header_view{"Host", origin.host()});
    const auto all_fields = staged_fields.view();
    const auto emitted_headers = all_fields.first(all_fields.size() - 1U);
    return prepare_request(origin, "GET", known_method, target.view(), emitted_headers,
        http_client_request_content_view::none(), head_buffer, policy, nullptr, all_fields);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare_connect(
    const http_origin_view& tunnel_origin, std::span<const http_header_view> headers,
    std::span<char> head_buffer, http1_client_request_wire_policy policy) const {
    return prepare_connect_request(&tunnel_origin, headers, head_buffer, policy);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare_connect(borrowed_text authority,
    std::span<const http_header_view> headers, std::span<char> head_buffer, http1_client_request_wire_policy policy) const {
    // RFC 9110 section 9.3.6: the tunnel destination needs a non-empty port,
    // and port zero is rejected by prepare_connect_request.
    const auto parsed_value = parse_http_authority(authority);
    if (!parsed_value || parsed_value->port_kind() != http_authority_port_kind::value) {
        return prepare_connect_request(nullptr, headers, head_buffer, policy);
    }
    const auto tunnel_origin =
        http_origin_view::http({.host_ = parsed_value->host(), .port_ = *parsed_value->port()});
    return prepare_connect_request(&tunnel_origin, headers, head_buffer, policy);
}

http1_response_head_plan prepare_http1_connect_response_head(
    const http_response& response, http_protocol_version version) {
    if ((version != http_protocol_version::http10 && version != http_protocol_version::http11) ||
        !response.status().is_successful() || response.file_body() || !response.body_bytes().empty()) {
        throw http1_response_head_error("invalid HTTP/1 CONNECT response head");
    }
    if (detail::response_has_known_header(
            response, detail::response_header_content_length | detail::response_header_transfer_encoding)) {
        throw http1_response_head_error("CONNECT response cannot contain message framing fields");
    }
    const auto body = plan_http_response_body(http_known_method::connect, response.status());
    const auto connection = version == http_protocol_version::http10
                                ? plan_http10_request_connection(true, false)
                                : http1_request_connection_plan::http11_close();
    return http1_close_delimited_response_stream_head_plan(body, connection);
}

}  // namespace ruvia

namespace ruvia::detail {

bool analyze_http1_client_request_field(http_header_view header, detail::http_field_kind kind,
    http1_client_request_field_facts& facts, http1_client_request_prepare_error& error,
    http_request_field_source source) noexcept {
    const auto name = header.name();
    const auto value = header.value();
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        error = http1_client_request_prepare_error::invalid_header;
        return false;
    }
    // The HTTP/1 writer owns Host and refuses message framing it does not
    // generate; the remaining semantics are version-independent.
    switch (kind) {
        case detail::http_field_kind::host:
            error = http1_client_request_prepare_error::host_header_managed_by_writer;
            return false;
        case detail::http_field_kind::transfer_encoding:
            error = http1_client_request_prepare_error::transfer_encoding_unsupported;
            return false;
        default:
            break;
    }
    switch (detail::validate_request_field(kind, value, detail::http_field_list_role::sender, facts.fields_, source)) {
        case detail::request_field_status::ok:
            break;
        case detail::request_field_status::invalid:
            error = http1_client_request_prepare_error::invalid_header;
            return false;
        case detail::request_field_status::managed_by_sender:
            error = kind == detail::http_field_kind::expect
                        ? http1_client_request_prepare_error::expect_header_managed_by_writer
                        : http1_client_request_prepare_error::content_length_managed_by_writer;
            return false;
    }
    if (kind == detail::http_field_kind::connection) {
        if (facts.connection_options_.parse_field(value, detail::http_connection_field_role::sender) !=
            detail::http_field_list_parse_status::ok) {
            error = http1_client_request_prepare_error::invalid_connection;
            return false;
        }
    } else if (kind == detail::http_field_kind::upgrade) {
        const auto role = source == http_request_field_source::forwarded
                              ? detail::http_field_list_role::recipient
                              : detail::http_field_list_role::sender;
        if (facts.upgrade_protocols_.parse_field(value, role) !=
            detail::http_field_list_parse_status::ok) {
            error = http1_client_request_prepare_error::invalid_upgrade;
            return false;
        }
        if (facts.upgrade_field_count_ == 0) {
            facts.first_upgrade_value_ = value;
        } else {
            ++facts.upgrade_value_bytes_;
        }
        facts.upgrade_value_bytes_ += value.size();
        ++facts.upgrade_field_count_;
    } else if (kind == detail::http_field_kind::te) {
        facts.has_te_ = true;
    } else if (kind == detail::http_field_kind::trailer) {
        facts.has_trailer_ = true;
    }

    if (!facts.section_.add_field(name, value)) {
        error = http1_client_request_prepare_error::header_too_large;
        return false;
    }
    return true;
}

bool finish_http1_client_request_fields(
    const http1_client_request_field_facts& facts, http1_client_request_prepare_error& error) noexcept {
    if (facts.upgrade_protocols_.has_field() && !facts.connection_options_.upgrade()) {
        error = http1_client_request_prepare_error::upgrade_connection_option_required;
        return false;
    }
    if (facts.has_te_ && !facts.connection_options_.te()) {
        error = http1_client_request_prepare_error::te_connection_option_required;
        return false;
    }
    return true;
}

bool analyze_http1_client_request_fields(std::span<const http_header_view> headers, http1_client_request_field_facts& facts,
    http1_client_request_prepare_error& error, http_request_field_source source) noexcept {
    for (const auto& header : headers) {
        if (!analyze_http1_client_request_field(header, detail::classify_http_field(header.name()), facts, error, source)) {
            return false;
        }
    }
    return finish_http1_client_request_fields(facts, error);
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

[[nodiscard]] char* write_chunked_trailers(char* output, std::span<const http_header_view> trailers) noexcept {
    detail::http_raw_wire_sink sink(output);
    sink.append(http1_last_chunk_prefix);
    for (const auto& field : trailers) {
        detail::emit_http1_field_line(sink, field.name(), field.value());
    }
    sink.append(http1_trailer_section_terminator);
    return sink.position();
}

}  // namespace

using error_type = http1_request_content_write_error;
http1_request_content_writer::http1_request_content_writer(const http1_client_streaming_request_content& plan) noexcept
    : length_(plan.content_length()),
      gated_(plan.continue_gated()) {}
void http1_request_content_writer::release_content() noexcept {
    gated_ = false;
}
void http1_request_content_writer::abort() noexcept {
    stopped_ = true;
    writing_ = false;
    finishing_ = false;
}
std::variant<http1_request_content_writer::chunk, error_type> http1_request_content_writer::plan_chunk(std::span<const char> payload_value) noexcept {
    if (stopped_ || finished_) {
        return error_type::stopped;
    }
    if (gated_) {
        return error_type::awaiting_continue;
    }
    if (writing_ || finishing_) {
        return error_type::write_pending;
    }
    if (payload_value.size() > std::numeric_limits<std::uint64_t>::max() - committed_) {
        return error_type::length_overflow;
    }
    if (length_ && payload_value.size() > *length_ - committed_) {
        return error_type::length_mismatch;
    }
    chunk result;
    result.payload_ = payload_value;
    if (!length_ && !payload_value.empty()) {
        http1_chunk_header header_value(payload_value.size());
        auto prefix = header_value.view();
        std::copy(prefix.begin(), prefix.end(), result.prefix_.begin());
        result.prefix_size_ = prefix.size();
        result.suffix_ = http1_chunk_data_terminator;
    }
    writing_ = true;
    pending_ = payload_value.size();
    return result;
}
void http1_request_content_writer::commit_chunk() {
    if (stopped_ || finished_ || !writing_) {
        throw std::logic_error("HTTP/1 request content chunk commit requires a pending chunk write");
    }
    committed_ += pending_;
    pending_ = 0;
    writing_ = false;
}
std::variant<std::string_view, error_type> http1_request_content_writer::plan_finish(std::span<char> buffer, std::span<const http_header_view> trailers) noexcept {
    if (stopped_ || finished_) {
        return error_type::stopped;
    }
    if (gated_) {
        return error_type::awaiting_continue;
    }
    if (writing_ || finishing_) {
        return error_type::write_pending;
    }
    if (length_ && committed_ != *length_) {
        return error_type::length_mismatch;
    }
    if (length_ && !trailers.empty()) {
        return error_type::trailers_require_chunked;
    }
    // The last-chunk line shares the section budget, which keeps the whole
    // encoded ending within the bounded wire snapshot.
    auto section = http_trailer_section_budget();
    (void)section.add(http1_last_chunk_prefix.size());
    for (const auto& field : trailers) {
        if (const auto error = detail::charge_http_request_trailer_field(section, field.name(), field.value())) {
            return *error == http_request_trailer_error::section_too_large ? error_type::header_too_large
                                                                           : error_type::invalid_trailer;
        }
    }
    const auto required = length_ ? std::size_t{0} : section.bytes();
    if (buffer.size() < required) {
        return error_type::output_too_small;
    }
    if (!length_) {
        const auto output = buffer.first(required);
        detail::write_http_wire(output,
            detail::http_wire_inputs_overlap(std::string_view(output.data(), output.size()), trailers),
            [trailers](char* destination) noexcept { return write_chunked_trailers(destination, trailers); });
    }
    finishing_ = true;
    return required ? std::string_view(buffer.data(), required) : std::string_view{};
}
void http1_request_content_writer::commit_finish() {
    if (stopped_ || finished_ || !finishing_) {
        throw std::logic_error("HTTP/1 request content finish commit requires a pending finish write");
    }
    finishing_ = false;
    finished_ = true;
}
}  // namespace ruvia

namespace ruvia {
namespace {
// The one application-header policy of the client handshake: the request
// writer owns every protocol-managed field (Host, framing, Expect and the
// Upgrade/Connection pair) and the handshake generates every Sec-WebSocket-*
// field.
bool is_reserved_handshake_header(std::string_view name, detail::http_field_kind kind) noexcept {
    switch (kind) {
        case detail::http_field_kind::sec_websocket_key:
        case detail::http_field_kind::sec_websocket_version:
        case detail::http_field_kind::sec_websocket_protocol:
        case detail::http_field_kind::sec_websocket_extensions:
            return true;
        case detail::http_field_kind::other:
            return detail::is_protocol_managed_request_field(kind, name) ||
                   http_ascii_equals_ignore_case(name, "sec-websocket-accept");
        default:
            return detail::is_protocol_managed_request_field(kind, name);
    }
}
}  // namespace

void validate_http1_websocket_client_handshake_configuration(std::span<const http_header_view> headers) {
    detail::http1_client_request_field_facts facts;
    http1_client_request_prepare_error error{};
    for (const auto& header : headers) {
        const auto kind = detail::classify_http_field(header.name());
        if (is_reserved_handshake_header(header.name(), kind)) {
            throw std::invalid_argument("invalid or reserved WebSocket handshake header");
        }
        if (!detail::analyze_http1_client_request_field(header, kind, facts, error)) {
            throw std::invalid_argument(http1_client_request_prepare_error_message(error).data());
        }
    }
    if (!detail::finish_http1_client_request_fields(facts, error)) {
        throw std::invalid_argument(http1_client_request_prepare_error_message(error).data());
    }
}

namespace {
template <typename header_type>
std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_websocket_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const header_type> headers, std::span<char> head_buffer, http_request_field_source source) {
    const auto negotiation_fields = negotiation.request_fields();
    detail::http_header_descriptor_storage<http_header_view> staged_headers;
    staged_headers.reserve(headers.size() + 3U + negotiation_fields.size());
    for (const auto& header : headers) {
        staged_headers.append(http_header_view{header.name(), header.value()});
    }
    staged_headers.append(http_header_view{"Upgrade", "websocket"});
    staged_headers.append(
        http_header_view{detail::http_connection_field_name, detail::http_connection_upgrade_option});
    staged_headers.append(
        http_header_view{"Sec-WebSocket-Key", std::string_view(encoded_key.data(), encoded_key.size())});
    for (const auto& field : negotiation_fields) {
        staged_headers.append(http_header_view{field.name(), field.value()});
    }
    return http1_client_request_writer()
        .prepare(origin, {.method_ = "GET", .target_ = target, .headers_ = staged_headers.view()}, head_buffer, {.field_source_ = source});
}

}  // namespace

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header> headers, std::span<char> head_buffer, http_request_field_source source) {
    return prepare_websocket_request(origin, target, encoded_key, negotiation, headers, head_buffer, source);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header_view> headers, std::span<char> head_buffer, http_request_field_source source) {
    return prepare_websocket_request(origin, target, encoded_key, negotiation, headers, head_buffer, source);
}

std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_http1_websocket_client_response(std::span<const char, 24> encoded_key,
    const websocket_client_negotiation& negotiation, const http1_parsed_client_response_head& response) {
    if (response.plan().protocol_upgrade() == nullptr ||
        response.head().status() != http_status::switching_protocols) {
        return websocket_client_handshake_error::response_status;
    }
    detail::websocket_accept_key_type expected{};
    detail::encode_websocket_accept(
        expected, std::string_view(encoded_key.data(), encoded_key.size()));
    std::size_t accepts = 0;
    bool accept_matches = false, has_upgrade = false, has_connection = false;
    for (const auto& header : response.head().headers()) {
        switch (detail::classify_http_field(header.name())) {
            case detail::http_field_kind::upgrade:
                has_upgrade = has_upgrade || detail::http_has_token(header.value(), "websocket");
                break;
            case detail::http_field_kind::connection:
                has_connection = has_connection ||
                                 detail::http_has_token(header.value(), detail::http_connection_upgrade_option);
                break;
            case detail::http_field_kind::other:
                if (http_ascii_equals_ignore_case(header.name(), "Sec-WebSocket-Accept")) {
                    ++accepts;
                    accept_matches = http_trim_ows(header.value()) ==
                                     std::string_view(expected.data(), expected.size());
                }
                break;
            default:
                break;
        }
    }
    if (accepts != 1 || !accept_matches) {
        return websocket_client_handshake_error::accept;
    }
    if (!has_upgrade) {
        return websocket_client_handshake_error::upgrade;
    }
    if (!has_connection) {
        return websocket_client_handshake_error::connection;
    }

    return detail::validate_websocket_client_selection(negotiation, response.head().headers(), false);
}
}  // namespace ruvia

namespace ruvia::detail {

struct http1_client_response_parse_result_access final {
    [[nodiscard]] static http1_client_response_parse_result need_more() noexcept {
        return http1_client_response_parse_result(http1_client_response_need_more());
    }

    [[nodiscard]] static http1_client_response_parse_result failure(
        http1_client_response_parse_error error) noexcept {
        return http1_client_response_parse_result(error);
    }

    [[nodiscard]] static http1_client_response_parse_result parsed(http_client_response_head head,
        http1_client_response_plan plan, std::size_t consumed_bytes) noexcept {
        return http1_client_response_parse_result(
            http1_parsed_client_response_head(std::move(head), std::move(plan), consumed_bytes));
    }
};

}  // namespace ruvia::detail

namespace ruvia {

namespace {

[[nodiscard]] constexpr detail::http1_client_request_content_phase receive_continue(
    detail::http1_client_request_content_phase phase) noexcept {
    switch (phase) {
        case detail::http1_client_request_content_phase::awaiting_continue:
        case detail::http1_client_request_content_phase::content_released_awaiting_continue:
            return detail::http1_client_request_content_phase::continue_received;
        case detail::http1_client_request_content_phase::content_complete_awaiting_continue:
            return detail::http1_client_request_content_phase::continue_received_content_complete;
        case detail::http1_client_request_content_phase::content_complete:
        case detail::http1_client_request_content_phase::content_pending:
        case detail::http1_client_request_content_phase::continue_received:
        case detail::http1_client_request_content_phase::continue_received_content_complete:
            return phase;
    }
    return phase;
}

}  // namespace

std::string_view http1_client_response_parse_error_message(
    http1_client_response_parse_error error) noexcept {
    switch (error) {
        case http1_client_response_parse_error::header_too_large:
            return "response header is too large";
        case http1_client_response_parse_error::invalid_status_line:
            return "invalid response status line";
        case http1_client_response_parse_error::unsupported_http_version:
            return "unsupported response HTTP version";
        case http1_client_response_parse_error::invalid_status_code:
            return "invalid response status code";
        case http1_client_response_parse_error::invalid_reason_phrase:
            return "invalid response reason phrase";
        case http1_client_response_parse_error::invalid_header:
            return "invalid response header";
        case http1_client_response_parse_error::invalid_connection:
            return "invalid response Connection header";
        case http1_client_response_parse_error::invalid_upgrade:
            return "invalid response Upgrade header";
        case http1_client_response_parse_error::invalid_content_length:
            return "invalid response Content-Length";
        case http1_client_response_parse_error::conflicting_content_length:
            return "conflicting response Content-Length";
        case http1_client_response_parse_error::invalid_transfer_encoding:
            return "invalid response Transfer-Encoding";
        case http1_client_response_parse_error::unsupported_transfer_encoding:
            return "unsupported response transfer coding";
        case http1_client_response_parse_error::content_length_and_transfer_encoding:
            return "response has both Content-Length and Transfer-Encoding";
        case http1_client_response_parse_error::invalid_protocol_switch:
            return "invalid Switching Protocols response";
    }
    return "invalid HTTP/1 response";
}

detail::http1_client_request_content_phase
http1_client_response_parser::initial_request_content_phase(
    const http1_client_exchange_state& state_value) noexcept {
    switch (detail::http1_client_exchange_state_access::content_state(state_value)) {
        case detail::http1_client_initial_content_state::complete:
            return detail::http1_client_request_content_phase::content_complete;
        case detail::http1_client_initial_content_state::pending:
            return detail::http1_client_request_content_phase::content_pending;
        case detail::http1_client_initial_content_state::awaiting_continue:
            return detail::http1_client_request_content_phase::awaiting_continue;
    }
    return detail::http1_client_request_content_phase::content_complete;
}

http1_client_response_parser::http1_client_response_parser(
    http1_client_exchange_state exchange_state) noexcept
    : exchange_state_(std::move(exchange_state)),
      request_content_phase_(initial_request_content_phase(exchange_state_)) {}

http1_client_response_parse_result http1_client_response_parser::parse(std::string_view buffer) {
    if (phase_ != phase_type::await_response) {
        throw std::logic_error("HTTP/1 client response parser is already terminal");
    }
    const auto fail = [this](http1_client_response_parse_error error) noexcept {
        phase_ = phase_type::failed;
        return detail::http1_client_response_parse_result_access::failure(error);
    };

    // The status line is validated once its line feed arrives, so a non-HTTP
    // peer or a bare LF fails without waiting for the whole head. Bytes before
    // header_scan_offset_ held no line feed in an earlier call.
    if (!status_line_) {
        const auto line_feed = buffer.substr(0, max_http_header_bytes).find('\n', header_scan_offset_);
        if (line_feed != std::string_view::npos) {
            if (line_feed == 0 || buffer[line_feed - 1] != '\r') {
                return fail(http1_client_response_parse_error::invalid_status_line);
            }
            const auto status_line = detail::parse_http1_client_status_line(buffer.substr(0, line_feed - 1));
            if (status_line.index() != 0) {
                return fail(std::get<1>(status_line));
            }
            status_line_ = std::get<0>(status_line);
            status_line_end_ = line_feed - 1;
        }
    }

    const auto header_bytes = detail::find_http_header_end(buffer, header_scan_offset_);
    if (header_bytes == std::string_view::npos) {
        // A field line ending in a bare LF can never complete the
        // CRLF-delimited head (RFC 9112 section 2.2).
        if (status_line_ &&
            detail::has_http_bare_line_feed(buffer, std::max(status_line_end_ + 2, header_scan_offset_))) {
            return fail(http1_client_response_parse_error::invalid_header);
        }
        if (buffer.size() >= max_http_header_bytes) {
            return fail(http1_client_response_parse_error::header_too_large);
        }
        header_scan_offset_ = buffer.size() > 3 ? buffer.size() - 3 : 0;
        return detail::http1_client_response_parse_result_access::need_more();
    }
    // A complete head holds a line feed, so its status line was validated
    // above unless the input broke the same-prefix contract of parse().
    // A retry after an allocation failure rescans from the start.
    if (!status_line_) {
        throw std::logic_error("HTTP/1 client response input does not extend the previous prefix");
    }
    const auto status_line = *status_line_;
    const auto fields_begin = status_line_end_ + 2;
    header_scan_offset_ = 0;
    status_line_.reset();
    status_line_end_ = 0;

    // Exclude only the final empty line: every field keeps its CRLF, matching
    // the request scanner even when the head contains no fields.
    auto field_section = buffer.substr(fields_begin, header_bytes - 2 - fields_begin);
    std::optional<std::string> unfolded_fields;
    // The status line is not a field: its following whitespace cannot be
    // folded into the reason phrase. Only normalize within the field block.
    if (const auto first_fold = detail::find_field_line_fold(field_section);
        first_fold != std::string_view::npos) {
        unfolded_fields.emplace();
        detail::copy_unfolded_field_lines(
            field_section, first_fold, detail::field_line_folding_policy::http, *unfolded_fields);
        field_section = *unfolded_fields;
    }
    auto parsed_head = detail::parse_http1_client_response_head_fields(
        status_line, field_section, exchange_state_);
    if ((parsed_head.index() != 0)) {
        return fail(std::get<1>(parsed_head));
    }
    const auto& parsed_value = std::get<0>(parsed_head);

    auto planning = detail::plan_http1_client_response(
        exchange_state_, parsed_value, request_content_phase_, interim_faulty_framing_);
    if ((planning.index() != 0)) {
        return fail(std::get<1>(planning));
    }
    auto plan = std::move(std::get<0>(planning));

    // No owning response head is observable until the entire head and framing
    // plan have validated. Field views borrow the input or unfolded_head until
    // they are copied below; neither failure nor allocation commits the exchange.
    auto head = detail::http_client_response_head_access::make(
        parsed_value.received_status_, parsed_value.protocol_version_);
    detail::http_client_response_head_access::content_length(head, parsed_value.content_length_.value());
    const auto parsed_headers = parsed_value.headers_.view();
    if (!parsed_headers.empty()) {
        auto& headers = detail::http_client_response_head_access::headers(head);
        headers.reserve(parsed_headers.size());
        for (const auto& header_value : parsed_headers) {
            headers.emplace_back(header_value.name(), header_value.value());
        }
    }

    auto result_value = detail::http1_client_response_parse_result_access::parsed(
        std::move(head), std::move(plan), header_bytes);
    if (parsed_value.status_code_.is_informational() &&
        parsed_value.protocol_version_ == http_protocol_version::http10 &&
        parsed_value.saw_transfer_encoding_) {
        interim_faulty_framing_ = true;
    }
    if (parsed_value.status_code_ == http_status::continue_value) {
        request_content_phase_ = receive_continue(request_content_phase_);
    }
    if (parsed_value.status_code_ == http_status::switching_protocols || parsed_value.status_code_.is_final()) {
        phase_ = phase_type::complete;
    }
    return result_value;
}

void http1_client_response_parser::release_request_content() noexcept {
    if (phase_ == phase_type::await_response &&
        request_content_phase_ == detail::http1_client_request_content_phase::awaiting_continue) {
        request_content_phase_ =
            detail::http1_client_request_content_phase::content_released_awaiting_continue;
    }
}

http1_client_request_content_completion_status
http1_client_response_parser::complete_request_content() noexcept {
    if (phase_ != phase_type::await_response) {
        return http1_client_request_content_completion_status::exchange_terminal;
    }
    switch (request_content_phase_) {
        case detail::http1_client_request_content_phase::content_pending:
            request_content_phase_ = detail::http1_client_request_content_phase::content_complete;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::awaiting_continue:
        case detail::http1_client_request_content_phase::content_released_awaiting_continue:
            request_content_phase_ =
                detail::http1_client_request_content_phase::content_complete_awaiting_continue;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::continue_received:
            request_content_phase_ =
                detail::http1_client_request_content_phase::continue_received_content_complete;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::content_complete:
        case detail::http1_client_request_content_phase::content_complete_awaiting_continue:
        case detail::http1_client_request_content_phase::continue_received_content_complete:
            return http1_client_request_content_completion_status::already_complete;
    }
    return http1_client_request_content_completion_status::already_complete;
}

}  // namespace ruvia

namespace ruvia::detail {

namespace {

[[nodiscard]] bool request_offers_protocol(
    const http1_client_exchange_state& exchange_state, const http_upgrade_protocol& selected) noexcept {
    const auto offered_protocols =
        http1_client_exchange_state_access::offered_upgrade_protocols(exchange_state);
    if (offered_protocols.empty()) {
        return false;
    }
    bool offered = false;
    http_upgrade_protocols protocols;
    // The writer already validated this offer. Forwarded Upgrade fields may
    // retain empty members (RFC 9110 sections 2.2 and 5.6.1); selecting a
    // response protocol must not reapply generation grammar to those bytes.
    if (protocols.parse_field(offered_protocols, http_field_list_role::recipient,
            [&selected, &offered](const http_upgrade_protocol& candidate_value) noexcept {
                offered = offered || http_upgrade_protocol_equals(candidate_value, selected);
                return true;
            }) != http_field_list_parse_status::ok) {
        return false;
    }
    return protocols.has_protocol() && offered;
}

}  // namespace

http1_client_status_line_parse_result_type parse_http1_client_status_line(std::string_view status_line) noexcept {
    const auto separator = status_line.find(' ');
    if (separator == std::string_view::npos) {
        return http1_client_response_parse_error::invalid_status_line;
    }

    const auto version = parse_http1_version(status_line.substr(0, separator));
    if (version == http1_version_parse_result::invalid || version == http1_version_parse_result::unsupported) {
        return http1_client_response_parse_error::unsupported_http_version;
    }

    // status-line = HTTP-version SP status-code SP [ reason-phrase ]. The
    // separator after the three digits is mandatory even for an empty phrase.
    if (status_line.size() < separator + 5 || status_line[separator + 4] != ' ') {
        return http1_client_response_parse_error::invalid_status_code;
    }

    // RFC 9110 section 15: a received 000..099 or 600..999 is processed as a
    // 5xx response.
    const auto parsed_status = http_received_status::try_parse(status_line.substr(separator + 1, 3));
    if (!parsed_status) {
        return http1_client_response_parse_error::invalid_status_code;
    }

    // Unlike a field value, reason-phrase can legitimately begin or end with
    // SP/HTAB. Validate bytes directly instead of applying OWS trimming rules.
    if (!is_valid_http_field_value_bytes(status_line.substr(separator + 5))) {
        return http1_client_response_parse_error::invalid_reason_phrase;
    }

    return http1_client_parsed_status_line{.received_status_ = *parsed_status,
        .protocol_version_ = version == http1_version_parse_result::http10 ? http_protocol_version::http10 : http_protocol_version::http11};
}

http1_client_parsed_response_head::http1_client_parsed_response_head(
    const http1_client_parsed_status_line& status_line) noexcept
    : received_status_(status_line.received_status_),
      status_code_(status_line.received_status_.status()),
      protocol_version_(status_line.protocol_version_) {}

http1_client_response_head_parse_result_type parse_http1_client_response_head_fields(
    const http1_client_parsed_status_line& status_line, std::string_view field_section,
    const http1_client_exchange_state& exchange_state) {
    http1_client_parsed_response_head output(status_line);

    const auto content_semantics = classify_http_response_content_semantics(
        http1_client_exchange_state_access::method(exchange_state), output.status_code_);
    // RFC 9112 section 6.3 rule 1 ends 1xx, 204, 304 and HEAD responses at the
    // header section whatever framing fields they carry, and rule 1 precedes
    // the rule 5 Content-Length check. A recipient therefore ignores their
    // framing fields, even invalid ones, rather than rejecting a message it can
    // still delimit exactly; HEAD and 304 Content-Length remains a retained
    // field describing the selected representation.
    const bool framing_fields_apply = content_semantics == http_response_content_semantics::with_content;
    bool parse_length_metadata = content_semantics == http_response_content_semantics::without_content &&
                                 output.status_code_ != http_status::no_content &&
                                 (http1_client_exchange_state_access::method(exchange_state) == http_known_method::head ||
                                     output.status_code_ == http_status::not_modified);

    std::size_t cursor_value = 0;
    while (cursor_value < field_section.size()) {
        // The field-count rule of http_field_section_budget.
        if (output.headers_.size() == max_http_header_fields) {
            return http1_client_response_parse_error::header_too_large;
        }
        const auto field = scan_http1_field_line(field_section, cursor_value);
        if (!field) {
            return http1_client_response_parse_error::invalid_header;
        }
        const auto name = field->name();
        const auto value = field->value();
        const auto kind = classify_http_field(name);
        output.headers_.append(http_header_view{name, value});

        switch (kind) {
            case http_field_kind::content_length:
                // RFC 9112 section 6.3 rule 1: 1xx (including 101), 204, 304,
                // and HEAD responses end at the first empty line regardless of
                // framing fields; successful CONNECT becomes a tunnel.
                if (framing_fields_apply || parse_length_metadata) {
                    if (const auto error = parse_http1_content_length_field<http1_client_response_parse_error>(value, output.content_length_)) {
                        if (framing_fields_apply) {
                            return *error;
                        }
                        // 无正文优先级不变；不一致的表示元数据只是不再提供已知长度。
                        output.content_length_ = {};
                        parse_length_metadata = false;
                    }
                }
                break;
            case http_field_kind::connection:
                if (output.connection_options_.parse_field(value, http_connection_field_role::response_recipient) !=
                    http_field_list_parse_status::ok) {
                    return http1_client_response_parse_error::invalid_connection;
                }
                break;
            case http_field_kind::transfer_encoding:
                output.saw_transfer_encoding_ = true;
                // Only a response with content is framed by Transfer-Encoding;
                // successful CONNECT is ignored by client-side rule.
                if (framing_fields_apply && output.protocol_version_ == http_protocol_version::http11) {
                    if (const auto error = parse_http1_transfer_encoding_field<http1_client_response_parse_error>(value, output.transfer_encoding_)) {
                        return *error;
                    }
                }
                break;
            case http_field_kind::upgrade:
                if (output.upgrade_protocols_.parse_field(value, http_field_list_role::recipient,
                        [&exchange_state, &output,
                            check_offered = content_semantics ==
                                            http_response_content_semantics::protocol_switch](
                            const http_upgrade_protocol& selected) noexcept {
                            if (check_offered && output.all_upgrade_protocols_offered_ &&
                                !request_offers_protocol(exchange_state, selected)) {
                                output.all_upgrade_protocols_offered_ = false;
                            }
                            return true;
                        }) != http_field_list_parse_status::ok) {
                    return http1_client_response_parse_error::invalid_upgrade;
                }
                break;
            default:
                break;
        }
    }

    if (output.transfer_encoding_.unsupported()) {
        return http1_client_response_parse_error::unsupported_transfer_encoding;
    }
    // RFC 9112 section 6.1: an HTTP/1.0 message carrying Transfer-Encoding is
    // treated as if its framing were faulty and the connection closes after it.
    // The transfer codings stay unparsed; the plan ignores Transfer-Encoding and
    // falls back to Content-Length or close delimiting.
    return output;
}

}  // namespace ruvia::detail

namespace ruvia {

std::optional<std::string_view> http_client_response_head::header(
    std::string_view name) const& noexcept {
    return detail::find_last_http_header<http_header>(headers_, name);
}

}  // namespace ruvia

// Whether a parsed response head lets the exchange continue: may this 101 switch
// protocols given what the request offered, does the body have a length, and
// does the connection survive the response.

namespace ruvia::detail {

namespace {

[[nodiscard]] bool request_allows_protocol_switch(const http1_client_exchange_state& exchange_state,
    const http1_client_parsed_response_head& response,
    http1_client_request_content_phase request_content_phase,
    bool interim_faulty_framing) noexcept {
    const bool request_content_allows_switch =
        request_content_phase == http1_client_request_content_phase::content_complete ||
        request_content_phase == http1_client_request_content_phase::content_complete_awaiting_continue ||
        request_content_phase == http1_client_request_content_phase::continue_received_content_complete;
    if (interim_faulty_framing ||
        http1_client_exchange_state_access::close_policy(exchange_state) ==
            http1_close_policy::close_after_response ||
        !request_content_allows_switch) {
        return false;
    }
    if (detail::http1_client_exchange_state_access::offered_upgrade_protocols(exchange_state).empty() ||
        response.connection_options_.close() || !response.connection_options_.upgrade() ||
        !response.upgrade_protocols_.has_protocol()) {
        return false;
    }
    return response.all_upgrade_protocols_offered_;
}

[[nodiscard]] std::optional<http_client_request_content_signal> request_content_signal(
    http1_client_request_content_phase phase, http_status_code status_code,
    bool response_will_close) noexcept {
    if (response_will_close && (phase == http1_client_request_content_phase::awaiting_continue ||
                                   phase == http1_client_request_content_phase::content_pending ||
                                   phase == http1_client_request_content_phase::content_released_awaiting_continue ||
                                   phase == http1_client_request_content_phase::continue_received)) {
        return http_client_request_content_signal::exchange_complete;
    }
    if (status_code == http_status::continue_value) {
        return phase == http1_client_request_content_phase::awaiting_continue
                   ? std::optional<http_client_request_content_signal>(
                         http_client_request_content_signal::continue_value)
                   : std::nullopt;
    }
    if (status_code.is_final() && phase == http1_client_request_content_phase::awaiting_continue) {
        // A final response cancels content only while Expect still gates it.
        // After 100 Continue or a finite wait releases the writer, RFC 9110
        // section 7.5 says to keep sending unless the server explicitly stops
        // it. Closing responses provide that signal and are handled above.
        return http_client_request_content_signal::exchange_complete;
    }
    return std::nullopt;
}

[[nodiscard]] http1_close_policy response_persistence(
    const http1_client_parsed_response_head& response) noexcept {
    // RFC 9112 section 6.1: an HTTP/1.0 message carrying Transfer-Encoding
    // has faulty framing and the connection closes after it, even when the
    // response ends at its header section and the field is otherwise ignored.
    if (response.connection_options_.close() ||
        (response.protocol_version_ == http_protocol_version::http10 && response.saw_transfer_encoding_)) {
        return http1_close_policy::close_after_response;
    }
    if (response.protocol_version_ == http_protocol_version::http11 ||
        response.connection_options_.keep_alive()) {
        return http1_close_policy::allow_reuse;
    }
    return http1_close_policy::close_after_response;
}

[[nodiscard]] http1_close_policy final_response_persistence(
    const http1_client_exchange_state& exchange_state,
    const http1_client_parsed_response_head& response,
    http1_client_request_content_phase request_content_phase,
    bool interim_faulty_framing) noexcept {
    // A final response while Expect still gates the content cancels content
    // whose framing the request head already committed. The server may keep
    // reading it (RFC 9110 section 10.1.1), so the next request on this
    // connection would be consumed as that content.
    // A rejected CONNECT is not a reusable HTTP exchange: RFC 9931 section 8
    // requires the proxy to close it before interpreting any following request.
    if (interim_faulty_framing ||
        http1_client_exchange_state_access::method(exchange_state) == http_known_method::connect ||
        http1_client_exchange_state_access::close_policy(exchange_state) ==
            http1_close_policy::close_after_response ||
        request_content_phase == http1_client_request_content_phase::awaiting_continue) {
        return http1_close_policy::close_after_response;
    }
    return response_persistence(response);
}

}  // namespace

struct http1_client_response_plan_access final {
    using request_content_signal_type = std::optional<http_client_request_content_signal>;

    [[nodiscard]] static http1_client_response_plan informational(
        http1_close_policy persistence, request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_informational_response(persistence)),
            request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan without_content(
        http1_close_policy persistence, request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_response_without_content(persistence)),
            request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan known_length(std::size_t content_length,
        http1_close_policy persistence, request_content_signal_type request_content_signal,
        bool discards_content) noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(
                http1_client_known_length_response(content_length, persistence)),
            request_content_signal, discards_content);
    }

    [[nodiscard]] static http1_client_response_plan chunked(http_transfer_codings transfer_codings,
        http1_close_policy persistence, request_content_signal_type request_content_signal,
        bool discards_content) {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_chunked_response(
                                              std::move(transfer_codings), persistence)),
            request_content_signal, discards_content);
    }

    [[nodiscard]] static http1_client_response_plan close_delimited(
        http_transfer_codings transfer_codings, request_content_signal_type request_content_signal,
        bool discards_content) {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_close_delimited_response(
                std::move(transfer_codings))),
            request_content_signal, discards_content);
    }

    [[nodiscard]] static http1_client_response_plan connect_tunnel() noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_connect_tunnel()), std::nullopt);
    }

    [[nodiscard]] static http1_client_response_plan protocol_upgrade() noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_protocol_upgrade()), std::nullopt);
    }
};

http1_client_response_planning_result_type plan_http1_client_response(
    const http1_client_exchange_state& exchange_state, const http1_client_parsed_response_head& response,
    http1_client_request_content_phase request_content_phase, bool interim_faulty_framing) {
    const auto content_semantics = classify_http_response_content_semantics(
        http1_client_exchange_state_access::method(exchange_state), response.status_code_);

    if (content_semantics == http_response_content_semantics::protocol_switch) {
        if (response.protocol_version_ != http_protocol_version::http11 ||
            !request_allows_protocol_switch(
                exchange_state, response, request_content_phase, interim_faulty_framing)) {
            return http1_client_response_parse_error::invalid_protocol_switch;
        }
        return http1_client_response_plan_access::protocol_upgrade();
    }
    if (content_semantics == http_response_content_semantics::informational) {
        // A 1xx never ends the exchange or cancels request content, so it cannot
        // close the connection either; the final response alone decides that
        // (RFC 9110 Section 15.2).
        return http1_client_response_plan_access::informational(http1_close_policy::allow_reuse,
            request_content_signal(request_content_phase, response.status_code_, false));
    }
    if (content_semantics == http_response_content_semantics::connect_tunnel) {
        return http1_client_response_plan_access::connect_tunnel();
    }

    // RFC 9110 section 15.3.6 forbids only generating 205 content. Its framing
    // is still authoritative, so delimited content is consumed and discarded
    // and the connection stays reusable.
    const bool discards_content = response.status_code_ == http_status::reset_content &&
                                  content_semantics == http_response_content_semantics::with_content;
    const auto content_length = response.content_length_.value();

    const auto persistence = final_response_persistence(
        exchange_state, response, request_content_phase, interim_faulty_framing);
    const auto persistent_content_signal = request_content_signal(request_content_phase,
        response.status_code_, persistence == http1_close_policy::close_after_response);
    if (content_semantics == http_response_content_semantics::without_content) {
        return http1_client_response_plan_access::without_content(persistence, persistent_content_signal);
    }

    const auto& transfer_encoding = response.transfer_encoding_.value();
    // RFC 9112 section 6.1: HTTP/1.0 Transfer-Encoding is faulty framing, so the
    // codings are ignored and the message falls back to Content-Length or close
    // delimiting while the connection closes after it.
    const bool http10_faulty_framing = response.protocol_version_ == http_protocol_version::http10 &&
                                       response.saw_transfer_encoding_;
    if (response.saw_transfer_encoding_ && !http10_faulty_framing) {
        if (content_length.has_value()) {
            return http1_client_response_parse_error::content_length_and_transfer_encoding;
        }
        if (!transfer_encoding.has_value()) {
            return http1_client_response_parse_error::invalid_transfer_encoding;
        }
        if (const auto* final_chunked = transfer_encoding->final_chunked()) {
            return http1_client_response_plan_access::chunked(
                final_chunked->transfer_codings(), persistence, persistent_content_signal,
                discards_content);
        }
        return http1_client_response_plan_access::close_delimited(
            transfer_encoding->non_chunked()->transfer_codings(),
            request_content_signal(request_content_phase, response.status_code_, true),
            discards_content);
    }

    if (content_length.has_value()) {
        if (*content_length > (std::numeric_limits<std::size_t>::max)()) {
            return http1_client_response_parse_error::invalid_content_length;
        }
        return http1_client_response_plan_access::known_length(
            static_cast<std::size_t>(*content_length), persistence, persistent_content_signal,
            discards_content);
    }

    // RFC 9112 section 6.3: a body-allowed response with no declared
    // length is delimited by server close and cannot return to a pool.
    return http1_client_response_plan_access::close_delimited(
        http_transfer_codings{},
        request_content_signal(request_content_phase, response.status_code_, true),
        discards_content);
}

}  // namespace ruvia::detail

// Media-type syntax (RFC 9110 section 8.3.1): splitting a field value into
// type/subtype and parameters, comparing parameter values across token and
// quoted-string forms, and validating a Content-Type field. Matching a media
// type against an Accept media-range lives in http_accept_match.cpp.

namespace ruvia::detail {

struct http_media_type_parts final {
    std::string_view type_;
    std::string_view subtype_;
};

// Both values have already passed parameter syntax validation. Compare their
// decoded bytes without materializing quoted strings.
[[nodiscard]] inline bool http_media_parameter_value_equals(
    std::string_view left, std::string_view right, bool ascii_case_insensitive = false) noexcept {
    if (left.front() != '"' && right.front() != '"') {
        return ascii_case_insensitive ? http_ascii_equals_ignore_case(left, right) : left == right;
    }
    struct cursor final {
        std::string_view value_;
        http_quoted_string_cursor quoted_cursor_;
        std::size_t position_{0};

        explicit cursor(std::string_view value) noexcept
            : value_(value),
              quoted_cursor_(value) {}

        [[nodiscard]] bool next(unsigned char& out) noexcept {
            if (value_.front() == '"') {
                return quoted_cursor_.next(out);
            }
            if (position_ == value_.size()) {
                return false;
            }
            out = static_cast<unsigned char>(value_[position_++]);
            return true;
        }
    };
    cursor lhs(left);
    cursor rhs(right);
    while (true) {
        unsigned char lhs_char = 0;
        unsigned char rhs_char = 0;
        const bool has_left = lhs.next(lhs_char);
        const bool has_right = rhs.next(rhs_char);
        if (has_left != has_right) {
            return false;
        }
        if (!has_left) {
            return true;
        }
        if (ascii_case_insensitive) {
            lhs_char = http_ascii_to_lower(lhs_char);
            rhs_char = http_ascii_to_lower(rhs_char);
        }
        if (lhs_char != rhs_char) {
            return false;
        }
    }
}

// Parameters are cached on the stack. A media type or media range with more
// than max_parameters parameters is invalid, which bounds the per-member
// duplicate-name check. Views stay valid only while the parsed field is
// unchanged; parsing and matching never allocate.
struct http_media_type_parameters final {
    static constexpr std::size_t max_parameters = 64;

    struct parameter final {
        const char* name_data_;
        std::size_t name_size_;
        const char* value_data_;
        std::size_t value_size_;

        [[nodiscard]] std::string_view name() const noexcept {
            return {name_data_, name_size_};
        }

        [[nodiscard]] std::string_view value() const noexcept {
            return {value_data_, value_size_};
        }
    };
    // Only the first size_ slots are initialized and read. Plain pointer/length
    // slots avoid constructing string_views for unused slots.
    std::array<parameter, max_parameters> values_;
    std::size_t size_{0};

    template <typename visitor_type>
    [[nodiscard]] bool visit(visitor_type&& visitor) const {
        for (std::size_t i = 0; i < size_; ++i) {
            if (!visitor(values_[i].name(), values_[i].value())) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::optional<std::string_view> find(std::string_view name) const noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            if (http_ascii_equals_ignore_case(values_[i].name(), name)) {
                return values_[i].value();
            }
        }
        return std::nullopt;
    }

    // A quality output selects Accept semantics: q is parsed as a weight,
    // including an invalid weight's zero quality, rather than a media parameter.
    [[nodiscard]] bool parse(std::string_view value, int* quality = nullptr) noexcept {
        size_ = 0;
        if (quality != nullptr) {
            *quality = 1000;
        }
        return http_all_parameters(value, [&](std::string_view part) noexcept {
            if (part.empty()) {
                return true;
            }
            const auto equals = part.find('=');
            if (equals == std::string_view::npos || size_ == values_.size()) {
                return false;
            }
            const auto name = part.substr(0, equals);
            const auto parameter_value = part.substr(equals + 1);
            if (!is_valid_http_header_name(name) || parameter_value.empty() || parameter_value != http_trim_ows(parameter_value)) {
                return false;
            }
            if (find(name).has_value()) {
                return false;
            }
            if (quality != nullptr && http_ascii_equals_ignore_case(name, "q")) {
                const auto parsed = http_parse_quality_value(parameter_value);
                *quality = parsed < 0 ? 0 : parsed;
            } else if (!is_http_token_or_quoted_string(parameter_value)) {
                return false;
            }
            values_[size_++] = {name.data(), name.size(), parameter_value.data(), parameter_value.size()};
            return true;
        });
    }

    [[nodiscard]] bool contains(std::string_view name, std::string_view value,
        const http_media_type_parts& parts) const noexcept {
        const auto parameter_value = find(name);
        if (!parameter_value.has_value()) {
            return false;
        }
        // RFC 3676 section 4 defines these case-insensitive values only for text/plain.
        const bool ascii_case_insensitive = http_ascii_equals_ignore_case(name, "charset") ||
                                            ((http_ascii_equals_ignore_case(name, "format") ||
                                                 http_ascii_equals_ignore_case(name, "delsp")) &&
                                                http_ascii_equals_ignore_case(parts.type_, "text") &&
                                                http_ascii_equals_ignore_case(parts.subtype_, "plain"));
        return http_media_parameter_value_equals(*parameter_value, value, ascii_case_insensitive);
    }
};

[[nodiscard]] inline bool http_parse_media_type_parts(
    std::string_view value, bool allow_wildcard, http_media_type_parts& parts) noexcept {
    value = http_header_token_before_parameters(value);
    const auto slash = value.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }

    parts.type_ = value.substr(0, slash);
    parts.subtype_ = value.substr(slash + 1);
    const bool type_wildcard = parts.type_ == "*";
    const bool subtype_wildcard = parts.subtype_ == "*";
    if ((type_wildcard || subtype_wildcard) && !allow_wildcard) {
        return false;
    }
    if (type_wildcard && !subtype_wildcard) {
        return false;
    }
    return (type_wildcard || is_valid_http_header_name(parts.type_)) &&
           (subtype_wildcard || is_valid_http_header_name(parts.subtype_));
}

template <http_temporary_owning_char_string value_type>
bool http_parse_media_type_parts(value_type&&, bool, http_media_type_parts&) = delete;

[[nodiscard]] inline bool http_parse_media_type(std::string_view value, http_media_type_parts& parts,
    http_media_type_parameters& parameters) noexcept {
    return http_parse_media_type_parts(value, false, parts) && parameters.parse(value);
}

template <http_temporary_owning_char_string value_type>
bool http_parse_media_type(value_type&&, http_media_type_parts&, http_media_type_parameters&) = delete;

[[nodiscard]] inline bool is_valid_http_content_type_field_value(std::string_view value) noexcept {
    http_media_type_parts parts;
    http_media_type_parameters parameters;
    return http_parse_media_type(value, parts, parameters);
}

template <http_temporary_owning_char_string value_type>
bool is_valid_http_content_type_field_value(value_type&&) = delete;

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

struct http_field_definition final {
    std::string_view name_;
    http_field_kind kind_;
    std::uint32_t response_bit_;
};

// One spelling authority for both directions. Length-indexed ranges keep
// classification bounded to the names of the same size without allocation.
constexpr http_field_definition http_field_definitions[] = {
    {"te", http_field_kind::te, 0},
    {"date", http_field_kind::other, response_header_date},
    {"etag", http_field_kind::other, response_header_etag},
    {"host", http_field_kind::host, 0},
    {"vary", http_field_kind::other, response_header_vary},
    {"allow", http_field_kind::other, response_header_allow},
    {"range", http_field_kind::range, 0},
    {"accept", http_field_kind::accept, 0},
    {"cookie", http_field_kind::cookie, 0},
    {"expect", http_field_kind::expect, 0},
    {"origin", http_field_kind::origin, 0},
    {"server", http_field_kind::other, response_header_server},
    {"trailer", http_field_kind::trailer, response_header_trailer},
    {"upgrade", http_field_kind::upgrade, response_header_upgrade},
    {"if-match", http_field_kind::if_match, 0},
    {"if-range", http_field_kind::if_range, 0},
    {"location", http_field_kind::other, response_header_location},
    {"forwarded", http_field_kind::forwarded, 0},
    {"connection", http_field_kind::connection, response_header_connection},
    {"keep-alive", http_field_kind::other, response_header_keep_alive},
    {"set-cookie", http_field_kind::other, response_header_set_cookie},
    {"user-agent", http_field_kind::user_agent, 0},
    {"content-type", http_field_kind::content_type, response_header_content_type},
    {"accept-ranges", http_field_kind::other, response_header_accept_ranges},
    {"authorization", http_field_kind::authorization, 0},
    {"cache-control", http_field_kind::other, response_header_cache_control},
    {"content-range", http_field_kind::other, response_header_content_range},
    {"if-none-match", http_field_kind::if_none_match, 0},
    {"last-modified", http_field_kind::other, response_header_last_modified},
    {"content-length", http_field_kind::content_length, response_header_content_length},
    {"accept-encoding", http_field_kind::accept_encoding, 0},
    {"x-forwarded-for", http_field_kind::x_forwarded_for, 0},
    {"content-encoding", http_field_kind::content_encoding, response_header_content_encoding},
    {"proxy-connection", http_field_kind::other, response_header_proxy_connection},
    {"if-modified-since", http_field_kind::if_modified_since, 0},
    {"sec-websocket-key", http_field_kind::sec_websocket_key, 0},
    {"transfer-encoding", http_field_kind::transfer_encoding, response_header_transfer_encoding},
    {"x-forwarded-proto", http_field_kind::x_forwarded_proto, 0},
    {"if-unmodified-since", http_field_kind::if_unmodified_since, 0},
    {"proxy-authorization", http_field_kind::proxy_authorization, 0},
    {"sec-websocket-version", http_field_kind::sec_websocket_version, 0},
    {"access-control-max-age", http_field_kind::other, response_header_access_control_max_age},
    {"sec-websocket-protocol", http_field_kind::sec_websocket_protocol, 0},
    {"sec-websocket-extensions", http_field_kind::sec_websocket_extensions, 0},
    {"access-control-allow-origin", http_field_kind::other, response_header_access_control_allow_origin},
    {"access-control-allow-headers", http_field_kind::other, response_header_access_control_allow_headers},
    {"access-control-allow-methods", http_field_kind::other, response_header_access_control_allow_methods},
    {"access-control-expose-headers", http_field_kind::other, response_header_access_control_expose_headers},
    {"access-control-request-method", http_field_kind::access_control_request_method, 0},
    {"access-control-request-headers", http_field_kind::access_control_request_headers, 0},
    {"access-control-allow-credentials", http_field_kind::other, response_header_access_control_allow_credentials},
};

static_assert([] {
    for (std::size_t i = 1; i < std::size(http_field_definitions); ++i) {
        if (http_field_definitions[i - 1].name_.size() > http_field_definitions[i].name_.size()) {
            return false;
        }
    }
    return true;
}());

constexpr auto http_field_length_offsets = [] {
    std::array<std::size_t, http_field_definitions[std::size(http_field_definitions) - 1].name_.size() + 2> offsets{};
    std::size_t index = 0;
    for (std::size_t length = 0; length < offsets.size(); ++length) {
        while (index < std::size(http_field_definitions) && http_field_definitions[index].name_.size() < length) {
            ++index;
        }
        offsets[length] = index;
    }
    return offsets;
}();

constexpr auto response_field_names = [] {
    std::array<std::string_view, std::bit_width(response_header_proxy_connection)> names{};
    for (const auto& field : http_field_definitions) {
        if (field.response_bit_ != 0) {
            names[std::countr_zero(field.response_bit_)] = field.name_;
        }
    }
    return names;
}();

}  // namespace

http_field_classification classify_http_field_name(std::string_view name) noexcept {
    if (name.empty() || name.size() >= http_field_length_offsets.size() - 1) {
        return {};
    }
    const auto first = http_ascii_to_lower(static_cast<unsigned char>(name.front()));
    const auto end = http_field_length_offsets[name.size() + 1];
    for (auto index = http_field_length_offsets[name.size()]; index < end; ++index) {
        const auto& field = http_field_definitions[index];
        if (first == static_cast<unsigned char>(field.name_.front()) && http_ascii_equals_ignore_case(name, field.name_)) {
            return {field.kind_, field.response_bit_};
        }
    }
    return {};
}

http_field_kind classify_http_field(std::string_view name) noexcept {
    return classify_http_field_name(name).kind_;
}

std::uint32_t classify_response_header_name(std::string_view name) noexcept {
    return classify_http_field_name(name).response_bit_;
}

std::string_view response_known_header_lowercase_name(std::uint32_t bit) noexcept {
    if (!std::has_single_bit(bit)) {
        return {};
    }
    const auto index = static_cast<std::size_t>(std::countr_zero(bit));
    return index < response_field_names.size() ? response_field_names[index] : std::string_view{};
}

}  // namespace ruvia::detail

// The method vocabulary: recognising the standard tokens, spelling them back,
// validating an unknown one, and the two properties -- safe and idempotent --
// that decide what a recipient may do with a request (RFC 9110 section 9.2).

namespace ruvia {

http_known_method classify_http_method(std::string_view method) noexcept {
    switch (method.size()) {
        case 3:
            if (method == "GET") {
                return http_known_method::get;
            }
            if (method == "PUT") {
                return http_known_method::put;
            }
            break;
        case 4:
            if (method == "POST") {
                return http_known_method::post;
            }
            if (method == "HEAD") {
                return http_known_method::head;
            }
            break;
        case 5:
            if (method == "PATCH") {
                return http_known_method::patch;
            }
            break;
        case 6:
            if (method == "DELETE") {
                return http_known_method::delete_value;
            }
            break;
        case 7:
            if (method == "OPTIONS") {
                return http_known_method::options;
            }
            if (method == "CONNECT") {
                return http_known_method::connect;
            }
            break;
        default:
            break;
    }
    return http_known_method::unknown;
}

std::string_view known_http_method_token(http_known_method method) noexcept {
    switch (method) {
        case http_known_method::get:
            return "GET";
        case http_known_method::post:
            return "POST";
        case http_known_method::put:
            return "PUT";
        case http_known_method::delete_value:
            return "DELETE";
        case http_known_method::patch:
            return "PATCH";
        case http_known_method::head:
            return "HEAD";
        case http_known_method::options:
            return "OPTIONS";
        case http_known_method::connect:
            return "CONNECT";
        case http_known_method::unknown:
        default:
            return {};
    }
}

bool is_valid_http_method_token(std::string_view method) noexcept {
    return is_valid_http_header_name(method);
}

bool is_http_method_safe(std::string_view method) noexcept {
    return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "TRACE";
}

bool is_http_method_idempotent(std::string_view method) noexcept {
    return is_http_method_safe(method) || method == "PUT" || method == "DELETE";
}

}  // namespace ruvia

namespace ruvia {

bool is_http_hop_by_hop_field_name(std::string_view field_name) noexcept {
    switch (field_name.size()) {
        case 2U:
            return http_ascii_equals_ignore_case(field_name, "TE");
        case 7U:
            return http_ascii_equals_ignore_case(field_name, "Trailer") ||
                   http_ascii_equals_ignore_case(field_name, "Upgrade");
        case 10U:
            return http_ascii_equals_ignore_case(field_name, "Connection") ||
                   http_ascii_equals_ignore_case(field_name, "Keep-Alive");
        case 16U:
            return http_ascii_equals_ignore_case(field_name, "Proxy-Connection");
        case 17U:
            return http_ascii_equals_ignore_case(field_name, "Transfer-Encoding");
        case 18U:
            return http_ascii_equals_ignore_case(field_name, "Proxy-Authenticate");
        case 19U:
            return http_ascii_equals_ignore_case(field_name, "Proxy-Authorization");
        default:
            return false;
    }
}

bool is_http_standard_connection_option(std::string_view option) noexcept {
    return (option.size() == 5U && http_ascii_equals_ignore_case(option, "close")) ||
           is_http_hop_by_hop_field_name(option);
}

void http_connection_nominated_fields::update(std::string_view connection_field_value) {
    detail::http_visit_comma_separated_tokens(connection_field_value, [&](std::string_view token) {
        if (!is_valid_http_header_name(token)) {
            return true;
        }
        if (http_ascii_equals_ignore_case(token, "close")) {
            has_close_ = true;
        } else if (http_ascii_equals_ignore_case(token, "keep-alive")) {
            has_keep_alive_ = true;
        } else if (http_ascii_equals_ignore_case(token, "Upgrade")) {
            has_upgrade_ = true;
        } else if (http_ascii_equals_ignore_case(token, "TE")) {
            has_te_ = true;
        }
        if (contains(token)) {
            return true;
        }
        std::string lower(token);
        for (auto& ch : lower) {
            ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
        }
        tokens_.push_back(std::move(lower));
        return true;
    });
}

void http_connection_nominated_fields::update_header_range(detail::http_header_range_view headers) {
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Connection")) {
            update(field.value());
        }
    }
}

bool http_connection_nominated_fields::contains(std::string_view field_name) const noexcept {
    return std::ranges::any_of(tokens_, [field_name](std::string_view token) noexcept {
        return http_ascii_equals_ignore_case(token, field_name);
    });
}

std::vector<std::string> http_connection_nominated_fields::non_standard_options() const {
    std::vector<std::string> result;
    for (const auto& token : tokens_) {
        if (!is_http_standard_connection_option(token)) {
            result.emplace_back(token);
        }
    }
    return result;
}

std::string http_connection_nominated_fields::rebuild_connection_field(
    bool retain_upgrade, bool include_non_standard) const {
    std::string output;
    if (retain_upgrade && has_upgrade_) {
        output.append("Upgrade");
    }
    if (include_non_standard) {
        for (const auto& token : tokens_) {
            if (is_http_standard_connection_option(token)) {
                continue;
            }
            if (!output.empty()) {
                output.append(", ");
            }
            output.append(token);
        }
    }
    return output;
}

http_connection_nominated_fields parse_http_connection_nominated_fields(
    std::string_view connection_field_value) {
    http_connection_nominated_fields nominated;
    nominated.update(connection_field_value);
    return nominated;
}
bool is_http_hop_by_hop_or_nominated_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated, bool retain_upgrade) noexcept {
    if (retain_upgrade && nominated.has_upgrade() &&
        http_ascii_equals_ignore_case(field_name, "Upgrade")) {
        return false;
    }
    return is_http_hop_by_hop_field_name(field_name) || nominated.contains(field_name);
}

bool is_http_hop_by_hop_trailer_field(
    std::string_view field_name, const http_connection_nominated_fields& nominated) noexcept {
    return is_http_hop_by_hop_field_name(field_name) || nominated.contains(field_name);
}

bool is_forbidden_http_request_trailer_field_name(std::string_view field_name) noexcept {
    return detail::is_forbidden_http_request_trailer_name(field_name);
}

bool is_forbidden_http_response_trailer_field_name(std::string_view field_name) noexcept {
    return detail::is_forbidden_response_trailer_name(field_name);
}
}  // namespace ruvia

namespace ruvia {

http_conditional_headers get_http_conditional_headers(const http_request& request) noexcept {
    return http_conditional_headers{
        detail::http_request_access::has_repeated_known_header(
            request, detail::http_field_kind::if_unmodified_since)
            ? std::string_view{}
            : detail::http_request_access::known_header(request, detail::http_field_kind::if_unmodified_since),
        detail::http_request_access::has_repeated_known_header(
            request, detail::http_field_kind::if_modified_since)
            ? std::string_view{}
            : detail::http_request_access::known_header(request, detail::http_field_kind::if_modified_since),
        detail::http_request_access::known_header(request, detail::http_field_kind::range),
        detail::http_request_access::known_header(request, detail::http_field_kind::if_range),
        detail::http_request_access::has_known_header(request, detail::http_field_kind::if_range)};
}

void http_etag_field_condition::update(
    std::string_view value, std::string_view expected, bool strong) noexcept {
    const bool already_present = present_;
    present_ = true;
    const auto trimmed = http_trim_ows(value);
    if (trimmed == "*") {
        wildcard_ = true;
        if (already_present) {
            valid_ = false;
        }
        return;
    }
    if (wildcard_) {
        valid_ = false;
    }
    const auto result_value = detail::http_parse_etag_list_matches(value, expected, strong);
    valid_ = valid_ && result_value.valid_;
    matched_ = matched_ || result_value.matched_;
}

http_etag_preconditions get_http_etag_preconditions(
    const http_request& request, std::string_view etag) noexcept {
    http_etag_preconditions result;
    // Both conditions are RFC list fields. Multiple field lines are equivalent
    // to comma-joining their values (RFC 9110 §5.3), but the request keeps
    // zero-copy views into separate wire lines. Fold them line by line and
    // retain whole-list validity without allocating a joined string.
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::if_match, [&result, etag](std::string_view value) noexcept {
            result.if_match_.update(value, etag, true);
            return true;
        });
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::if_none_match, [&result, etag](std::string_view value) noexcept {
            result.if_none_match_.update(value, etag, false);
            return true;
        });
    return result;
}

bool http_date_not_modified(std::string_view header_value, std::time_t modified_seconds) noexcept {
    const auto date = parse_http_date(header_value);
    return date.has_value() && modified_seconds <= *date;
}

bool http_date_unmodified(std::string_view header_value, std::time_t modified_seconds) noexcept {
    const auto date = parse_http_date(header_value);
    return !date.has_value() || modified_seconds <= *date;
}

bool http_if_range_allows(std::string_view header_value, std::string_view etag,
    std::optional<std::time_t> modified_seconds, bool date_validator_strong) noexcept {
    if (header_value.empty()) {
        return false;
    }
    const auto value = http_trim_ows(header_value);
    if (!value.empty() && (value.front() == '"' || value.starts_with("W/"))) {
        return detail::http_strong_etag_equals(value, etag);
    }
    if (!date_validator_strong || !modified_seconds) {
        return false;
    }
    const auto date = parse_http_date(value);
    return date.has_value() && *modified_seconds == *date;
}

bool is_valid_http_entity_tag(std::string_view value) noexcept {
    if (value.starts_with("W/")) {
        value.remove_prefix(2);
    }
    if (value.size() < 2U || value.front() != '"' || value.back() != '"') {
        return false;
    }
    for (std::size_t i = 1U; i + 1U < value.size(); ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte == '"' || (byte != 0x21U && !(byte >= 0x23U && byte <= 0x7eU) && byte < 0x80U)) {
            return false;
        }
    }
    return true;
}

bool is_weak_http_entity_tag(std::string_view value) noexcept {
    return value.starts_with("W/") && is_valid_http_entity_tag(value);
}

bool is_strong_http_entity_tag(std::string_view value) noexcept {
    return !value.starts_with("W/") && is_valid_http_entity_tag(value);
}

bool http_strong_etag_equals(std::string_view left, std::string_view right) noexcept {
    return is_strong_http_entity_tag(left) && is_strong_http_entity_tag(right) && left == right;
}

bool http_weak_etag_equals(std::string_view left, std::string_view right) noexcept {
    return is_valid_http_entity_tag(left) && is_valid_http_entity_tag(right) &&
           detail::http_weak_etag_equals(left, right);
}

std::string weaken_http_etag(std::string_view etag) {
    etag = http_trim_ows(etag);
    if (is_strong_http_entity_tag(etag)) {
        std::string weakened;
        weakened.reserve(2U + etag.size());
        weakened.append("W/");
        weakened.append(etag);
        return weakened;
    }
    return std::string(etag);
}
}  // namespace ruvia

namespace ruvia::detail {
namespace {

enum class http_accept_token_match : int {
    none = -1,
    wildcard = 0,
    prefix = 1,
    exact = 2,
};

// RFC 4647 section 3.3.1 basic filtering: a language range matches a tag that
// equals it or extends it at a subtag boundary, so "en" matches "en-US" but
// never "english". Only Accept-Language uses this; charsets match exactly or by
// wildcard.
[[nodiscard]] http_accept_token_match http_accept_token_matches(
    std::string_view range, std::string_view offered, bool prefix_matching) noexcept {
    if (range == "*") {
        return http_accept_token_match::wildcard;
    }
    if (http_ascii_equals_ignore_case(range, offered)) {
        return http_accept_token_match::exact;
    }
    if (prefix_matching && offered.size() > range.size() && offered[range.size()] == '-' &&
        http_ascii_equals_ignore_case(range, offered.substr(0, range.size()))) {
        return http_accept_token_match::prefix;
    }
    return http_accept_token_match::none;
}

void http_accumulate_token_acceptance(std::string_view accept, std::string_view offered,
    bool prefix_matching, int& best_specificity, int& best_quality) noexcept {
    http_visit_comma_separated_quoted(accept,
        [offered, prefix_matching, &best_specificity, &best_quality](std::string_view item) noexcept {
            const auto range = http_header_token_before_parameters(item);
            const auto match = http_accept_token_matches(range, offered, prefix_matching);
            if (match == http_accept_token_match::none) {
                return true;
            }
            const auto specificity = prefix_matching && match != http_accept_token_match::wildcard
                                         ? static_cast<int>(std::min(range.size(),
                                               static_cast<std::size_t>((std::numeric_limits<int>::max)())))
                                         : static_cast<int>(match);
            const auto quality = http_weight_parameter(item);
            if (specificity > best_specificity ||
                (specificity == best_specificity && quality > best_quality)) {
                best_specificity = specificity;
                best_quality = quality;
            }
            return true;
        });
}

[[nodiscard]] int http_media_range_match_specificity(std::string_view range,
    const http_media_type_parts& offered_parts, const http_media_type_parameters& offered_parameters,
    std::size_t& parameter_count, int& quality) noexcept {
    http_media_type_parts range_parts;
    if (!http_parse_media_type_parts(range, true, range_parts)) {
        return -1;
    }
    if (range_parts.type_ != "*" && !http_ascii_equals_ignore_case(range_parts.type_, offered_parts.type_)) {
        return -1;
    }
    if (range_parts.subtype_ != "*" && !http_ascii_equals_ignore_case(range_parts.subtype_, offered_parts.subtype_)) {
        return -1;
    }
    http_media_type_parameters parameters;
    if (!parameters.parse(range, &quality)) {
        return -1;
    }
    parameter_count = 0;
    if (!parameters.visit([&](std::string_view name, std::string_view value) noexcept {
            if (http_ascii_equals_ignore_case(name, "q")) {
                return true;
            }
            if (!offered_parameters.contains(name, value, offered_parts)) {
                return false;
            }
            ++parameter_count;
            return true;
        })) {
        return -1;
    }
    return range_parts.type_ == "*" ? 0 : (range_parts.subtype_ == "*" ? 1 : 2);
}

void http_accumulate_media_type_acceptance(std::string_view accept, std::string_view offered,
    int& best_specificity, std::size_t& best_parameter_count, int& best_quality) noexcept {
    http_media_type_parts offered_parts;
    http_media_type_parameters offered_parameters;
    if (!http_parse_media_type(offered, offered_parts, offered_parameters)) {
        return;
    }
    http_visit_comma_separated_quoted(accept,
        [offered_parts, &offered_parameters, &best_specificity, &best_parameter_count, &best_quality](std::string_view item) noexcept {
            int quality = 1000;
            std::size_t parameter_count = 0;
            const auto specificity = http_media_range_match_specificity(item, offered_parts, offered_parameters, parameter_count, quality);
            if (specificity >= 0) {
                if (specificity > best_specificity ||
                    (specificity == best_specificity &&
                        (parameter_count > best_parameter_count ||
                            (parameter_count == best_parameter_count && quality > best_quality)))) {
                    best_specificity = specificity;
                    best_parameter_count = parameter_count;
                    best_quality = quality;
                }
            }
            return true;
        });
}

}  // namespace
}  // namespace ruvia::detail
namespace ruvia {

std::string_view http_media_type_only(borrowed_text value) noexcept {
    return detail::http_header_token_before_parameters(value.view());
}

bool is_valid_http_content_type_field_value(std::string_view value) noexcept {
    return detail::is_valid_http_content_type_field_value(value);
}

void http_accept_match::update_media_type(std::string_view field, std::string_view offered) noexcept {
    detail::http_accumulate_media_type_acceptance(field, offered, specificity_, parameter_specificity_, quality_);
}

void http_accept_match::update_token(std::string_view field, std::string_view offered,
    http_accept_token_match_mode mode) noexcept {
    detail::http_accumulate_token_acceptance(
        field, offered, mode == http_accept_token_match_mode::language_prefix, specificity_, quality_);
}

}  // namespace ruvia

namespace ruvia {
namespace {
using item_type = detail::http_structured_item;

template <typename input_type>
std::variant<std::monostate, http_priority_error> parse_priority_members(detail::http_structured_parser<input_type> parser, http_priority_fields& fields_value) noexcept {
    parser.spaces();
    while (!parser.empty()) {
        const auto key = parser.key();
        if (key.empty()) {
            return http_priority_error::invalid_syntax;
        }
        item_type item{.kind_ = item_type::kind_type::boolean, .boolean_ = true};
        if (parser.take('=')) {
            item = {};
            if (!parser.member(item)) {
                return http_priority_error::invalid_syntax;
            }
        } else if (!parser.parameters()) {
            return http_priority_error::invalid_syntax;
        }
        if (key == "u") {
            fields_value.urgency_ = item.kind_ == item_type::kind_type::integer
                                        ? http_priority_urgency::try_from_value(item.integer_)
                                        : std::nullopt;
        }
        if (key == "i") {
            fields_value.incremental_ = item.kind_ == item_type::kind_type::boolean ? std::optional(item.boolean_) : std::nullopt;
        }
        parser.ows();
        if (parser.empty()) {
            break;
        }
        if (!parser.take(',')) {
            return http_priority_error::invalid_syntax;
        }
        parser.ows();
        if (parser.empty()) {
            return http_priority_error::invalid_syntax;
        }
    }
    return {};
}
}  // namespace

std::variant<http_priority_fields, http_priority_error> parse_http_priority(std::string_view value) noexcept {
    http_priority_fields fields;
    if (auto parsed_value = parse_priority_members(detail::http_structured_parser{detail::http_structured_text_input{value}}, fields); parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    return fields;
}
namespace detail {
std::variant<http_priority_fields, http_priority_error> parse_http_priority_headers(
    http_header_range_view headers) noexcept {
    http_priority_fields fields;
    if (auto parsed_value = parse_priority_members(
            http_structured_parser{
                http_structured_header_input{headers.begin(), headers.end(), "priority"}},
            fields);
        parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    return fields;
}
}  // namespace detail
std::variant<std::size_t, http_priority_error> encode_http_priority(std::span<char> output, http_priority_fields fields_value) noexcept {
    std::array<char, 12> bytes_value{};
    std::size_t count = 0;
    if (fields_value.urgency_) {
        bytes_value[count++] = 'u';
        bytes_value[count++] = '=';
        bytes_value[count++] = static_cast<char>('0' + fields_value.urgency_->value());
    }
    if (fields_value.incremental_) {
        if (*fields_value.incremental_) {
            if (count) {
                bytes_value[count++] = ',';
                bytes_value[count++] = ' ';
            }
            bytes_value[count++] = 'i';
        } else {
            if (count) {
                bytes_value[count++] = ',';
            }
            bytes_value[count++] = 'i';
            bytes_value[count++] = '=';
            bytes_value[count++] = '?';
            bytes_value[count++] = '0';
        }
    }
    if (output.size() < count) {
        return http_priority_error::output_too_small;
    }
    std::copy_n(bytes_value.begin(), count, output.begin());
    return count;
}
std::variant<http_priority_update, http_priority_error> decode_http2_priority_update(std::span<const char> payload_value) noexcept {
    if (payload_value.size() < 4) {
        return http_priority_error::invalid_frame;
    }
    const auto* id_bytes = reinterpret_cast<const unsigned char*>(payload_value.data());
    const auto id = ((static_cast<std::uint32_t>(id_bytes[0]) & 0x7fU) << 24) |
                    (static_cast<std::uint32_t>(id_bytes[1]) << 16) |
                    (static_cast<std::uint32_t>(id_bytes[2]) << 8) | static_cast<std::uint32_t>(id_bytes[3]);
    if (!id) {
        return http_priority_error::invalid_value;
    }
    auto fields_value = parse_http_priority({payload_value.data() + 4, payload_value.size() - 4});
    if ((fields_value.index() != 0)) {
        return std::get<1>(fields_value);
    }
    return http_priority_update{id, (id & 1U) == 0, std::get<0>(fields_value)};
}
std::variant<std::size_t, http_priority_error> encode_http2_priority_update(std::span<char> output, std::uint32_t id, http_priority_fields fields_value) noexcept {
    if (!id || id > 0x7fffffffU) {
        return http_priority_error::invalid_value;
    }
    std::array<char, 12> value{};
    const auto length = encode_http_priority(value, fields_value);
    if ((length.index() != 0)) {
        return std::get<1>(length);
    }
    if (output.size() < 13 + std::get<0>(length)) {
        return http_priority_error::output_too_small;
    }
    (void)encode_http2_frame_header(output.first(http2_frame_header_bytes),
        static_cast<std::uint32_t>(4 + std::get<0>(length)), http2_frame_type::priority_update, 0, 0);
    for (std::size_t index = 0; index < 4; ++index) {
        output[http2_frame_header_bytes + index] = static_cast<char>((id >> (24 - 8 * index)) & 0xffU);
    }
    std::copy_n(value.begin(), std::get<0>(length), output.begin() + 13);
    return 13 + std::get<0>(length);
}
}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] bool is_valid_http_cors_request_header_names(std::string_view value) noexcept {
    http_field_name_list names(value);
    bool saw_name = false;
    while (names.next()) {
        saw_name = true;
    }
    // Access-Control-Request-Headers requires at least one field name, unlike
    // the general #field-name grammar.
    return saw_name && names.valid();
}

// Host is a once-only field (RFC 9112 section 3.2) without a cached singleton
// bit; its kind index is below 32 like every singleton kind.
[[nodiscard]] constexpr std::uint32_t once_only_bit(http_field_kind kind) noexcept {
    return kind == http_field_kind::host ? 1U << static_cast<std::uint32_t>(kind)
                                         : singleton_request_header_bit(kind);
}

// RFC 9110 section 5.6.6 parameter: no whitespace around '='.
[[nodiscard]] bool is_exact_http_parameter(std::string_view part) noexcept {
    const auto equals = part.find('=');
    return equals != std::string_view::npos && is_valid_http_header_name(part.substr(0, equals)) &&
           is_http_token_or_quoted_string(part.substr(equals + 1));
}

[[nodiscard]] bool http_expectation_item(std::string_view item) noexcept {
    item = http_trim_ows(item);
    if (item.empty()) {
        return false;
    }

    const auto parameters_start = http_find_unquoted_delimiter(item, 0, ';');
    const auto leading = http_trim_ows(item.substr(0, parameters_start));
    if (leading.find('=') == std::string_view::npos) {
        return parameters_start == item.size() && is_valid_http_header_name(leading);
    }
    // RFC 9110 section 5.6.6: parameters = *( OWS ";" OWS [ parameter ] ).
    return is_exact_http_parameter(leading) && http_all_parameters(item, [](std::string_view parameter) noexcept {
        return parameter.empty() || is_exact_http_parameter(parameter);
    });
}

// Recipient-side Expect grammar. Empty list members are ignored, but every
// non-empty member still has to be a syntactically valid expectation before
// product policy decides whether unsupported extensions are rejected with 417
// or ignored.
[[nodiscard]] bool is_valid_received_http_expect_field_value(std::string_view value) noexcept {
    return http_visit_list_members(value, http_field_list_role::recipient, [](std::string_view item) noexcept {
        return http_expectation_item(item) ? http_field_list_parse_status::ok : http_field_list_parse_status::malformed;
    }) == http_field_list_parse_status::ok;
}

[[nodiscard]] bool is_valid_te(
    std::string_view value, http_field_list_role role, request_field_framing framing) noexcept {
    if (framing == request_field_framing::binary) {
        return is_valid_binary_http_te_field_value(value, role);
    }
    return is_valid_http_te_field_value(value, role);
}

[[nodiscard]] bool is_valid_value(http_field_kind kind, std::string_view value,
    http_field_list_role role, request_field_framing framing, http_request_field_source source) noexcept {
    // RFC 9110 sections 2.2 and 5.6.1: forwarding is not generation. Reuse the
    // recipient value rules only for end-to-end application metadata. Ownership
    // and singleton checks below still use the actual sender role.
    const auto application_role = source == http_request_field_source::forwarded
                                      ? http_field_list_role::recipient
                                      : role;
    switch (kind) {
        case http_field_kind::origin:
            return application_role == http_field_list_role::recipient || is_valid_http_origin_field_value(value);
        case http_field_kind::access_control_request_method:
            return application_role == http_field_list_role::recipient || is_valid_http_method_token(value);
        case http_field_kind::access_control_request_headers:
            return application_role == http_field_list_role::recipient || is_valid_http_cors_request_header_names(value);
        case http_field_kind::content_type:
            // Recipients interpret media types at the application boundary.
            return application_role == http_field_list_role::recipient ||
                   is_valid_http_content_type_field_value(value);
        case http_field_kind::content_encoding:
            // Admission leaves received metadata to the decoder. Forwarding
            // validates the recipient list grammar, not arbitrary nonempty text.
            return role == http_field_list_role::recipient ||
                   is_valid_http_content_encoding_field_value(value, application_role);
        case http_field_kind::trailer:
            // The declaration neither frames content nor authorizes actual trailers.
            return role == http_field_list_role::recipient ||
                   is_valid_http_request_trailer_field_value(value, role);
        case http_field_kind::te:
            return is_valid_te(value, role, framing);
        case http_field_kind::expect:
            // Binary field semantics feed the existing 417/ignore policy, not malformed-message rejection.
            return framing == request_field_framing::binary ||
                   is_valid_received_http_expect_field_value(value);
        case http_field_kind::host:
            return is_valid_host_header(value);
        default:
            return true;
    }
}

}  // namespace

bool request_field_state::seen(http_field_kind kind) const noexcept {
    return (seen_ & once_only_bit(kind)) != 0;
}

request_field_status validate_request_field(http_field_kind kind, std::string_view value,
    http_field_list_role role, request_field_state& state, http_request_field_source source) noexcept {
    if (!is_valid_http_request_field_source(source)) {
        return request_field_status::invalid;
    }
    if (role == http_field_list_role::sender &&
        (kind == http_field_kind::expect || kind == http_field_kind::content_length)) {
        return request_field_status::managed_by_sender;
    }
    if (!is_valid_value(kind, value, role, state.framing_, source)) {
        return request_field_status::invalid;
    }
    // Invalid date conditions, including repeated field lines, are ignored
    // rather than rejecting the request (RFC 9110 sections 13.1.3 and 13.1.4).
    // A repeated User-Agent violates only the sender rule of RFC 9110 section
    // 5.3 and controls no framing, routing or access, so recipients keep its
    // lines. Senders still cannot generate these singleton fields more than once.
    if (role == http_field_list_role::recipient &&
        (kind == http_field_kind::if_modified_since ||
            kind == http_field_kind::if_unmodified_since || kind == http_field_kind::user_agent)) {
        return request_field_status::ok;
    }
    if (const auto bit = once_only_bit(kind); bit != 0) {
        if ((state.seen_ & bit) != 0) {
            return request_field_status::invalid;
        }
        state.seen_ |= bit;
    }
    return request_field_status::ok;
}

bool is_protocol_managed_request_field(http_field_kind kind, std::string_view name) noexcept {
    switch (kind) {
        case http_field_kind::host:
        case http_field_kind::content_length:
        case http_field_kind::transfer_encoding:
        case http_field_kind::trailer:
        case http_field_kind::expect:
        case http_field_kind::connection:
        case http_field_kind::te:
        case http_field_kind::upgrade:
            return true;
        case http_field_kind::other:
            // Keep-Alive and Proxy-Connection have no request identity.
            return is_forbidden_http_binary_connection_field(name);
        default:
            return false;
    }
}

}  // namespace ruvia::detail

namespace ruvia {

bool is_http_protocol_managed_request_field(std::string_view name) noexcept {
    return detail::is_protocol_managed_request_field(detail::classify_http_field(name), name);
}

bool is_valid_http_request_application_field(std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return false;
    }
    const auto kind = detail::classify_http_field(name);
    if (detail::is_protocol_managed_request_field(kind, name)) {
        return false;
    }
    // TE 是唯一按 HTTP 版本区分的请求字段语义，已由托管字段边界排除。
    // 每次只检查一个字段；重复检测由调用方持有的字段集合负责。
    detail::request_field_state state(detail::request_field_framing::http1);
    return detail::validate_request_field(kind, value, detail::http_field_list_role::sender, state) ==
           detail::request_field_status::ok;
}

std::optional<std::string_view> http_field_name_list::next() noexcept {
    while (!finished_) {
        const auto separator = remaining_.find(',');
        const auto member = remaining_.substr(0, separator);
        if (separator == std::string_view::npos) {
            finished_ = true;
        } else {
            remaining_.remove_prefix(separator + 1);
        }
        const auto name = http_trim_ows(member);
        if (name.empty()) {
            if (finished_) {
                valid_ = true;
            }
            continue;
        }
        if (!is_valid_http_header_name(name)) {
            finished_ = true;
            valid_ = false;
            return std::nullopt;
        }
        if (finished_) {
            valid_ = true;
        }
        return name;
    }
    return std::nullopt;
}

void http_request_expectations::parse_field(std::string_view value) noexcept {
    detail::http_visit_comma_separated_quoted(value, [this](std::string_view member) noexcept {
        if (http_ascii_equals_ignore_case(member, detail::http_continue_expectation_token)) {
            flags_ |= continue_value;
        } else {
            flags_ |= unsupported;
        }
        return true;
    });
}
}  // namespace ruvia

namespace ruvia::detail {

bool is_valid_long_http_field_value_bytes(const char* data, std::size_t size) noexcept {
    return http_field_value_prefix_size({data, size}) == size;
}

bool is_forbidden_common_trailer_name(
    std::string_view name, http_field_kind kind) noexcept {
    switch (kind) {
        case http_field_kind::host:
        case http_field_kind::content_length:
        case http_field_kind::transfer_encoding:
        case http_field_kind::connection:
        case http_field_kind::content_encoding:
        case http_field_kind::content_type:
        case http_field_kind::cookie:
        case http_field_kind::expect:
        case http_field_kind::if_match:
        case http_field_kind::if_modified_since:
        case http_field_kind::if_none_match:
        case http_field_kind::if_range:
        case http_field_kind::if_unmodified_since:
        case http_field_kind::range:
        case http_field_kind::upgrade:
        case http_field_kind::authorization:
        case http_field_kind::te:
        case http_field_kind::trailer:
        case http_field_kind::proxy_authorization:
            return true;
        case http_field_kind::other:
        case http_field_kind::accept:
        case http_field_kind::accept_encoding:
        case http_field_kind::access_control_request_headers:
        case http_field_kind::access_control_request_method:
        case http_field_kind::origin:
        case http_field_kind::user_agent:
        case http_field_kind::sec_websocket_key:
        case http_field_kind::sec_websocket_protocol:
        case http_field_kind::sec_websocket_version:
        case http_field_kind::forwarded:
        case http_field_kind::x_forwarded_for:
        case http_field_kind::x_forwarded_proto:
        case http_field_kind::sec_websocket_extensions:
            break;
    }

    switch (name.size()) {
        case 10:
            return http_ascii_equals_ignore_case(name, "Keep-Alive") ||
                   http_ascii_equals_ignore_case(name, "Set-Cookie");
        case 12:
            return http_ascii_equals_ignore_case(name, "Max-Forwards");
        case 13:
            return http_ascii_equals_ignore_case(name, "Cache-Control") ||
                   http_ascii_equals_ignore_case(name, "Content-Range");
        case 16:
            return http_ascii_equals_ignore_case(name, "Proxy-Connection");
        case 18:
            return http_ascii_equals_ignore_case(name, "Proxy-Authenticate");
        default:
            return false;
    }
}

bool is_forbidden_http_request_trailer_name(std::string_view name) noexcept {
    const auto kind = classify_http_field(name);
    if (is_forbidden_common_trailer_name(name, kind)) {
        return true;
    }
    return kind == http_field_kind::access_control_request_headers ||
           kind == http_field_kind::access_control_request_method ||
           kind == http_field_kind::origin ||
           http_ascii_equals_ignore_case(name, "Accept-Ranges");
}

bool is_forbidden_response_trailer_name(std::string_view name) noexcept {
    // The response header classifier is the authoritative set of standardized
    // fields Ruvia manages. RFC 9110 explicitly permits only ETag (section
    // 8.8.3) and Accept-Ranges (section 14.3) from that set in trailers; every
    // other known field lacks trailer permission or controls framing,
    // representation handling, caching, routing, cookies, methods, or CORS.
    if (const auto known = classify_response_header_name(name);
        known != 0 && known != response_header_etag && known != response_header_accept_ranges) {
        return true;
    }

    if (is_forbidden_common_trailer_name(name, classify_http_field(name))) {
        return true;
    }

    switch (name.size()) {
        case 3:
            // Response control data (RFC 9110 §7.4).
            return http_ascii_equals_ignore_case(name, "Age");
        case 6:
            return http_ascii_equals_ignore_case(name, "Pragma");
        case 7:
            return http_ascii_equals_ignore_case(name, "Expires") ||
                   http_ascii_equals_ignore_case(name, "Warning");
        case 11:
            return http_ascii_equals_ignore_case(name, "Retry-After");
        case 15:
            return http_ascii_equals_ignore_case(name, "X-Frame-Options") ||
                   http_ascii_equals_ignore_case(name, "Referrer-Policy") ||
                   http_ascii_equals_ignore_case(name, "Clear-Site-Data");
        case 16:
            return http_ascii_equals_ignore_case(name, "X-XSS-Protection") ||
                   http_ascii_equals_ignore_case(name, "WWW-Authenticate");
        case 18:
            return http_ascii_equals_ignore_case(name, "Permissions-Policy");
        case 19:
            return http_ascii_equals_ignore_case(name, "Content-Disposition");
        case 22:
            return http_ascii_equals_ignore_case(name, "X-Content-Type-Options");
        case 23:
            return http_ascii_equals_ignore_case(name, "Content-Security-Policy");
        case 25:
            return http_ascii_equals_ignore_case(name, "Strict-Transport-Security");
        case 35:
            return http_ascii_equals_ignore_case(name, "Content-Security-Policy-Report-Only");
        default:
            return false;
    }
}

bool is_forbidden_http_binary_response_field(std::string_view name) noexcept {
    return is_forbidden_http_binary_connection_field(name) || http_ascii_equals_ignore_case(name, "te");
}

[[nodiscard]] bool is_valid_http_response_trailer_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role,
        [](std::string_view name) noexcept { return is_forbidden_response_trailer_name(name); });
}

[[nodiscard]] bool response_trailer_field_valid(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && !is_forbidden_response_trailer_name(name) &&
           is_valid_http_header_value(value);
}

response_field_status validate_response_field(http_field_kind kind, std::string_view value,
    response_field_state& state) noexcept {
    switch (kind) {
        case http_field_kind::content_type:
            if (!is_valid_http_content_type_field_value(value)) {
                return response_field_status::invalid;
            }
            if (state.content_type_seen_) {
                return response_field_status::repeated_singleton;
            }
            state.content_type_seen_ = true;
            break;
        case http_field_kind::content_encoding:
            if (!is_valid_http_content_encoding_field_value(value, http_field_list_role::sender)) {
                return response_field_status::invalid;
            }
            break;
        case http_field_kind::trailer:
            if (!is_valid_http_response_trailer_field_value(value, http_field_list_role::sender)) {
                return response_field_status::invalid;
            }
            break;
        default:
            break;
    }
    return response_field_status::ok;
}

}  // namespace ruvia::detail

namespace ruvia {

bool is_valid_http_response_field(std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return false;
    }
    detail::response_field_state state;
    return detail::validate_response_field(detail::classify_http_field(name), value, state) ==
           detail::response_field_status::ok;
}

http_response_trailer_section validate_http_response_trailers(
    std::span<const http_header_view> trailers) {
    auto section_size = http_trailer_section_budget();
    for (const auto& trailer : trailers) {
        if (!detail::response_trailer_field_valid(trailer.name(), trailer.value()) ||
            !section_size.add_field(trailer.name(), trailer.value())) {
            throw std::invalid_argument("invalid HTTP response trailer section");
        }
    }
    return http_response_trailer_section(trailers);
}

}  // namespace ruvia

namespace ruvia {
namespace {

// Parse a delta-seconds value (RFC 9111 section 1.2.2): an optionally DQUOTE-wrapped non-negative
// integer. Quoted-pairs have their RFC 9110 section 5.6.4 semantic value, so
// `"6\0"` is equivalent to `60`. Syntactically valid overflow saturates to the
// greatest convenient representation; non-digit / empty input remains invalid.
[[nodiscard]] std::optional<std::uint64_t> parse_delta_seconds(std::string_view value) noexcept {
    std::uint64_t result_value = 0;
    bool any_digit = false;
    const auto accumulate = [&result_value, &any_digit](unsigned char ch) noexcept {
        if (ch < '0' || ch > '9') {
            return false;
        }
        constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        result_value = result_value > (maximum - digit) / 10 ? maximum : result_value * 10 + digit;
        any_digit = true;
        return true;
    };
    if (!value.empty() && value.front() == '"') {
        http_quoted_string_cursor cursor(value);
        unsigned char ch = 0;
        while (cursor.next(ch)) {
            if (!accumulate(ch)) {
                return std::nullopt;
            }
        }
        if (!cursor.closed() || cursor.consumed() != value.size()) {
            return std::nullopt;
        }
    } else {
        for (const auto ch : value) {
            if (!accumulate(static_cast<unsigned char>(ch))) {
                return std::nullopt;
            }
        }
    }
    if (!any_digit) {
        return std::nullopt;
    }
    return result_value;
}

[[nodiscard]] bool parse_nominated_field_list_arg(
    std::string_view arg, auto&& add_field) noexcept {
    if (arg.empty()) {
        return false;
    }
    std::array<char, 256> unescaped_buf{};
    std::string_view list_text = arg;
    if (arg.front() == '"') {
        http_quoted_string_cursor cursor(arg);
        std::size_t len = 0;
        unsigned char ch = 0;
        while (cursor.next(ch)) {
            if (len >= unescaped_buf.size()) {
                return false;
            }
            unescaped_buf[len++] = static_cast<char>(ch);
        }
        if (!cursor.closed() || cursor.consumed() != arg.size()) {
            return false;
        }
        list_text = std::string_view(unescaped_buf.data(), len);
    } else if (!is_valid_http_header_name(arg)) {
        return false;
    }

    http_field_name_list names(list_text);
    bool saw_name = false;
    while (const auto name = names.next()) {
        saw_name = true;
        if (!add_field(*name)) {
            return false;
        }
    }
    return saw_name && names.valid();
}
}  // namespace

void cache_control_field_parser::update(std::string_view value) noexcept {
    detail::http_visit_comma_separated_quoted(value, [this](std::string_view token) noexcept {
        const auto eq = token.find('=');
        const bool has_argument = eq != std::string_view::npos;
        const auto name = eq == std::string_view::npos ? token : token.substr(0, eq);
        const auto arg = eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1);
        switch (name.size()) {
            case 6U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "public")) {
                    value_.set(cache_control_directive::public_value);
                }
                break;
            case 7U:
                if (http_ascii_equals_ignore_case(name, "max-age")) {
                    if (!max_age_seen_) {
                        max_age_seen_ = true;
                        value_.max_age_ = parse_delta_seconds(arg);
                    }
                } else if (http_ascii_equals_ignore_case(name, "private")) {
                    if (!has_argument) {
                        value_.set(cache_control_directive::private_value);
                        value_.set(cache_control_directive::private_unqualified);
                        value_.private_fields_.clear();
                    } else if (detail::is_http_token_or_quoted_string(arg)) {
                        value_.set(cache_control_directive::private_value);
                        if (!value_.is_unqualified_private()) {
                            const bool added = parse_nominated_field_list_arg(
                                arg, [this](std::string_view field_name) noexcept {
                                    return value_.private_fields_.add_lowercase(field_name);
                                });
                            if (!added) {
                                value_.set(cache_control_directive::private_unqualified);
                                value_.private_fields_.clear();
                            }
                        }
                    }
                }
                break;
            case 8U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "no-store")) {
                    value_.set(cache_control_directive::no_store);
                } else if (http_ascii_equals_ignore_case(name, "no-cache")) {
                    if (!has_argument) {
                        value_.set(cache_control_directive::no_cache);
                        value_.set(cache_control_directive::no_cache_unqualified);
                        value_.no_cache_fields_.clear();
                    } else if (detail::is_http_token_or_quoted_string(arg)) {
                        value_.set(cache_control_directive::no_cache);
                        if (!value_.is_unqualified_no_cache()) {
                            const bool added = parse_nominated_field_list_arg(
                                arg, [this](std::string_view field_name) noexcept {
                                    return value_.no_cache_fields_.add_lowercase(field_name);
                                });
                            if (!added) {
                                value_.set(cache_control_directive::no_cache_unqualified);
                                value_.no_cache_fields_.clear();
                            }
                        }
                    }
                } else if (http_ascii_equals_ignore_case(name, "s-maxage")) {
                    if (!s_max_age_seen_) {
                        s_max_age_seen_ = true;
                        value_.s_max_age_ = parse_delta_seconds(arg);
                    }
                }
                break;
            case 9U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "immutable")) {
                    value_.set(cache_control_directive::immutable);
                } else if (http_ascii_equals_ignore_case(name, "max-stale")) {
                    if (!max_stale_seen_) {
                        max_stale_seen_ = true;
                        if (!has_argument) {
                            value_.set(cache_control_directive::max_stale_any);
                        } else {
                            value_.max_stale_ = parse_delta_seconds(arg);
                        }
                    }
                } else if (http_ascii_equals_ignore_case(name, "min-fresh")) {
                    if (!min_fresh_seen_) {
                        min_fresh_seen_ = true;
                        value_.min_fresh_ = parse_delta_seconds(arg);
                    }
                }
                break;
            case 12U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "no-transform")) {
                    value_.set(cache_control_directive::no_transform);
                }
                break;
            case 14U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "only-if-cached")) {
                    value_.set(cache_control_directive::only_if_cached);
                } else if (http_ascii_equals_ignore_case(name, "stale-if-error")) {
                    if (!stale_if_error_seen_) {
                        stale_if_error_seen_ = true;
                        value_.stale_if_error_ = parse_delta_seconds(arg);
                    }
                }
                break;
            case 15U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "must-revalidate")) {
                    value_.set(cache_control_directive::must_revalidate);
                }
                break;
            case 16U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "proxy-revalidate")) {
                    value_.set(cache_control_directive::proxy_revalidate);
                }
                break;
            case 22U:
                if (http_ascii_equals_ignore_case(name, "stale-while-revalidate")) {
                    if (!stale_while_revalidate_seen_) {
                        stale_while_revalidate_seen_ = true;
                        value_.stale_while_revalidate_ = parse_delta_seconds(arg);
                    }
                }
                break;
            default:
                break;
        }
        return true;
    });
}

cache_control parse_cache_control(std::string_view value) noexcept {
    cache_control_field_parser parser;
    parser.update(value);
    return parser.finish();
}

cache_control detail::parse_cache_control_headers(detail::http_header_range_view headers) noexcept {
    cache_control_field_parser parser;
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            parser.update(field.value());
        }
    }
    return parser.finish();
}

bool http_cache_control_has_no_transform(std::string_view field_value) noexcept {
    return parse_cache_control(field_value).no_transform();
}

bool cache_control::nominated_field_set::contains(std::string_view field_name) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (http_ascii_equals_ignore_case(at(i), field_name)) {
            return true;
        }
    }
    return false;
}

bool cache_control::nominated_field_set::add_lowercase(std::string_view field_name) noexcept {
    if (field_name.empty() || field_name.size() > (std::numeric_limits<std::uint8_t>::max)()) {
        return false;
    }
    if (contains(field_name)) {
        return true;
    }
    if (count_ >= max_nominated_fields ||
        field_name.size() > max_nominated_field_bytes - storage_used_) {
        return false;
    }
    offsets_[count_] = storage_used_;
    lengths_[count_] = static_cast<std::uint8_t>(field_name.size());
    for (std::size_t i = 0; i < field_name.size(); ++i) {
        storage_[storage_used_ + i] =
            static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(field_name[i])));
    }
    storage_used_ = static_cast<std::uint16_t>(storage_used_ + field_name.size());
    ++count_;
    return true;
}

bool cache_control::has_private_field(std::string_view field_name) const noexcept {
    return !is_unqualified_private() && private_fields_.contains(field_name);
}

bool cache_control::has_no_cache_field(std::string_view field_name) const noexcept {
    return !is_unqualified_no_cache() && no_cache_fields_.contains(field_name);
}

template <typename input_type>
std::optional<cache_control> parse_targeted_cache_control_from_input(input_type input) noexcept {
    detail::http_structured_parser<input_type> parser{std::move(input)};
    parser.ows();
    if (parser.empty()) {
        return std::nullopt;
    }
    cache_control result;
    bool saw_member = false;
    while (!parser.empty()) {
        const auto key = parser.key();
        if (key.empty()) {
            return std::nullopt;
        }
        saw_member = true;
        detail::http_structured_item item{
            .kind_ = detail::http_structured_item::kind_type::boolean,
            .boolean_ = true,
        };
        if (parser.take('=')) {
            item = {};
            if (!parser.member(item)) {
                return std::nullopt;
            }
        } else if (!parser.parameters()) {
            return std::nullopt;
        }

        const auto apply_bool_directive = [&](cache_control_directive dir) noexcept {
            if (item.kind_ == detail::http_structured_item::kind_type::boolean) {
                if (item.boolean_) {
                    result.set(dir);
                } else {
                    result.clear_directive(dir);
                }
            } else {
                result.clear_directive(dir);
            }
        };
        const auto apply_delta_directive =
            [&](std::optional<std::uint64_t>& target) noexcept {
                if (item.kind_ == detail::http_structured_item::kind_type::integer &&
                    item.integer_ >= 0) {
                    target = static_cast<std::uint64_t>(item.integer_);
                } else {
                    target.reset();
                }
            };
        const auto apply_qualified_directive = [&](cache_control_directive flag_dir,
                                                   cache_control_directive unqual_dir,
                                                   cache_control::nominated_field_set& fields) noexcept {
            if (item.kind_ == detail::http_structured_item::kind_type::boolean) {
                fields.clear();
                if (item.boolean_) {
                    result.set(flag_dir);
                    result.set(unqual_dir);
                } else {
                    result.clear_directive(flag_dir);
                    result.clear_directive(unqual_dir);
                }
            } else if (item.kind_ == detail::http_structured_item::kind_type::string ||
                       item.kind_ == detail::http_structured_item::kind_type::token) {
                fields.clear();
                result.clear_directive(unqual_dir);
                result.set(flag_dir);
                std::array<char, 256> unescaped{};
                std::string_view list_text = item.text_;
                if (item.kind_ == detail::http_structured_item::kind_type::string &&
                    item.has_escapes_) {
                    std::size_t out_len = 0;
                    for (std::size_t i = 0; i < item.text_.size(); ++i) {
                        if (item.text_[i] == '\\' && i + 1U < item.text_.size()) {
                            ++i;
                        }
                        if (out_len >= unescaped.size()) {
                            result.set(unqual_dir);
                            fields.clear();
                            return;
                        }
                        unescaped[out_len++] = item.text_[i];
                    }
                    list_text = std::string_view(unescaped.data(), out_len);
                }
                http_field_name_list names(list_text);
                bool saw_name = false;
                bool ok = true;
                while (const auto field_name = names.next()) {
                    saw_name = true;
                    if (!fields.add_lowercase(*field_name)) {
                        ok = false;
                        break;
                    }
                }
                if (!saw_name || !names.valid() || !ok) {
                    result.set(unqual_dir);
                    fields.clear();
                }
            } else {
                fields.clear();
                result.clear_directive(flag_dir);
                result.clear_directive(unqual_dir);
            }
        };

        if (key == "no-store") {
            apply_bool_directive(cache_control_directive::no_store);
        } else if (key == "no-transform") {
            apply_bool_directive(cache_control_directive::no_transform);
        } else if (key == "must-revalidate") {
            apply_bool_directive(cache_control_directive::must_revalidate);
        } else if (key == "proxy-revalidate") {
            apply_bool_directive(cache_control_directive::proxy_revalidate);
        } else if (key == "public") {
            apply_bool_directive(cache_control_directive::public_value);
        } else if (key == "immutable") {
            apply_bool_directive(cache_control_directive::immutable);
        } else if (key == "only-if-cached") {
            apply_bool_directive(cache_control_directive::only_if_cached);
        } else if (key == "private") {
            apply_qualified_directive(cache_control_directive::private_value,
                cache_control_directive::private_unqualified, result.private_fields_);
        } else if (key == "no-cache") {
            apply_qualified_directive(cache_control_directive::no_cache,
                cache_control_directive::no_cache_unqualified, result.no_cache_fields_);
        } else if (key == "max-age") {
            apply_delta_directive(result.max_age_);
        } else if (key == "s-maxage") {
            apply_delta_directive(result.s_max_age_);
        } else if (key == "stale-while-revalidate") {
            apply_delta_directive(result.stale_while_revalidate_);
        } else if (key == "stale-if-error") {
            apply_delta_directive(result.stale_if_error_);
        } else if (key == "min-fresh") {
            apply_delta_directive(result.min_fresh_);
        } else if (key == "max-stale") {
            if (item.kind_ == detail::http_structured_item::kind_type::boolean && item.boolean_) {
                result.set(cache_control_directive::max_stale_any);
                result.max_stale_.reset();
            } else if (item.kind_ == detail::http_structured_item::kind_type::integer &&
                       item.integer_ >= 0) {
                result.clear_directive(cache_control_directive::max_stale_any);
                result.max_stale_ = static_cast<std::uint64_t>(item.integer_);
            } else {
                result.clear_directive(cache_control_directive::max_stale_any);
                result.max_stale_.reset();
            }
        }

        parser.ows();
        if (parser.empty()) {
            break;
        }
        if (!parser.take(',')) {
            return std::nullopt;
        }
        parser.ows();
        if (parser.empty()) {
            return std::nullopt;
        }
    }
    if (!saw_member) {
        return std::nullopt;
    }
    return result;
}

std::optional<cache_control> parse_targeted_cache_control(std::string_view value) noexcept {
    return parse_targeted_cache_control_from_input(detail::http_structured_text_input{value});
}

std::optional<cache_control> detail::parse_targeted_cache_control_headers(
    detail::http_header_range_view headers, std::string_view header_name) noexcept {
    if (header_name.empty()) {
        return std::nullopt;
    }
    return parse_targeted_cache_control_from_input(
        detail::http_structured_header_input{headers.begin(), headers.end(), header_name});
}

targeted_cache_control_selection detail::resolve_targeted_cache_control_headers(
    detail::http_header_range_view headers,
    std::span<const std::string_view> target_headers) noexcept {
    const auto has_cache_control = [&headers]() noexcept {
        for (const auto field : headers) {
            if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
                return true;
            }
        }
        return false;
    };
    for (const auto target_name : target_headers) {
        if (target_name.empty()) {
            continue;
        }
        if (http_ascii_equals_ignore_case(target_name, "Cache-Control")) {
            if (has_cache_control()) {
                return {
                    .control_ = parse_cache_control_headers(headers),
                    .selected_header_ = target_name,
                    .targeted_ = false,
                };
            }
            continue;
        }
        if (const auto targeted = parse_targeted_cache_control_headers(headers, target_name)) {
            return {
                .control_ = *targeted,
                .selected_header_ = target_name,
                .targeted_ = true,
            };
        }
    }
    return {
        .control_ = parse_cache_control_headers(headers),
        .selected_header_ = has_cache_control() ? std::string_view("Cache-Control")
                                                : std::string_view{},
        .targeted_ = false,
    };
}

bool strip_http_cache_private_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    if (control.is_field_qualified_private()) {
        std::erase_if(headers, [&control](const http_header& field) noexcept {
            return control.has_private_field(field.name());
        });
    }
    return true;
}

bool strip_http_cache_private_headers(
    http_response_headers& headers, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    const auto count = control.private_field_count();
    for (std::size_t i = 0; i < count; ++i) {
        headers.remove_header(control.private_field(i));
    }
    return true;
}

bool strip_http_cache_private_headers(
    http_response& response, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    if (!control.is_field_qualified_private()) {
        return true;
    }
    auto staged = response.headers().clone();
    (void)strip_http_cache_private_headers(staged, control);
    response.commit_headers(std::move(staged));
    return true;
}

bool strip_http_cache_private_headers(std::vector<http_header>& headers) noexcept {
    return strip_http_cache_private_headers(headers, parse_cache_control(headers));
}

bool strip_http_cache_private_headers(http_response_headers& headers) {
    return strip_http_cache_private_headers(headers, parse_cache_control(headers));
}

bool strip_http_cache_private_headers(http_response& response) {
    return strip_http_cache_private_headers(response, parse_cache_control(response));
}

bool strip_http_cache_no_cache_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    if (control.is_field_qualified_no_cache()) {
        std::erase_if(headers, [&control](const http_header& field) noexcept {
            return control.has_no_cache_field(field.name());
        });
    }
    return true;
}

bool strip_http_cache_no_cache_headers(
    http_response_headers& headers, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    const auto count = control.no_cache_field_count();
    for (std::size_t i = 0; i < count; ++i) {
        headers.remove_header(control.no_cache_field(i));
    }
    return true;
}

bool strip_http_cache_no_cache_headers(
    http_response& response, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    if (!control.is_field_qualified_no_cache()) {
        return true;
    }
    auto staged = response.headers().clone();
    (void)strip_http_cache_no_cache_headers(staged, control);
    response.commit_headers(std::move(staged));
    return true;
}

namespace {

void append_preserved_cache_control_directives(
    std::string_view field_value, std::string& output) {
    detail::http_visit_comma_separated_quoted(field_value, [&](std::string_view token) {
        const auto eq = token.find('=');
        const auto name =
            http_trim_ows(eq == std::string_view::npos ? token : token.substr(0, eq));
        if (name.empty() || http_ascii_equals_ignore_case(name, "max-age") ||
            http_ascii_equals_ignore_case(name, "s-maxage")) {
            return true;
        }
        if (!output.empty()) {
            output.append(", ");
        }
        output.append(token);
        return true;
    });
}

void finish_rewritten_cache_control_max_age(
    std::string& output, std::uint64_t client_max_age) {
    if (!output.empty()) {
        output.append(", ");
    }
    output.append("max-age=");
    http_append_unsigned(output, client_max_age);
}

[[nodiscard]] std::string normalize_vary_field_value(std::string_view raw) {
    const auto trimmed = http_trim_ows(raw);
    std::string normalized;
    normalized.reserve(trimmed.size());
    bool in_ows = false;
    for (const char ch : trimmed) {
        if (ch == ' ' || ch == '\t') {
            if (!in_ows) {
                normalized.push_back(' ');
                in_ows = true;
            }
        } else {
            normalized.push_back(ch);
            in_ows = false;
        }
    }
    return normalized;
}

template <typename target_headers_type>
void merge_304_headers_impl(
    target_headers_type& cached_headers, detail::http_header_range_view not_modified_headers) {
    const auto nominated = parse_http_connection_nominated_fields(not_modified_headers);
    std::vector<std::string_view> updated_names;
    for (const auto field : not_modified_headers) {
        const auto name = field.name();
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(field.value()) ||
            is_http_hop_by_hop_or_nominated_field(name, nominated) ||
            is_http_304_forbidden_header(name)) {
            continue;
        }
        const bool already_recorded =
            std::ranges::any_of(updated_names, [name](std::string_view existing) noexcept {
                return http_ascii_equals_ignore_case(existing, name);
            });
        if (!already_recorded) {
            updated_names.push_back(name);
        }
    }
    if (updated_names.empty()) {
        return;
    }
    if constexpr (std::same_as<target_headers_type, http_response_headers>) {
        for (const auto name : updated_names) {
            cached_headers.remove_header(name);
        }
    } else {
        std::erase_if(cached_headers, [&](const http_header& existing) noexcept {
            return std::ranges::any_of(updated_names, [&](std::string_view updated) noexcept {
                return http_ascii_equals_ignore_case(existing.name(), updated);
            });
        });
    }
    for (const auto field : not_modified_headers) {
        const auto name = field.name();
        const bool should_copy =
            std::ranges::any_of(updated_names, [name](std::string_view updated) noexcept {
                return http_ascii_equals_ignore_case(name, updated);
            });
        if (!should_copy) {
            continue;
        }
        if constexpr (std::same_as<target_headers_type, http_response_headers>) {
            if (cached_headers.header(name).has_value() &&
                !http_ascii_equals_ignore_case(name, "Date") &&
                !http_ascii_equals_ignore_case(name, "ETag") &&
                !http_ascii_equals_ignore_case(name, "Expires") &&
                !http_ascii_equals_ignore_case(name, "Content-Location") &&
                !http_ascii_equals_ignore_case(name, "Last-Modified")) {
                cached_headers.header(
                    name, field.value(), {.mode_ = http_response_header_mode::append});
            } else {
                cached_headers.header(name, field.value());
            }
        } else {
            cached_headers.emplace_back(name, field.value());
        }
    }
}
}  // namespace

std::string rewrite_http_cache_control_max_age(
    std::string_view field_value, std::uint64_t client_max_age) {
    std::string output;
    append_preserved_cache_control_directives(field_value, output);
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

std::string detail::rewrite_http_cache_control_max_age_headers(
    detail::http_header_range_view headers, std::uint64_t client_max_age) {
    std::string output;
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            append_preserved_cache_control_directives(field.value(), output);
        }
    }
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

void rewrite_http_response_cache_max_age(
    std::vector<http_header>& response_headers, std::uint64_t client_max_age) {
    auto rewritten = rewrite_http_cache_control_max_age(response_headers, client_max_age);
    std::erase_if(response_headers, [](const http_header& field) noexcept {
        return http_ascii_equals_ignore_case(field.name(), "Cache-Control") ||
               http_ascii_equals_ignore_case(field.name(), "Expires");
    });
    response_headers.emplace_back("Cache-Control", rewritten);
}

void rewrite_http_response_cache_max_age(
    http_response_headers& response_headers, std::uint64_t client_max_age) {
    std::string output;
    for (const auto& field : response_headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            append_preserved_cache_control_directives(field.value(), output);
        }
    }
    finish_rewritten_cache_control_max_age(output, client_max_age);
    response_headers.remove_header("Expires");
    response_headers.header("Cache-Control", output);
}

void rewrite_http_response_cache_max_age(
    http_response& response, std::uint64_t client_max_age) {
    auto staged = response.headers().clone(1);
    rewrite_http_response_cache_max_age(staged, client_max_age);
    response.commit_headers(std::move(staged));
}

void http_vary_spec::update(std::string_view field_value) {
    if (wildcard_) {
        return;
    }
    detail::http_visit_comma_separated_tokens(field_value, [&](std::string_view token) {
        if (token == "*") {
            wildcard_ = true;
            field_names_.clear();
            return false;
        }
        if (!is_valid_http_header_name(token)) {
            return true;
        }
        std::string lower(token);
        for (auto& ch : lower) {
            ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
        }
        const auto it = std::ranges::lower_bound(field_names_, lower);
        if (it == field_names_.end() || *it != lower) {
            field_names_.insert(it, std::move(lower));
        }
        return true;
    });
}

void http_vary_spec::update_header_range(detail::http_header_range_view headers) {
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Vary")) {
            update(field.value());
        }
    }
}

http_vary_spec parse_http_vary(std::string_view field_value) {
    http_vary_spec spec;
    spec.update(field_value);
    return spec;
}

std::optional<std::string> detail::format_http_vary_cache_key_headers(
    const http_vary_spec& vary, detail::http_header_range_view request_headers) {
    if (vary.is_wildcard()) {
        return std::nullopt;
    }
    std::string key;
    for (const auto& field_name : vary.field_names()) {
        key.append(field_name);
        bool present = false;
        for (const auto header : request_headers) {
            if (!http_ascii_equals_ignore_case(header.name(), field_name)) {
                continue;
            }
            if (!present) {
                key.append(":1:");
                present = true;
            } else {
                key.append(", ");
            }
            key.append(normalize_vary_field_value(header.value()));
        }
        if (!present) {
            key.append(":0");
        }
        key.push_back('\n');
    }
    return key;
}

std::optional<std::uint64_t> parse_http_age(std::string_view field_value) noexcept {
    const auto trimmed = http_trim_ows(field_value);
    if (trimmed.empty() || trimmed.front() == '"') {
        return std::nullopt;
    }
    return parse_delta_seconds(trimmed);
}

std::uint64_t detail::compute_http_cache_age_headers(
    detail::http_header_range_view response_headers, std::time_t request_time,
    std::time_t response_time, std::time_t now) noexcept {
    std::optional<std::time_t> date_value;
    std::optional<std::uint64_t> age_value;
    for (const auto field : response_headers) {
        if (!date_value.has_value() && http_ascii_equals_ignore_case(field.name(), "Date")) {
            date_value = parse_http_date(field.value());
        } else if (!age_value.has_value() && http_ascii_equals_ignore_case(field.name(), "Age")) {
            age_value = parse_http_age(field.value());
        }
    }
    return compute_http_cache_age(date_value, age_value, request_time, response_time, now);
}

bool is_http_304_forbidden_header(std::string_view field_name) noexcept {
    switch (field_name.size()) {
        case 6U:
            return http_ascii_equals_ignore_case(field_name, "Digest");
        case 7U:
            return http_ascii_equals_ignore_case(field_name, "Trailer");
        case 11U:
            return http_ascii_equals_ignore_case(field_name, "Content-MD5") ||
                   http_ascii_equals_ignore_case(field_name, "Repr-Digest");
        case 12U:
            return http_ascii_equals_ignore_case(field_name, "Content-Type");
        case 13U:
            return http_ascii_equals_ignore_case(field_name, "Content-Range");
        case 14U:
            return http_ascii_equals_ignore_case(field_name, "Content-Length") ||
                   http_ascii_equals_ignore_case(field_name, "Content-Digest");
        case 16U:
            return http_ascii_equals_ignore_case(field_name, "Content-Encoding") ||
                   http_ascii_equals_ignore_case(field_name, "Content-Language");
        case 17U:
            return http_ascii_equals_ignore_case(field_name, "Transfer-Encoding");
        case 19U:
            return http_ascii_equals_ignore_case(field_name, "Content-Disposition");
        default:
            return false;
    }
}

void sanitize_http_304_response_headers(std::vector<http_header>& headers) {
    std::erase_if(headers, [](const http_header& field) noexcept {
        return is_http_304_forbidden_header(field.name());
    });
}

void sanitize_http_304_response_headers(http_response_headers& headers) {
    std::vector<std::string> to_remove;
    for (const auto& field : headers) {
        if (is_http_304_forbidden_header(field.name())) {
            to_remove.emplace_back(field.name());
        }
    }
    for (const auto& name : to_remove) {
        headers.remove_header(name);
    }
}

void sanitize_http_304_response_headers(http_response& response) {
    auto staged = response.headers().clone();
    sanitize_http_304_response_headers(staged);
    response.commit_headers(std::move(staged));
}

void detail::merge_http_304_response_headers_into(
    std::vector<http_header>& cached_headers, detail::http_header_range_view not_modified_headers) {
    merge_304_headers_impl(cached_headers, not_modified_headers);
}

void detail::merge_http_304_response_headers_into(
    http_response_headers& cached_headers, detail::http_header_range_view not_modified_headers) {
    merge_304_headers_impl(cached_headers, not_modified_headers);
}
void merge_http_304_response_headers(
    http_response& cached_response, const http_response& not_modified_response) {
    auto staged = cached_response.headers().clone(not_modified_response.headers().size());
    merge_http_304_response_headers(staged, not_modified_response.headers());
    cached_response.commit_headers(std::move(staged));
}

}  // namespace ruvia

namespace ruvia::detail {

// UTC calendar fields for an HTTP-date, or no value outside years 0000-9999
// (the four-digit IMF-fixdate year). Civil conversion is independent of the C
// runtime's date range and locale; in particular, Windows gmtime_s cannot
// represent pre-1970 UTC timestamps. The range check runs before calendar
// conversion so huge time_t values cannot overflow it.
[[nodiscard]] std::optional<std::tm> http_utc_tm(std::time_t time) noexcept {
    using namespace std::chrono;
    constexpr auto first = duration_cast<seconds>(sys_days{year{0} / January / 1}.time_since_epoch()).count();
    constexpr auto end = duration_cast<seconds>(sys_days{year{10000} / January / 1}.time_since_epoch()).count();
    if (std::cmp_less(time, first) || std::cmp_greater_equal(time, end)) {
        return std::nullopt;
    }
    const sys_seconds instant{seconds{static_cast<seconds::rep>(time)}};
    const auto date = floor<days>(instant);
    const year_month_day calendar{date};
    const hh_mm_ss clock{instant - date};
    std::tm utc{};
    utc.tm_year = static_cast<int>(calendar.year()) - 1900;
    utc.tm_mon = static_cast<int>(static_cast<unsigned>(calendar.month())) - 1;
    utc.tm_mday = static_cast<int>(static_cast<unsigned>(calendar.day()));
    utc.tm_wday = static_cast<int>(weekday{date}.c_encoding());
    utc.tm_hour = static_cast<int>(clock.hours().count());
    utc.tm_min = static_cast<int>(clock.minutes().count());
    utc.tm_sec = static_cast<int>(clock.seconds().count());
    return utc;
}

std::optional<std::time_t> http_civil_to_time_t(
    int year, int month, int day, int hour, int minute, int second) noexcept {
    if (year < static_cast<int>((std::chrono::year::min)()) || year > static_cast<int>((std::chrono::year::max)()) ||
        month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const auto days_since_epoch = static_cast<std::int64_t>(std::chrono::sys_days{date}.time_since_epoch().count());
    const auto total = days_since_epoch * 86400 + static_cast<std::int64_t>(hour) * 3600 +
                       static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second);
    if (!std::in_range<std::time_t>(total)) {
        return std::nullopt;
    }
    return static_cast<std::time_t>(total);
}

namespace {

// "Date: <value>\r\n" -- the cache is the single owner of this wire format.
// Both the full-line accessor (HTTP/1 text response) and the
// value-only accessor (HPACK :date for HTTP/2) derive their views from these
// constants, so the framing lives in exactly one place.
inline constexpr std::string_view date_header_prefix = "Date: ";
inline constexpr std::string_view date_header_suffix = "\r\n";

struct date_cache final {
    std::array<char, 64> line_{};
    std::size_t size_{0};
    std::time_t second_{};
    bool initialized_{false};
};

[[nodiscard]] date_cache& worker_date_cache() noexcept {
    thread_local date_cache cache;
    return cache;
}

}  // namespace

std::string_view cached_date_header() noexcept {
    const auto now = std::time(nullptr);
    auto& cache = worker_date_cache();
    if (!cache.initialized_ || cache.second_ != now) {
        cache.second_ = now;
        cache.initialized_ = true;
        cache.size_ = 0;
        // RFC 9110 section 6.6.1: do not generate Date without a usable clock.
        if (now != std::time_t{-1}) {
            if (const auto date = format_http_date(now)) {
                std::memcpy(cache.line_.data(), date_header_prefix.data(), date_header_prefix.size());
                std::memcpy(cache.line_.data() + date_header_prefix.size(), date->data(), date->size());
                cache.size_ = date_header_prefix.size() + date->size();
                cache.line_[cache.size_++] = date_header_suffix[0];
                cache.line_[cache.size_++] = date_header_suffix[1];
            }
        }
    }
    return std::string_view(cache.line_.data(), cache.size_);
}

std::string_view cached_date_value() noexcept {
    const auto line = cached_date_header();
    constexpr std::size_t framing = date_header_prefix.size() + date_header_suffix.size();
    if (line.size() <= framing) {
        return {};
    }
    return line.substr(date_header_prefix.size(), line.size() - framing);
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

// IMF-fixdate day names (RFC 9110 section 5.6.7), indexed by tm_wday.
constexpr std::array<std::string_view, 7> http_weekday_names{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

[[nodiscard]] int month_index(std::string_view value) noexcept {
    for (std::size_t i = 0; i < detail::http_month_names.size(); ++i) {
        if (value == detail::http_month_names[i]) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

[[nodiscard]] bool is_short_weekday(std::string_view value) noexcept {
    return std::ranges::find(http_weekday_names, value) != http_weekday_names.end();
}

[[nodiscard]] bool is_long_weekday(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 7> weekdays{
        "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    return std::ranges::find(weekdays, value) != weekdays.end();
}

[[nodiscard]] std::optional<int> parse_fixed_digits(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    int parsed_value = 0;
    for (const auto c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        parsed_value = parsed_value * 10 + (c - '0');
    }
    return parsed_value;
}

[[nodiscard]] std::optional<std::time_t> parse_imf_fixdate(std::string_view value) noexcept {
    if (value.size() != http_imf_fixdate_size || !is_short_weekday(value.substr(0, 3)) || value[3] != ',' ||
        value[4] != ' ' || value[7] != ' ' || value[11] != ' ' || value[16] != ' ' ||
        value[19] != ':' || value[22] != ':' || value[25] != ' ' || value.substr(26, 3) != "GMT") {
        return std::nullopt;
    }
    const auto day = parse_fixed_digits(value.substr(5, 2));
    const auto month = month_index(value.substr(8, 3));
    const auto year = parse_fixed_digits(value.substr(12, 4));
    const auto hour = parse_fixed_digits(value.substr(17, 2));
    const auto minute = parse_fixed_digits(value.substr(20, 2));
    const auto second = parse_fixed_digits(value.substr(23, 2));
    if (!day || month == 0 || !year || !hour || !minute || !second) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] std::optional<std::time_t> parse_asctime_date(std::string_view value) noexcept {
    if (value.size() != 24 || value[3] != ' ' || value[7] != ' ' || value[10] != ' ' ||
        value[13] != ':' || value[16] != ':' || value[19] != ' ' ||
        !is_short_weekday(value.substr(0, 3))) {
        return std::nullopt;
    }
    const auto month = month_index(value.substr(4, 3));
    auto day_field = value.substr(8, 2);
    if (day_field.front() == ' ') {
        day_field.remove_prefix(1);
    }
    const auto day = parse_fixed_digits(day_field);
    const auto hour = parse_fixed_digits(value.substr(11, 2));
    const auto minute = parse_fixed_digits(value.substr(14, 2));
    const auto second = parse_fixed_digits(value.substr(17, 2));
    const auto year = parse_fixed_digits(value.substr(20, 4));
    if (month == 0 || !day || !hour || !minute || !second || !year) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] int resolve_rfc850_year(int short_year, int current_year) noexcept {
    const auto current_century = current_year - current_year % 100;
    auto year = current_century + short_year;
    // RFC 9110 section 5.6.7: an obsolete two-digit date that appears more
    // than 50 years in the future denotes the most recent past year with the
    // same final two digits. This is a rolling pivot, not the fixed POSIX
    // 68/69 split.
    if (year > current_year + 50) {
        year -= 100;
    }
    return year;
}

[[nodiscard]] std::optional<std::time_t> parse_rfc850_date(std::string_view value) noexcept {
    const auto comma = value.find(", ");
    if (comma == std::string_view::npos || !is_long_weekday(value.substr(0, comma))) {
        return std::nullopt;
    }
    const auto body = value.substr(comma + 2);
    if (body.size() != 22 || body[2] != '-' || body[6] != '-' || body[9] != ' ' ||
        body[12] != ':' || body[15] != ':' || body[18] != ' ' || body.substr(19, 3) != "GMT") {
        return std::nullopt;
    }
    const auto day = parse_fixed_digits(body.substr(0, 2));
    const auto month = month_index(body.substr(3, 3));
    const auto short_year = parse_fixed_digits(body.substr(7, 2));
    const auto hour = parse_fixed_digits(body.substr(10, 2));
    const auto minute = parse_fixed_digits(body.substr(13, 2));
    const auto second = parse_fixed_digits(body.substr(16, 2));
    if (!day || month == 0 || !short_year || !hour || !minute || !second) {
        return std::nullopt;
    }
    const auto now = std::time(nullptr);
    if (now == std::time_t{-1}) {
        return std::nullopt;
    }
    const auto current_utc = detail::http_utc_tm(now);
    if (!current_utc) {
        return std::nullopt;
    }
    const int current_year = current_utc->tm_year + 1900;
    auto year = resolve_rfc850_year(*short_year, current_year);
    // RFC 9110 section 5.6.7 applies the rolling 50-year pivot to the full
    // timestamp. Equal years alone do not establish that a date is in range.
    const std::array date_fields{month, *day, *hour, *minute, *second};
    const std::array reference_fields{current_utc->tm_mon + 1, current_utc->tm_mday, current_utc->tm_hour,
        current_utc->tm_min, current_utc->tm_sec};
    if (year == current_year + 50 && date_fields > reference_fields) {
        year -= 100;
    }
    return detail::http_civil_to_time_t(year, month, *day, *hour, *minute, *second);
}

}  // namespace

// Allocation-free wire value; an unrepresentable date must not be truncated or
// substituted with a different timestamp. Fixed English names are independent
// of the process locale, as required by RFC 9110 section 5.6.7.
std::optional<std::array<char, http_imf_fixdate_size>> format_http_date(std::time_t value) noexcept {
    const auto utc = detail::http_utc_tm(value);
    if (!utc) {
        return std::nullopt;
    }
    std::array<char, http_imf_fixdate_size> output{};
    auto* out = output.data();
    const long year = static_cast<long>(utc->tm_year) + 1900;

    std::size_t i = 0;
    const auto put2 = [out, &i](int field) noexcept {
        out[i++] = static_cast<char>('0' + (field / 10) % 10);
        out[i++] = static_cast<char>('0' + field % 10);
    };
    const auto put3 = [out, &i](std::string_view name) noexcept {
        out[i++] = name[0];
        out[i++] = name[1];
        out[i++] = name[2];
    };

    put3(http_weekday_names[static_cast<std::size_t>(utc->tm_wday)]);
    out[i++] = ',';
    out[i++] = ' ';
    put2(utc->tm_mday);
    out[i++] = ' ';
    put3(detail::http_month_names[static_cast<std::size_t>(utc->tm_mon)]);
    out[i++] = ' ';
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 1000) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 100) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 10) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>(year % 10));
    out[i++] = ' ';
    put2(utc->tm_hour);
    out[i++] = ':';
    put2(utc->tm_min);
    out[i++] = ':';
    put2(utc->tm_sec);
    out[i++] = ' ';
    out[i++] = 'G';
    out[i++] = 'M';
    out[i++] = 'T';
    return output;
}

std::optional<std::time_t> parse_http_date(std::string_view value) noexcept {
    value = http_trim_ows(value);
    if (const auto imf = parse_imf_fixdate(value)) {
        return imf;
    }
    if (const auto rfc850 = parse_rfc850_date(value)) {
        return rfc850;
    }
    return parse_asctime_date(value);
}

}  // namespace ruvia

namespace ruvia {
namespace {

constexpr std::string_view v1_signature = "PROXY ";
// Spec §2.1: the longest line, "PROXY UNKNOWN" with two IPv6 addresses and
// two ports, including CRLF.
constexpr std::size_t v1_max_line_bytes = 107;
constexpr std::array<std::uint8_t, 12> v2_signature{
    0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A};
constexpr std::size_t v2_fixed_bytes = 16;
constexpr std::size_t v2_inet_block_bytes = 12;
constexpr std::size_t v2_inet6_block_bytes = 36;
constexpr std::size_t v2_unix_block_bytes = 216;
constexpr std::size_t v2_tlv_header_bytes = 3;

constexpr std::uint8_t af_unspec = 0x0;
constexpr std::uint8_t af_inet = 0x1;
constexpr std::uint8_t af_inet6 = 0x2;
constexpr std::uint8_t af_unix = 0x3;

[[noreturn]] void reject(const char* reason) {
    throw proxy_protocol_error(reason);
}

[[nodiscard]] constexpr std::uint8_t byte_value(std::byte value) noexcept {
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] constexpr std::uint16_t load_big_endian_16(std::span<const std::byte> bytes) noexcept {
    return static_cast<std::uint16_t>((byte_value(bytes[0]) << 8) | byte_value(bytes[1]));
}

// Spec §2.2: the address block that precedes TLVs; AF_UNSPEC has none whose
// size is known.
[[nodiscard]] constexpr std::size_t v2_address_block_bytes(std::uint8_t family) noexcept {
    switch (family) {
        case af_inet:
            return v2_inet_block_bytes;
        case af_inet6:
            return v2_inet6_block_bytes;
        case af_unix:
            return v2_unix_block_bytes;
        default:
            return 0;
    }
}

[[nodiscard]] std::string_view as_text(std::span<const std::byte> bytes) noexcept {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Spec §2.1: canonical address text of the announced family, without a zone.
[[nodiscard]] std::optional<ip_address> parse_v1_address(std::string_view text, ip_address_family family) noexcept {
    if (text.find('%') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto address = ip_address::parse(text);
    if (!address || address->family() != family) {
        return std::nullopt;
    }
    return address;
}

// Spec §2.1: decimal 0..65535 without heading zeroes.
[[nodiscard]] std::optional<std::uint16_t> parse_v1_port(std::string_view text) noexcept {
    if (text.empty() || text.size() > 5 || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint32_t>(digit - '0');
    }
    if (value > 0xFFFF) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

// Splits "<src> <dst> <sport> <dport>" at single spaces; an empty field or a
// field count other than four rejects.
[[nodiscard]] std::optional<std::array<std::string_view, 4>> split_v1_fields(std::string_view text) noexcept {
    std::array<std::string_view, 4> fields;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto space = text.find(' ');
        const bool last = i + 1 == fields.size();
        if (last != (space == std::string_view::npos)) {
            return std::nullopt;
        }
        fields[i] = text.substr(0, space);
        if (fields[i].empty()) {
            return std::nullopt;
        }
        if (!last) {
            text.remove_prefix(space + 1);
        }
    }
    return fields;
}

[[nodiscard]] ip_endpoint load_v2_endpoint(
    std::span<const std::byte> address, std::span<const std::byte> port, ip_address_family family) noexcept {
    if (family == ip_address_family::ipv4) {
        std::array<std::uint8_t, 4> bytes{};
        std::transform(address.begin(), address.end(), bytes.begin(), byte_value);
        return ip_endpoint(ip_address::ipv4(bytes), load_big_endian_16(port));
    }
    std::array<std::uint8_t, 16> bytes{};
    std::transform(address.begin(), address.end(), bytes.begin(), byte_value);
    return ip_endpoint(ip_address::ipv6(bytes), load_big_endian_16(port));
}

}  // namespace

proxy_protocol_parser::proxy_protocol_parser(proxy_protocol_parser_options options)
    : max_v2_header_bytes_(options.max_v2_header_bytes_) {
    if (max_v2_header_bytes_ < v2_fixed_bytes + v2_inet6_block_bytes) {
        throw std::invalid_argument("PROXY protocol v2 header limit cannot hold an AF_INET6 address block");
    }
}

proxy_protocol_parse_result proxy_protocol_parser::parse(std::span<const std::byte> input) & {
    if (state_ != proxy_protocol_parser_state::active) {
        throw std::logic_error("PROXY protocol parser is not active");
    }
    try {
        return parse_bytes(input);
    } catch (...) {
        // Fail closed: the caller cannot know how much of a rejected or
        // unbufferable input was taken.
        state_ = proxy_protocol_parser_state::failed;
        throw;
    }
}

const proxy_protocol_header& proxy_protocol_parser::header() const& {
    if (state_ != proxy_protocol_parser_state::finished) {
        throw std::logic_error("PROXY protocol header is not complete");
    }
    return header_;
}

void proxy_protocol_parser::reset() noexcept {
    buffer_.clear();
    tlvs_.clear();
    header_ = proxy_protocol_header();
    v2_header_size_ = 0;
    v2_family_ = 0;
    phase_ = parse_phase::signature;
    state_ = proxy_protocol_parser_state::active;
}

proxy_protocol_parse_result proxy_protocol_parser::parse_bytes(std::span<const std::byte> input) {
    std::size_t position = 0;
    while (position < input.size() && state_ == proxy_protocol_parser_state::active) {
        switch (phase_) {
            case parse_phase::signature:
                position = consume_signature(input, position);
                break;
            case parse_phase::v1_line:
                position = consume_v1_line(input, position);
                break;
            case parse_phase::v2_fixed:
                position = consume_v2_fixed(input, position);
                break;
            case parse_phase::v2_block:
                position = consume_v2_block(input, position);
                break;
        }
    }
    return {.complete_ = state_ == proxy_protocol_parser_state::finished, .consumed_bytes_ = position};
}

// The first byte selects the only signature it can begin ('P' or CR), so each
// later byte either continues that signature or rules out both.
std::size_t proxy_protocol_parser::consume_signature(std::span<const std::byte> input, std::size_t position) {
    const auto value = byte_value(input[position]);
    const auto index = buffer_.size();
    if (index == 0) {
        if (value == static_cast<std::uint8_t>(v1_signature.front())) {
            header_.version_ = proxy_protocol_version::v1;
        } else if (value == v2_signature.front()) {
            header_.version_ = proxy_protocol_version::v2;
        } else {
            reject("input does not begin a PROXY protocol signature");
        }
    } else {
        const auto expected = header_.version_ == proxy_protocol_version::v1
                                  ? static_cast<std::uint8_t>(v1_signature[index])
                                  : v2_signature[index];
        if (value != expected) {
            reject("input does not begin a PROXY protocol signature");
        }
    }
    buffer_.push_back(input[position]);
    if (header_.version_ == proxy_protocol_version::v1 && buffer_.size() == v1_signature.size()) {
        phase_ = parse_phase::v1_line;
    } else if (header_.version_ == proxy_protocol_version::v2 && buffer_.size() == v2_signature.size()) {
        phase_ = parse_phase::v2_fixed;
    }
    return position + 1;
}

// Spec §2.1: the line ends at the first CRLF within 107 bytes; a lone CR or LF
// rejects. Bytes before the end are only searched for CR and LF here; the
// fields are decoded once, after the CRLF arrives.
std::size_t proxy_protocol_parser::consume_v1_line(std::span<const std::byte> input, std::size_t position) {
    // The signature ends in a space, so a buffered trailing CR is a line's CR
    // whose LF did not arrive in the previous input.
    if (buffer_.back() == std::byte{'\r'}) {
        if (input[position] != std::byte{'\n'}) {
            reject("PROXY protocol v1 CR is not followed by LF");
        }
        buffer_.push_back(input[position]);
        finish_v1();
        return position + 1;
    }
    const auto chunk = input.subspan(position, std::min(input.size() - position, v1_max_line_bytes - buffer_.size()));
    const auto stop = std::find_if(chunk.begin(), chunk.end(),
        [](std::byte value) { return value == std::byte{'\r'} || value == std::byte{'\n'}; });
    if (stop != chunk.end() && *stop == std::byte{'\n'}) {
        reject("PROXY protocol v1 line ends in a bare LF");
    }
    const auto taken = static_cast<std::size_t>(stop - chunk.begin()) + (stop == chunk.end() ? 0 : 1);
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(taken));
    // Full without CRLF, or a CR that leaves no room for its LF.
    if (buffer_.size() >= v1_max_line_bytes) {
        reject("PROXY protocol v1 line exceeds 107 bytes");
    }
    return position + taken;
}

std::size_t proxy_protocol_parser::consume_v2_fixed(std::span<const std::byte> input, std::size_t position) {
    const auto value = byte_value(input[position]);
    switch (buffer_.size()) {
        case 12:
            // Spec §2.2: the receiver must only accept version 2 and must drop
            // unassigned commands.
            if ((value >> 4) != 0x2) {
                reject("PROXY protocol v2 version is not 2");
            }
            switch (value & 0x0F) {
                case 0x0:
                    header_.command_ = proxy_protocol_command::local;
                    break;
                case 0x1:
                    header_.command_ = proxy_protocol_command::proxy;
                    break;
                default:
                    reject("PROXY protocol v2 command is unassigned");
            }
            break;
        case 13:
            v2_family_ = static_cast<std::uint8_t>(value >> 4);
            if (v2_family_ > af_unix) {
                reject("PROXY protocol v2 address family is unspecified");
            }
            switch (value & 0x0F) {
                case 0x0:
                    header_.transport_ = proxy_protocol_transport::unspecified;
                    break;
                case 0x1:
                    header_.transport_ = proxy_protocol_transport::stream;
                    break;
                case 0x2:
                    header_.transport_ = proxy_protocol_transport::datagram;
                    break;
                default:
                    reject("PROXY protocol v2 transport protocol is unspecified");
            }
            break;
        default:
            break;
    }
    buffer_.push_back(input[position]);
    if (buffer_.size() < v2_fixed_bytes) {
        return position + 1;
    }
    const std::size_t length = load_big_endian_16(std::span<const std::byte>(buffer_).subspan(14, 2));
    if (length > max_v2_header_bytes_ - v2_fixed_bytes) {
        reject("PROXY protocol v2 header exceeds the configured limit");
    }
    if (header_.command_ == proxy_protocol_command::proxy && length < v2_address_block_bytes(v2_family_)) {
        reject("PROXY protocol v2 length does not cover the address block");
    }
    v2_header_size_ = v2_fixed_bytes + length;
    phase_ = parse_phase::v2_block;
    if (length == 0) {
        finish_v2();
    }
    return position + 1;
}

// Spec §2.2: the receiver must not parse an address before the whole block
// has arrived.
std::size_t proxy_protocol_parser::consume_v2_block(std::span<const std::byte> input, std::size_t position) {
    const auto taken = std::min(input.size() - position, v2_header_size_ - buffer_.size());
    const auto chunk = input.subspan(position, taken);
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
    if (buffer_.size() == v2_header_size_) {
        finish_v2();
    }
    return position + taken;
}

void proxy_protocol_parser::finish_v1() {
    // Without the signature and the CRLF.
    const auto line = as_text(std::span<const std::byte>(buffer_)).substr(v1_signature.size(), buffer_.size() - v1_signature.size() - 2);
    header_.command_ = proxy_protocol_command::proxy;
    constexpr std::string_view unknown = "UNKNOWN";
    if (line.starts_with(unknown) && (line.size() == unknown.size() || line[unknown.size()] == ' ')) {
        // Spec §2.1: anything after UNKNOWN is ignored and the real endpoints
        // are used.
        header_.transport_ = proxy_protocol_transport::unspecified;
        state_ = proxy_protocol_parser_state::finished;
        return;
    }
    const bool tcp4 = line.starts_with("TCP4 ");
    if (!tcp4 && !line.starts_with("TCP6 ")) {
        reject("PROXY protocol v1 protocol is not TCP4, TCP6 or UNKNOWN");
    }
    const auto family = tcp4 ? ip_address_family::ipv4 : ip_address_family::ipv6;
    const auto fields = split_v1_fields(line.substr(5));
    if (!fields) {
        reject("PROXY protocol v1 line does not have four single-space separated fields");
    }
    const auto source_address = parse_v1_address((*fields)[0], family);
    const auto destination_address = parse_v1_address((*fields)[1], family);
    if (!source_address || !destination_address) {
        reject("PROXY protocol v1 address is not canonical text of the announced family");
    }
    const auto source_port = parse_v1_port((*fields)[2]);
    const auto destination_port = parse_v1_port((*fields)[3]);
    if (!source_port || !destination_port) {
        reject("PROXY protocol v1 port is not a decimal 0..65535 without heading zeroes");
    }
    header_.transport_ = proxy_protocol_transport::stream;
    header_.source_ = ip_endpoint(*source_address, *source_port);
    header_.destination_ = ip_endpoint(*destination_address, *destination_port);
    state_ = proxy_protocol_parser_state::finished;
}

void proxy_protocol_parser::finish_v2() {
    // Spec §2.2: LOCAL discards the block, family included. With AF_UNSPEC the
    // address length is unknown, so no TLV boundary can be located.
    if (header_.command_ == proxy_protocol_command::local || v2_family_ == af_unspec) {
        state_ = proxy_protocol_parser_state::finished;
        return;
    }
    const auto block = std::span<const std::byte>(buffer_).subspan(v2_fixed_bytes);
    if (header_.transport_ != proxy_protocol_transport::unspecified && v2_family_ != af_unix) {
        const auto family = v2_family_ == af_inet ? ip_address_family::ipv4 : ip_address_family::ipv6;
        const std::size_t address_bytes = family == ip_address_family::ipv4 ? 4 : 16;
        const auto ports = block.subspan(2 * address_bytes, 4);
        header_.source_ = load_v2_endpoint(block.subspan(0, address_bytes), ports.subspan(0, 2), family);
        header_.destination_ = load_v2_endpoint(block.subspan(address_bytes, address_bytes), ports.subspan(2, 2), family);
    }
    // Spec §2.2.3: PP2_TYPE_CRC32C verification is a MAY; every TLV, that one
    // included, is exposed raw.
    auto rest = block.subspan(v2_address_block_bytes(v2_family_));
    while (!rest.empty()) {
        if (rest.size() < v2_tlv_header_bytes) {
            reject("PROXY protocol v2 TLV header overruns the header");
        }
        const std::size_t value_bytes = load_big_endian_16(rest.subspan(1, 2));
        if (value_bytes > rest.size() - v2_tlv_header_bytes) {
            reject("PROXY protocol v2 TLV value overruns the header");
        }
        tlvs_.push_back({.type_ = byte_value(rest[0]), .value_ = rest.subspan(v2_tlv_header_bytes, value_bytes)});
        rest = rest.subspan(v2_tlv_header_bytes + value_bytes);
    }
    header_.tlvs_ = tlvs_;
    state_ = proxy_protocol_parser_state::finished;
}

}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] std::optional<std::uint32_t> parse_zone(std::string_view text) noexcept {
    if (text.empty() || text.front() == '0') {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] char* append_ipv4(char* cursor, char* end, const std::uint8_t* bytes) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        if (i != 0) {
            *cursor++ = '.';
        }
        cursor = std::to_chars(cursor, end, bytes[i]).ptr;
    }
    return cursor;
}

}  // namespace

std::optional<ip_address> ip_address::parse(std::string_view text) noexcept {
    if (text.find(':') == std::string_view::npos) {
        const auto octets = detail::parse_ipv4_address(text);
        if (!octets) {
            return std::nullopt;
        }
        return ip_address::ipv4(*octets);
    }
    std::uint32_t scope_id = 0;
    const auto zone = text.find('%');
    if (zone != std::string_view::npos) {
        const auto parsed_zone = parse_zone(text.substr(zone + 1));
        if (!parsed_zone) {
            return std::nullopt;
        }
        scope_id = *parsed_zone;
        text = text.substr(0, zone);
    }
    const auto groups = detail::parse_ipv6_address(text);
    if (!groups) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < groups->size(); ++i) {
        bytes[2 * i] = static_cast<std::uint8_t>((*groups)[i] >> 8);
        bytes[2 * i + 1] = static_cast<std::uint8_t>((*groups)[i] & 0xffU);
    }
    return ip_address::ipv6(bytes, scope_id);
}

std::string ip_address::to_string() const {
    // "ffff:" * 7 + "ffff" + "%" + 10 zone digits.
    std::array<char, 56> buffer;
    char* const end = buffer.data() + buffer.size();
    char* cursor = buffer.data();
    if (family_ == ip_address_family::ipv4) {
        cursor = append_ipv4(cursor, end, bytes_.data());
    } else {
        bool mapped = bytes_[10] == 0xffU && bytes_[11] == 0xffU;
        for (std::size_t i = 0; mapped && i < 10; ++i) {
            mapped = bytes_[i] == 0;
        }
        if (mapped) {
            const std::string_view prefix = "::ffff:";
            cursor = std::copy(prefix.begin(), prefix.end(), cursor);
            cursor = append_ipv4(cursor, end, bytes_.data() + 12);
        } else {
            std::array<std::uint16_t, 8> groups{};
            for (std::size_t i = 0; i < groups.size(); ++i) {
                groups[i] = static_cast<std::uint16_t>(bytes_[2 * i] << 8 | bytes_[2 * i + 1]);
            }
            // RFC 5952 §4.2: compress the first longest run of two or more
            // zero groups.
            std::size_t best_begin = groups.size();
            std::size_t best_size = 1;
            for (std::size_t i = 0; i < groups.size();) {
                if (groups[i] != 0) {
                    ++i;
                    continue;
                }
                std::size_t run_end = i;
                while (run_end < groups.size() && groups[run_end] == 0) {
                    ++run_end;
                }
                if (run_end - i > best_size) {
                    best_begin = i;
                    best_size = run_end - i;
                }
                i = run_end;
            }
            for (std::size_t i = 0; i < groups.size();) {
                if (i == best_begin) {
                    *cursor++ = ':';
                    *cursor++ = ':';
                    i += best_size;
                    continue;
                }
                if (i != 0 && i != best_begin + best_size) {
                    *cursor++ = ':';
                }
                cursor = std::to_chars(cursor, end, groups[i], 16).ptr;
                ++i;
            }
        }
        if (scope_id_ != 0) {
            *cursor++ = '%';
            cursor = std::to_chars(cursor, end, scope_id_).ptr;
        }
    }
    return std::string(buffer.data(), static_cast<std::size_t>(cursor - buffer.data()));
}

namespace {

[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> to_mapped_ipv6_bytes(
    const ip_address& address) noexcept {
    if (address.family() == ip_address_family::ipv4) {
        const auto v4 = address.bytes();
        return std::array<std::uint8_t, 16>{
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, v4[0], v4[1], v4[2], v4[3]};
    }
    // Scoped IPv6 literals compare by their 128-bit address prefix in CIDR blocks.
    std::array<std::uint8_t, 16> v6{};
    const auto raw = address.bytes();
    std::copy_n(raw.data(), 16, v6.data());
    return v6;
}

}  // namespace

std::optional<ip_cidr> ip_cidr::parse(std::string_view text) noexcept {
    const auto slash = text.find('/');
    const auto address_text = slash == std::string_view::npos ? text : text.substr(0, slash);
    const auto parsed = ip_address::parse(address_text);
    if (!parsed) {
        return std::nullopt;
    }
    const auto mapped = to_mapped_ipv6_bytes(*parsed);
    if (!mapped) {
        return std::nullopt;
    }
    const bool was_v4 = parsed->family() == ip_address_family::ipv4;
    const unsigned max_bits = was_v4 ? 32U : 128U;
    unsigned bits = max_bits;
    if (slash != std::string_view::npos) {
        const auto prefix_text = text.substr(slash + 1);
        if (prefix_text.empty() || prefix_text.size() > 3) {
            return std::nullopt;
        }
        const auto [end, err] =
            std::from_chars(prefix_text.data(), prefix_text.data() + prefix_text.size(), bits);
        if (err != std::errc{} || end != prefix_text.data() + prefix_text.size() ||
            bits > max_bits) {
            return std::nullopt;
        }
    }
    ip_cidr result;
    result.network_ = *mapped;
    result.prefix_bits_ = static_cast<std::uint8_t>(was_v4 ? bits + 96U : bits);
    return result;
}

bool ip_cidr::contains(const ip_address& address) const noexcept {
    const auto peer = to_mapped_ipv6_bytes(address);
    if (!peer) {
        return false;
    }
    const std::size_t full_bytes = prefix_bits_ / 8U;
    const std::uint8_t rem_bits = prefix_bits_ % 8U;
    if (!std::equal(network_.begin(), network_.begin() + full_bytes, peer->begin())) {
        return false;
    }
    if (rem_bits == 0) {
        return true;
    }
    const auto mask = static_cast<std::uint8_t>(0xffU << (8U - rem_bits));
    return (network_[full_bytes] & mask) == ((*peer)[full_bytes] & mask);
}

bool ip_cidr::contains(std::string_view address_text) const noexcept {
    const auto parsed = ip_address::parse(address_text);
    return parsed.has_value() && contains(*parsed);
}

}  // namespace ruvia

namespace ruvia {

http_connect_udp_target::http_connect_udp_target(std::string host, port_number port) noexcept
    : host_(std::move(host)),
      port_(port.value()) {}

namespace {
constexpr std::string_view prefix{"/.well-known/masque/udp/"};
bool valid_host(std::string_view host) {
    return !host.empty() && host.find('%') == std::string_view::npos &&
           (host.find(':') != std::string_view::npos ? detail::parse_ipv6_address(host).has_value() : detail::is_valid_reg_name(host));
}
// RFC 9297 section 3.2: these statuses cannot start the Capsule Protocol.
constexpr bool forbids_capsule_protocol(std::uint16_t status) noexcept {
    return status == 204 || status == 205 || status == 206;
}
[[nodiscard]] bool is_http_or_https_scheme(std::string_view scheme) noexcept {
    return http_ascii_equals_ignore_case(scheme, "http") ||
           http_ascii_equals_ignore_case(scheme, "https");
}
enum class handshake_message_role : std::uint8_t {
    request_recipient,
    request_sender,
    response_recipient,
    response_sender,
};
enum class capsule_protocol_state : std::uint8_t {
    absent,
    valid_true,
    invalid,
};
struct handshake_fields {
    capsule_protocol_state capsule_protocol_{capsule_protocol_state::absent};
    bool upgrade_{false}, upgrade_field_{false}, connection_{false}, connection_field_{false};
    bool host_{false}, forbidden_{false}, http2_forbidden_{false};
    std::string_view host_value_{};
};
template <typename header_type>
std::variant<handshake_fields, http_connect_udp_error> fields(std::span<const header_type> headers,
    handshake_message_role role, http_connect_udp_error syntax_error) noexcept {
    const auto connection_role =
        role == handshake_message_role::request_recipient
            ? detail::http_connection_field_role::request_recipient
            : (role == handshake_message_role::response_recipient
                      ? detail::http_connection_field_role::response_recipient
                      : detail::http_connection_field_role::sender);
    const auto list_role =
        (role == handshake_message_role::request_recipient ||
            role == handshake_message_role::response_recipient)
            ? detail::http_field_list_role::recipient
            : detail::http_field_list_role::sender;
    handshake_fields found;
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    bool connect_udp_offered = false;
    for (const auto& field : headers) {
        if (!is_valid_http_header_name(field.name()) || !is_valid_http_header_value(field.value())) {
            return syntax_error;
        }
        if (detail::is_forbidden_http_binary_connection_field(field.name())) {
            found.http2_forbidden_ = true;
        } else if (http_ascii_equals_ignore_case(field.name(), "te")) {
            if (role == handshake_message_role::response_recipient ||
                role == handshake_message_role::response_sender ||
                !detail::is_valid_binary_http_te_field_value(field.value(), list_role)) {
                found.http2_forbidden_ = true;
            }
        }
        if (http_ascii_equals_ignore_case(field.name(), "capsule-protocol")) {
            // RFC 9297 section 3.4: recipients handle a non-Boolean value as if the
            // field were absent; ?0 has the same meaning as absence. Only senders
            // require a single ?1 Capsule-Protocol field.
            if (list_role == detail::http_field_list_role::sender) {
                const auto value = parse_http_capsule_protocol(field.value());
                found.capsule_protocol_ =
                    (found.capsule_protocol_ == capsule_protocol_state::absent &&
                        value == std::optional<bool>(true))
                        ? capsule_protocol_state::valid_true
                        : capsule_protocol_state::invalid;
            }
        } else if (http_ascii_equals_ignore_case(field.name(), "upgrade")) {
            found.upgrade_field_ = true;
            if (upgrade_protocols.parse_field(field.value(), list_role,
                    [&](const detail::http_upgrade_protocol& protocol) noexcept {
                        const bool is_connect_udp =
                            protocol.version_.empty() &&
                            http_ascii_equals_ignore_case(protocol.name_, "connect-udp");
                        if (role == handshake_message_role::request_recipient) {
                            if (is_connect_udp) {
                                connect_udp_offered = true;
                            }
                            return true;
                        }
                        if (!is_connect_udp ||
                            (role != handshake_message_role::response_recipient && connect_udp_offered)) {
                            return false;
                        }
                        connect_udp_offered = true;
                        return true;
                    }) != detail::http_field_list_parse_status::ok) {
                return syntax_error;
            }
        } else if (http_ascii_equals_ignore_case(field.name(), "connection")) {
            found.connection_field_ = true;
            if (connection_options.parse_field(field.value(), connection_role) !=
                detail::http_field_list_parse_status::ok) {
                return syntax_error;
            }
            found.connection_ = connection_options.contains(detail::http_connection_option::upgrade);
        } else if (http_ascii_equals_ignore_case(field.name(), "host")) {
            const auto trimmed_host = http_trim_ows(field.value());
            if (found.host_ || !parse_http_authority(trimmed_host).has_value()) {
                return syntax_error;
            }
            found.host_ = true;
            found.host_value_ = trimmed_host;
        } else if (detail::http_field_name_in(field.name(), detail::http_connect_forbidden_fields)) {
            // RFC 9297 Section 3.2: the Capsule Protocol MUST NOT be used with
            // messages carrying Content-Length, Content-Type, or Transfer-Encoding
            // (the list also covers Content-Encoding and Trailer); a receiver that
            // observes one treats the message as malformed.
            found.forbidden_ = true;
        }
    }
    found.upgrade_ = upgrade_protocols.has_protocol() && connect_udp_offered;
    if (found.upgrade_field_ && !found.upgrade_) {
        return syntax_error;
    }
    return found;
}
template <typename header_type>
std::optional<http_connect_udp_error> validate_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const header_type> headers) noexcept {
    if (version == http_protocol_version::http10) {
        return http_connect_udp_error::invalid_response;
    }
    const bool extended_connect = version != http_protocol_version::http11;
    const auto parsed_value =
        fields(headers, handshake_message_role::response_recipient, http_connect_udp_error::invalid_response);
    if (parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    const auto& found = std::get<0>(parsed_value);
    // The connect-udp token, not Capsule-Protocol, selects the Capsule Protocol
    // (RFC 9298 section 3); RFC 9297 section 3.2 makes 204/205/206 malformed.
    if (found.forbidden_ ||
        (extended_connect
                ? status < 200 || status >= 300 || forbids_capsule_protocol(status) || found.upgrade_field_ ||
                      found.connection_field_ || found.http2_forbidden_
                : status != 101 || !found.upgrade_ || !found.connection_)) {
        return http_connect_udp_error::invalid_response;
    }
    return std::nullopt;
}
}  // namespace
std::variant<std::string, http_connect_udp_error> encode_http_connect_udp_path(http_connect_udp_target_view target) {
    if (!valid_host(target.host_) || target.port_ == 0) {
        return http_connect_udp_error::invalid_target;
    }
    std::size_t encoded_host_size = 0;
    for (const unsigned char c : target.host_.view()) {
        encoded_host_size += detail::is_unreserved_byte(c) ? 1U : 3U;
    }
    std::array<char, http_max_unsigned_decimal_size> port_buffer{};
    const auto port_digits = http_format_unsigned(port_buffer, target.port_.value());
    std::string result_value;
    result_value.reserve(prefix.size() + encoded_host_size + 1U + port_digits.size() + 1U);
    result_value.append(prefix);
    for (const unsigned char c : target.host_.view()) {
        if (detail::is_unreserved_byte(c)) {
            result_value.push_back(static_cast<char>(c));
        } else {
            http_append_percent_byte(result_value, c);
        }
    }
    result_value.push_back('/');
    result_value.append(port_digits);
    result_value.push_back('/');
    return result_value;
}
std::variant<http_connect_udp_target, http_connect_udp_error> parse_http_connect_udp_path(std::string_view path) {
    if (!path.starts_with(prefix) || path.size() <= prefix.size() || !path.ends_with('/')) {
        return http_connect_udp_error::invalid_path;
    }
    path.remove_prefix(prefix.size());
    path.remove_suffix(1);
    const auto slash = path.find('/');
    if (slash == std::string_view::npos || path.find('/', slash + 1) != std::string_view::npos) {
        return http_connect_udp_error::invalid_path;
    }
    const auto raw_host = path.substr(0, slash);
    // Colons in IPv6 addresses must be escaped in this URI-template variable.
    if (raw_host.find(':') != std::string_view::npos) {
        return http_connect_udp_error::invalid_target;
    }
    const auto port = detail::parse_port_value(path.substr(slash + 1));
    if ((port.index() != 0) || !std::get<0>(port)) {
        return http_connect_udp_error::invalid_target;
    }
    std::string host;
    const auto decoded_host = decode_url_component(raw_host, host);
    if (!decoded_host || !valid_host(*decoded_host)) {
        return http_connect_udp_error::invalid_target;
    }
    if (decoded_host->data() == raw_host.data()) {
        host.assign(raw_host);
    }
    return http_connect_udp_target{std::move(host), std::get<0>(port)};
}
namespace {
std::optional<http_connect_udp_error> validate_request(http_connect_udp_request_view request, bool sending) noexcept {
    const auto authority = parse_http_authority(request.authority_);
    if (!authority || authority->host().empty() || !is_valid_http_origin_form_target(request.path_)) {
        return http_connect_udp_error::invalid_request;
    }
    const auto parsed_value = fields(request.headers_.view(),
        sending ? handshake_message_role::request_sender : handshake_message_role::request_recipient,
        http_connect_udp_error::invalid_request);
    if (parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    const auto& found = std::get<0>(parsed_value);
    if (sending && found.capsule_protocol_ != capsule_protocol_state::valid_true) {
        return http_connect_udp_error::invalid_capsule_protocol;
    }
    if (found.forbidden_ || request.authority_.empty() || request.path_.empty()) {
        return http_connect_udp_error::invalid_request;
    }
    if (request.version_ == http_protocol_version::http10) {
        return http_connect_udp_error::invalid_request;
    }
    if (found.host_) {
        const auto default_port = detail::http_uri_scheme_default_port(request.scheme_);
        if (!http_authorities_equal(request.authority_, found.host_value_, default_port)) {
            return http_connect_udp_error::invalid_request;
        }
    }
    if (request.version_ != http_protocol_version::http11) {
        if (request.method_.view() != "CONNECT" || request.protocol_.view() != "connect-udp" ||
            !is_http_or_https_scheme(request.scheme_) || found.connection_field_ ||
            found.upgrade_field_ || found.http2_forbidden_) {
            return http_connect_udp_error::invalid_request;
        }
    } else if (request.method_.view() != "GET" ||
               (!request.scheme_.empty() && !is_http_or_https_scheme(request.scheme_)) ||
               !found.upgrade_ || !found.connection_ || !found.host_) {
        return http_connect_udp_error::invalid_request;
    }
    return std::nullopt;
}
}  // namespace
std::optional<http_connect_udp_error> validate_http_connect_udp_request(http_connect_udp_request_view request) noexcept {
    return validate_request(request, false);
}
std::optional<http_connect_udp_error> validate_http_connect_udp_request_to_send(
    http_connect_udp_request_view request) noexcept {
    return validate_request(request, true);
}
std::optional<http_connect_udp_error> validate_http_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const http_header_view> headers) noexcept {
    return validate_connect_udp_response(version, status, headers);
}
std::optional<http_connect_udp_error> validate_http_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const http_header> headers) noexcept {
    return validate_connect_udp_response(version, status, headers);
}
bool is_http_connect_udp_upgrade_request(const http_request& request) noexcept {
    if (request.protocol_version() != http_protocol_version::http11 ||
        request.known_method() != http_known_method::get) {
        return false;
    }
    detail::http_upgrade_protocols upgrade_protocols;
    bool connect_udp = false;
    const bool valid = detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::upgrade, [&](std::string_view value) noexcept {
            return upgrade_protocols.parse_field(value, detail::http_field_list_role::recipient,
                       [&connect_udp](const detail::http_upgrade_protocol& protocol) noexcept {
                           if (protocol.version_.empty() &&
                               http_ascii_equals_ignore_case(protocol.name_, "connect-udp")) {
                               connect_udp = true;
                           }
                           return true;
                       }) == detail::http_field_list_parse_status::ok;
        });
    return valid && upgrade_protocols.has_protocol() && connect_udp;
}
std::optional<http_connect_udp_error> validate_http_connect_udp_request(const http_request& request) noexcept {
    if (request.target_form() == http_request_target_form::absolute &&
        !detail::is_valid_uri_component(request.query_string(), true, true)) {
        return http_connect_udp_error::invalid_request;
    }
    const auto path = request.target_form() == http_request_target_form::absolute
                          ? request.path()
                          : request.target();
    return validate_http_connect_udp_request({.version_ = request.protocol_version(), .method_ = request.method(), .protocol_ = request.protocol(), .scheme_ = request.scheme(), .authority_ = request.authority().empty() ? detail::http_request_access::known_header(request, detail::http_field_kind::host) : request.authority(), .path_ = path, .headers_ = request.headers()});
}
std::variant<http_response, http_connect_udp_error> prepare_http_connect_udp_response(http_response response, http_protocol_version version) {
    if (version == http_protocol_version::http10 || !response.status().is_successful() ||
        forbids_capsule_protocol(response.status().value()) || response.file_body() || !response.body_bytes().empty()) {
        return http_connect_udp_error::invalid_response;
    }
    // The response's own fields go through the one CONNECT-UDP handshake walk;
    // the protocol, not the application, generates the Upgrade pair.
    const auto parsed_value = fields(std::span(response.headers().begin(), response.headers().size()),
        handshake_message_role::response_sender, http_connect_udp_error::invalid_response);
    if (parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    const auto& found = std::get<0>(parsed_value);
    if (found.upgrade_field_ || found.connection_field_ || found.forbidden_ ||
        (version != http_protocol_version::http11 && found.http2_forbidden_)) {
        return http_connect_udp_error::invalid_response;
    }
    // As the sender, the planner only lets a single ?1 through.
    if (found.capsule_protocol_ == capsule_protocol_state::invalid) {
        return http_connect_udp_error::invalid_capsule_protocol;
    }
    if (found.capsule_protocol_ == capsule_protocol_state::absent) {
        response.header_stable_view("Capsule-Protocol", "?1");
    }
    if (version == http_protocol_version::http11) {
        response.status_code_ = http_status::switching_protocols;
        response.header_stable_view(
            detail::http_connection_field_name, detail::http_connection_upgrade_option);
        response.header_stable_view("Upgrade", "connect-udp");
    }
    return response;
}
http1_response_head_plan prepare_http1_connect_udp_response_head(const http_response& response) {
    if (response.status() != http_status::switching_protocols || response.file_body() || !response.body_bytes().empty() ||
        validate_connect_udp_response(http_protocol_version::http11, response.status().value(),
            std::span(response.headers().begin(), response.headers().size()))) {
        throw http1_response_head_error("invalid HTTP/1 CONNECT-UDP response head");
    }
    return http1_close_delimited_response_stream_head_plan(plan_http_response_body(http_known_method::get, response.status()), http1_request_connection_plan::http11_close());
}
}  // namespace ruvia

namespace ruvia {
namespace {
// CONNECT-UDP context ID 0 (RFC 9298 section 5) as its one-byte variable-length integer.
constexpr std::string_view udp_payload_context_id{"\0", 1};
}  // namespace
// One DATAGRAM capsule (RFC 9297 section 3.5) whose value is `context_prefix`
// followed by the borrowed payload.
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::plan_datagram_capsule(
    std::span<const char> payload_value, std::string_view context_prefix) noexcept {
    if (payload_value.size() > detail::quic_varint_max - context_prefix.size()) {
        return http_datagram_error::payload_too_large;
    }
    http_datagram_write_plan plan;
    plan.payload_ = payload_value;
    const auto header = encode_http_capsule_header(
        plan.prefix_, http_datagram_capsule_type, context_prefix.size() + payload_value.size());
    if (header.index() != 0) {
        return http_datagram_error::payload_too_large;
    }
    plan.prefix_size_ = std::get<0>(header);
    std::ranges::copy(context_prefix, plan.prefix_.data() + plan.prefix_size_);
    plan.prefix_size_ += context_prefix.size();
    return plan;
}
std::optional<bool> parse_http_capsule_protocol(std::string_view value) noexcept {
    detail::http_structured_parser parser{detail::http_structured_text_input{value}};
    parser.spaces();
    detail::http_structured_item item;
    if (!parser.item(item) || !parser.parameters()) {
        return std::nullopt;
    }
    // RFC 8941 4.2: only SP surrounds a top-level Item; OWS (with HTAB)
    // belongs between List and Dictionary members.
    parser.spaces();
    if (!parser.empty() || item.kind_ != detail::http_structured_item::kind_type::boolean) {
        return std::nullopt;
    }
    return item.boolean_;
}

std::optional<http_udp_datagram_view> decode_http_udp_datagram(std::span<const char> input) noexcept {
    auto id = detail::decode_quic_varint(input);
    if (id.index() != 0) {
        return std::nullopt;
    }
    return http_udp_datagram_view{std::get<0>(id).value_, input.subspan(std::get<0>(id).encoded_bytes_)};
}
std::variant<std::size_t, http_capsule_codec_error> encode_http_udp_datagram_prefix(std::span<char> output, std::uint64_t context_id) noexcept {
    return detail::encode_quic_varint(output, context_id);
}
std::variant<std::size_t, http_capsule_codec_error> encode_http_capsule_header(std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept {
    if (type > detail::quic_varint_max || length > detail::quic_varint_max) {
        return http_capsule_codec_error::value_out_of_range;
    }
    const auto type_size = detail::quic_varint_size(type);
    const auto length_size = detail::quic_varint_size(length);
    if (output.size() < type_size + length_size) {
        return http_capsule_codec_error::output_too_small;
    }
    (void)detail::encode_quic_varint(output, type);
    (void)detail::encode_quic_varint(output.subspan(type_size), length);
    return type_size + length_size;
}
http_capsule_decoder::http_capsule_decoder(http_capsule_config config)
    : config_(config) {
    if (config.max_capsule_length_ > detail::quic_varint_max) {
        throw std::invalid_argument("capsule length exceeds varint range");
    }
}
http_capsule_status http_capsule_decoder::feed(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, http_capsule_retention retention) {
    return feed_impl(input, fin, callback_value, context_value, false, retention).status_;
}
http_capsule_feed_result http_capsule_decoder::feed_one(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, http_capsule_retention retention) {
    return feed_impl(input, fin, callback_value, context_value, true, retention);
}
http_capsule_feed_result http_capsule_decoder::feed_impl(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, bool one, http_capsule_retention retention) {
    if (feeding_) {
        throw std::logic_error("recursive capsule feed");
    }
    if (status_ != http_capsule_status::need_more_data) {
        return {.status_ = status_};
    }
    struct feeding_guard final {
        bool& value_;
        explicit feeding_guard(bool& value) noexcept
            : value_(value) {
            value_ = true;
        }
        ~feeding_guard() {
            value_ = false;
        }
    } feeding_scope(feeding_);
    const auto initial_size = input.size();
    bool completed{};
    const auto limit_reached = [&] {
        status_ = http_capsule_status::limit;
        return http_capsule_feed_result{.status_ = status_, .consumed_bytes_ = initial_size - input.size()};
    };
    try {
        while (!input.empty()) {
            if (!payload_) {
                header_[header_size_++] = input.front();
                input = input.subspan(1);
                if (type_size_ == 0) {
                    auto type = detail::decode_quic_varint(std::span<const char>(header_).first(header_size_));
                    if ((type.index() != 0)) {
                        continue;
                    }
                    type_ = std::get<0>(type).value_;
                    type_size_ = std::get<0>(type).encoded_bytes_;
                }
                auto length = detail::decode_quic_varint(std::span<const char>(header_).subspan(type_size_, header_size_ - type_size_));
                if ((length.index() != 0)) {
                    continue;
                }
                remaining_ = std::get<0>(length).value_;
                header_size_ = 0;
                type_size_ = 0;
                const bool kept_type = retention == http_capsule_retention::all || type_ == http_datagram_capsule_type;
                if (!kept_type) {
                    // RFC 9297 section 3.2: drop and skip over it, at any length.
                    mode_ = payload_mode::skip;
                } else if (retention == http_capsule_retention::udp_payloads && remaining_ != 0) {
                    // RFC 9298 section 4: an unknown Context ID drops the
                    // datagram, so the limit waits for the Context ID.
                    mode_ = payload_mode::context_id;
                    context_id_size_ = 0;
                } else if (remaining_ > config_.max_capsule_length_) {
                    return limit_reached();
                } else {
                    mode_ = payload_mode::deliver;
                }
                payload_ = remaining_ != 0;
                if (!payload_ && kept_type) {
                    if (callback_value) {
                        callback_value(context_value, {type_, {}, true});
                    }
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            } else if (mode_ == payload_mode::skip) {
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), remaining_));
                input = input.subspan(count);
                remaining_ -= count;
                payload_ = remaining_ != 0;
            } else if (mode_ == payload_mode::context_id) {
                context_id_[context_id_size_++] = input.front();
                input = input.subspan(1);
                --remaining_;
                const auto context_id = detail::decode_quic_varint(std::span<const char>(context_id_).first(context_id_size_));
                if (context_id.index() != 0 && remaining_ != 0) {
                    continue;
                }
                payload_ = remaining_ != 0;
                if (context_id.index() == 0 && std::get<0>(context_id).value_ != 0) {
                    mode_ = payload_mode::skip;
                    continue;
                }
                // Context ID 0, or a value too short to hold its Context ID.
                if (context_id_size_ + remaining_ > config_.max_capsule_length_) {
                    return limit_reached();
                }
                mode_ = payload_mode::deliver;
                if (callback_value) {
                    callback_value(context_value, {type_, std::span<const char>(context_id_).first(context_id_size_), !payload_});
                }
                if (!payload_) {
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            } else {
                auto count = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), remaining_));
                auto part = input.first(count);
                input = input.subspan(count);
                remaining_ -= count;
                payload_ = remaining_ != 0;
                if (callback_value) {
                    callback_value(context_value, {type_, part, !payload_});
                }
                if (!payload_) {
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            }
        }
        if (fin && input.empty()) {
            status_ = payload_ || header_size_ ? http_capsule_status::truncated : http_capsule_status::end;
        }
        return {.status_ = status_, .consumed_bytes_ = initial_size - input.size(), .capsule_complete_ = completed};
    } catch (...) {
        status_ = http_capsule_status::truncated;
        throw;
    }
}
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::prepare_datagram(
    std::span<const char> payload_value) const noexcept {
    if (!send_open_) {
        return http_datagram_error::send_closed;
    }
    return plan_datagram_capsule(payload_value, {});
}
std::optional<std::span<const char>> http_datagram_session::receive_datagram(
    std::span<const char> input) const noexcept {
    if (!receive_open_) {
        return std::nullopt;
    }
    return input;
}
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::prepare_udp_datagram(
    std::span<const char> payload_value) const noexcept {
    if (!send_open_) {
        return http_datagram_error::send_closed;
    }
    if (payload_value.size() > 65527) {
        return http_datagram_error::payload_too_large;
    }
    return plan_datagram_capsule(payload_value, udp_payload_context_id);
}
std::variant<std::optional<http_udp_datagram_view>, http_datagram_error> http_datagram_session::receive_udp_datagram(
    std::span<const char> input) const noexcept {
    if (!receive_open_) {
        return std::nullopt;
    }
    const auto udp = decode_http_udp_datagram(input);
    if (!udp.has_value()) {
        return http_datagram_error::malformed;
    }
    if (udp->context_id_ != 0) {
        return std::nullopt;
    }
    if (udp->payload_.size() > 65527) {
        return http_datagram_error::payload_too_large;
    }
    return *udp;
}
}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] constexpr std::uint32_t rotl32(std::uint32_t value, unsigned int shift) noexcept {
    return (value << shift) | (value >> (32U - shift));
}

[[nodiscard]] constexpr std::uint32_t rotr32(std::uint32_t value, unsigned int shift) noexcept {
    return (value >> shift) | (value << (32U - shift));
}

[[nodiscard]] std::array<char, 32> md5_hex_lower(std::string_view input) noexcept {
    static constexpr std::array<std::uint32_t, 64> k_table = {
        0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU,
        0xa8304613U, 0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
        0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U,
        0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
        0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U,
        0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
        0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU,
        0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
        0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U,
        0xffeff47dU, 0x85845dd1U, 0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
        0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U};
    static constexpr std::array<unsigned int, 64> s_table = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    std::uint32_t a0 = 0x67452301U;
    std::uint32_t b0 = 0xefcdab89U;
    std::uint32_t c0 = 0x98badcfeU;
    std::uint32_t d0 = 0x10325476U;

    const auto process_block = [&](const std::uint8_t* block) noexcept {
        std::array<std::uint32_t, 16> m{};
        for (std::size_t i = 0; i < 16; ++i) {
            m[i] = static_cast<std::uint32_t>(block[i * 4]) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8U) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16U) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24U);
        }
        std::uint32_t a = a0;
        std::uint32_t b = b0;
        std::uint32_t c = c0;
        std::uint32_t d = d0;
        for (std::size_t i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            std::size_t g = 0;
            if (i < 16) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | ((~d) & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | (~d));
                g = (7 * i) % 16;
            }
            const std::uint32_t temp = d;
            d = c;
            c = b;
            b = b + rotl32(a + f + k_table[i] + m[g], s_table[i]);
            a = temp;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    };

    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t offset = 0;
    while (input.size() - offset >= 64) {
        process_block(bytes + offset);
        offset += 64;
    }

    std::array<std::uint8_t, 128> tail{};
    const std::size_t rem = input.size() - offset;
    if (rem != 0) {
        std::memcpy(tail.data(), bytes + offset, rem);
    }
    tail[rem] = 0x80U;
    const std::size_t padded_len = rem < 56 ? 64 : 128;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(input.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[padded_len - 8 + i] = static_cast<std::uint8_t>((bit_len >> (i * 8U)) & 0xffU);
    }
    process_block(tail.data());
    if (padded_len == 128) {
        process_block(tail.data() + 64);
    }

    std::array<std::uint8_t, 16> digest{};
    const std::array<std::uint32_t, 4> words = {a0, b0, c0, d0};
    for (std::size_t i = 0; i < 4; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(words[i] & 0xffU);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((words[i] >> 8U) & 0xffU);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((words[i] >> 16U) & 0xffU);
        digest[i * 4 + 3] = static_cast<std::uint8_t>((words[i] >> 24U) & 0xffU);
    }

    static constexpr char hex_digits[] = "0123456789abcdef";
    std::array<char, 32> out{};
    for (std::size_t i = 0; i < 16; ++i) {
        out[i * 2] = hex_digits[(digest[i] >> 4U) & 0x0fU];
        out[i * 2 + 1] = hex_digits[digest[i] & 0x0fU];
    }
    return out;
}

[[nodiscard]] std::array<char, 64> sha256_hex_lower(std::string_view input) noexcept {
    static constexpr std::array<std::uint32_t, 64> k_table = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
        0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
        0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
        0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    std::array<std::uint32_t, 8> state = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

    const auto process_block = [&](const std::uint8_t* block) noexcept {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            const std::uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + s1 + ch + k_table[i] + w[i];
            const std::uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    };

    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t offset = 0;
    while (input.size() - offset >= 64) {
        process_block(bytes + offset);
        offset += 64;
    }

    std::array<std::uint8_t, 128> tail{};
    const std::size_t rem = input.size() - offset;
    if (rem != 0) {
        std::memcpy(tail.data(), bytes + offset, rem);
    }
    tail[rem] = 0x80U;
    const std::size_t padded_len = rem < 56 ? 64 : 128;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(input.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[padded_len - 1 - i] = static_cast<std::uint8_t>((bit_len >> (i * 8U)) & 0xffU);
    }
    process_block(tail.data());
    if (padded_len == 128) {
        process_block(tail.data() + 64);
    }

    static constexpr char hex_digits[] = "0123456789abcdef";
    std::array<char, 64> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t b = 0; b < 4; ++b) {
            const auto byte =
                static_cast<std::uint8_t>((state[i] >> ((3U - b) * 8U)) & 0xffU);
            const std::size_t pos = (i * 4 + b) * 2;
            out[pos] = hex_digits[(byte >> 4U) & 0x0fU];
            out[pos + 1] = hex_digits[byte & 0x0fU];
        }
    }
    return out;
}

template <typename int_type>
void append_decimal_list(std::string& out, std::span<const int_type> items) {
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            out.push_back('-');
        }
        http_append_unsigned(out, static_cast<std::uint64_t>(items[index]));
    }
}

void append_hex_u16(std::string& out, std::uint16_t value) {
    static constexpr char hex_digits[] = "0123456789abcdef";
    out.push_back(hex_digits[(value >> 12U) & 0x0fU]);
    out.push_back(hex_digits[(value >> 8U) & 0x0fU]);
    out.push_back(hex_digits[(value >> 4U) & 0x0fU]);
    out.push_back(hex_digits[value & 0x0fU]);
}

[[nodiscard]] std::string_view ja4_version_code(std::uint16_t version) noexcept {
    switch (version) {
        case 0x0304U:
            return "13";
        case 0x0303U:
            return "12";
        case 0x0302U:
            return "11";
        case 0x0301U:
            return "10";
        case 0x0300U:
            return "s3";
        case 0x0002U:
            return "s2";
        case 0xfeffU:
            return "d1";
        case 0xfefdU:
            return "d2";
        case 0xfefcU:
            return "d3";
        default:
            return "00";
    }
}

[[nodiscard]] constexpr bool is_ascii_alnum(char ch) noexcept {
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

}  // namespace

std::uint16_t tls_client_hello_info::version() const noexcept {
    if (!supported_versions_.empty()) {
        std::uint16_t highest = 0;
        for (const auto ver : supported_versions_) {
            if (ver > highest) {
                highest = ver;
            }
        }
        return highest;
    }
    return legacy_version_;
}

std::string tls_client_hello_info::ja3_raw() const {
    std::string out;
    http_append_unsigned(out, legacy_version_);
    out.push_back(',');
    append_decimal_list<std::uint16_t>(out, cipher_suites_);
    out.push_back(',');
    append_decimal_list<std::uint16_t>(out, extensions_);
    out.push_back(',');
    append_decimal_list<std::uint16_t>(out, supported_groups_);
    out.push_back(',');
    append_decimal_list<std::uint8_t>(out, ec_point_formats_);
    return out;
}

std::string tls_client_hello_info::ja3() const {
    const auto raw = ja3_raw();
    const auto digest = md5_hex_lower(raw);
    return std::string(digest.data(), digest.size());
}

std::string tls_client_hello_info::ja4(bool quic) const {
    std::string out;
    out.reserve(36);

    out.push_back(quic ? 'q' : 't');
    out.append(ja4_version_code(version()));

    const bool has_domain_sni =
        !server_name_.empty() && !ip_address::parse(server_name_).has_value();
    out.push_back(has_domain_sni ? 'd' : 'i');

    const auto append_two_digits = [&out](std::size_t count) {
        const auto clamped = (std::min)(std::size_t{99}, count);
        out.push_back(static_cast<char>('0' + (clamped / 10U)));
        out.push_back(static_cast<char>('0' + (clamped % 10U)));
    };
    append_two_digits(cipher_suites_.size());
    append_two_digits(extensions_.size());

    if (alpn_.empty() || alpn_.front().empty()) {
        out.append("00");
    } else {
        const std::string_view first_alpn = alpn_.front();
        const char first_char = first_alpn.front();
        const char last_char = first_alpn.back();
        if (is_ascii_alnum(first_char) && is_ascii_alnum(last_char)) {
            out.push_back(first_char);
            out.push_back(last_char);
        } else {
            static constexpr char hex_digits[] = "0123456789abcdef";
            const auto first_byte = static_cast<unsigned char>(first_char);
            out.push_back(hex_digits[(first_byte >> 4U) & 0x0fU]);
            out.push_back(hex_digits[first_byte & 0x0fU]);
        }
    }
    out.push_back('_');

    if (cipher_suites_.empty()) {
        out.append("000000000000");
    } else {
        std::vector<std::uint16_t> sorted_ciphers(cipher_suites_);
        std::ranges::sort(sorted_ciphers);
        std::string cipher_list;
        cipher_list.reserve(sorted_ciphers.size() * 5U);
        for (std::size_t index = 0; index < sorted_ciphers.size(); ++index) {
            if (index != 0) {
                cipher_list.push_back(',');
            }
            append_hex_u16(cipher_list, sorted_ciphers[index]);
        }
        const auto hash_b = sha256_hex_lower(cipher_list);
        out.append(hash_b.data(), 12U);
    }
    out.push_back('_');

    std::vector<std::uint16_t> filtered_exts;
    filtered_exts.reserve(extensions_.size());
    for (const auto ext : extensions_) {
        if (ext != 0x0000U && ext != 0x0010U) {
            filtered_exts.push_back(ext);
        }
    }
    std::ranges::sort(filtered_exts);

    if (filtered_exts.empty() && signature_algorithms_.empty()) {
        out.append("000000000000");
    } else {
        std::string ext_sig_list;
        for (std::size_t index = 0; index < filtered_exts.size(); ++index) {
            if (index != 0) {
                ext_sig_list.push_back(',');
            }
            append_hex_u16(ext_sig_list, filtered_exts[index]);
        }
        if (!signature_algorithms_.empty()) {
            ext_sig_list.push_back('_');
            for (std::size_t index = 0; index < signature_algorithms_.size(); ++index) {
                if (index != 0) {
                    ext_sig_list.push_back(',');
                }
                append_hex_u16(ext_sig_list, signature_algorithms_[index]);
            }
        }
        const auto hash_c = sha256_hex_lower(ext_sig_list);
        out.append(hash_c.data(), 12U);
    }

    return out;
}

std::string tls_client_hello_info::peetprint() const {
    std::string out;
    if (!supported_versions_.empty()) {
        append_decimal_list<std::uint16_t>(out, supported_versions_);
    } else {
        http_append_unsigned(out, legacy_version_);
    }
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, cipher_suites_);
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, extensions_);
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, supported_groups_);
    out.push_back('|');
    append_decimal_list<std::uint8_t>(out, ec_point_formats_);
    out.push_back('|');
    for (std::size_t index = 0; index < alpn_.size(); ++index) {
        if (index != 0) {
            out.push_back('-');
        }
        out.append(alpn_[index]);
    }
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, signature_algorithms_);
    return out;
}

std::string tls_client_hello_info::peetprint_hash() const {
    const auto raw = peetprint();
    const auto digest = md5_hex_lower(raw);
    return std::string(digest.data(), digest.size());
}

tls_client_hello_fingerprints tls_client_hello_info::fingerprints(bool quic) const {
    tls_client_hello_fingerprints result;
    result.ja3_raw_ = ja3_raw();
    const auto ja3_md5 = md5_hex_lower(result.ja3_raw_);
    result.ja3_.assign(ja3_md5.data(), ja3_md5.size());
    result.ja4_ = ja4(quic);
    result.peetprint_ = peetprint();
    const auto peet_md5 = md5_hex_lower(result.peetprint_);
    result.peetprint_hash_.assign(peet_md5.data(), peet_md5.size());
    return result;
}

std::optional<tls_client_hello_info> parse_tls_client_hello(
    std::span<const std::byte> message) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(message.data());
    std::size_t size = message.size();

    // Unwrap optional 5-byte TLS record header (ContentType::handshake = 0x16).
    if (size >= 9 && data[0] == 0x16U && data[1] == 0x03U) {
        const std::size_t record_len =
            (static_cast<std::size_t>(data[3]) << 8U) | static_cast<std::size_t>(data[4]);
        if (record_len == size - 5U && data[5] == 0x01U) {
            data += 5U;
            size -= 5U;
        }
    }

    // Handshake message header: msg_type == 1 (ClientHello) + 3-byte length.
    if (size < 4 || data[0] != 0x01U) {
        return std::nullopt;
    }
    const std::size_t body_len = (static_cast<std::size_t>(data[1]) << 16U) |
                                 (static_cast<std::size_t>(data[2]) << 8U) |
                                 static_cast<std::size_t>(data[3]);
    if (body_len != size - 4U) {
        return std::nullopt;
    }

    const std::uint8_t* const body = data + 4U;
    std::size_t cursor = 0;
    const auto read_u8 = [&](std::uint8_t& out) noexcept {
        if (cursor + 1U > body_len) {
            return false;
        }
        out = body[cursor++];
        return true;
    };
    const auto read_u16 = [&](std::uint16_t& out) noexcept {
        if (cursor + 2U > body_len) {
            return false;
        }
        out = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(body[cursor]) << 8U) | body[cursor + 1U]);
        cursor += 2U;
        return true;
    };

    tls_client_hello_info info;
    if (!read_u16(info.legacy_version_)) {
        return std::nullopt;
    }
    // 32-byte ClientHello random.
    if (cursor + 32U > body_len) {
        return std::nullopt;
    }
    cursor += 32U;

    // Session ID (0..32 bytes).
    std::uint8_t session_id_len = 0;
    if (!read_u8(session_id_len) || session_id_len > 32U || cursor + session_id_len > body_len) {
        return std::nullopt;
    }
    cursor += session_id_len;

    // Cipher suites (2-byte byte length, non-empty, even).
    std::uint16_t cipher_bytes = 0;
    if (!read_u16(cipher_bytes) || cipher_bytes < 2U || (cipher_bytes % 2U) != 0 ||
        cursor + cipher_bytes > body_len) {
        return std::nullopt;
    }
    const std::size_t cipher_count = cipher_bytes / 2U;
    info.cipher_suites_.reserve(cipher_count);
    for (std::size_t i = 0; i < cipher_count; ++i) {
        std::uint16_t suite = 0;
        (void)read_u16(suite);
        if (!is_tls_grease_value(suite)) {
            info.cipher_suites_.push_back(suite);
        }
    }

    // Compression methods (1-byte length, non-empty).
    std::uint8_t comp_len = 0;
    if (!read_u8(comp_len) || comp_len == 0 || cursor + comp_len > body_len) {
        return std::nullopt;
    }
    cursor += comp_len;

    if (cursor == body_len) {
        return info;
    }

    std::uint16_t extensions_len = 0;
    if (!read_u16(extensions_len) || cursor + extensions_len != body_len) {
        return std::nullopt;
    }

    while (cursor < body_len) {
        std::uint16_t ext_type = 0;
        std::uint16_t ext_len = 0;
        if (!read_u16(ext_type) || !read_u16(ext_len) || cursor + ext_len > body_len) {
            return std::nullopt;
        }
        const std::uint8_t* const ext_data = body + cursor;
        cursor += ext_len;

        if (is_tls_grease_value(ext_type)) {
            continue;
        }
        info.extensions_.push_back(ext_type);

        switch (ext_type) {
            case 0x0000U: {  // server_name
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len =
                    (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len != ext_len - 2U) {
                    return std::nullopt;
                }
                std::size_t pos = 2U;
                while (pos < ext_len) {
                    if (pos + 3U > ext_len) {
                        return std::nullopt;
                    }
                    const std::uint8_t name_type = ext_data[pos];
                    const std::size_t name_len =
                        (static_cast<std::size_t>(ext_data[pos + 1U]) << 8U) | ext_data[pos + 2U];
                    pos += 3U;
                    if (name_len == 0 || pos + name_len > ext_len) {
                        return std::nullopt;
                    }
                    if (name_type == 0 && info.server_name_.empty()) {
                        info.server_name_.assign(
                            reinterpret_cast<const char*>(ext_data + pos), name_len);
                    }
                    pos += name_len;
                }
                break;
            }
            case 0x000aU: {  // supported_groups
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len =
                    (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len != ext_len - 2U || (list_len % 2U) != 0) {
                    return std::nullopt;
                }
                for (std::size_t pos = 2U; pos < ext_len; pos += 2U) {
                    const auto group = static_cast<std::uint16_t>(
                        (static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]);
                    if (!is_tls_grease_value(group)) {
                        info.supported_groups_.push_back(group);
                    }
                }
                break;
            }
            case 0x000bU: {  // ec_point_formats
                if (ext_len < 1U) {
                    return std::nullopt;
                }
                const std::size_t list_len = ext_data[0];
                if (list_len == 0 || list_len != ext_len - 1U) {
                    return std::nullopt;
                }
                for (std::size_t pos = 1U; pos < ext_len; ++pos) {
                    info.ec_point_formats_.push_back(ext_data[pos]);
                }
                break;
            }
            case 0x000dU: {  // signature_algorithms
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len =
                    (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len != ext_len - 2U || (list_len % 2U) != 0) {
                    return std::nullopt;
                }
                for (std::size_t pos = 2U; pos < ext_len; pos += 2U) {
                    const auto scheme = static_cast<std::uint16_t>(
                        (static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]);
                    if (!is_tls_grease_value(scheme)) {
                        info.signature_algorithms_.push_back(scheme);
                    }
                }
                break;
            }
            case 0x0010U: {  // application_layer_protocol_negotiation
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len =
                    (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len == 0 || list_len != ext_len - 2U) {
                    return std::nullopt;
                }
                std::size_t pos = 2U;
                while (pos < ext_len) {
                    const std::size_t proto_len = ext_data[pos++];
                    if (proto_len == 0 || pos + proto_len > ext_len) {
                        return std::nullopt;
                    }
                    const bool grease_alpn =
                        proto_len == 2U &&
                        is_tls_grease_value(static_cast<std::uint16_t>(
                            (static_cast<std::uint16_t>(ext_data[pos]) << 8U) |
                            ext_data[pos + 1U]));
                    if (!grease_alpn) {
                        info.alpn_.emplace_back(
                            reinterpret_cast<const char*>(ext_data + pos), proto_len);
                    }
                    pos += proto_len;
                }
                break;
            }
            case 0x002bU: {  // supported_versions
                if (ext_len < 1U) {
                    return std::nullopt;
                }
                const std::size_t list_len = ext_data[0];
                if (list_len < 2U || list_len != ext_len - 1U || (list_len % 2U) != 0) {
                    return std::nullopt;
                }
                for (std::size_t pos = 1U; pos < ext_len; pos += 2U) {
                    const auto ver = static_cast<std::uint16_t>(
                        (static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]);
                    if (!is_tls_grease_value(ver)) {
                        info.supported_versions_.push_back(ver);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    return info;
}

std::optional<tls_client_hello_info> parse_tls_client_hello(std::string_view message) {
    return parse_tls_client_hello(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(message.data()), message.size()));
}

std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(
    std::span<const std::byte> message, bool quic) {
    const auto parsed = parse_tls_client_hello(message);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->fingerprints(quic);
}

std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(
    std::string_view message, bool quic) {
    const auto parsed = parse_tls_client_hello(message);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->fingerprints(quic);
}

}  // namespace ruvia

namespace ruvia::detail {

// RFC 6265bis section 5.6 storage limits. The sender rejects anything the
// receiving parser would truncate or drop, so set_cookie can find and replace
// cookies it emitted earlier by re-parsing them.
inline constexpr std::size_t max_cookie_name_value_bytes = 4096;
inline constexpr std::size_t max_cookie_attribute_value_bytes = 1024;

[[nodiscard]] inline bool is_valid_cookie_attribute(std::string_view value) noexcept {
    // RFC 6265bis path-value is *av-octet: ASCII %x20-3A / %x3C-7E.
    // HTTP field values can carry HTAB and obs-text, but emitting either inside
    // Path produces a Set-Cookie value outside the server grammar.
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7e || c == ';') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool is_valid_cookie_domain(std::string_view value) noexcept {
    // RFC 6265bis domain-value is an RFC 1034 subdomain as relaxed by RFC 1123:
    // ASCII LDH labels, each at most 63 octets, with a letter or digit at both
    // ends. The root separators consume the remaining two octets of DNS's
    // 255-octet wire limit, so an unqualified textual name is at most 253.
    return value.empty() || is_valid_dns_domain_name(value);
}

[[nodiscard]] inline std::string_view cookie_priority_token(cookie_priority priority) noexcept {
    switch (priority) {
        case cookie_priority::low:
            return "Low";
        case cookie_priority::medium:
            return "Medium";
        case cookie_priority::high:
            return "High";
    }
    return {};
}

[[nodiscard]] inline std::string_view cookie_same_site_token(cookie_same_site same_site) noexcept {
    switch (same_site) {
        case cookie_same_site::strict:
            return "Strict";
        case cookie_same_site::lax:
            return "Lax";
        case cookie_same_site::none:
            return "None";
    }
    return {};
}

[[nodiscard]] inline bool cookie_attribute_emitted(cookie_attribute_policy policy) {
    switch (policy) {
        case cookie_attribute_policy::omit:
            return false;
        case cookie_attribute_policy::emit:
            return true;
    }
    throw std::invalid_argument("invalid cookie attribute policy");
}

// `secure` and `partitioned` are resolved attribute policies; the plan also
// checks Max-Age before converting it to unsigned storage.
inline void validate_cookie(std::string_view name, std::string_view value, const cookie_options& options,
    bool secure, bool partitioned) {
    if (!is_valid_http_header_name(name)) {
        throw std::invalid_argument("invalid cookie name");
    }
    if (!::ruvia::is_valid_cookie_value(value)) {
        throw std::invalid_argument("invalid cookie value");
    }
    const auto prefix_size = options.prefix_ ? ::ruvia::http_cookie_prefix_text(*options.prefix_).size() : 0;
    if (name.size() > max_cookie_name_value_bytes - prefix_size ||
        value.size() > max_cookie_name_value_bytes - prefix_size - name.size()) {
        throw std::invalid_argument("cookie name and value exceed 4096 bytes");
    }
    // Receivers trim OWS around Path, so edge spaces would not round-trip and
    // trailing SP would end the Set-Cookie field value in whitespace.
    const std::string_view path = options.path_;
    if (!is_valid_cookie_attribute(path) || path.size() > max_cookie_attribute_value_bytes ||
        http_trim_ows(path).size() != path.size()) {
        throw std::invalid_argument("invalid cookie attribute");
    }
    if (!is_valid_cookie_domain(options.domain_)) {
        throw std::invalid_argument("invalid cookie domain");
    }
    if (options.expires_.has_value()) {
        using namespace std::chrono;
        constexpr auto minimum = duration_cast<seconds>(sys_days{year{1601} / January / 1}.time_since_epoch());
        const auto expires = floor<seconds>(options.expires_->time_since_epoch());
        if (expires < minimum) {
            throw std::invalid_argument("cookie Expires must not precede year 1601");
        }
    }
    if (options.priority_ && cookie_priority_token(*options.priority_).empty()) {
        throw std::invalid_argument("invalid cookie Priority");
    }
    if (options.same_site_ && cookie_same_site_token(*options.same_site_).empty()) {
        throw std::invalid_argument("invalid cookie SameSite");
    }
    if (options.same_site_ == cookie_same_site::none && !secure) {
        throw std::invalid_argument("SameSite=None cookie requires Secure");
    }
    if (options.prefix_ && ::ruvia::http_cookie_prefix_text(*options.prefix_).empty()) {
        throw std::invalid_argument("invalid cookie prefix");
    }
    if (partitioned && !secure) {
        throw std::invalid_argument("partitioned cookie requires Secure");
    }
    // User agents apply __Host-/__Secure- constraints case-insensitively. Mirror
    // that receive-side rule for literal wire names so every cookie emitted by
    // this sender can actually be stored. A typed prefix is canonical and is the
    // outermost wire prefix, so it takes precedence over bytes in `name`.
    const auto prefix = options.prefix_ ? options.prefix_ : ::ruvia::cookie_name_prefix(name);
    if (prefix && !::ruvia::cookie_prefix_requirements_met(*prefix, secure, options.path_, options.domain_)) {
        throw std::invalid_argument(*prefix == cookie_prefix::host
                                        ? "__Host- cookie requires Secure, Path=/, and no Domain"
                                        : "__Secure- cookie requires Secure");
    }
}

}  // namespace ruvia::detail

namespace ruvia {

set_cookie_plan::set_cookie_plan(
    std::string_view name, std::string_view value, const cookie_options& options)
    : name_(name),
      value_(value),
      path_(options.path_),
      domain_(options.domain_),
      prefix_text_(options.prefix_ ? http_cookie_prefix_text(*options.prefix_) : std::string_view{}),
      priority_text_(options.priority_ ? detail::cookie_priority_token(*options.priority_) : std::string_view{}),
      same_site_text_(options.same_site_ ? detail::cookie_same_site_token(*options.same_site_) : std::string_view{}),
      http_only_(detail::cookie_attribute_emitted(options.http_only_)),
      secure_(detail::cookie_attribute_emitted(options.secure_)),
      partitioned_(detail::cookie_attribute_emitted(options.partitioned_)) {
    // max-age-av starts with a non-zero digit (RFC 6265 section 4.1.1);
    // an immediate expiry is written as the past deletion Expires instead.
    const bool expire_now = options.max_age_.has_value() && options.max_age_->count() == 0;
    if (expire_now || options.expires_.has_value()) {
        const auto expires = expire_now ? cookie_deletion_expires : *options.expires_;
        const auto seconds = std::chrono::floor<std::chrono::seconds>(expires.time_since_epoch()).count();
        if (!std::in_range<std::time_t>(seconds)) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        const auto date = format_http_date(static_cast<std::time_t>(seconds));
        if (!date) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        expires_size_ = date->size();
        std::memcpy(expires_buffer_.data(), date->data(), expires_size_);
    }
    if (options.max_age_.has_value() && options.max_age_->count() != 0) {
        if (options.max_age_->count() < 0) {
            throw std::invalid_argument("cookie Max-Age must not be negative");
        }
        max_age_value_ = static_cast<std::uint64_t>(options.max_age_->count());
    }

    const auto add_size = [this](std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - size_) {
            throw std::length_error("Set-Cookie value is too large");
        }
        size_ += amount;
    };

    // Compute the complete wire length before validating the borrowed views.
    // This keeps a hostile oversized view from reaching a grammar scan and
    // prevents the later uninitialized response-header write from receiving a
    // wrapped length.
    add_size(prefix_text_.size());
    add_size(name_.size());
    add_size(1);
    add_size(value_.size());
    if (!path_.empty()) {
        add_size(std::string_view("; Path=").size());
        add_size(path_.size());
    }
    if (!domain_.empty()) {
        add_size(std::string_view("; Domain=").size());
        add_size(domain_.size());
    }
    if (max_age_value_ != 0) {
        add_size(std::string_view("; Max-Age=").size());
        add_size(http_unsigned_decimal_size(max_age_value_));
    }
    if (expires_size_ != 0) {
        add_size(std::string_view("; Expires=").size());
        add_size(expires_size_);
    }
    if (http_only_) {
        add_size(std::string_view("; HttpOnly").size());
    }
    if (secure_) {
        add_size(std::string_view("; Secure").size());
    }
    if (!same_site_text_.empty()) {
        add_size(std::string_view("; SameSite=").size());
        add_size(same_site_text_.size());
    }
    if (!priority_text_.empty()) {
        add_size(std::string_view("; Priority=").size());
        add_size(priority_text_.size());
    }
    if (partitioned_) {
        add_size(std::string_view("; Partitioned").size());
    }

    detail::validate_cookie(name, value, options, secure_, partitioned_);
}

char* write_cookie_request_pair(char* out, std::string_view name, std::string_view value) {
    const std::string_view written(out, cookie_request_pair_size(name, value));
    const bool name_overlaps = detail::http_text_overlaps(name, written);
    const bool value_overlaps = detail::http_text_overlaps(value, written);
    if (name_overlaps || value_overlaps) {
        // Snapshot only bytes that writing the pair could overwrite.
        const auto name_bytes = name_overlaps ? name.size() : 0;
        const auto value_bytes = value_overlaps ? value.size() : 0;
        std::array<char, 256> stack_snapshot;
        if (value_bytes <= stack_snapshot.size() &&
            name_bytes <= stack_snapshot.size() - value_bytes) {
            if (name_overlaps) {
                std::memcpy(stack_snapshot.data(), name.data(), name_bytes);
            }
            if (value_overlaps) {
                std::memcpy(stack_snapshot.data() + name_bytes, value.data(), value_bytes);
            }
            return write_cookie_request_pair(out,
                name_overlaps ? std::string_view(stack_snapshot.data(), name_bytes) : name,
                value_overlaps ? std::string_view(stack_snapshot.data() + name_bytes, value_bytes) : value);
        }
        std::string snapshot;
        snapshot.reserve(name_bytes + value_bytes);
        if (name_overlaps) {
            snapshot.append(name);
        }
        if (value_overlaps) {
            snapshot.append(value);
        }
        return write_cookie_request_pair(out,
            name_overlaps ? std::string_view(snapshot.data(), name_bytes) : name,
            value_overlaps ? std::string_view(snapshot.data() + name_bytes, value_bytes) : value);
    }
    detail::http_raw_wire_sink sink(out);
    sink.append(name);
    sink.append('=');
    sink.append(value);
    return sink.position();
}

void set_cookie_plan::write(char* cursor_value) const {
    (void)write_fields(cursor_value);
}

set_cookie_plan::written_fields set_cookie_plan::write_fields(char* cursor_value) const noexcept {
    written_fields fields;
    detail::http_raw_wire_sink sink(cursor_value);
    const auto* const name_begin = sink.position();
    sink.append(prefix_text_);
    sink.append(name_);
    fields.wire_name_ = {name_begin, static_cast<std::size_t>(sink.position() - name_begin)};
    sink.append('=');
    sink.append(value_);
    if (!path_.empty()) {
        sink.append("; Path=");
        fields.path_ = {sink.position(), path_.size()};
        sink.append(path_);
    }
    if (!domain_.empty()) {
        sink.append("; Domain=");
        fields.domain_ = {sink.position(), domain_.size()};
        sink.append(domain_);
    }
    if (max_age_value_ != 0) {
        // The constructor sized this slice with http_unsigned_decimal_size().
        sink.append("; Max-Age=");
        sink.append_unsigned(max_age_value_);
    }
    if (expires_size_ != 0) {
        sink.append("; Expires=");
        sink.append(std::string_view(expires_buffer_.data(), expires_size_));
    }
    if (http_only_) {
        sink.append("; HttpOnly");
    }
    if (secure_) {
        sink.append("; Secure");
    }
    if (!same_site_text_.empty()) {
        sink.append("; SameSite=");
        sink.append(same_site_text_);
    }
    if (!priority_text_.empty()) {
        sink.append("; Priority=");
        sink.append(priority_text_);
    }
    if (partitioned_) {
        sink.append("; Partitioned");
    }
    return fields;
}

}  // namespace ruvia

namespace ruvia {

bool is_valid_cookie_value(std::string_view value) noexcept {
    // cookie-value = *cookie-octet / ( DQUOTE *cookie-octet DQUOTE ); the quotes
    // are part of the value (RFC 6265 section 4.1.1).
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7f || c == '"' || c == ',' || c == ';' || c == '\\') {
            return false;
        }
    }
    return true;
}

std::optional<cookie_prefix> cookie_name_prefix(std::string_view name) noexcept {
    const auto starts_with_ignore_case = [name](std::string_view prefix) noexcept {
        return name.size() >= prefix.size() && http_ascii_equals_ignore_case(name.substr(0, prefix.size()), prefix);
    };
    if (starts_with_ignore_case("__Host-")) {
        return cookie_prefix::host;
    }
    if (starts_with_ignore_case("__Secure-")) {
        return cookie_prefix::secure;
    }
    return std::nullopt;
}

bool cookie_prefix_requirements_met(
    cookie_prefix prefix, bool secure, std::string_view path, std::string_view domain) noexcept {
    switch (prefix) {
        case cookie_prefix::secure:
            return secure;
        case cookie_prefix::host:
            return secure && path == "/" && domain.empty();
    }
    return false;
}

std::string_view http_cookie_prefix_text(cookie_prefix prefix) noexcept {
    switch (prefix) {
        case cookie_prefix::secure:
            return "__Secure-";
        case cookie_prefix::host:
            return "__Host-";
    }
    return {};
}

namespace {

[[nodiscard]] bool is_cookie_ip_host(std::string_view host) noexcept {
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
        return true;
    }
    return ip_address::parse(host).has_value();
}

[[nodiscard]] std::chrono::system_clock::time_point compute_cookie_expiration(
    std::chrono::system_clock::time_point now, std::int64_t max_age_seconds) noexcept {
    using clock_type = std::chrono::system_clock;
    const auto available =
        std::chrono::duration_cast<std::chrono::seconds>(clock_type::time_point::max() - now);
    if (std::chrono::seconds(max_age_seconds) >= available) {
        return clock_type::time_point::max();
    }
    return now +
           std::chrono::duration_cast<clock_type::duration>(std::chrono::seconds(max_age_seconds));
}

[[nodiscard]] bool satisfies_cookie_name_prefix(
    const http_set_cookie_view& cookie, bool secure) noexcept {
    const auto prefix = cookie_name_prefix(cookie.name());
    return !prefix ||
           cookie_prefix_requirements_met(*prefix, secure, cookie.path(), cookie.domain());
}

}  // namespace

http_cookie_jar::http_cookie_jar(const http_origin_view& origin, http_cookie_jar_options options)
    : http_cookie_jar(origin.scheme(), origin.host(), options) {}

http_cookie_jar::http_cookie_jar(
    http_scheme scheme, std::string_view host, http_cookie_jar_options options)
    : host_(host),
      scheme_(scheme),
      max_cookies_(options.max_cookies_),
      max_cookie_bytes_(options.max_cookie_bytes_) {
    if (!host_.empty() && host_.front() == '[' && host_.back() == ']') {
        host_.erase(host_.size() - 1, 1);
        host_.erase(0, 1);
    }
}

bool http_cookie_jar::cookie_domain_matches(
    std::string_view host, std::string_view domain) noexcept {
    if (domain.empty()) {
        return true;
    }
    if (http_ascii_equals_ignore_case(host, domain)) {
        return true;
    }
    if (is_cookie_ip_host(host)) {
        return false;
    }
    return host.size() > domain.size() && host[host.size() - domain.size() - 1] == '.' &&
           http_ascii_equals_ignore_case(host.substr(host.size() - domain.size()), domain);
}

bool http_cookie_jar::cookie_path_matches(
    std::string_view request_path, std::string_view cookie_path) noexcept {
    if (cookie_path.empty() || cookie_path == "/") {
        return !request_path.empty() && request_path.front() == '/';
    }
    if (!request_path.starts_with(cookie_path)) {
        return false;
    }
    return request_path.size() == cookie_path.size() || cookie_path.back() == '/' ||
           request_path[cookie_path.size()] == '/';
}

std::string_view http_cookie_jar::request_path_only(std::string_view target) noexcept {
    return target.substr(0, target.find_first_of("?#"));
}

std::string_view http_cookie_jar::default_cookie_path(std::string_view target) noexcept {
    const auto path = request_path_only(target);
    if (path.empty() || path.front() != '/') {
        return "/";
    }
    const auto slash = path.rfind('/');
    return slash == 0 || slash == std::string_view::npos ? std::string_view("/")
                                                         : path.substr(0, slash);
}

std::size_t http_cookie_jar::entry_storage_bytes(std::string_view name, std::string_view value,
    std::string_view path, std::string_view domain) noexcept {
    std::size_t total = 0;
    for (const auto field : {name, value, path, domain}) {
        if (field.size() > (std::numeric_limits<std::size_t>::max)() - total) {
            return (std::numeric_limits<std::size_t>::max)();
        }
        total += field.size();
    }
    return total;
}

bool http_cookie_jar::has_capacity(
    std::size_t replaced_bytes, std::size_t replacement_bytes, bool adding) const noexcept {
    if (adding && cookies_.size() >= max_cookies_) {
        return false;
    }
    const auto retained_bytes = cookie_bytes_ - replaced_bytes;
    return replacement_bytes <=
           max_cookie_bytes_ - std::min(retained_bytes, max_cookie_bytes_);
}

void http_cookie_jar::reserve_cookie_slot() {
    if (cookies_.size() < cookies_.capacity()) {
        return;
    }
    const auto remaining = max_cookies_ - cookies_.size();
    const auto growth = std::min(remaining, std::max(std::size_t{4}, cookies_.size()));
    cookies_.reserve(cookies_.size() + growth);
}

void http_cookie_jar::recompute_next_expiry() noexcept {
    next_expiry_.reset();
    for (const auto& cookie : cookies_) {
        if (cookie.expires_.has_value() &&
            (!next_expiry_.has_value() || *cookie.expires_ < *next_expiry_)) {
            next_expiry_ = *cookie.expires_;
        }
    }
}

void http_cookie_jar::discard_expired(std::chrono::system_clock::time_point now) {
    std::optional<std::chrono::system_clock::time_point> earliest_expiry;
    std::erase_if(cookies_, [this, now, &earliest_expiry](const stored_cookie& cookie) {
        if (!cookie.expires_.has_value()) {
            return false;
        }
        if (*cookie.expires_ > now) {
            if (!earliest_expiry.has_value() || *cookie.expires_ < *earliest_expiry) {
                earliest_expiry = *cookie.expires_;
            }
            return false;
        }
        cookie_bytes_ -=
            entry_storage_bytes(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
        return true;
    });
    next_expiry_ = earliest_expiry;
}

void http_cookie_jar::clear() noexcept {
    cookies_.clear();
    next_expiry_.reset();
    cookie_bytes_ = 0;
}

void http_cookie_jar::add_configured_cookie(std::string_view name, std::string_view value) {
    cookies_.emplace_back(name, value);
    cookie_bytes_ += entry_storage_bytes(name, value, "/", {});
}

std::optional<http_cookie_jar::received_cookie> http_cookie_jar::accept_set_cookie_view(
    std::string_view request_target, std::string_view set_cookie_value,
    std::chrono::system_clock::time_point now) const {
    const auto parsed = parse_set_cookie(set_cookie_value);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    const bool secure = parsed->has(http_set_cookie_attribute::secure);
    const bool partitioned = parsed->has(http_set_cookie_attribute::partitioned);
    if ((secure && scheme_ != http_scheme::https) ||
        !cookie_domain_matches(host_, parsed->domain()) ||
        (parsed->same_site_none() && !secure) ||
        (partitioned && !secure) ||
        !satisfies_cookie_name_prefix(*parsed, secure)) {
        return std::nullopt;
    }

    received_cookie cookie{
        .name_ = parsed->name(),
        .value_ = parsed->value(),
        .path_ = parsed->path().empty() ? default_cookie_path(request_target) : parsed->path(),
        .domain_ = parsed->domain(),
        .expires_ = std::nullopt,
        .secure_ = secure,
        .partitioned_ = partitioned,
    };
    if (const auto max_age_seconds = parsed->max_age_seconds()) {
        cookie.expired_ = *max_age_seconds <= 0;
        if (!cookie.expired_) {
            cookie.expires_ = compute_cookie_expiration(now, *max_age_seconds);
        }
    } else if (const auto expires_at = parsed->expires()) {
        cookie.expired_ = *expires_at <= std::chrono::system_clock::to_time_t(now);
        if (!cookie.expired_) {
            const auto expiration_limit = std::chrono::system_clock::to_time_t(
                compute_cookie_expiration(now, max_cookie_age_seconds));
            cookie.expires_ =
                std::chrono::system_clock::from_time_t(std::min(*expires_at, expiration_limit));
        }
    }
    return cookie;
}

bool http_cookie_jar::store_received_cookie(const received_cookie& cookie) {
    const auto identity_domain = [this](std::string_view domain) {
        return domain.empty() ? std::string_view(host_) : domain;
    };
    const auto cookie_domain = identity_domain(cookie.domain_);
    const auto match = std::ranges::find_if(cookies_, [&](const stored_cookie& stored) {
        return !stored.configured_ && stored.name_ == cookie.name_ &&
               stored.path_ == cookie.path_ && stored.partitioned_ == cookie.partitioned_ &&
               http_ascii_equals_ignore_case(identity_domain(stored.domain_), cookie_domain);
    });
    const auto replaced_bytes =
        match == cookies_.end()
            ? 0
            : entry_storage_bytes(match->name_, match->value_, match->path_, match->domain_);
    if (cookie.expired_) {
        if (match != cookies_.end()) {
            const bool removed_earliest =
                match->expires_.has_value() && next_expiry_ == match->expires_;
            cookie_bytes_ -= replaced_bytes;
            cookies_.erase(match);
            if (removed_earliest) {
                recompute_next_expiry();
            }
        }
        return true;
    }
    const auto replacement_bytes =
        entry_storage_bytes(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
    if (!has_capacity(replaced_bytes, replacement_bytes, match == cookies_.end())) {
        return false;
    }

    if (match == cookies_.end()) {
        stored_cookie replacement(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
        replacement.expires_ = cookie.expires_;
        replacement.secure_ = cookie.secure_;
        replacement.partitioned_ = cookie.partitioned_;
        replacement.configured_ = false;
        const auto insertion = std::ranges::find_if(cookies_,
            [&cookie](const stored_cookie& stored) { return stored.path_.size() < cookie.path_.size(); });
        const auto insertion_index = insertion - cookies_.begin();
        reserve_cookie_slot();
        cookies_.emplace(cookies_.begin() + insertion_index, std::move(replacement));
        if (cookie.expires_.has_value() &&
            (!next_expiry_.has_value() || *cookie.expires_ < *next_expiry_)) {
            next_expiry_ = *cookie.expires_;
        }
    } else {
        const auto old_expires = match->expires_;
        std::optional<std::string> replacement_value;
        if (match->value_ != cookie.value_ && cookie.value_.size() > match->value_.capacity()) {
            replacement_value.emplace(cookie.value_);
        }
        std::optional<std::string> replacement_domain;
        if (match->domain_ != cookie.domain_ && cookie.domain_.size() > match->domain_.capacity()) {
            replacement_domain.emplace(cookie.domain_);
        }
        if (replacement_value.has_value()) {
            match->value_.swap(*replacement_value);
        } else if (match->value_ != cookie.value_) {
            match->value_.assign(cookie.value_);
        }
        if (replacement_domain.has_value()) {
            match->domain_.swap(*replacement_domain);
        } else if (match->domain_ != cookie.domain_) {
            match->domain_.assign(cookie.domain_);
        }
        match->expires_ = cookie.expires_;
        match->secure_ = cookie.secure_;
        match->partitioned_ = cookie.partitioned_;
        if (cookie.expires_.has_value() &&
            (!next_expiry_.has_value() || *cookie.expires_ <= *next_expiry_)) {
            next_expiry_ = *cookie.expires_;
        } else if (old_expires.has_value() && next_expiry_ == old_expires) {
            recompute_next_expiry();
        }
    }
    cookie_bytes_ = cookie_bytes_ - replaced_bytes + replacement_bytes;
    return true;
}

bool http_cookie_jar::absorb_set_cookie(std::string_view request_target,
    std::string_view set_cookie_value, std::chrono::system_clock::time_point now) {
    if (!cookies_.empty() && next_expiry_.has_value() && now >= *next_expiry_) {
        discard_expired(now);
    }
    const auto accepted = accept_set_cookie_view(request_target, set_cookie_value, now);
    if (!accepted.has_value()) {
        return false;
    }
    return store_received_cookie(*accepted);
}

void http_cookie_jar::retain_response_cookies(std::string_view request_target,
    std::span<const http_header> headers, std::chrono::system_clock::time_point now) {
    bool discarded_expired = cookies_.empty();
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "set-cookie")) {
            continue;
        }
        if (!discarded_expired) {
            discarded_expired = true;
            if (next_expiry_.has_value() && now >= *next_expiry_) {
                discard_expired(now);
            }
        }
        if (const auto accepted = accept_set_cookie_view(request_target, header.value(), now)) {
            (void)store_received_cookie(*accepted);
        }
    }
}

void http_cookie_jar::retain_response_cookies(std::string_view request_target,
    std::span<const http_header_view> headers, std::chrono::system_clock::time_point now) {
    bool discarded_expired = cookies_.empty();
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "set-cookie")) {
            continue;
        }
        if (!discarded_expired) {
            discarded_expired = true;
            if (next_expiry_.has_value() && now >= *next_expiry_) {
                discard_expired(now);
            }
        }
        if (const auto accepted = accept_set_cookie_view(request_target, header.value(), now)) {
            (void)store_received_cookie(*accepted);
        }
    }
}

std::string http_cookie_jar::cookie_header(std::string_view request_target,
    bool include_received, std::chrono::system_clock::time_point now) {
    std::string output;
    visit_matching_cookies(request_target, include_received, now,
        [&output](std::string_view name, std::string_view value) {
            append_cookie_request_pair(output, name, value);
        });
    return output;
}

}  // namespace ruvia

namespace ruvia {
namespace {

std::pair<std::string_view, std::string_view> split_attribute(std::string_view value) noexcept {
    const auto equals = value.find('=');
    if (equals == std::string_view::npos) {
        return {http_trim_ows(value), {}};
    }
    return {http_trim_ows(value.substr(0, equals)), http_trim_ows(value.substr(equals + 1))};
}

std::optional<std::int64_t> parse_max_age_seconds(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }

    std::int64_t seconds = 0;
    const auto [parsed_value, error] =
        std::from_chars(value.data(), value.data() + value.size(), seconds);
    // Even overflow must consume the complete attribute before saturation.
    if (parsed_value != value.data() + value.size()) {
        return std::nullopt;
    }
    if (error == std::errc::result_out_of_range) {
        return value.front() == '-' ? std::numeric_limits<std::int64_t>::min() : max_cookie_age_seconds;
    }
    if (error != std::errc{}) {
        return std::nullopt;
    }
    return seconds > max_cookie_age_seconds ? max_cookie_age_seconds : seconds;
}

bool contains_rejected_received_cookie_control(std::string_view value) noexcept {
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte <= 0x08 || (byte >= 0x0a && byte <= 0x1f) || byte == 0x7f) {
            return true;
        }
    }
    return false;
}

bool is_cookie_date_delimiter(unsigned char byte) noexcept {
    return byte == 0x09 || (byte >= 0x20 && byte <= 0x2f) || (byte >= 0x3b && byte <= 0x40) ||
           (byte >= 0x5b && byte <= 0x60) || (byte >= 0x7b && byte <= 0x7e);
}

// Consume only on success. Any remaining suffix starts with a non-digit,
// as required by RFC 6265 section 5.1.1 and verified erratum 4148.
std::optional<int> consume_cookie_date_digits(
    std::string_view& value, std::size_t minimum_digits, std::size_t maximum_digits) noexcept {
    std::size_t digits = 0;
    int result_value = 0;
    while (digits < value.size() && value[digits] >= '0' && value[digits] <= '9') {
        if (digits == maximum_digits) {
            return std::nullopt;
        }
        result_value = result_value * 10 + (value[digits] - '0');
        ++digits;
    }
    if (digits < minimum_digits) {
        return std::nullopt;
    }
    value.remove_prefix(digits);
    return result_value;
}

struct cookie_date_time final {
    int hour_;
    int minute_;
    int second_;
};

std::optional<cookie_date_time> parse_cookie_date_time(std::string_view value) noexcept {
    const auto hour = consume_cookie_date_digits(value, 1, 2);
    if (!hour || value.empty() || value.front() != ':') {
        return std::nullopt;
    }
    value.remove_prefix(1);
    const auto minute = consume_cookie_date_digits(value, 1, 2);
    if (!minute || value.empty() || value.front() != ':') {
        return std::nullopt;
    }
    value.remove_prefix(1);
    const auto second = consume_cookie_date_digits(value, 1, 2);
    if (!second) {
        return std::nullopt;
    }
    return cookie_date_time{*hour, *minute, *second};
}

std::optional<int> parse_cookie_date_month(std::string_view value) noexcept {
    if (value.size() < 3) {
        return std::nullopt;
    }
    value = value.substr(0, 3);
    for (std::size_t index = 0; index < detail::http_month_names.size(); ++index) {
        if (http_ascii_equals_ignore_case(value, detail::http_month_names[index])) {
            return static_cast<int>(index + 1);
        }
    }
    return std::nullopt;
}

std::optional<std::time_t> parse_cookie_date(std::string_view value) noexcept {
    std::optional<cookie_date_time> time;
    std::optional<int> day;
    std::optional<int> month;
    std::optional<int> year;

    std::size_t cursor_value = 0;
    while (cursor_value < value.size()) {
        while (cursor_value < value.size() &&
               is_cookie_date_delimiter(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        const auto begin = cursor_value;
        while (cursor_value < value.size() &&
               !is_cookie_date_delimiter(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        if (begin == cursor_value) {
            continue;
        }
        auto token = value.substr(begin, cursor_value - begin);
        if (!time) {
            time = parse_cookie_date_time(token);
            if (time) {
                continue;
            }
        }
        if (!day) {
            day = consume_cookie_date_digits(token, 1, 2);
            if (day) {
                continue;
            }
        }
        if (!month) {
            month = parse_cookie_date_month(token);
            if (month) {
                continue;
            }
        }
        if (!year) {
            year = consume_cookie_date_digits(token, 2, 4);
        }
    }

    if (!time || !day || !month || !year) {
        return std::nullopt;
    }
    if (*year >= 70 && *year <= 99) {
        *year += 1900;
    } else if (*year <= 69) {
        *year += 2000;
    }
    if (*year < 1601 || *day < 1 || *day > 31 || time->hour_ > 23 || time->minute_ > 59 ||
        time->second_ > 59) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, *month, *day, time->hour_, time->minute_, time->second_);
}

}  // namespace

std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept {
    const auto first_end = value.find(';');
    const auto cookie_pair = value.substr(0, first_end);
    // RFC 6265 section 5.2 steps 2 and 5: a pair without '=' or with an empty
    // name makes the user agent ignore the whole set-cookie-string.
    const auto equals = cookie_pair.find('=');
    if (equals == std::string_view::npos) {
        return std::nullopt;
    }
    const auto name = http_trim_ows(cookie_pair.substr(0, equals));
    const auto cookie_value = http_trim_ows(cookie_pair.substr(equals + 1));
    if (name.empty() || name.size() > detail::max_cookie_name_value_bytes ||
        cookie_value.size() > detail::max_cookie_name_value_bytes - name.size() ||
        contains_rejected_received_cookie_control(name) ||
        contains_rejected_received_cookie_control(cookie_value)) {
        return std::nullopt;
    }

    http_set_cookie_view result(name, cookie_value);
    std::size_t offset = first_end == std::string_view::npos ? value.size() : first_end + 1;
    while (offset < value.size()) {
        const auto end = value.find(';', offset);
        const auto [attribute, argument] = split_attribute(value.substr(
            offset, end == std::string_view::npos ? value.size() - offset : end - offset));
        if (contains_rejected_received_cookie_control(attribute) ||
            contains_rejected_received_cookie_control(argument)) {
            return std::nullopt;
        }
        if (argument.size() <= detail::max_cookie_attribute_value_bytes) {
            switch (attribute.size()) {
                case 4U:
                    if (http_ascii_equals_ignore_case(attribute, "Path")) {
                        // RFC 6265 section 5.2.4: a value that is empty or does not start
                        // with '/' selects the default-path; the last Path attribute wins.
                        result.path_ = argument.empty() || argument.front() != '/' ? std::string_view{} : argument;
                    }
                    break;
                case 6U:
                    if (http_ascii_equals_ignore_case(attribute, "Domain")) {
                        // RFC 6265 section 5.2.3: an empty value ignores this cookie-av; a
                        // leading '.' is dropped, so "Domain=." yields a host-only cookie.
                        // Domain-match against the request host is the storage step's job.
                        if (!argument.empty()) {
                            result.domain_ = argument.front() == '.' ? argument.substr(1) : argument;
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "Secure")) {
                        result.set(http_set_cookie_attribute::secure);
                    }
                    break;
                case 7U:
                    if (http_ascii_equals_ignore_case(attribute, "Expires")) {
                        if (const auto expires = parse_cookie_date(argument)) {
                            result.expires_ = expires;
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "Max-Age")) {
                        if (const auto seconds = parse_max_age_seconds(argument)) {
                            result.max_age_seconds_ = seconds;
                        }
                    }
                    break;
                case 8U:
                    if (http_ascii_equals_ignore_case(attribute, "SameSite")) {
                        if (http_ascii_equals_ignore_case(argument, "Strict")) {
                            result.same_site_ = cookie_same_site::strict;
                        } else if (http_ascii_equals_ignore_case(argument, "Lax")) {
                            result.same_site_ = cookie_same_site::lax;
                        } else if (http_ascii_equals_ignore_case(argument, "None")) {
                            result.same_site_ = cookie_same_site::none;
                        } else {
                            result.same_site_.reset();
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "HttpOnly")) {
                        result.set(http_set_cookie_attribute::http_only);
                    }
                    break;
                case 11U:
                    if (http_ascii_equals_ignore_case(attribute, "Partitioned")) {
                        result.set(http_set_cookie_attribute::partitioned);
                    }
                    break;
                default:
                    break;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    }
    return result;
}

}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] bool is_valid_cors_origin_value(std::string_view origin) noexcept {
    return origin == "null" || is_valid_http_serialized_origin(origin);
}

[[nodiscard]] std::string to_lower_copy(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) {
        ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
    }
    return result;
}

[[nodiscard]] std::string join_tokens(std::span<const std::string> items) {
    std::string output;
    if (items.empty()) {
        return output;
    }
    std::size_t total = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            total += 2U;
        }
        total += items[index].size();
    }
    output.reserve(total);
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            output.append(", ");
        }
        output.append(items[index]);
    }
    return output;
}

[[nodiscard]] bool vary_value_contains_token(
    std::string_view vary_value, std::string_view target_token) noexcept {
    bool found = false;
    detail::http_visit_comma_separated_tokens(vary_value, [&](std::string_view token) noexcept {
        if (token == "*" || http_ascii_equals_ignore_case(token, target_token)) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

[[nodiscard]] bool headers_have_vary_token(
    std::span<const http_header> headers, std::string_view token) noexcept {
    for (const auto& header : headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Vary") &&
            vary_value_contains_token(header.value(), token)) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool is_http_cors_controlled_response_header(std::string_view name) noexcept {
    switch (name.size()) {
        case 22U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Max-Age");
        case 27U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Origin");
        case 28U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Methods") ||
                   http_ascii_equals_ignore_case(name, "Access-Control-Allow-Headers");
        case 29U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Expose-Headers");
        case 32U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Credentials");
        default:
            return false;
    }
}

void merge_http_vary_tokens(
    std::vector<http_header>& headers, std::span<const std::string_view> tokens) {
    std::vector<std::string_view> missing;
    for (const auto token : tokens) {
        const auto trimmed = http_trim_ows(token);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed == "*") {
            if (!headers_have_vary_token(headers, "*")) {
                std::erase_if(headers, [](const http_header& field) noexcept {
                    return http_ascii_equals_ignore_case(field.name(), "Vary");
                });
                headers.emplace_back("Vary", "*");
            }
            return;
        }
        if (!is_valid_http_header_name(trimmed)) {
            throw std::invalid_argument("Vary token must be a valid HTTP field name");
        }
        if (headers_have_vary_token(headers, trimmed)) {
            continue;
        }
        const bool duplicate = std::ranges::any_of(missing, [trimmed](std::string_view current) noexcept {
            return http_ascii_equals_ignore_case(current, trimmed);
        });
        if (!duplicate) {
            missing.push_back(trimmed);
        }
    }
    if (missing.empty()) {
        return;
    }

    for (std::size_t index = 0; index < headers.size(); ++index) {
        if (!http_ascii_equals_ignore_case(headers[index].name(), "Vary")) {
            continue;
        }
        std::string combined;
        for (const auto& field : headers) {
            if (!http_ascii_equals_ignore_case(field.name(), "Vary")) {
                continue;
            }
            const auto trimmed = http_trim_ows(field.value());
            if (trimmed.empty()) {
                continue;
            }
            if (!combined.empty()) {
                combined.append(", ");
            }
            combined.append(trimmed);
        }
        for (const auto token : missing) {
            if (!combined.empty()) {
                combined.append(", ");
            }
            combined.append(token);
        }
        headers[index] = http_header("Vary", combined);
        for (std::size_t rest = headers.size(); rest > index + 1; --rest) {
            if (http_ascii_equals_ignore_case(headers[rest - 1].name(), "Vary")) {
                headers.erase(headers.begin() + static_cast<std::ptrdiff_t>(rest - 1));
            }
        }
        return;
    }

    std::string value;
    for (std::size_t index = 0; index < missing.size(); ++index) {
        if (index != 0) {
            value.append(", ");
        }
        value.append(missing[index]);
    }
    headers.emplace_back("Vary", value);
}

void merge_http_vary_token(std::vector<http_header>& headers, std::string_view token) {
    merge_http_vary_tokens(headers, std::span<const std::string_view>(&token, 1));
}

http_cors_policy::http_cors_policy(const http_cors_policy_config& config)
    : allow_any_origin_(config.allow_any_origin_),
      reflect_request_headers_(config.reflect_request_headers_),
      allow_credentials_(config.allow_credentials_) {
    const auto add_suffix = [this](std::string_view pattern) {
        if (pattern.empty()) {
            throw std::invalid_argument("CORS origin suffix must not be empty");
        }
        suffix_rule rule;
        auto rest = pattern;
        if (const auto scheme_pos = rest.find("://"); scheme_pos != std::string_view::npos) {
            const auto scheme = rest.substr(0, scheme_pos);
            if (!http_ascii_equals_ignore_case(scheme, "http") &&
                !http_ascii_equals_ignore_case(scheme, "https")) {
                throw std::invalid_argument("CORS origin suffix scheme must be http or https");
            }
            rule.scheme_ = to_lower_copy(scheme);
            rest = rest.substr(scheme_pos + 3U);
        }
        if (rest.starts_with("*.")) {
            rule.subdomain_only_ = true;
            rest.remove_prefix(2U);
        } else if (rest.starts_with('.')) {
            rule.subdomain_only_ = true;
            rest.remove_prefix(1U);
        }
        const auto authority = parse_http_authority(rest);
        if (!authority || authority->host().empty() ||
            authority->port_kind() == http_authority_port_kind::empty ||
            authority->host().starts_with('[') ||
            is_valid_http_ipv4_literal(authority->host()) ||
            authority->host().front() == '.' || authority->host().back() == '.') {
            throw std::invalid_argument("CORS origin suffix must name a valid domain");
        }
        rule.host_suffix_ = to_lower_copy(authority->host());
        rule.port_ = authority->port();
        suffix_rules_.push_back(std::move(rule));
    };

    for (const auto& origin_entry : config.allowed_origins_) {
        const std::string_view view(origin_entry);
        if (view == "*") {
            allow_any_origin_ = true;
            continue;
        }
        if (view.starts_with("*.") || view.starts_with('.') ||
            view.find("://*.") != std::string_view::npos ||
            view.find("://.") != std::string_view::npos) {
            add_suffix(view);
            continue;
        }
        if (!is_valid_cors_origin_value(view)) {
            throw std::invalid_argument("CORS allowed origin must be a WHATWG serialized origin");
        }
        exact_origins_.emplace_back(view);
    }

    for (const auto& suffix_entry : config.allowed_origin_suffixes_) {
        add_suffix(suffix_entry);
    }
    if (exact_origins_.empty() && suffix_rules_.empty()) {
        allow_any_origin_ = true;
    }
    for (const auto& method : config.allowed_methods_) {
        const std::string_view view(method);
        if (view == "*") {
            allow_any_method_ = true;
            continue;
        }
        if (!is_valid_http_method_token(view)) {
            throw std::invalid_argument("CORS allowed method must be a valid HTTP method token");
        }
        allowed_methods_.emplace_back(view);
    }
    if (allowed_methods_.empty()) {
        allow_any_method_ = true;
    }
    joined_allowed_methods_ = join_tokens(allowed_methods_);

    for (const auto& header_name : config.allowed_headers_) {
        const std::string_view view(header_name);
        if (view == "*") {
            reflect_request_headers_ = true;
            continue;
        }
        if (!is_valid_http_header_name(view)) {
            throw std::invalid_argument("CORS allowed header must be a valid HTTP field name");
        }
        allowed_headers_.emplace_back(view);
    }
    if (allowed_headers_.empty()) {
        reflect_request_headers_ = true;
    }
    joined_allowed_headers_ = join_tokens(allowed_headers_);
    for (const auto& expose_name : config.expose_headers_) {
        if (!is_valid_http_header_name(expose_name)) {
            throw std::invalid_argument("CORS exposed header must be a valid HTTP field name");
        }
    }
    joined_expose_headers_ = join_tokens(config.expose_headers_);

    if (config.max_age_.has_value()) {
        if (config.max_age_->count() < 0) {
            throw std::invalid_argument("CORS max age must not be negative");
        }
        std::array<char, http_max_unsigned_decimal_size> buffer{};
        const auto formatted =
            http_format_unsigned(buffer, static_cast<std::uint64_t>(config.max_age_->count()));
        max_age_.assign(formatted);
    }
}

bool http_cors_policy::allows_origin(std::string_view origin) const noexcept {
    if (!is_valid_cors_origin_value(origin)) {
        return false;
    }
    if (allow_any_origin_) {
        return true;
    }
    if (origin == "null") {
        return std::ranges::any_of(exact_origins_, [](std::string_view configured) noexcept {
            return configured == "null";
        });
    }

    const auto scheme_sep = origin.find("://");
    if (scheme_sep == std::string_view::npos) {
        return false;
    }
    const auto scheme = origin.substr(0, scheme_sep);
    const auto authority_text = origin.substr(scheme_sep + 3U);

    for (const auto& configured : exact_origins_) {
        if (configured == "null" || configured.size() != origin.size()) {
            continue;
        }
        const auto conf_sep = configured.find("://");
        if (conf_sep != scheme_sep ||
            !http_ascii_equals_ignore_case(scheme, std::string_view(configured).substr(0, conf_sep))) {
            continue;
        }
        if (http_ascii_equals_ignore_case(
                authority_text, std::string_view(configured).substr(conf_sep + 3U))) {
            return true;
        }
    }

    if (suffix_rules_.empty()) {
        return false;
    }
    const auto authority = parse_http_authority(authority_text);
    if (!authority || authority->host().empty() || authority->host().starts_with('[') ||
        is_valid_http_ipv4_literal(authority->host())) {
        return false;
    }
    const auto host = authority->host();
    const auto port = authority->port();

    for (const auto& rule : suffix_rules_) {
        if (!rule.scheme_.empty() && !http_ascii_equals_ignore_case(scheme, rule.scheme_)) {
            continue;
        }
        if (rule.port_.has_value() && port != rule.port_) {
            continue;
        }
        const std::string_view suffix(rule.host_suffix_);
        if (!rule.subdomain_only_ && http_ascii_equals_ignore_case(host, suffix)) {
            return true;
        }
        if (host.size() > suffix.size() && host[host.size() - suffix.size() - 1U] == '.' &&
            http_ascii_equals_ignore_case(host.substr(host.size() - suffix.size()), suffix)) {
            return true;
        }
    }
    return false;
}

bool http_cors_policy::allows_method(std::string_view method) const noexcept {
    if (!is_valid_http_method_token(method)) {
        return false;
    }
    if (allow_any_method_) {
        return true;
    }
    return std::ranges::any_of(allowed_methods_, [method](std::string_view allowed) noexcept {
        return http_ascii_equals_ignore_case(allowed, method);
    });
}

bool http_cors_policy::allows_request_header(std::string_view name) const noexcept {
    if (!is_valid_http_header_name(name)) {
        return false;
    }
    if (reflect_request_headers_) {
        return true;
    }
    return std::ranges::any_of(allowed_headers_, [name](std::string_view allowed) noexcept {
        return http_ascii_equals_ignore_case(allowed, name);
    });
}

http_cors_preflight_result http_cors_policy::evaluate_preflight(
    http_cors_preflight_input input) const {
    const auto method = input.method_.view();
    const auto origin = input.origin_.view();
    const auto requested_method = http_trim_ows(input.access_control_request_method_.view());
    const auto requested_headers = input.access_control_request_headers_.view();

    if (method != "OPTIONS" || origin.empty() || !allows_origin(origin) ||
        requested_method.empty() || !allows_method(requested_method)) {
        return http_cors_preflight_result();
    }

    std::string reflected_headers;
    if (!http_trim_ows(requested_headers).empty()) {
        http_field_name_list names(requested_headers);
        bool saw_name = false;
        while (const auto header_name = names.next()) {
            saw_name = true;
            if (!allows_request_header(*header_name)) {
                return http_cors_preflight_result();
            }
            if (reflect_request_headers_) {
                if (!reflected_headers.empty()) {
                    reflected_headers.append(", ");
                }
                reflected_headers.append(*header_name);
            }
        }
        if (!saw_name || !names.valid()) {
            return http_cors_preflight_result();
        }
    }

    std::vector<http_header> headers;
    headers.reserve(6);
    const std::string_view allow_origin =
        (allow_any_origin_ && !allow_credentials_) ? std::string_view("*") : origin;
    headers.emplace_back("Access-Control-Allow-Origin", allow_origin);
    if (allow_credentials_) {
        headers.emplace_back("Access-Control-Allow-Credentials", "true");
    }
    const std::string_view allow_methods =
        !joined_allowed_methods_.empty() ? std::string_view(joined_allowed_methods_) : requested_method;
    headers.emplace_back("Access-Control-Allow-Methods", allow_methods);
    if (reflect_request_headers_) {
        if (!reflected_headers.empty()) {
            headers.emplace_back("Access-Control-Allow-Headers", reflected_headers);
        }
    } else if (!joined_allowed_headers_.empty()) {
        headers.emplace_back("Access-Control-Allow-Headers", joined_allowed_headers_);
    }
    if (!max_age_.empty()) {
        headers.emplace_back("Access-Control-Max-Age", max_age_);
    }
    headers.emplace_back("Vary",
        reflect_request_headers_
            ? "Origin, Access-Control-Request-Method, Access-Control-Request-Headers"
            : "Origin, Access-Control-Request-Method");

    return http_cors_preflight_result(true, std::move(headers));
}

http_cors_preflight_result http_cors_policy::evaluate_preflight(
    const http_request& request) const {
    const auto origin = request.header("Origin");
    const auto requested_method = request.header("Access-Control-Request-Method");
    if (!origin.has_value() || !requested_method.has_value()) {
        return http_cors_preflight_result();
    }
    std::string combined_headers;
    for (const auto& field : request.headers()) {
        if (!http_ascii_equals_ignore_case(field.name(), "Access-Control-Request-Headers")) {
            continue;
        }
        if (http_trim_ows(field.value()).empty()) {
            return http_cors_preflight_result();
        }
        if (!combined_headers.empty()) {
            combined_headers.append(", ");
        }
        combined_headers.append(field.value());
    }
    return evaluate_preflight({
        .method_ = request.method(),
        .origin_ = *origin,
        .access_control_request_method_ = *requested_method,
        .access_control_request_headers_ = combined_headers,
    });
}

bool http_cors_policy::rewrite_response_headers(
    borrowed_text request_origin, std::vector<http_header>& response_headers) const {
    std::erase_if(response_headers, [](const http_header& field) noexcept {
        return is_http_cors_controlled_response_header(field.name());
    });

    const auto origin = request_origin.view();
    const bool granted = !origin.empty() && allows_origin(origin);
    if (!allow_any_origin_ || allow_credentials_) {
        merge_http_vary_token(response_headers, "Origin");
    }
    if (!granted) {
        return false;
    }

    const std::string_view allow_origin =
        (allow_any_origin_ && !allow_credentials_) ? std::string_view("*") : origin;
    response_headers.emplace_back("Access-Control-Allow-Origin", allow_origin);
    if (allow_credentials_) {
        response_headers.emplace_back("Access-Control-Allow-Credentials", "true");
    }
    if (!joined_expose_headers_.empty()) {
        response_headers.emplace_back("Access-Control-Expose-Headers", joined_expose_headers_);
    }
    return true;
}

bool http_cors_policy::rewrite_response_headers(
    borrowed_text request_origin, http_response_headers& response_headers) const {
    response_headers.remove_header("Access-Control-Allow-Origin");
    response_headers.remove_header("Access-Control-Allow-Credentials");
    response_headers.remove_header("Access-Control-Expose-Headers");
    response_headers.remove_header("Access-Control-Max-Age");
    response_headers.remove_header("Access-Control-Allow-Methods");
    response_headers.remove_header("Access-Control-Allow-Headers");

    const auto origin = request_origin.view();
    const bool granted = !origin.empty() && allows_origin(origin);
    if (!allow_any_origin_ || allow_credentials_) {
        response_headers.add_vary_token("Origin");
    }
    if (!granted) {
        return false;
    }

    const std::string_view allow_origin =
        (allow_any_origin_ && !allow_credentials_) ? std::string_view("*") : origin;
    response_headers.header("Access-Control-Allow-Origin", allow_origin);
    if (allow_credentials_) {
        response_headers.header_stable_view("Access-Control-Allow-Credentials", "true");
    }
    if (!joined_expose_headers_.empty()) {
        response_headers.header("Access-Control-Expose-Headers", joined_expose_headers_);
    }
    return true;
}

bool http_cors_policy::rewrite_response_headers(
    borrowed_text request_origin, http_response& response) const {
    auto staged = response.headers().clone(4);
    const bool granted = rewrite_response_headers(request_origin, staged);
    response.commit_headers(std::move(staged));
    return granted;
}

bool http_cors_policy::apply_response_headers(
    const http_request& request, http_response& response) const {
    const bool options = request.known_method() == http_known_method::options;
    if (options) {
        response.add_vary_token("Origin");
        response.add_vary_token("Access-Control-Request-Method");
        if (reflect_request_headers_) {
            response.add_vary_token("Access-Control-Request-Headers");
        }
    } else if (!allow_any_origin_ || allow_credentials_) {
        response.add_vary_token("Origin");
    }

    const auto origin = request.header("Origin");
    if (origin.has_value() && !allows_origin(*origin)) {
        return false;
    }
    const auto requested_method =
        options ? request.header("Access-Control-Request-Method") : std::nullopt;
    if (requested_method.has_value() && !allows_method(http_trim_ows(*requested_method))) {
        return false;
    }
    const bool preflight = options && origin.has_value() && requested_method.has_value();
    const bool reflect_headers =
        preflight && reflect_request_headers_ &&
        !response.header("Access-Control-Allow-Headers").has_value();
    std::string reflected_headers;
    if (options && request.header("Access-Control-Request-Headers").has_value()) {
        std::size_t request_header_names_size = 0;
        for (const auto& field : request.headers()) {
            if (field.name().size() != 30U ||
                !http_ascii_equals_ignore_case(field.name(), "Access-Control-Request-Headers")) {
                continue;
            }
            http_field_name_list names(field.value());
            bool saw_name = false;
            while (const auto header_name = names.next()) {
                saw_name = true;
                if (!allows_request_header(*header_name)) {
                    return false;
                }
                const auto separator_size =
                    request_header_names_size == 0 ? std::size_t{0} : std::size_t{2};
                const auto maximum = (std::numeric_limits<std::size_t>::max)();
                if (header_name->size() > maximum - separator_size ||
                    request_header_names_size > maximum - separator_size - header_name->size()) {
                    return false;
                }
                request_header_names_size += separator_size + header_name->size();
                if (reflect_headers) {
                    if (!reflected_headers.empty()) {
                        reflected_headers.append(", ");
                    }
                    reflected_headers.append(*header_name);
                }
            }
            if (!saw_name || !names.valid()) {
                return false;
            }
        }
    }

    if (!response.header("Access-Control-Allow-Origin").has_value()) {
        if (allow_any_origin_ && !allow_credentials_) {
            response.header_stable_view("Access-Control-Allow-Origin", "*");
        } else if (origin.has_value()) {
            response.header("Access-Control-Allow-Origin", *origin);
        } else if (exact_origins_.size() == 1 && suffix_rules_.empty()) {
            response.header("Access-Control-Allow-Origin", exact_origins_.front());
        }
    }
    if (allow_credentials_ && !response.header("Access-Control-Allow-Credentials").has_value()) {
        response.header_stable_view("Access-Control-Allow-Credentials", "true");
    }

    if (preflight) {
        if (!response.header("Access-Control-Allow-Methods").has_value()) {
            if (!joined_allowed_methods_.empty()) {
                response.header("Access-Control-Allow-Methods", joined_allowed_methods_);
            } else if (const auto allow = response.header("Allow");
                allow.has_value() && !allow->empty()) {
                response.header("Access-Control-Allow-Methods", *allow);
            } else {
                response.header("Access-Control-Allow-Methods", http_trim_ows(*requested_method));
            }
        }
        if (!response.header("Access-Control-Allow-Headers").has_value()) {
            if (!reflect_request_headers_ && !joined_allowed_headers_.empty()) {
                response.header("Access-Control-Allow-Headers", joined_allowed_headers_);
            } else if (reflect_request_headers_ && !reflected_headers.empty()) {
                response.header("Access-Control-Allow-Headers", reflected_headers);
            }
        }
        if (!max_age_.empty() && !response.header("Access-Control-Max-Age").has_value()) {
            response.header("Access-Control-Max-Age", max_age_);
        }
        return true;
    }

    if (!joined_expose_headers_.empty() &&
        !response.header("Access-Control-Expose-Headers").has_value()) {
        response.header("Access-Control-Expose-Headers", joined_expose_headers_);
    }
    return true;
}

}  // namespace ruvia

namespace ruvia {
namespace {

// "[2001:db8::1]:443" -> "2001:db8::1"; "192.0.2.1:8080" -> "192.0.2.1".
[[nodiscard]] std::string_view extract_forwarded_node_address(std::string_view node_value) noexcept {
    node_value = http_trim_quoted_field_value(http_trim_ows(node_value));
    if (!node_value.empty() && node_value.front() == '[') {
        const auto close = node_value.find(']');
        if (close == std::string_view::npos) {
            return {};
        }
        const auto rest = node_value.substr(close + 1);
        if (!rest.empty() && rest.front() != ':') {
            return {};
        }
        return node_value.substr(1, close - 1);
    }
    const auto colon = node_value.find(':');
    if (colon != std::string_view::npos &&
        node_value.find(':', colon + 1) == std::string_view::npos) {
        return node_value.substr(0, colon);
    }
    return node_value;
}

[[nodiscard]] std::optional<http_scheme> parse_forwarded_http_scheme(
    std::string_view token) noexcept {
    token = http_trim_quoted_field_value(http_trim_ows(token));
    if (http_ascii_equals_ignore_case(token, "http")) {
        return http_scheme::http;
    }
    if (http_ascii_equals_ignore_case(token, "https")) {
        return http_scheme::https;
    }
    return std::nullopt;
}

struct forwarded_hop final {
    std::optional<ip_address> address_{};
    std::optional<http_scheme> scheme_{};
};

[[nodiscard]] std::optional<ip_address> parse_hop_address(std::string_view node_value) noexcept {
    const auto address_text = extract_forwarded_node_address(node_value);
    if (address_text.empty()) {
        return std::nullopt;
    }
    const auto parsed = ip_address::parse(address_text);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->normalized_ipv4();
}

[[nodiscard]] forwarded_hop parse_rfc7239_element(std::string_view element) noexcept {
    forwarded_hop hop{};
    bool saw_for = false;
    bool saw_proto = false;
    http_visit_semicolon_parameters_quoted_field(
        element, [&](std::string_view name, std::string_view value) noexcept {
            if (http_ascii_equals_ignore_case(name, "for")) {
                if (saw_for) {
                    hop.address_.reset();
                } else {
                    saw_for = true;
                    hop.address_ = parse_hop_address(value);
                }
            } else if (http_ascii_equals_ignore_case(name, "proto")) {
                if (saw_proto) {
                    hop.scheme_.reset();
                } else {
                    saw_proto = true;
                    hop.scheme_ = parse_forwarded_http_scheme(value);
                }
            }
            return true;
        });
    return hop;
}

class forwarding_chain final {
public:
    void append(const http_client_ip_extractor& extractor, forwarded_hop hop) noexcept {
        if (size_ == 0) {
            leftmost_ = hop;
        }
        const bool trusted = hop.address_.has_value() && extractor.is_trusted_proxy(*hop.address_);
        if (trusted) {
            if (awaiting_nearest_trusted_) {
                nearest_trusted_ = hop.address_;
                awaiting_nearest_trusted_ = false;
            }
        } else {
            client_hop_ = hop;
            client_index_ = size_;
            client_identified_ = hop.address_.has_value();
            has_client_hop_ = true;
            nearest_trusted_.reset();
            awaiting_nearest_trusted_ = true;
        }
        ++size_;
    }

    [[nodiscard]] forwarded_hop client() const noexcept {
        if (!has_client_hop_) {
            return leftmost_;
        }
        return {client_identified_ ? client_hop_.address_ : nearest_trusted_, client_hop_.scheme_};
    }

    [[nodiscard]] std::size_t client_index() const noexcept {
        return has_client_hop_ ? client_index_ : 0;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

private:
    forwarded_hop leftmost_{};
    forwarded_hop client_hop_{};
    std::optional<ip_address> nearest_trusted_{};
    std::size_t client_index_{0};
    std::size_t size_{0};
    bool has_client_hop_{false};
    bool client_identified_{false};
    bool awaiting_nearest_trusted_{false};
};

template <typename header_span_type>
[[nodiscard]] std::optional<http_client_ip_result> resolve_from_header(
    const http_client_ip_extractor& extractor, const ip_address& normalized_peer,
    std::string_view header_name, http_forwarded_header_kind kind,
    header_span_type headers) noexcept {
    if (kind == http_forwarded_header_kind::single_ip) {
        std::string_view single_value{};
        std::size_t line_count = 0;
        for (const auto& field : headers) {
            if (http_ascii_equals_ignore_case(field.name(), header_name)) {
                single_value = field.value();
                ++line_count;
            }
        }
        if (line_count != 1) {
            return std::nullopt;
        }
        const auto trimmed = http_trim_ows(single_value);
        if (trimmed.empty() || trimmed.find(',') != std::string_view::npos) {
            return std::nullopt;
        }
        const auto parsed = parse_hop_address(trimmed);
        if (!parsed.has_value() || extractor.is_trusted_proxy(*parsed)) {
            return std::nullopt;
        }
        return http_client_ip_result{
            .address_ = *parsed, .from_forwarded_header_ = true, .scheme_ = std::nullopt};
    }

    forwarding_chain chain;
    for (const auto& field : headers) {
        if (!http_ascii_equals_ignore_case(field.name(), header_name)) {
            continue;
        }
        http_visit_comma_separated_quoted_field_items(
            field.value(), [&](std::string_view item) noexcept {
                if (item.empty()) {
                    return true;
                }
                if (kind == http_forwarded_header_kind::rfc7239_forwarded) {
                    chain.append(extractor, parse_rfc7239_element(item));
                } else {
                    chain.append(extractor, {.address_ = parse_hop_address(item), .scheme_ = std::nullopt});
                }
                return true;
            });
    }
    if (chain.size() == 0) {
        return std::nullopt;
    }
    auto resolved = chain.client();
    if (kind == http_forwarded_header_kind::comma_separated_list &&
        extractor.trust_x_forwarded_proto() &&
        http_ascii_equals_ignore_case(header_name, "X-Forwarded-For")) {
        std::size_t proto_count = 0;
        std::optional<http_scheme> matched_scheme{};
        const auto target_index = chain.client_index();
        for (const auto& field : headers) {
            if (!http_ascii_equals_ignore_case(field.name(), "X-Forwarded-Proto")) {
                continue;
            }
            http_visit_comma_separated_quoted_field_items(
                field.value(), [&](std::string_view item) noexcept {
                    if (item.empty()) {
                        return true;
                    }
                    if (proto_count == target_index) {
                        matched_scheme = parse_forwarded_http_scheme(item);
                    }
                    ++proto_count;
                    return true;
                });
        }
        if (proto_count == chain.size()) {
            resolved.scheme_ = matched_scheme;
        }
    }
    if (!resolved.address_.has_value() && !resolved.scheme_.has_value()) {
        return std::nullopt;
    }
    return http_client_ip_result{
        .address_ = resolved.address_.value_or(normalized_peer),
        .from_forwarded_header_ = true,
        .scheme_ = resolved.scheme_,
    };
}

[[nodiscard]] http_forwarded_header_kind resolve_header_kind(
    std::string_view name, http_forwarded_header_kind configured) noexcept {
    if (configured != http_forwarded_header_kind::auto_detect) {
        return configured;
    }
    if (http_ascii_equals_ignore_case(name, "Forwarded")) {
        return http_forwarded_header_kind::rfc7239_forwarded;
    }
    if (http_ascii_equals_ignore_case(name, "X-Forwarded-For")) {
        return http_forwarded_header_kind::comma_separated_list;
    }
    return http_forwarded_header_kind::single_ip;
}
}  // namespace

http_client_ip_extractor::http_client_ip_extractor(const http_client_ip_config& config)
    : enabled_(config.enabled_),
      trust_x_forwarded_proto_(config.trust_x_forwarded_proto_) {
    trusted_cidrs_.reserve(config.trusted_proxies_.size());
    for (const auto& cidr_text : config.trusted_proxies_) {
        const auto parsed = ip_cidr::parse(cidr_text);
        if (!parsed.has_value()) {
            throw std::invalid_argument("trusted proxy entry must be a valid IP address or CIDR");
        }
        trusted_cidrs_.push_back(*parsed);
    }

    if (config.forwarding_headers_.empty()) {
        compiled_header xff;
        xff.name_.assign("X-Forwarded-For");
        xff.kind_ = http_forwarded_header_kind::comma_separated_list;
        headers_.push_back(std::move(xff));

        compiled_header fwd;
        fwd.name_.assign("Forwarded");
        fwd.kind_ = http_forwarded_header_kind::rfc7239_forwarded;
        headers_.push_back(std::move(fwd));
        return;
    }

    headers_.reserve(config.forwarding_headers_.size());
    for (const auto& spec : config.forwarding_headers_) {
        if (!is_valid_http_header_name(spec.name_)) {
            throw std::invalid_argument("forwarding header name must be a valid HTTP field name");
        }
        compiled_header entry;
        entry.name_.assign(spec.name_);
        entry.kind_ = resolve_header_kind(spec.name_, spec.kind_);
        headers_.push_back(std::move(entry));
    }
}

bool http_client_ip_extractor::is_trusted_proxy(const ip_address& address) const noexcept {
    return std::ranges::any_of(trusted_cidrs_, [&address](const ip_cidr& cidr) noexcept {
        return cidr.contains(address);
    });
}

bool http_client_ip_extractor::is_trusted_proxy(std::string_view address_text) const noexcept {
    const auto parsed = ip_address::parse(address_text);
    return parsed.has_value() && is_trusted_proxy(*parsed);
}

http_client_ip_result http_client_ip_extractor::resolve(
    const ip_address& peer_ip, std::span<const http_header> headers) const noexcept {
    const auto normalized_peer = peer_ip.normalized_ipv4();
    if (!enabled_ || trusted_cidrs_.empty() || !is_trusted_proxy(normalized_peer)) {
        return {.address_ = normalized_peer, .from_forwarded_header_ = false};
    }
    for (const auto& header_spec : headers_) {
        const bool present = std::ranges::any_of(headers, [&](const http_header& field) noexcept {
            return http_ascii_equals_ignore_case(field.name(), header_spec.name_);
        });
        if (!present) {
            continue;
        }
        if (const auto extracted = resolve_from_header(
                *this, normalized_peer, header_spec.name_, header_spec.kind_, headers)) {
            return *extracted;
        }
    }
    return {.address_ = normalized_peer, .from_forwarded_header_ = false};
}

http_client_ip_result http_client_ip_extractor::resolve(
    const ip_address& peer_ip, std::span<const http_header_view> headers) const noexcept {
    const auto normalized_peer = peer_ip.normalized_ipv4();
    if (!enabled_ || trusted_cidrs_.empty() || !is_trusted_proxy(normalized_peer)) {
        return {.address_ = normalized_peer, .from_forwarded_header_ = false};
    }
    for (const auto& header_spec : headers_) {
        const bool present =
            std::ranges::any_of(headers, [&](const http_header_view& field) noexcept {
                return http_ascii_equals_ignore_case(field.name(), header_spec.name_);
            });
        if (!present) {
            continue;
        }
        if (const auto extracted = resolve_from_header(
                *this, normalized_peer, header_spec.name_, header_spec.kind_, headers)) {
            return *extracted;
        }
    }
    return {.address_ = normalized_peer, .from_forwarded_header_ = false};
}

http_client_ip_result http_client_ip_extractor::resolve(
    const ip_address& peer_ip, const http_request& request) const noexcept {
    return resolve(peer_ip, request.headers());
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(
    std::string_view peer_ip, std::span<const http_header> headers) const noexcept {
    const auto parsed = ip_address::parse(peer_ip);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return resolve(*parsed, headers);
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(
    std::string_view peer_ip, std::span<const http_header_view> headers) const noexcept {
    const auto parsed = ip_address::parse(peer_ip);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return resolve(*parsed, headers);
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(
    std::string_view peer_ip, const http_request& request) const noexcept {
    return resolve(peer_ip, request.headers());
}

}  // namespace ruvia
