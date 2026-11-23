#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "decode_step.h"
#include "http_io_buffer.h"
#include "http_protocol.h"
#include "http_status.h"

namespace ruvia {
namespace detail {
struct multipart_part_access;
struct multipart_stream_part_access;
}  // namespace detail

// Buffered multipart parsing borrows unmodified Content-Disposition names,
// filenames, Content-Type values, and part bodies from the caller-owned
// request body; unfolded header blocks and escaped names own string storage.
class multipart_part final {
public:
    multipart_part(const multipart_part&) = default;
    multipart_part(multipart_part&&) noexcept = default;
    multipart_part& operator=(const multipart_part&) = default;
    multipart_part& operator=(multipart_part&&) noexcept = default;

    [[nodiscard]] std::string_view name() const& noexcept RUVIA_LIFETIMEBOUND {
        return view_text(name_);
    }
    [[nodiscard]] std::string_view name() const&& = delete;
    [[nodiscard]] std::string take_name() && {
        return take_text(name_);
    }

    [[nodiscard]] std::string_view filename() const& noexcept RUVIA_LIFETIMEBOUND {
        return view_text(filename_);
    }
    [[nodiscard]] std::string_view filename() const&& = delete;
    [[nodiscard]] std::string take_filename() && {
        auto result = take_text(filename_);
        filename_present_ = false;
        return result;
    }

    [[nodiscard]] bool has_filename() const noexcept {
        return filename_present_;
    }
    [[nodiscard]] std::string_view content_type() const& noexcept RUVIA_LIFETIMEBOUND {
        return view_text(content_type_);
    }
    std::string_view content_type() const&& = delete;
    [[nodiscard]] std::string take_content_type() && {
        return take_text(content_type_);
    }

    [[nodiscard]] std::string_view body() const noexcept {
        return body_;
    }

    [[nodiscard]] byte_view body_bytes() const noexcept {
        return as_bytes(body_);
    }

private:
    friend struct detail::multipart_part_access;
    friend class multipart_parser;

    using text_storage_type = std::variant<std::string_view, std::string>;
    using content_type_storage_type = text_storage_type;

    [[nodiscard]] static std::string_view view_text(const text_storage_type& storage) noexcept {
        if (const auto* borrowed = std::get_if<std::string_view>(&storage)) {
            return *borrowed;
        }
        return std::get<std::string>(storage);
    }

    [[nodiscard]] static std::string take_text(text_storage_type& storage) {
        if (auto* owned = std::get_if<std::string>(&storage)) {
            auto result = std::move(*owned);
            storage = std::string_view{};
            return result;
        }
        std::string copied(std::get<std::string_view>(storage));
        storage = std::string_view{};
        return copied;
    }

    multipart_part(text_storage_type name, text_storage_type filename, content_type_storage_type content_type_value, std::string_view body, bool filename_present) noexcept :
        name_(std::move(name)),
        filename_(std::move(filename)),
        content_type_(std::move(content_type_value)),
        body_(body),
        filename_present_(filename_present) {}
    text_storage_type name_;
    text_storage_type filename_;
    content_type_storage_type content_type_;
    std::string_view body_;
    bool filename_present_{false};
};

// RFC 2046 multipart boundary value. The validated bytes are stored inline so
// every buffered/streaming parser consumes the same allocation-free value and
// cannot observe a dangling Content-Type substring.
class multipart_boundary final {
public:
    explicit multipart_boundary(std::string_view value) {
        if (!valid(value)) {
            throw std::invalid_argument("invalid multipart boundary");
        }
        assign(value);
    }

    // Validation-only entry for parsing paths that must report failure as a
    // value rather than drive control flow through the throwing constructor.
    [[nodiscard]] static std::optional<multipart_boundary> try_parse(std::string_view value) noexcept {
        if (!valid(value)) {
            return std::nullopt;
        }
        multipart_boundary boundary;
        boundary.assign(value);
        return boundary;
    }

    [[nodiscard]] constexpr std::string_view value() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::string_view(bytes_.data(), size_);
    }
    [[nodiscard]] std::string_view value() const&& = delete;

private:
    constexpr multipart_boundary() noexcept = default;

    static constexpr std::size_t max_size = 70;

    [[nodiscard]] static constexpr bool non_space_char(char value) noexcept {
        return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '\'' || value == '(' || value == ')' || value == '+' || value == '_' || value == ',' || value == '-' || value == '.' || value == '/' || value == ':' || value == '=' || value == '?';
    }

    [[nodiscard]] static constexpr bool valid(std::string_view value) noexcept {
        if (value.empty() || value.size() > max_size || value.back() == ' ') {
            return false;
        }
        return std::ranges::all_of(value, [](char byte) noexcept {
            return byte == ' ' || non_space_char(byte);
        });
    }

    constexpr void assign(std::string_view value) noexcept {
        for (std::size_t index = 0; index < value.size(); ++index) {
            bytes_[index] = value[index];
        }
        size_ = static_cast<std::uint8_t>(value.size());
    }

    std::array<char, max_size> bytes_{};
    std::uint8_t size_{0};
};

struct multipart_parse_options final {
    multipart_boundary boundary_;
    std::size_t max_parts_{1024};
    // Cumulative wire header bytes bound decoded names, filenames and metadata.
    std::size_t max_metadata_bytes_{1024 * 1024};
};

// Why a Content-Type value yields no multipart boundary: it names another
// media type, or its multipart/form-data declaration is malformed.
enum class multipart_boundary_error : std::uint8_t {
    not_form_data,
    invalid_boundary,
};

[[nodiscard]] http_protocol_error make_http_protocol_error(multipart_boundary_error error) noexcept;

// Extracts the RFC 2046 boundary parameter from multipart/form-data. MIME
// quoted-pairs are decoded into the owned multipart_boundary value. Empty HTTP
// parameter slots are ignored; a nonempty, unique boundary remains required.
[[nodiscard]] std::variant<multipart_boundary, multipart_boundary_error> parse_multipart_boundary(std::string_view content_type_value);

enum class multipart_chunk_phase : std::uint8_t {
    complete,
    first,
    middle,
    last,
};

class multipart_stream_part final {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::string_view filename() const noexcept {
        return filename_;
    }

    [[nodiscard]] constexpr bool has_filename() const noexcept {
        return filename_present_;
    }

    [[nodiscard]] std::string_view content_type() const noexcept {
        return content_type_;
    }

    [[nodiscard]] std::string_view body() const noexcept {
        return body_;
    }

    [[nodiscard]] byte_view body_bytes() const noexcept {
        return as_bytes(body_);
    }

    [[nodiscard]] constexpr multipart_chunk_phase phase() const noexcept {
        return phase_;
    }

private:
    friend struct detail::multipart_stream_part_access;

    constexpr multipart_stream_part(std::string_view name, std::string_view filename, std::string_view content_type_value, std::string_view body, multipart_chunk_phase phase, bool filename_present) noexcept :
        name_(name),
        filename_(filename),
        content_type_(content_type_value),
        body_(body),
        phase_(phase),
        filename_present_(filename_present) {}

    std::string_view name_;
    std::string_view filename_;
    std::string_view content_type_;
    std::string_view body_;
    multipart_chunk_phase phase_{multipart_chunk_phase::middle};
    bool filename_present_{false};
};

enum class multipart_parse_error : std::uint8_t {
    incomplete_body,
    invalid_delimiter,
    preamble_too_large,
    part_headers_too_large,
    invalid_part_headers,
    invalid_content_disposition,
    missing_field_name,
    delimiter_line_too_large,
    too_many_parts,
    metadata_too_large,
};

[[nodiscard]] http_protocol_error make_http_protocol_error(multipart_parse_error error) noexcept;

using multipart_parse_failure = decode_failure<multipart_parse_error>;

// One poll() outcome: more input needed, a borrowed part chunk, the end of the
// body, or a failure. The parser owns its buffered input, so consumed_bytes()
// is always zero. All borrowed views remain valid only until the next feed(),
// finish_input(), poll() or reset() call.
using multipart_poll_result = decode_step<decode_need_input, multipart_stream_part, decode_complete, multipart_parse_failure>;

// One-shot multipart parsing yields either every part or the first failure
// (the one poll() reports); malformed or over-limit peer bytes are values, not
// exceptions.
using multipart_body_parse_result = std::variant<std::vector<multipart_part>, multipart_parse_error>;

class multipart_parser final {
public:
    explicit multipart_parser(multipart_parse_options options);

    multipart_parser(const multipart_parser&) = delete;
    multipart_parser& operator=(const multipart_parser&) = delete;
    multipart_parser(multipart_parser&&) = delete;
    multipart_parser& operator=(multipart_parser&&) = delete;

    // Copies input into parser-owned buffer storage, including chunks borrowed from
    // the latest part body. Allocation failure leaves that body valid for retry.
    // finish_input() marks the enclosing HTTP body's end so a close delimiter
    // ending exactly at EOF can be distinguished from a line split across chunks.
    // A failure is terminal: later poll() calls repeat the exact error. feed()
    // throws std::logic_error once the parser is not active or input ended.
    void feed(std::string_view chunk);
    template <typename bytes_type>
        requires(!std::convertible_to<bytes_type&&, std::string_view> && std::convertible_to<bytes_type&&, byte_view>)
    void feed(bytes_type&& chunk) {
        feed(as_chars(byte_view(std::forward<bytes_type>(chunk))));
    }
    void finish_input() noexcept;

    // If owning part metadata throws, the part stays pending without consuming
    // its limits. poll() may be retried after allocations become available.
    [[nodiscard]] multipart_poll_result poll() & RUVIA_LIFETIMEBOUND;
    multipart_poll_result poll() && = delete;

    [[nodiscard]] decoder_state state() const noexcept;
    // Starts a new body with the constructed boundary and limits, keeping
    // only small scratch buffers.
    void reset() noexcept;

private:
    struct complete_input_tag_type final {};

    friend multipart_body_parse_result parse_multipart_body(std::string_view body RUVIA_LIFETIMEBOUND, multipart_parse_options options);

    multipart_parser(std::string_view complete_body, multipart_parse_options options, complete_input_tag_type);

    enum class progress_state_type : std::uint8_t { boundary,
        headers,
        body,
        done };

    using state_type = std::variant<progress_state_type, multipart_parse_error>;

    enum class step_progress_type : std::uint8_t {
        need_input,
        continue_,
    };

    using step_result_type = std::variant<step_progress_type, multipart_parse_error>;

    [[nodiscard]] std::string_view buffer_view() const noexcept;
    void consume(std::size_t bytes) noexcept;
    void reset_scan_cursors() noexcept;
    void compact_pending();
    void release_body_buffers() noexcept;
    [[nodiscard]] multipart_poll_result fail(multipart_parse_error error) noexcept;
    [[nodiscard]] step_result_type consume_boundary();
    [[nodiscard]] step_result_type consume_headers();
    [[nodiscard]] multipart_stream_part make_part(std::string_view body, bool part_end);
    [[nodiscard]] multipart_poll_result read_body_chunk();

    multipart_boundary boundary_;
    std::variant<std::string_view, http_io_buffer> input_;
    multipart_part::text_storage_type current_name_;
    multipart_part::text_storage_type current_filename_;
    multipart_part::text_storage_type current_content_type_;
    std::string current_headers_;
    state_type state_{progress_state_type::boundary};
    std::size_t pending_erase_bytes_{0};
    std::size_t max_parts_;
    std::size_t max_metadata_bytes_;
    std::size_t remaining_parts_;
    std::size_t remaining_metadata_bytes_;
    std::size_t header_scan_offset_{0};
    std::size_t delimiter_scan_offset_{0};
    std::size_t delimiter_padding_offset_{0};
    bool input_eof_{false};
    bool next_chunk_is_first_{false};
    bool first_boundary_{true};
    bool has_current_filename_{false};
};

// Parses a complete multipart/form-data body without I/O. Returned part bodies
// borrow `body`. Names, filenames, and Content-Type values from unfolded header
// blocks own string storage; unmodified header blocks keep borrowing `body`.
[[nodiscard]] multipart_body_parse_result parse_multipart_body(std::string_view body RUVIA_LIFETIMEBOUND, multipart_parse_options options);

template <detail::http_temporary_owning_char_string body_type>
multipart_body_parse_result parse_multipart_body(body_type&&, multipart_parse_options) = delete;

[[nodiscard]] inline multipart_body_parse_result parse_multipart_body(byte_view body RUVIA_LIFETIMEBOUND, multipart_parse_options options) {
    return parse_multipart_body(as_chars(body), std::move(options));
}

template <detail::http_temporary_owning_range<std::byte> body_type>
multipart_body_parse_result parse_multipart_body(body_type&&, multipart_parse_options) = delete;
}  // namespace ruvia
