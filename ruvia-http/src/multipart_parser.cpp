#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "http_multipart.h"

// The MIME entity grammar multipart bodies are validated against (RFC 2045
// sections 5.1 and 5.3): which characters form a token, what a field name, field
// body, parameter and media type may contain, and the duplicate-parameter guard
// a media type needs.

#include <string>

#include "borrowed_text.h"
#include "http1_server_response.h"
#include "http_connection_fields.h"
#include "http_header.h"
#include "http_request.h"

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

[[nodiscard]] inline bool http_parse_mime_parameter(std::string_view parameter, std::string_view& name, std::string_view& value, bool strict_equals = true) noexcept {
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
    return (!strict_equals || (name.size() == raw_name.size() && value.size() == raw_value.size())) && !name.empty() && std::ranges::all_of(name, http_mime_token_char) && http_valid_mime_parameter_value(value);
}

template <http_temporary_owning_char_string parameter_type>
bool http_parse_mime_parameter(parameter_type&&, std::string_view&, std::string_view&, bool = true) = delete;

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
    if (slash == std::string_view::npos || media_type.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }
    const auto type = media_type.substr(0, slash);
    const auto subtype = media_type.substr(slash + 1);
    if (type == "*" || subtype == "*" || !std::ranges::all_of(type, http_mime_token_char) || !std::ranges::all_of(subtype, http_mime_token_char) || type.empty() || subtype.empty()) {
        return false;
    }
    http_mime_parameter_names parameter_names;
    return http_all_parameters(value, [&parameter_names](std::string_view parameter) noexcept {
        std::string_view name;
        std::string_view parameter_value;
        return http_parse_mime_parameter(parameter, name, parameter_value, false) && parameter_names.record(name);
    });
}

// The multipart state machine: find the next delimiter, read one part's header
// block, then hand out that part's body in chunks -- driven entirely by what is
// currently buffered, so a caller may feed the body in any pieces.

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

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_need_input(std::size_t offset = 0) noexcept {
        return {.kind_ = http_multipart_delimiter_kind::need_input, .offset_ = offset};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_part(std::size_t offset, std::size_t line_bytes) noexcept {
        return {.kind_ = http_multipart_delimiter_kind::part, .offset_ = offset, .line_bytes_ = line_bytes};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_close(std::size_t offset, std::size_t line_bytes) noexcept {
        return {.kind_ = http_multipart_delimiter_kind::close, .offset_ = offset, .line_bytes_ = line_bytes};
    }

    [[nodiscard]] constexpr http_multipart_delimiter_result rebased(std::size_t base) const noexcept {
        if (kind_ == http_multipart_delimiter_kind::need_input || kind_ == http_multipart_delimiter_kind::part || kind_ == http_multipart_delimiter_kind::close) {
            return {.kind_ = kind_, .offset_ = base + offset_, .line_bytes_ = line_bytes_};
        }
        return *this;
    }
};

[[nodiscard]] inline bool http_multipart_marker_prefix_matches(std::string_view input, std::string_view boundary) noexcept {
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
[[nodiscard]] inline http_multipart_delimiter_result http_match_multipart_delimiter_line(std::string_view input, const multipart_boundary& boundary, bool input_finished, std::size_t& padding_cursor) noexcept {
    const auto value = boundary.value();
    const auto marker_size = value.size() + 2;
    if (!http_multipart_marker_prefix_matches(input, value)) {
        return http_multipart_delimiter_result::make_no_match();
    }
    if (input.size() < marker_size) {
        return input_finished ? http_multipart_delimiter_result::make_no_match() : http_multipart_delimiter_result::make_need_input();
    }

    std::size_t cursor_value = marker_size;
    bool close = false;
    if (cursor_value < input.size() && input[cursor_value] == '-') {
        if (cursor_value + 1 >= input.size()) {
            return input_finished ? http_multipart_delimiter_result::make_no_match() : http_multipart_delimiter_result::make_need_input();
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
        return input_finished ? http_multipart_delimiter_result::make_no_match() : http_multipart_delimiter_result::make_need_input();
    }
    if (input[cursor_value] != '\r') {
        return http_multipart_delimiter_result::make_invalid();
    }
    if (cursor_value + 1 >= input.size()) {
        return input_finished ? http_multipart_delimiter_result::make_no_match() : http_multipart_delimiter_result::make_need_input();
    }
    if (input[cursor_value + 1] != '\n') {
        return http_multipart_delimiter_result::make_invalid();
    }
    return close ? http_multipart_delimiter_result::make_close(0, cursor_value + 2) : http_multipart_delimiter_result::make_part(0, cursor_value + 2);
}

// Cursors belong to one retained input prefix and must be reset after consume.
// Searches keep at most the three-byte delimiter prefix overlap; a candidate's
// transport padding resumes at its last inspected byte.
[[nodiscard]] inline http_multipart_delimiter_result http_find_multipart_delimiter_from_cursor(std::string_view input, const multipart_boundary& boundary, bool input_finished, bool check_leading_dash, std::size_t rebase_crlf_offset, std::size_t& search_cursor, std::size_t& padding_cursor) noexcept {
    if (check_leading_dash && search_cursor == 0 && !input.empty() && input.front() == '-') {
        auto match = http_match_multipart_delimiter_line(input, boundary, input_finished, padding_cursor);
        if (match.kind_ != http_multipart_delimiter_kind::no_match) {
            return match;
        }
        padding_cursor = 0;
    }
    for (auto prefix = input.find("\r\n--", search_cursor); prefix != std::string_view::npos; prefix = input.find("\r\n--", prefix + 1)) {
        auto match = http_match_multipart_delimiter_line(input.substr(prefix + 2), boundary, input_finished, padding_cursor);
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

[[nodiscard]] inline http_multipart_delimiter_result http_find_initial_multipart_delimiter(std::string_view input, const multipart_boundary& boundary, bool input_finished, std::size_t& search_cursor, std::size_t& padding_cursor) noexcept {
    auto match = http_find_multipart_delimiter_from_cursor(input, boundary, input_finished, true, 2, search_cursor, padding_cursor);
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
[[nodiscard]] inline http_multipart_delimiter_result http_find_multipart_body_delimiter(std::string_view input, const multipart_boundary& boundary, bool input_finished, bool at_body_start, std::size_t& search_cursor, std::size_t& padding_cursor) noexcept {
    return http_find_multipart_delimiter_from_cursor(input, boundary, input_finished, at_body_start, 0, search_cursor, padding_cursor);
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

using http_multipart_part_header_parse_result = std::variant<http_multipart_part_headers, multipart_parse_error>;

[[nodiscard]] inline http_multipart_part_header_parse_result http_parse_multipart_part_headers(std::string_view headers) noexcept {
    std::optional<std::string_view> disposition;
    std::optional<std::string_view> content_type;
    auto remaining_headers = headers;
    while (!remaining_headers.empty()) {
        const auto line_end = remaining_headers.find("\r\n");
        const auto line = line_end == std::string_view::npos ? remaining_headers : remaining_headers.substr(0, line_end);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || !http_valid_mime_field_name(line.substr(0, colon)) || !is_valid_http_field_value_bytes(line.substr(colon + 1))) {
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
    [[nodiscard]] static multipart_part make_decoded(multipart_part::text_storage_type name, multipart_part::text_storage_type filename, multipart_part::text_storage_type content_type_value, std::string_view body, bool filename_present) noexcept {
        return multipart_part(std::move(name), std::move(filename), std::move(content_type_value), body, filename_present);
    }

    template <http_temporary_owning_char_string body_type>
    static multipart_part make_decoded(multipart_part::text_storage_type, multipart_part::text_storage_type, multipart_part::text_storage_type, body_type&&, bool) = delete;
};

struct multipart_stream_part_access final {
    [[nodiscard]] static constexpr multipart_stream_part make(std::string_view name, std::string_view filename, std::string_view content_type_value, std::string_view body, multipart_chunk_phase phase, bool filename_present) noexcept {
        return multipart_stream_part(name, filename, content_type_value, body, phase, filename_present);
    }

    template <http_temporary_owning_char_string name_type>
    static multipart_stream_part make(name_type&&, std::string_view, std::string_view, std::string_view, multipart_chunk_phase, bool) = delete;

    template <http_temporary_owning_char_string filename_type>
    static multipart_stream_part make(std::string_view, filename_type&&, std::string_view, std::string_view, multipart_chunk_phase, bool) = delete;

    template <http_temporary_owning_char_string content_type>
    static multipart_stream_part make(std::string_view, std::string_view, content_type&&, std::string_view, multipart_chunk_phase, bool) = delete;

    template <http_temporary_owning_char_string body_type>
    static multipart_stream_part make(std::string_view, std::string_view, std::string_view, body_type&&, multipart_chunk_phase, bool) = delete;
};

}  // namespace ruvia::detail

namespace ruvia {

namespace {

// Decodes a validated boundary parameter (token or quoted-string) into
// `decoded`; the boundary rule itself is checked by multipart_boundary.
[[nodiscard]] std::optional<std::string_view> decode_multipart_boundary_parameter(std::string_view parameter, std::array<char, 70>& decoded) noexcept {
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
    if (!http_ascii_equals_ignore_case(detail::http_header_token_before_parameters(content_type_value), "multipart/form-data")) {
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
            boundary = decoded_value.has_value() ? multipart_boundary::try_create(*decoded_value) : std::nullopt;
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

[[nodiscard]] std::string_view multipart_text_view(const std::variant<std::string_view, std::string>& storage) noexcept {
    if (const auto* borrowed = std::get_if<std::string_view>(&storage)) {
        return *borrowed;
    }
    return std::get<std::string>(storage);
}

std::string& emplace_owned_multipart_text(std::variant<std::string_view, std::string>& storage) {
    if (auto* owned = std::get_if<std::string>(&storage)) {
        owned->clear();
        return *owned;
    }
    return storage.emplace<std::string>();
}

[[nodiscard]] http_protocol_error multipart_protocol_error(multipart_parse_error error) noexcept {
    switch (error) {
        case multipart_parse_error::incomplete_body:
            return http_protocol_error(http_status::bad_request, "multipart_incomplete_body", "incomplete multipart body");
        case multipart_parse_error::invalid_delimiter:
            return http_protocol_error(http_status::bad_request, "multipart_invalid_delimiter", "invalid multipart delimiter");
        case multipart_parse_error::preamble_too_large:
            return http_protocol_error(http_status::content_too_large, "multipart_preamble_too_large", "multipart preamble exceeds limit");
        case multipart_parse_error::part_headers_too_large:
            return http_protocol_error(http_status::content_too_large, "multipart_part_headers_too_large", "multipart part headers exceed limit");
        case multipart_parse_error::invalid_part_headers:
            return http_protocol_error(http_status::bad_request, "multipart_invalid_part_headers", "invalid multipart part headers");
        case multipart_parse_error::invalid_content_disposition:
            return http_protocol_error(http_status::bad_request, "multipart_invalid_content_disposition", "invalid multipart content disposition");
        case multipart_parse_error::missing_field_name:
            return http_protocol_error(http_status::bad_request, "multipart_missing_field_name", "invalid multipart field name");
        case multipart_parse_error::delimiter_line_too_large:
            return http_protocol_error(http_status::content_too_large, "multipart_delimiter_line_too_large", "multipart delimiter line exceeds limit");
        case multipart_parse_error::too_many_parts:
            return http_protocol_error(http_status::content_too_large, "multipart_too_many_parts", "multipart part count exceeds limit");
        case multipart_parse_error::metadata_too_large:
            return http_protocol_error(http_status::content_too_large, "multipart_metadata_too_large", "multipart metadata exceeds limit");
    }
    return http_protocol_error(http_status::bad_request, "multipart_invalid_body", "invalid multipart body");
}

}  // namespace

http_protocol_error multipart_poll_failure::protocol_error() const noexcept {
    return multipart_protocol_error(error_);
}

multipart_parser::multipart_parser(multipart_parse_options options) :
    boundary_(std::move(options.boundary_)),
    input_(std::in_place_type<http_io_buffer>),
    remaining_parts_(options.max_parts_),
    remaining_metadata_bytes_(options.max_metadata_bytes_) {}

multipart_parser::multipart_parser(std::string_view complete_body, multipart_parse_options options, complete_input_tag_type) :
    boundary_(std::move(options.boundary_)),
    input_(complete_body),
    remaining_parts_(options.max_parts_),
    remaining_metadata_bytes_(options.max_metadata_bytes_),
    input_eof_(true) {}

std::vector<multipart_part> parse_multipart_body(std::string_view body, multipart_parse_options options) {
    multipart_parser parser(body, std::move(options), multipart_parser::complete_input_tag_type{});
    std::vector<multipart_part> parts;
    for (;;) {
        auto result_value = parser.poll();
        if (const auto* part = result_value.part()) {
            parts.push_back(detail::multipart_part_access::make_decoded(std::exchange(parser.current_name_, std::string_view{}), std::exchange(parser.current_filename_, std::string_view{}), std::exchange(parser.current_content_type_, std::string_view{}), part->body(), part->has_filename()));
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
    delimiter_scan_offset_ = delimiter_scan_offset_ >= bytes_value ? delimiter_scan_offset_ - bytes_value : 0;
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
    if (owned == nullptr || input_eof_ || (state_.index() != 0) || std::get<0>(state_) == progress_state_type::done) {
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
        const auto delimiter = detail::http_find_initial_multipart_delimiter(buffer_view(), boundary_, input_eof_, delimiter_scan_offset_, delimiter_padding_offset_);
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

    const auto delimiter = detail::http_match_multipart_delimiter_line(buffer_view(), boundary_, input_eof_, delimiter_padding_offset_);
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::need_input) {
        if (buffer_view().size() > max_multipart_delimiter_line_bytes) {
            return multipart_parse_error::delimiter_line_too_large;
        }
        return step_progress_type::need_input;
    }
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::part || delimiter.kind_ == detail::http_multipart_delimiter_kind::close) {
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
        detail::copy_unfolded_field_lines(headers, first_fold, detail::field_line_folding_policy::mime, current_headers_);
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
        append_mime_parameter_value(emplace_owned_multipart_text(current_name_), part_headers->name());
    }
    has_current_filename_ = part_headers->has_filename();
    if (has_current_filename_ && !http_trim_quoted_field_value(part_headers->filename()).empty()) {
        if (borrowed_headers && part_headers->filename().find('\\') == std::string_view::npos) {
            current_filename_ = http_trim_quoted_field_value(part_headers->filename());
        } else {
            append_mime_parameter_value(emplace_owned_multipart_text(current_filename_), part_headers->filename());
        }
    } else {
        current_filename_ = std::string_view{};
    }
    if (part_headers->content_type().empty()) {
        current_content_type_ = std::string_view{};
    } else if (borrowed_headers) {
        current_content_type_ = part_headers->content_type();
    } else {
        emplace_owned_multipart_text(current_content_type_).assign(part_headers->content_type().data(), part_headers->content_type().size());
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
    const auto phase = next_chunk_is_first_ ? (part_end ? multipart_chunk_phase::complete : multipart_chunk_phase::first) : (part_end ? multipart_chunk_phase::last : multipart_chunk_phase::middle);
    auto part = detail::multipart_stream_part_access::make(multipart_text_view(current_name_), multipart_text_view(current_filename_), multipart_text_view(current_content_type_), body, phase, has_current_filename_);
    next_chunk_is_first_ = false;
    return part;
}

multipart_poll_result multipart_parser::read_body_chunk() {
    const auto buffer = buffer_view();
    const auto delimiter = detail::http_find_multipart_body_delimiter(buffer, boundary_, input_eof_, next_chunk_is_first_, delimiter_scan_offset_, delimiter_padding_offset_);
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::invalid) {
        return fail(multipart_parse_error::invalid_delimiter);
    }
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::part || delimiter.kind_ == detail::http_multipart_delimiter_kind::close) {
        if (delimiter.line_bytes_ > max_multipart_delimiter_line_bytes) {
            return fail(multipart_parse_error::delimiter_line_too_large);
        }
        const auto crlf_bytes = buffer.substr(delimiter.offset_).starts_with("\r\n") ? std::size_t{2} : std::size_t{0};
        auto part = make_part(buffer.substr(0, delimiter.offset_), true);
        pending_erase_bytes_ = delimiter.offset_ + crlf_bytes + delimiter.line_bytes_;
        reset_scan_cursors();
        state_ = delimiter.kind_ == detail::http_multipart_delimiter_kind::part ? progress_state_type::headers : progress_state_type::done;
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
