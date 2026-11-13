#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_accept_match.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_field_values.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"

#include "coding/http_zlib_stream.h"
#include "field/http_connection_fields.h"
#include "http_wire_sink.h"
#include "response/http_response_header_state.h"

// The Content-Encoding field itself (RFC 9110 sections 8.4 and 5.6.1): the token
// each coding is spelled with, the list-grammar accumulator across field lines,
// and which codec a decode or encode of that coding runs on.

#include <zlib.h>

#include <limits>

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
