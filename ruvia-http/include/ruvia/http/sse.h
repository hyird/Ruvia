#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_protocol_error.h"
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
