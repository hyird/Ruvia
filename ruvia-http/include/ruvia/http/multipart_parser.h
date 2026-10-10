#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http_protocol_error.h"

namespace ruvia {

namespace detail {
struct multipart_part_access;
struct multipart_stream_part_access;
}  // namespace detail

// Buffered multipart parsing owns decoded Content-Disposition names while the
// content type and body remain borrowed views into the caller-owned request body.
class multipart_part final {
public:
    [[nodiscard]] std::string_view name() const& noexcept {
        return name_;
    }
    [[nodiscard]] std::string_view name() const&& = delete;

    [[nodiscard]] std::string_view filename() const& noexcept {
        return filename_;
    }
    [[nodiscard]] std::string_view filename() const&& = delete;

    [[nodiscard]] bool has_filename() const noexcept {
        return filename_present_;
    }

    [[nodiscard]] std::string_view content_type() const noexcept {
        return content_type_;
    }

    [[nodiscard]] std::string_view body() const noexcept {
        return body_;
    }

private:
    friend struct detail::multipart_part_access;

    multipart_part(std::pmr::string name, std::pmr::string filename, std::string_view content_type_value,
        std::string_view body, bool filename_present) noexcept
        : name_(std::move(name)),
          filename_(std::move(filename)),
          content_type_(content_type_value),
          body_(body),
          filename_present_(filename_present) {}

    std::pmr::string name_;
    std::pmr::string filename_;
    std::string_view content_type_;
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
    [[nodiscard]] static std::optional<multipart_boundary> try_create(
        std::string_view value) noexcept {
        if (!valid(value)) {
            return std::nullopt;
        }
        multipart_boundary boundary;
        boundary.assign(value);
        return boundary;
    }

    [[nodiscard]] constexpr std::string_view value() const& noexcept {
        return std::string_view(bytes_.data(), size_);
    }
    [[nodiscard]] std::string_view value() const&& = delete;

private:
    constexpr multipart_boundary() noexcept = default;

    static constexpr std::size_t max_size = 70;

    [[nodiscard]] static constexpr bool non_space_char(char value) noexcept {
        return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
               (value >= 'a' && value <= 'z') || value == '\'' || value == '(' || value == ')' ||
               value == '+' || value == '_' || value == ',' || value == '-' || value == '.' ||
               value == '/' || value == ':' || value == '=' || value == '?';
    }

    [[nodiscard]] static constexpr bool valid(std::string_view value) noexcept {
        if (value.empty() || value.size() > max_size || value.back() == ' ') {
            return false;
        }
        return std::ranges::all_of(
            value, [](char byte) noexcept { return byte == ' ' || non_space_char(byte); });
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
    std::pmr::memory_resource* resource_{nullptr};
    std::size_t max_parts_{1024};
    // Cumulative wire header bytes bound decoded names, filenames and metadata.
    std::size_t max_metadata_bytes_{1024 * 1024};
};

class multipart_boundary_not_applicable final {
private:
    friend class multipart_boundary_parse_result;

    constexpr multipart_boundary_not_applicable() noexcept = default;
};

class multipart_boundary_parse_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept {
        return http_protocol_error(http_status::bad_request, "invalid multipart boundary");
    }

private:
    friend class multipart_boundary_parse_result;

    constexpr multipart_boundary_parse_failure() noexcept = default;
};

// Content-Type boundary extraction distinguishes another media type from a
// malformed multipart/form-data declaration. The successful alternative owns
// its decoded boundary bytes.
class multipart_boundary_parse_result final {
public:
    [[nodiscard]] constexpr const multipart_boundary* boundary() const& noexcept {
        return std::get_if<multipart_boundary>(&value_);
    }
    const multipart_boundary* boundary() const&& = delete;

    [[nodiscard]] constexpr const multipart_boundary_not_applicable* not_applicable() const& noexcept {
        return std::get_if<multipart_boundary_not_applicable>(&value_);
    }
    const multipart_boundary_not_applicable* not_applicable() const&& = delete;

    [[nodiscard]] constexpr const multipart_boundary_parse_failure* failure() const& noexcept {
        return std::get_if<multipart_boundary_parse_failure>(&value_);
    }
    const multipart_boundary_parse_failure* failure() const&& = delete;

private:
    friend multipart_boundary_parse_result parse_multipart_boundary(std::string_view);

    using value_type = std::variant<multipart_boundary, multipart_boundary_not_applicable,
        multipart_boundary_parse_failure>;

    explicit multipart_boundary_parse_result(multipart_boundary boundary)
        : value_(boundary) {}

    [[nodiscard]] static constexpr multipart_boundary_parse_result make_not_applicable() noexcept {
        return multipart_boundary_parse_result(multipart_boundary_not_applicable());
    }

    [[nodiscard]] static constexpr multipart_boundary_parse_result make_failure() noexcept {
        return multipart_boundary_parse_result(multipart_boundary_parse_failure());
    }

    explicit constexpr multipart_boundary_parse_result(
        multipart_boundary_not_applicable not_applicable) noexcept
        : value_(not_applicable) {}

    explicit constexpr multipart_boundary_parse_result(multipart_boundary_parse_failure failure) noexcept
        : value_(failure) {}

    value_type value_;
};

// Extracts the RFC 2046 boundary parameter from multipart/form-data. MIME
// quoted-pairs are decoded into the owned multipart_boundary value. Empty HTTP
// parameter slots are ignored; a nonempty, unique boundary remains required.
[[nodiscard]] multipart_boundary_parse_result parse_multipart_boundary(std::string_view content_type_value);

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

    [[nodiscard]] constexpr multipart_chunk_phase phase() const noexcept {
        return phase_;
    }

private:
    friend struct detail::multipart_stream_part_access;

    constexpr multipart_stream_part(std::string_view name, std::string_view filename,
        std::string_view content_type_value, std::string_view body, multipart_chunk_phase phase,
        bool filename_present) noexcept
        : name_(name),
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

class multipart_poll_need_input final {
private:
    friend class multipart_poll_result;
    constexpr multipart_poll_need_input() noexcept = default;
};

class multipart_poll_done final {
private:
    friend class multipart_poll_result;
    constexpr multipart_poll_done() noexcept = default;
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

class multipart_poll_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept;

private:
    friend class multipart_poll_result;
    friend class multipart_body_parse_result;

    explicit constexpr multipart_poll_failure(multipart_parse_error error) noexcept
        : error_(error) {}

    multipart_parse_error error_;
};

// Incremental multipart parsing has four mutually exclusive outcomes. Only
// the part alternative exposes borrowed part metadata/body, while only a
// failure exposes a protocol error. Need-input and done are payload-free
// lifecycle signals. All borrowed views remain valid only until the next
// feed(), finish_input(), or poll() call.
class multipart_poll_result final {
public:
    [[nodiscard]] constexpr const multipart_poll_need_input* need_input() const& noexcept {
        return std::get_if<multipart_poll_need_input>(&value_);
    }
    const multipart_poll_need_input* need_input() const&& = delete;

    [[nodiscard]] constexpr const multipart_stream_part* part() const& noexcept {
        return std::get_if<multipart_stream_part>(&value_);
    }
    const multipart_stream_part* part() const&& = delete;

    [[nodiscard]] constexpr const multipart_poll_done* done() const& noexcept {
        return std::get_if<multipart_poll_done>(&value_);
    }
    const multipart_poll_done* done() const&& = delete;

    [[nodiscard]] constexpr const multipart_poll_failure* failure() const& noexcept {
        return std::get_if<multipart_poll_failure>(&value_);
    }
    const multipart_poll_failure* failure() const&& = delete;

private:
    friend class multipart_parser;

    using value_type = std::variant<multipart_poll_need_input, multipart_stream_part, multipart_poll_done,
        multipart_poll_failure>;

    explicit constexpr multipart_poll_result(multipart_poll_need_input value) noexcept
        : value_(value) {}

    explicit constexpr multipart_poll_result(multipart_stream_part value) noexcept
        : value_(value) {}

    explicit constexpr multipart_poll_result(multipart_poll_done value) noexcept
        : value_(value) {}

    explicit constexpr multipart_poll_result(multipart_poll_failure value) noexcept
        : value_(value) {}

    [[nodiscard]] static constexpr multipart_poll_result make_need_input() noexcept {
        return multipart_poll_result(multipart_poll_need_input());
    }

    [[nodiscard]] static constexpr multipart_poll_result make_part(multipart_stream_part part) noexcept {
        return multipart_poll_result(part);
    }

    [[nodiscard]] static constexpr multipart_poll_result make_done() noexcept {
        return multipart_poll_result(multipart_poll_done());
    }

    [[nodiscard]] static constexpr multipart_poll_result make_failure(
        multipart_parse_error error) noexcept {
        return multipart_poll_result(multipart_poll_failure(error));
    }

    value_type value_;
};

class multipart_body final {
public:
    multipart_body(const multipart_body&) = delete;
    multipart_body& operator=(const multipart_body&) = delete;
    multipart_body(multipart_body&&) noexcept = default;
    multipart_body& operator=(multipart_body&&) = delete;

    [[nodiscard]] const std::pmr::vector<multipart_part>& parts() const& noexcept {
        return parts_;
    }
    const std::pmr::vector<multipart_part>& parts() const&& = delete;

    [[nodiscard]] std::pmr::vector<multipart_part> take_parts() && noexcept {
        return std::move(parts_);
    }

private:
    friend class multipart_body_parse_result;

    explicit multipart_body(std::pmr::vector<multipart_part> parts) noexcept
        : parts_(std::move(parts)) {}

    std::pmr::vector<multipart_part> parts_;
};

class multipart_body_parse_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept;

private:
    friend class multipart_body_parse_result;

    explicit constexpr multipart_body_parse_failure(multipart_parse_error error) noexcept
        : error_(error) {}

    multipart_parse_error error_;
};

class multipart_body_parse_result final {
public:
    multipart_body_parse_result(const multipart_body_parse_result&) = delete;
    multipart_body_parse_result& operator=(const multipart_body_parse_result&) = delete;
    multipart_body_parse_result(multipart_body_parse_result&&) noexcept = default;
    multipart_body_parse_result& operator=(multipart_body_parse_result&&) = delete;

    [[nodiscard]] multipart_body* body() & noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }

    [[nodiscard]] const multipart_body* body() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    multipart_body* body() && = delete;
    const multipart_body* body() const&& = delete;

    [[nodiscard]] const multipart_body_parse_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const multipart_body_parse_failure* failure() const&& = delete;

private:
    friend multipart_body_parse_result parse_multipart_body(std::string_view, multipart_parse_options);

    using value_type = std::variant<multipart_body, multipart_body_parse_failure>;

    explicit multipart_body_parse_result(std::pmr::vector<multipart_part> parts) noexcept
        : value_(multipart_body(std::move(parts))) {}

    explicit multipart_body_parse_result(multipart_parse_error error) noexcept
        : value_(multipart_body_parse_failure(error)) {}

    explicit multipart_body_parse_result(const multipart_poll_failure& failure) noexcept
        : value_(multipart_body_parse_failure(failure.error_)) {}

    value_type value_;
};

namespace detail {

struct multipart_borrowed_input final {
    std::string_view bytes_;
};

struct multipart_streaming_input_open final {
    explicit multipart_streaming_input_open(std::pmr::memory_resource* resource)
        : bytes_(resource) {}
    std::pmr::string bytes_;
};

struct multipart_streaming_input_eof final {
    explicit multipart_streaming_input_eof(std::pmr::string&& input)
        : bytes_(std::move(input)) {}
    std::pmr::string bytes_;
};

// Owns the multipart byte source and its EOF lifecycle. Parser grammar progress
// remains orthogonal in multipart_parser::state_.
class multipart_input_lifecycle final {
public:
    explicit multipart_input_lifecycle(std::pmr::memory_resource* resource);
    explicit multipart_input_lifecycle(multipart_borrowed_input input) noexcept;

    [[nodiscard]] const multipart_borrowed_input* borrowed() const& noexcept;
    [[nodiscard]] const multipart_borrowed_input* borrowed() const&& = delete;
    [[nodiscard]] const multipart_streaming_input_open* streaming_open() const& noexcept;
    [[nodiscard]] const multipart_streaming_input_open* streaming_open() const&& = delete;
    [[nodiscard]] const multipart_streaming_input_eof* streaming_eof() const& noexcept;
    [[nodiscard]] const multipart_streaming_input_eof* streaming_eof() const&& = delete;
    [[nodiscard]] bool eof() const noexcept;
    [[nodiscard]] std::string_view view() const& noexcept;
    [[nodiscard]] std::string_view view() const&& = delete;

    void feed(std::string_view chunk);
    void finish_input() noexcept;
    void consume(std::size_t bytes) noexcept;
    void compact_consumed_prefix(std::size_t threshold);

private:
    static constexpr std::size_t compact_consumed_prefix_bytes = std::size_t{64} * 1024;
    using value_type = std::variant<multipart_borrowed_input, multipart_streaming_input_open,
        multipart_streaming_input_eof>;

    [[nodiscard]] std::pmr::string* owned_bytes() noexcept;
    [[nodiscard]] const std::pmr::string* owned_bytes() const noexcept;

    value_type value_;
    std::size_t offset_{0};
};

}  // namespace detail

class multipart_parser final {
public:
    explicit multipart_parser(multipart_parse_options options);

    multipart_parser(const multipart_parser&) = delete;
    multipart_parser& operator=(const multipart_parser&) = delete;
    multipart_parser(multipart_parser&&) = delete;
    multipart_parser& operator=(multipart_parser&&) = delete;

    // Copies input into parser-owned PMR storage. finish_input() is required when
    // the enclosing HTTP body ends so a close delimiter ending exactly at EOF
    // can be distinguished from a delimiter line split across input chunks.
    // A protocol failure is terminal: later poll() calls repeat the exact error
    // and feed() rejects further bytes.
    void feed(std::string_view chunk);
    void finish_input() noexcept;

    // If owning part metadata throws, the part stays pending without consuming
    // its limits. poll() may be retried after allocations become available.
    [[nodiscard]] multipart_poll_result poll();

private:
    struct complete_input_tag_type final {};

    friend multipart_body_parse_result parse_multipart_body(std::string_view, multipart_parse_options);

    multipart_parser(std::string_view complete_body, multipart_parse_options options, complete_input_tag_type);

    enum class progress_state_type : std::uint8_t { boundary,
        headers,
        body,
        done };

    using state_type = std::variant<progress_state_type, multipart_parse_error>;

    enum class step_progress_type : std::uint8_t {
        need_input,
        continue_value,
        done,
    };

    using step_result_type = std::variant<step_progress_type, multipart_parse_error>;

    [[nodiscard]] std::string_view buffer_view() const noexcept;
    void consume(std::size_t bytes) noexcept;
    void compact_pending();
    [[nodiscard]] multipart_poll_result fail(multipart_parse_error error) noexcept;
    [[nodiscard]] step_result_type process_boundary();
    [[nodiscard]] step_result_type process_headers();
    [[nodiscard]] multipart_stream_part make_part(std::string_view body, bool part_end);
    [[nodiscard]] multipart_poll_result read_body_chunk();

    std::pmr::memory_resource* resource_;
    multipart_boundary boundary_;
    detail::multipart_input_lifecycle input_;
    std::pmr::string current_name_;
    std::pmr::string current_filename_;
    std::pmr::string current_content_type_;
    std::string_view current_content_type_view_;
    state_type state_{progress_state_type::boundary};
    std::size_t pending_erase_bytes_{0};
    std::size_t remaining_parts_;
    std::size_t remaining_metadata_bytes_;
    std::size_t header_scan_offset_{0};
    std::size_t delimiter_scan_offset_{0};
    std::size_t delimiter_padding_offset_{0};
    bool next_chunk_is_first_{false};
    bool first_boundary_{true};
    bool current_filename_present_{false};
};

// Parses a complete multipart/form-data body without I/O. Returned part bodies
// and content types borrow `body`; decoded name/filename values own PMR storage.
[[nodiscard]] multipart_body_parse_result parse_multipart_body(
    std::string_view body, multipart_parse_options options);

template <detail::http_temporary_owning_char_string body_type>
multipart_body_parse_result parse_multipart_body(body_type&&, multipart_parse_options) = delete;

}  // namespace ruvia
