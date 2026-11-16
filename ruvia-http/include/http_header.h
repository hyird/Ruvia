#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"

namespace ruvia {

class http_response;
class http_response_headers;

[[nodiscard]] constexpr unsigned char http_ascii_to_lower(unsigned char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] constexpr bool http_ascii_equals_ignore_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (http_ascii_to_lower(static_cast<unsigned char>(left[i])) != http_ascii_to_lower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool http_ascii_starts_with_ignore_case(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && http_ascii_equals_ignore_case(text.substr(0, prefix.size()), prefix);
}

[[nodiscard]] constexpr bool http_ascii_ends_with_ignore_case(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && http_ascii_equals_ignore_case(text.substr(text.size() - suffix.size()), suffix);
}

// Owning copy of `text` with ASCII letters lowercased; other bytes are kept.
[[nodiscard]] inline std::string http_ascii_lowercase(std::string_view text) {
    std::string result(text);
    for (auto& c : result) {
        c = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(c)));
    }
    return result;
}

// The single owner of HTTP field-line syntax (RFC 9110 section 5): token and
// field-content byte classes and the normalized field name/value predicates.
// Field-specific grammars build on these; field-line OWS belongs to wire
// parsers, so normalized values never start or end with SP or HTAB.
// OWS / BWS byte: SP or HTAB (RFC 9110 section 5.6.3).
[[nodiscard]] constexpr bool is_http_ows_char(char c) noexcept {
    return c == ' ' || c == '\t';
}

// Removes only SP and HTAB from the ends of an HTTP field value (OWS,
// RFC 9110 section 5.6.3). The result borrows the caller's input.
[[nodiscard]] constexpr std::string_view http_trim_ows(borrowed_text text) noexcept {
    auto value = text.view();
    while (!value.empty() && is_http_ows_char(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_http_ows_char(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

// 256-entry character class tables (picohttpparser/llhttp style): one load
// replaces multi-comparison chains and lets scan loops validate as they move.
inline constexpr std::array<bool, 256> http_token_char_table = [] {
    std::array<bool, 256> table_value{};
    for (unsigned c = '0'; c <= '9'; ++c) {
        table_value[c] = true;
    }
    for (unsigned c = 'A'; c <= 'Z'; ++c) {
        table_value[c] = true;
    }
    for (unsigned c = 'a'; c <= 'z'; ++c) {
        table_value[c] = true;
    }
    for (const unsigned char c : {'!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~'}) {
        table_value[c] = true;
    }
    return table_value;
}();

// field-content bytes: HTAB, printable ASCII, and obs-text (0x80-0xFF).
// CR/LF/NUL/other controls and DEL are excluded.
inline constexpr std::array<bool, 256> http_field_value_char_table = [] {
    std::array<bool, 256> table_value{};
    for (unsigned c = 0; c < 256; ++c) {
        table_value[c] = c == '\t' || (c >= 0x20 && c != 0x7F);
    }
    return table_value;
}();

[[nodiscard]] constexpr bool is_http_token_char(unsigned char c) noexcept {
    return http_token_char_table[c];
}

[[nodiscard]] constexpr bool is_http_field_value_char(unsigned char c) noexcept {
    return http_field_value_char_table[c];
}

// Find the first non-token byte. Independent table loads let valid runs avoid
// one loop branch per byte; the final scan preserves the exact stop position.
[[nodiscard]] inline std::size_t http_token_prefix_size(std::string_view value) noexcept {
    std::size_t index = 0;
    while (value.size() - index >= 4) {
        const auto* bytes_value = value.data() + index;
        // Plain bool operands keep the bitwise '&' branchless without the
        // short-circuit warning on call operands.
        const bool first = is_http_token_char(static_cast<unsigned char>(bytes_value[0]));
        const bool second = is_http_token_char(static_cast<unsigned char>(bytes_value[1]));
        const bool third = is_http_token_char(static_cast<unsigned char>(bytes_value[2]));
        const bool fourth = is_http_token_char(static_cast<unsigned char>(bytes_value[3]));
        if (!(first & second & third & fourth)) {
            break;
        }
        index += 4;
    }
    while (index < value.size() && is_http_token_char(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    return index;
}

namespace detail {
// Keep bulk-scan register use outside inlined short-value callers.
[[nodiscard]] bool is_valid_long_http_field_value_bytes(const char* data, std::size_t size) noexcept;
}  // namespace detail

// Byte repertoire only. Field parsing separately handles leading/trailing OWS.
[[nodiscard]] inline bool is_valid_http_field_value_bytes(std::string_view value) noexcept {
    if (value.size() > 32) {
        return detail::is_valid_long_http_field_value_bytes(value.data(), value.size());
    }
    // Preserve a compile-time loop bound for short-value unrolling.
    const auto size = std::min<std::size_t>(32, value.size());
    for (std::size_t index = 0; index < size; ++index) {
        if (!is_http_field_value_char(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

// field-name = token.
[[nodiscard]] inline bool is_valid_http_header_name(std::string_view name) noexcept {
    return !name.empty() && http_token_prefix_size(name) == name.size();
}

// A normalized field value: field-content bytes without leading or trailing
// OWS. Wire parsers that accept field-line OWS trim it before this check.
[[nodiscard]] inline bool is_valid_http_header_value(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }
    if (is_http_ows_char(value.front()) || is_http_ows_char(value.back())) {
        return false;
    }
    return is_valid_http_field_value_bytes(value);
}

// 校验单个出站响应字段：上述名称/规范化值语法及响应发送方的已知字段
// 语义（Content-Type、Content-Encoding、Trailer），不分配内存。
// 不应用请求专属字段语义或请求托管字段黑名单，不用于接收方校验。
// 调用方仍负责重复字段、版本相关连接/分帧规则、应用禁改字段政策和
// 完整响应约束；返回 true 不代表该字段在任意响应上下文中都可发送。
[[nodiscard]] bool is_valid_http_response_field(std::string_view name, std::string_view value) noexcept;

// Index of the next `delimiter` at or after `start` that is outside an RFC 9110
// section 5.6.4 quoted-string, or value.size() when there is none. A quoted-pair
// (`\"`) does not end the string. This is the only quote-aware delimiter scan;
// every list and parameter visitor below is built on it.
[[nodiscard]] constexpr std::size_t http_find_unquoted_delimiter(std::string_view value, std::size_t start, char delimiter) noexcept {
    if (start >= value.size()) {
        return value.size();
    }
    bool quoted = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (quoted) {
            if (ch == '\\' && index + 1 < value.size()) {
                ++index;
            } else if (ch == '"') {
                quoted = false;
            }
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == delimiter && index >= start) {
            return index;
        }
    }
    return value.size();
}

namespace detail {

// Splits one OWS-trimmed `name=value` segment on its first '=' and hands the
// OWS-trimmed pair to the visitor. A segment without '=' is not a parameter and
// is skipped. Returns the visitor's keep-going result.
template <typename visitor_type>
[[nodiscard]] inline bool http_emit_field_parameter(std::string_view part, visitor_type& visitor) {
    const auto equals = part.find('=');
    if (equals == std::string_view::npos) {
        return true;
    }
    return visitor(http_trim_ows(part.substr(0, equals)), http_trim_ows(part.substr(equals + 1)));
}

}  // namespace detail

// Visits comma-separated field members, treating commas inside quoted strings
// (including escaped quotes) as data. Members are OWS-trimmed and empty members
// are reported; framing-sensitive callers can reject them. Returning false from
// the visitor stops traversal. Views borrow the input field.
template <typename visitor_type>
inline void http_visit_comma_separated_quoted_field_items(std::string_view value, visitor_type&& visitor) {
    std::size_t start = 0;
    for (;;) {
        const auto end = start + http_find_unquoted_delimiter(value.substr(start), 0, ',');
        if (!visitor(http_trim_ows(value.substr(start, end - start))) || end == value.size()) {
            return;
        }
        start = end + 1;
    }
}

// Visits semicolon-separated field parameters while treating semicolons inside
// quoted strings (including escaped quotes) as data. Parameter names and values
// are trimmed of OWS; segments without '=' are ignored. Values keep their
// quotes. Do not use for Cookie: RFC 6265 gives '"' no special meaning there.
// Views borrow the input.
template <typename visitor_type>
inline void http_visit_semicolon_parameters_quoted_field(std::string_view value, visitor_type&& visitor) {
    std::size_t start = 0;
    for (;;) {
        const auto end = start + http_find_unquoted_delimiter(value.substr(start), 0, ';');
        if (!detail::http_emit_field_parameter(http_trim_ows(value.substr(start, end - start)), visitor) || end == value.size()) {
            return;
        }
        start = end + 1;
    }
}

// Removes a surrounding pair of DQUOTE bytes when present; does not unescape
// quoted-pairs. The returned view borrows the input.
[[nodiscard]] constexpr std::string_view http_trim_quoted_field_value(borrowed_text text) noexcept {
    auto value = text.view();
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return value;
}

// Allocation-free RFC 9110 section 5.6.4 quoted-string reader. The input starts
// at the opening DQUOTE; next() validates and decodes one content byte at a time
// (qdtext or the byte of a quoted-pair: HTAB, SP, VCHAR, obs-text) and returns
// false at the closing DQUOTE or on malformed input. closed() tells the two
// apart; consumed() then counts the bytes read, both quotes included. Bytes after
// the closing DQUOTE are never read.
class http_quoted_string_cursor final {
public:
    explicit constexpr http_quoted_string_cursor(borrowed_text input) noexcept :
        input_(input.view()),
        state_(!input_.empty() && input_.front() == '"' ? state_type::open : state_type::failed) {}

    [[nodiscard]] constexpr bool next(unsigned char& out) noexcept {
        if (state_ != state_type::open) {
            return false;
        }
        if (position_ == input_.size()) {
            state_ = state_type::failed;
            return false;
        }
        auto byte = static_cast<unsigned char>(input_[position_++]);
        if (byte == '"') {
            state_ = state_type::closed;
            return false;
        }
        if (byte == '\\') {
            if (position_ == input_.size()) {
                state_ = state_type::failed;
                return false;
            }
            byte = static_cast<unsigned char>(input_[position_++]);
        }
        if (!is_http_field_value_char(byte)) {
            state_ = state_type::failed;
            return false;
        }
        out = byte;
        return true;
    }

    [[nodiscard]] constexpr bool closed() const noexcept {
        return state_ == state_type::closed;
    }

    [[nodiscard]] constexpr std::size_t consumed() const noexcept {
        return position_;
    }

private:
    enum class state_type : unsigned char { open,
        closed,
        failed };

    std::string_view input_;
    std::size_t position_{1};
    state_type state_;
};

// Byte length of the well-formed quoted-string that starts `input`, quotes
// included, or 0 when `input` does not start with one.
[[nodiscard]] constexpr std::size_t http_quoted_string_size(std::string_view input) noexcept {
    http_quoted_string_cursor cursor(input);
    unsigned char byte = 0;
    while (cursor.next(byte)) {
    }
    return cursor.closed() ? cursor.consumed() : 0;
}

// True when the whole of `value` is exactly one well-formed quoted-string.
[[nodiscard]] constexpr bool http_is_quoted_string(std::string_view value) noexcept {
    return !value.empty() && http_quoted_string_size(value) == value.size();
}

// Iterates a comma-delimited HTTP field-name list without allocating. Empty
// members are ignored, including an entirely empty list. Each returned view
// borrows the input. Drain next() until it returns nullopt, then check valid()
// to distinguish end from malformed input.
class http_field_name_list final {
public:
    explicit http_field_name_list(borrowed_text field) noexcept :
        remaining_(field.view()) {}

    [[nodiscard]] std::optional<std::string_view> next() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return finished_ && valid_;
    }

private:
    std::string_view remaining_;
    bool finished_{false};
    bool valid_{false};
};

}  // namespace ruvia

namespace ruvia::detail {

// RFC 9110 section 5.6.1 deliberately gives senders and recipients different
// list obligations: senders cannot generate empty members, while recipients
// must ignore a reasonable number of them. The HTTP/1 head-size limit bounds
// recipient work, so the tolerant path remains allocation-free and O(head).
enum class http_field_list_role : std::uint8_t { recipient,
    sender };

enum class http_field_list_parse_status : std::uint8_t { ok,
    malformed,
    rejected };

// The one role-aware list walk (RFC 9110 section 5.6.1) over the quote-aware
// splitter: an empty member is malformed for a sender and skipped for a
// recipient; every other member goes to `visitor`, whose non-ok status stops
// the walk and is returned.
template <typename visitor_type>
[[nodiscard]] inline http_field_list_parse_status http_visit_list_members(std::string_view value, http_field_list_role role, visitor_type&& visitor) {
    auto status = http_field_list_parse_status::ok;
    http_visit_comma_separated_quoted_field_items(value, [&status, role, &visitor](std::string_view member) {
        if (member.empty()) {
            if (role == http_field_list_role::sender) {
                status = http_field_list_parse_status::malformed;
            }
        } else {
            status = visitor(member);
        }
        return status == http_field_list_parse_status::ok;
    });
    return status;
}

// `#token` lists: a member that is not a token is malformed; a visitor that
// returns false rejects the field.
template <typename visitor_type>
[[nodiscard]] inline http_field_list_parse_status http_visit_token_list(std::string_view value, http_field_list_role role, visitor_type&& visitor) {
    return http_visit_list_members(value, role, [&visitor](std::string_view member) {
        if (!is_valid_http_header_name(member)) {
            return http_field_list_parse_status::malformed;
        }
        return visitor(member) ? http_field_list_parse_status::ok : http_field_list_parse_status::rejected;
    });
}

// The first nonempty list member accepted by `predicate`, or an empty view.
// Members are not validated; a quoted-string member is never split.
template <typename predicate_type>
[[nodiscard]] inline std::string_view http_find_header_token(std::string_view value, predicate_type&& predicate) noexcept {
    std::string_view found;
    http_visit_comma_separated_quoted_field_items(value, [&found, &predicate](std::string_view member) noexcept {
        if (!member.empty() && predicate(member)) {
            found = member;
            return false;
        }
        return true;
    });
    return found;
}

template <http_temporary_owning_char_string value_type, typename predicate_type>
std::string_view http_find_header_token(value_type&&, predicate_type&&) = delete;

// Accept-style negotiation fields skip empty list items without a role rule;
// grammar-checked lists use http_visit_list_members.
template <typename visitor_type>
inline void http_visit_comma_separated_quoted(std::string_view value, visitor_type&& visitor) {
    http_visit_comma_separated_quoted_field_items(value, [&visitor](std::string_view item) {
        return item.empty() || visitor(item);
    });
}

}  // namespace ruvia::detail

namespace ruvia {

// An owned field returned by protocol parsers and events. The same value type
// represents initial fields and trailers for requests and responses.
class http_header final {
public:
    http_header(std::string_view name, std::string_view value) :
        name_size_(name.size()),
        value_size_(value.size()),
        owned_(true) {
        if (value.size() > (std::numeric_limits<std::size_t>::max)() - name.size()) {
            throw std::length_error("HTTP header storage size overflows size_t");
        }
        const auto size = name.size() + value.size();
        if (size != 0) {
            bytes_ = new char[size];
            if (!name.empty()) {
                std::memcpy(bytes_, name.data(), name.size());
            }
            if (!value.empty()) {
                std::memcpy(bytes_ + name.size(), value.data(), value.size());
            }
        }
    }

    http_header(const http_header& other) :
        bytes_(!other.owned_ ? other.bytes_ : nullptr),
        name_size_(other.name_size_),
        value_size_(other.value_size_),
        owned_(other.owned_) {
        if (owned_) {
            const auto size = name_size_ + value_size_;
            if (size != 0) {
                bytes_ = new char[size];
                std::memcpy(bytes_, other.bytes_, size);
            }
        }
    }

    http_header(http_header&& other) noexcept :
        bytes_(std::exchange(other.bytes_, nullptr)),
        name_size_(std::exchange(other.name_size_, 0)),
        value_size_(std::exchange(other.value_size_, 0)),
        owned_(std::exchange(other.owned_, false)) {}

    http_header& operator=(const http_header& other) {
        if (this != &other) {
            http_header replacement(other);
            swap_storage(replacement);
        }
        return *this;
    }

    http_header& operator=(http_header&& other) noexcept {
        if (this != &other) {
            http_header retired(std::move(*this));
            bytes_ = std::exchange(other.bytes_, nullptr);
            name_size_ = std::exchange(other.name_size_, 0);
            value_size_ = std::exchange(other.value_size_, 0);
            owned_ = std::exchange(other.owned_, false);
        }
        return *this;
    }

    ~http_header() noexcept {
        release();
    }

    // Owning boundaries that normalize field names lower-case them while
    // copying, so the field still costs one allocation.
    [[nodiscard]] static http_header copy_with_lowercase_name(std::string_view name, std::string_view value) {
        auto result = http_header(name, value);
        for (std::size_t index = 0; index < result.name_size_; ++index) {
            result.bytes_[index] = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(result.bytes_[index])));
        }
        return result;
    }

    [[nodiscard]] std::string_view name() const& noexcept RUVIA_LIFETIMEBOUND {
        return name_size_ != 0 ? std::string_view(bytes_, name_size_) : std::string_view{};
    }
    std::string_view name() const&& = delete;
    [[nodiscard]] std::string_view value() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_size_ != 0 ? std::string_view(bytes_ + name_size_, value_size_) : std::string_view{};
    }
    std::string_view value() const&& = delete;

private:
    friend class websocket_client_negotiation;

    constexpr http_header() noexcept = default;

    constexpr http_header(const char* static_bytes, std::size_t name_size, std::size_t value_size) noexcept :
        bytes_(const_cast<char*>(static_bytes)),
        name_size_(name_size),
        value_size_(value_size),
        owned_(false) {}

    void release() noexcept {
        auto* const bytes = std::exchange(bytes_, nullptr);
        name_size_ = 0;
        value_size_ = 0;
        if (std::exchange(owned_, false) && bytes != nullptr) {
            delete[] bytes;
        }
    }

    void swap_storage(http_header& other) noexcept {
        std::swap(bytes_, other.bytes_);
        std::swap(name_size_, other.name_size_);
        std::swap(value_size_, other.value_size_);
        std::swap(owned_, other.owned_);
    }

    char* bytes_{nullptr};
    std::size_t name_size_{};
    std::size_t value_size_{};
    bool owned_{false};
};

class http_header_view final {
public:
    constexpr http_header_view() noexcept = default;

    constexpr http_header_view(std::string_view name, std::string_view value) noexcept :
        name_(name),
        value_(value) {}

    template <typename name_type, typename value_type>
        requires(detail::http_temporary_owning_char_string<name_type> || detail::http_temporary_owning_char_string<value_type>)
    http_header_view(name_type&& name, value_type&& value) = delete;

    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

private:
    std::string_view name_;
    std::string_view value_;
};

// Zero-copy HTTP header-view span held by a request, response, handshake, or
// client option struct. Like borrowed_text, it accepts borrowed spans and
// contiguous owning-container lvalues while rejecting temporary owning
// containers (std::vector, std::array) and initializer_list
// values whose backing storage would expire before the struct is consumed.
using borrowed_http_headers = borrowed_span<http_header_view>;

static_assert(sizeof(borrowed_http_headers) == sizeof(std::span<const http_header_view>));

template <typename field_type>
concept http_header_field = requires(const std::remove_reference_t<field_type>& field) {
    { field.name() } -> std::convertible_to<std::string_view>;
    { field.value() } -> std::convertible_to<std::string_view>;
};

template <typename range_type>
concept http_header_field_range = std::ranges::forward_range<const std::remove_cvref_t<range_type>> && std::ranges::sized_range<const std::remove_cvref_t<range_type>> && http_header_field<std::ranges::range_reference_t<const std::remove_cvref_t<range_type>>>;

// Outcome of looking up one name in a collection that permits repeated names
// (received fields, JSON object members, SQL result columns). A repeated name
// never resolves to one of its occurrences.
enum class unique_lookup_status : unsigned char { absent,
    found,
    repeated };

// Result of looking up a name that must not resolve to one of several
// occurrences. value_ is meaningful only when found; a view or pointer value
// borrows the searched storage.
template <typename value_type>
struct unique_lookup final {
    unique_lookup_status status_{unique_lookup_status::absent};
    value_type value_{};

    // absent -> found(value) -> repeated (value cleared); repeated is terminal.
    constexpr void add_occurrence(value_type value) noexcept {
        if (status_ == unique_lookup_status::absent) {
            status_ = unique_lookup_status::found;
            value_ = value;
        } else {
            status_ = unique_lookup_status::repeated;
            value_ = {};
        }
    }
};

using http_unique_field_lookup = unique_lookup<std::string_view>;

class http_header_range_view;

// A message exposing its fields through headers() (http_request,
// http_response, http_client_response_head).
template <typename message_type>
concept http_header_field_message = !http_header_field_range<message_type> && requires(const std::remove_cvref_t<message_type>& message) {
    { message.headers() } -> http_header_field_range;
};

// Anything a function can read header fields from: a sized forward range of
// fields, a message exposing one through headers(), or an already bound view.
template <typename source_type>
concept http_header_field_source = std::same_as<std::remove_cvref_t<source_type>, http_header_range_view> || http_header_field_range<source_type> || http_header_field_message<source_type>;

// The single read-only header parameter: a type-erased view over a field
// range or message, so a function reading fields is declared once, as
// `const http_header_range_view&`, and accepts every header container. It
// borrows the source for the call that receives it and, like argument_list,
// also binds a temporary container: that storage lasts for the full call
// expression. It can be neither copied nor moved, so it cannot be returned by
// a reader, captured by value or stored in a member.
class http_header_range_view final {
public:
    using accessor_type = http_header_view (*)(const void*, std::size_t) noexcept;

    constexpr http_header_range_view() noexcept = default;

    template <http_header_field_range range_type>
    constexpr http_header_range_view(const range_type& fields) noexcept :
        size_(std::ranges::size(fields)) {
        if constexpr (std::ranges::contiguous_range<const range_type>) {
            data_ = std::ranges::data(fields);
            at_ = +[](const void* base, std::size_t index) noexcept -> http_header_view {
                const auto& field = static_cast<const std::ranges::range_value_t<const range_type>*>(base)[index];
                return {field.name(), field.value()};
            };
        } else {
            using cursor_type = std::ranges::iterator_t<const range_type>;
            static_assert(std::is_trivially_copyable_v<cursor_type> && sizeof(cursor_type) <= cursor_bytes,
                "Non-contiguous header iterators must fit the allocation-free cursor");
            data_ = &fields;
            at_ = +[](const void* base, std::size_t index) noexcept -> http_header_view {
                const auto& field = *std::ranges::next(std::ranges::begin(*static_cast<const range_type*>(base)), static_cast<std::ranges::range_difference_t<const range_type>>(index));
                return {field.name(), field.value()};
            };
            begin_cursor_ = +[](const void* base, std::byte* storage) noexcept {
                const auto cursor = std::ranges::begin(*static_cast<const range_type*>(base));
                std::memcpy(storage, &cursor, sizeof(cursor));
            };
            read_cursor_ = +[](const std::byte* storage) noexcept -> http_header_view {
                cursor_type cursor;
                std::memcpy(&cursor, storage, sizeof(cursor));
                const auto& field = *cursor;
                return {field.name(), field.value()};
            };
            next_cursor_ = +[](std::byte* storage) noexcept {
                cursor_type cursor;
                std::memcpy(&cursor, storage, sizeof(cursor));
                ++cursor;
                std::memcpy(storage, &cursor, sizeof(cursor));
            };
        }
    }

    template <http_header_field_message message_type>
    constexpr http_header_range_view(const message_type& message) noexcept :
        http_header_range_view(message.headers()) {
        static_assert(std::ranges::contiguous_range<decltype(message.headers())> || std::is_lvalue_reference_v<decltype(message.headers())>,
            "A non-contiguous message header range must be returned by reference");
    }

    http_header_range_view(const http_header_range_view&) = delete;
    http_header_range_view& operator=(const http_header_range_view&) = delete;

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] constexpr http_header_view operator[](std::size_t index) const noexcept {
        return at_(data_, index);
    }

    // Iteration is linear for every source; indexed access to a forward-only
    // source advances from its beginning.

    class iterator final {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using value_type = http_header_view;
        using difference_type = std::ptrdiff_t;

        constexpr iterator() noexcept = default;
        constexpr iterator(const void* data, accessor_type at, std::size_t index) noexcept :
            data_(data),
            at_(at),
            index_(index) {}

        iterator(const void* data, accessor_type at, std::size_t index,
            void (*begin_cursor)(const void*, std::byte*) noexcept,
            http_header_view (*read_cursor)(const std::byte*) noexcept,
            void (*next_cursor)(std::byte*) noexcept) noexcept :
            iterator(data, at, index) {
            read_cursor_ = read_cursor;
            next_cursor_ = next_cursor;
            begin_cursor(data, cursor_.data());
        }

        [[nodiscard]] constexpr http_header_view operator*() const noexcept {
            return read_cursor_ != nullptr ? read_cursor_(cursor_.data()) : at_(data_, index_);
        }
        constexpr iterator& operator++() noexcept {
            if (next_cursor_ != nullptr) {
                next_cursor_(cursor_.data());
            }
            ++index_;
            return *this;
        }
        constexpr iterator operator++(int) noexcept {
            auto copy = *this;
            ++index_;
            return copy;
        }
        [[nodiscard]] friend constexpr bool operator==(const iterator& left, const iterator& right) noexcept {
            return left.data_ == right.data_ && left.index_ == right.index_;
        }

    private:
        const void* data_{nullptr};
        accessor_type at_{nullptr};
        std::size_t index_{0};
        std::array<std::byte, 4 * sizeof(void*)> cursor_{};
        http_header_view (*read_cursor_)(const std::byte*) noexcept {nullptr};
        void (*next_cursor_)(std::byte*) noexcept {nullptr};
    };

    [[nodiscard]] constexpr iterator begin() const noexcept {
        if (begin_cursor_ != nullptr) {
            return {data_, at_, 0, begin_cursor_, read_cursor_, next_cursor_};
        }
        return {data_, at_, 0};
    }

    [[nodiscard]] constexpr iterator end() const noexcept {
        return {data_, at_, size_};
    }

private:
    const void* data_{nullptr};
    static constexpr std::size_t cursor_bytes = 4 * sizeof(void*);
    std::size_t size_{0};
    accessor_type at_{nullptr};
    void (*begin_cursor_)(const void*, std::byte*) noexcept {nullptr};
    http_header_view (*read_cursor_)(const std::byte*) noexcept {nullptr};
    void (*next_cursor_)(std::byte*) noexcept {nullptr};
};

namespace detail {

template <typename source_type>
concept http_borrowed_header_source = http_header_field_source<source_type> && (std::is_lvalue_reference_v<source_type&&> || std::ranges::borrowed_range<source_type> || requires(source_type&& source) {
    { std::forward<source_type>(source).headers() } -> std::ranges::borrowed_range;
});

}  // namespace detail

// Case-insensitive field-name match over any header field source; stops at
// the second occurrence. The found value borrows the source's field storage.
// Only lvalue owners and borrowed views (including temporary spans) may escape.
template <detail::http_borrowed_header_source source_type>
[[nodiscard]] constexpr http_unique_field_lookup find_unique_http_header(source_type&& fields, std::string_view name) noexcept {
    if constexpr (http_header_field_message<source_type>) {
        return find_unique_http_header(fields.headers(), name);
    } else {
        http_unique_field_lookup result;
        for (const auto& field : fields) {
            if (http_ascii_equals_ignore_case(field.name(), name)) {
                result.add_occurrence(field.value());
                if (result.status_ == unique_lookup_status::repeated) {
                    break;
                }
            }
        }
        return result;
    }
}

// Singular convenience accessors over received fields select the first
// matching field line (see doc/ruvia-http.md#api-http-header-h) without copying its value.
template <detail::http_borrowed_header_source source_type>
[[nodiscard]] constexpr std::optional<std::string_view> find_first_http_header(source_type&& fields, std::string_view name) noexcept {
    if constexpr (http_header_field_message<source_type>) {
        return find_first_http_header(fields.headers(), name);
    } else {
        for (const auto& field : fields) {
            if (http_ascii_equals_ignore_case(field.name(), name)) {
                return std::string_view(field.value());
            }
        }
        return std::nullopt;
    }
}

// The single mutable header parameter: a header container edited in place,
// so a function rewriting fields is declared once and accepts an lvalue
// std::vector<http_header>, http_response_headers or http_response. A
// response target keeps its own field rules: names and values are validated
// and a rejected one throws std::invalid_argument. The container is borrowed
// for the duration of the call that receives this parameter.
//
// Each operation below either applies completely or leaves the container
// unchanged, and the removals cannot fail. An edit that writes more than once
// goes through transact().
class editable_http_headers final {
public:
    editable_http_headers(std::vector<http_header>& fields) noexcept :
        target_(&fields) {}
    editable_http_headers(http_response_headers& block) noexcept :
        target_(&block) {}
    editable_http_headers(http_response& response) noexcept :
        target_(&response) {}

    // A caller-owned candidate with set/append/remove operations can be edited
    // directly, without materializing owned http_header objects. Each mutation
    // must be atomic; remove must not throw and must accept a borrowed name.
    template <http_header_field_range range_type>
        requires requires(range_type& fields, std::string_view name, std::string_view value) {
            fields.set(name, value);
            fields.append(name, value);
            { fields.remove(name) } noexcept;
        }
    editable_http_headers(range_type& fields) noexcept :
        target_(custom_target{
            &fields,
            +[](const void* target) noexcept -> http_header_range_view {
                return http_header_range_view(*static_cast<const range_type*>(target));
            },
            +[](void* target, std::string_view name) noexcept {
                static_cast<range_type*>(target)->remove(name);
            },
            +[](void* target, std::string_view name, std::string_view value) {
                static_cast<range_type*>(target)->set(name, value);
            },
            +[](void* target, std::string_view name, std::string_view value) {
                static_cast<range_type*>(target)->append(name, value);
            }
        }) {}

    // Current field lines. Any mutation below invalidates the view.
    [[nodiscard]] http_header_range_view fields() const noexcept;
    // Erases every line named `name` (case-insensitive); a name no field can
    // carry erases nothing. `name` must not borrow this container.
    void remove(std::string_view name) noexcept;
    // Erases every line whose name `matches`.
    void remove_if(bool (*matches)(std::string_view field_name) noexcept) noexcept;
    // Leaves exactly one line for `name`: the first existing line is replaced
    // and later ones are erased, or the line is added.
    void set(std::string_view name, std::string_view value);
    // Adds one more line for `name`, keeping existing ones.
    void append(std::string_view name, std::string_view value);

    // Runs a multi-write edit as one header transaction. An http_response is
    // edited through a candidate copy of its header block (with room for
    // `additional_fields` more lines) that is published without failure only
    // after `edit` returns, so a throwing write leaves the response unchanged.
    // A std::vector<http_header> or http_response_headers is already the
    // caller's own candidate storage and is edited directly.
    template <std::invocable<editable_http_headers> edit_type>
    void transact(edit_type edit, std::size_t additional_fields = 0) {
        run_transaction(
            [](void* callable, editable_http_headers candidate) {
            (*static_cast<edit_type*>(callable))(candidate);
        },
            &edit, additional_fields);
    }

private:
    void run_transaction(void (*edit)(void* callable, editable_http_headers candidate), void* callable, std::size_t additional_fields);

    struct custom_target final {
        void* object_;
        http_header_range_view (*fields_)(const void*) noexcept;
        void (*remove_)(void*, std::string_view) noexcept;
        void (*set_)(void*, std::string_view, std::string_view);
        void (*append_)(void*, std::string_view, std::string_view);
    };

    std::variant<std::vector<http_header>*, http_response_headers*, http_response*, custom_target> target_;
};

}  // namespace ruvia

namespace std::ranges {

template <>
inline constexpr bool enable_borrowed_range<ruvia::http_header_range_view> = true;

}  // namespace std::ranges
