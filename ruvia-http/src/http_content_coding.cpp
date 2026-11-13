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
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/websocket_handshake.h"

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
