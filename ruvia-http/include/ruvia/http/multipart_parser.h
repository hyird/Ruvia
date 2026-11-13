#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_protocol_error.h"

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
        if (const auto* borrowed = std::get_if<std::string_view>(&name_)) {
            return *borrowed;
        }
        return std::get<std::string>(name_);
    }
    [[nodiscard]] std::string_view name() const&& = delete;

    [[nodiscard]] std::string_view filename() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* borrowed = std::get_if<std::string_view>(&filename_)) {
            return *borrowed;
        }
        return std::get<std::string>(filename_);
    }
    [[nodiscard]] std::string_view filename() const&& = delete;

    [[nodiscard]] bool has_filename() const noexcept {
        return filename_present_;
    }
    [[nodiscard]] std::string_view content_type() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* borrowed = std::get_if<std::string_view>(&content_type_)) {
            return *borrowed;
        }
        return std::get<std::string>(content_type_);
    }
    std::string_view content_type() const&& = delete;

    [[nodiscard]] std::string_view body() const noexcept {
        return body_;
    }

private:
    friend struct detail::multipart_part_access;
    friend class multipart_parser;

    using text_storage_type = std::variant<std::string_view, std::string>;
    using content_type_storage_type = text_storage_type;

    multipart_part(text_storage_type name, text_storage_type filename,
        content_type_storage_type content_type_value, std::string_view body,
        bool filename_present) noexcept
        : name_(std::move(name)),
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
    [[nodiscard]] static std::optional<multipart_boundary> try_create(
        std::string_view value) noexcept {
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
        return http_protocol_error(
            http_status::bad_request, "multipart_invalid_boundary", "invalid multipart boundary");
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
    [[nodiscard]] constexpr const multipart_boundary* boundary() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<multipart_boundary>(&value_);
    }
    const multipart_boundary* boundary() const&& = delete;

    [[nodiscard]] constexpr const multipart_boundary_not_applicable* not_applicable() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<multipart_boundary_not_applicable>(&value_);
    }
    const multipart_boundary_not_applicable* not_applicable() const&& = delete;

    [[nodiscard]] constexpr const multipart_boundary_parse_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
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
    [[nodiscard]] constexpr const multipart_poll_need_input* need_input() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<multipart_poll_need_input>(&value_);
    }
    const multipart_poll_need_input* need_input() const&& = delete;

    [[nodiscard]] constexpr const multipart_stream_part* part() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<multipart_stream_part>(&value_);
    }
    const multipart_stream_part* part() const&& = delete;

    [[nodiscard]] constexpr const multipart_poll_done* done() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<multipart_poll_done>(&value_);
    }
    const multipart_poll_done* done() const&& = delete;

    [[nodiscard]] constexpr const multipart_poll_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
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
    // A protocol failure is terminal: later poll() calls repeat the exact error
    // and feed() rejects further bytes.
    void feed(std::string_view chunk);
    void finish_input() noexcept;

    // If owning part metadata throws, the part stays pending without consuming
    // its limits. poll() may be retried after allocations become available.
    [[nodiscard]] multipart_poll_result poll();

private:
    struct complete_input_tag_type final {};

    friend std::vector<multipart_part> parse_multipart_body(std::string_view, multipart_parse_options);

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
    void reset_scan_cursors() noexcept;
    void compact_pending();
    [[nodiscard]] multipart_poll_result fail(multipart_parse_error error) noexcept;
    [[nodiscard]] step_result_type process_boundary();
    [[nodiscard]] step_result_type process_headers();
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
[[nodiscard]] std::vector<multipart_part> parse_multipart_body(
    std::string_view body, multipart_parse_options options);

template <detail::http_temporary_owning_char_string body_type>
std::vector<multipart_part> parse_multipart_body(body_type&&, multipart_parse_options) = delete;

}  // namespace ruvia

namespace ruvia {

struct sse_message final {
    // SSE messages may be retained before they are formatted. Keep their text
    // zero-copy while preventing a temporary owning string from leaving an
    // already-dangling view in the saved message.
    // Absence emits no data field, so an event/id/retry-only block does not
    // dispatch a MessageEvent. A present empty value emits `data:` and therefore
    // dispatches one event whose data is empty.
    std::optional<::ruvia::borrowed_text> data_{};
    ::ruvia::borrowed_text event_{};
    std::optional<::ruvia::borrowed_text> id_{};
    std::optional<std::chrono::milliseconds> retry_{};
};

// Build one complete UTF-8 event-stream block. Invalid event/id line syntax
// or a field that is not well-formed UTF-8 throws std::invalid_argument.
[[nodiscard]] std::string format_sse_message(const sse_message& message);

struct sse_parser_options final {
    // Bytes buffered for the event being assembled: its data lines with their
    // LFs, its event type, and a pending event or id field value. Comments and
    // ignored fields buffer nothing. Exceeding it fails the stream.
    protocol_byte_limit event_limit_{protocol_byte_limit::limited(default_max_sse_event_bytes)};
};

enum class sse_parser_state : std::uint8_t {
    active,
    // finish() ended the stream.
    finished,
    // An event exceeded sse_parser_options::event_limit_.
    failed,
};

// One dispatched event (WHATWG HTML, "Dispatch the event"). Its views borrow
// the parser and remain valid until the parser's next parse(), finish(),
// reset(), move or destruction. Every view is well-formed UTF-8.
class sse_event final {
public:
    // The event type; "message" when the event named none.
    [[nodiscard]] constexpr std::string_view type() const noexcept {
        return type_;
    }
    // The data lines joined by LF, without a trailing LF.
    [[nodiscard]] constexpr std::string_view data() const noexcept {
        return data_;
    }
    [[nodiscard]] constexpr std::string_view last_event_id() const noexcept {
        return last_event_id_;
    }

private:
    friend class sse_parser;

    constexpr sse_event(std::string_view type, std::string_view data, std::string_view last_event_id) noexcept
        : type_(type),
          data_(data),
          last_event_id_(last_event_id) {}

    std::string_view type_;
    std::string_view data_;
    std::string_view last_event_id_;
};

struct sse_parse_result final {
    // Input bytes the parser consumed: through the blank line that completed
    // event_, up to the field bytes that failed the stream, or all of it.
    std::size_t consumed_bytes_{0};
    std::optional<sse_event> event_{};
};

// Sans-I/O text/event-stream parser (WHATWG HTML, "Parsing an event stream")
// for one response body, fed in chunks of any size. It skips one leading
// UTF-8 BOM, ends lines at CRLF, LF or CR (a CRLF split across chunks
// included), replaces ill-formed UTF-8 with U+FFFD in event values, and
// handles the event, data, id and retry fields; comments and other fields are
// ignored. Each input byte is scanned once: an incomplete line keeps only its
// field's routed value, never the raw line. The end of the stream discards an
// incomplete line or event. Only reset() leaves the finished or failed state;
// it starts a new stream and keeps buffer capacity within a small bound.
class sse_parser final {
public:
    explicit sse_parser(sse_parser_options options = {});
    sse_parser(const sse_parser&) = delete;
    sse_parser& operator=(const sse_parser&) = delete;
    sse_parser(sse_parser&&) noexcept = default;
    sse_parser& operator=(sse_parser&&) = delete;
    ~sse_parser() = default;

    // Consumes input until the next event is complete, the stream fails, or
    // the input ends. Call it again with the unconsumed suffix. A parser that
    // is not active throws std::logic_error.
    [[nodiscard]] sse_parse_result parse(std::string_view input) & RUVIA_LIFETIMEBOUND;
    sse_parse_result parse(std::string_view input) && = delete;
    // End of the stream: an active parser discards any incomplete event and
    // becomes finished. No effect otherwise.
    void finish() noexcept;
    void reset() noexcept;

    [[nodiscard]] sse_parser_state state() const noexcept {
        return state_;
    }
    // The stream's last event ID string, set by each blank line from the id
    // fields received so far; the value of a reconnection's Last-Event-ID.
    [[nodiscard]] std::string_view last_event_id() const& noexcept RUVIA_LIFETIMEBOUND {
        return last_event_id_;
    }
    std::string_view last_event_id() const&& = delete;
    // The latest valid retry field; absent until one arrives.
    [[nodiscard]] std::optional<std::chrono::milliseconds> reconnection_time() const noexcept {
        return reconnection_time_;
    }

private:
    enum class line_phase : std::uint8_t {
        name,
        // After the colon: one leading space is still to be skipped.
        value_start,
        value,
        comment,
    };
    enum class field_kind : std::uint8_t {
        ignored,
        data,
        event,
        id,
        retry,
    };

    [[nodiscard]] std::size_t buffered_bytes() const noexcept;
    void append_name(std::string_view bytes) noexcept;
    void classify_field() noexcept;
    // False when the bytes would exceed the event limit.
    [[nodiscard]] bool append_value(std::string_view bytes);
    [[nodiscard]] bool end_line();
    // True when the blank line dispatches an event.
    [[nodiscard]] bool dispatch();
    void reset_line() noexcept;
    void release_event_buffers() noexcept;
    void fail() noexcept;

    std::string data_;
    std::string event_type_;
    // A pending event or id value, committed at its line end.
    std::string value_;
    std::string last_event_id_buffer_;
    std::string last_event_id_;
    std::optional<std::chrono::milliseconds> reconnection_time_{};
    protocol_byte_limit event_limit_;
    std::uint64_t retry_value_{0};
    // Field name bytes, kept only up to the longest field name; longer names
    // name no field.
    std::array<char, 5> name_{};
    std::uint8_t name_size_{0};
    bool name_overflow_{false};
    bool retry_digits_{false};
    bool retry_valid_{true};
    // The views of the last returned event still borrow data_ and event_type_.
    bool event_published_{false};
    // The previous input ended in CR: an LF opening the next one belongs to it.
    bool skip_line_feed_{false};
    // Leading BOM bytes matched so far while the stream start is unsettled.
    std::uint8_t bom_matched_{0};
    bool bom_settled_{false};
    line_phase phase_{line_phase::name};
    field_kind field_{field_kind::ignored};
    sse_parser_state state_{sse_parser_state::active};
};

}  // namespace ruvia
