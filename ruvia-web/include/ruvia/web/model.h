#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/http/http_response.h"
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define RUVIA_JSON_SCAN_SSE2 1
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define RUVIA_JSON_SCAN_NEON 1
#endif

namespace ruvia {

enum class integer_parse_error {
    invalid_format,
    out_of_range,
};

namespace detail {
template <typename t_type>
concept parsed_integer =
    std::integral<t_type> && !std::same_as<std::remove_cv_t<t_type>, bool> &&
    !std::same_as<std::remove_cv_t<t_type>, char> &&
    !std::same_as<std::remove_cv_t<t_type>, wchar_t> &&
    !std::same_as<std::remove_cv_t<t_type>, char8_t> &&
    !std::same_as<std::remove_cv_t<t_type>, char16_t> &&
    !std::same_as<std::remove_cv_t<t_type>, char32_t> &&
    requires(const char* first, const char* last, t_type& value) {
        std::from_chars(first, last, value, 10);
    };
}  // namespace detail

// Strict decimal input: no whitespace, leading '+', or trailing characters.
// A leading '-' is accepted only for signed integers. Format errors take
// precedence over overflow when the input also contains trailing characters.
template <detail::parsed_integer t_type>
[[nodiscard]] constexpr std::variant<t_type, integer_parse_error> parse_integer(std::string_view text) noexcept {
    if (text.empty()) {
        return integer_parse_error::invalid_format;
    }
    t_type value{};
    const auto* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value, 10);
    if (error == std::errc::invalid_argument || ptr != end) {
        return integer_parse_error::invalid_format;
    }
    if (error == std::errc::result_out_of_range) {
        return integer_parse_error::out_of_range;
    }
    return value;
}

template <std::size_t n>
struct fixed_string {
    char value_[n]{};

    constexpr fixed_string(const char (&text)[n]) noexcept {
        for (std::size_t i = 0; i < n; ++i) {
            value_[i] = text[i];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::string_view(value_, n - 1);
    }
    [[nodiscard]] constexpr std::string_view view() const&& = delete;
    [[nodiscard]] constexpr const char* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }
    [[nodiscard]] constexpr const char* data() const&& = delete;
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return n - 1;
    }
    [[nodiscard]] constexpr bool empty() const noexcept {
        return n == 1;
    }
    constexpr operator std::string_view() const& noexcept RUVIA_LIFETIMEBOUND {
        return view();
    }
    constexpr operator std::string_view() const&& = delete;
};

template <std::size_t n>
fixed_string(const char (&)[n]) -> fixed_string<n>;

template <std::size_t left_n, std::size_t right_n>
[[nodiscard]] constexpr bool operator==(
    const fixed_string<left_n>& left, const fixed_string<right_n>& right) noexcept {
    if constexpr (left_n != right_n) {
        return false;
    } else {
        return std::ranges::equal(left.value_, right.value_);
    }
}

namespace detail {

// Compile-time lookup shared by every schema keyed by fixed_string names
// (model fields, entity columns, database relations).

// Position of `name` in `names`, or names.size() when it is absent.
[[nodiscard]] consteval std::size_t fixed_name_index(
    std::span<const std::string_view> names, std::string_view name) noexcept {
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == name) {
            return index;
        }
    }
    return names.size();
}

// Whether no two entries of `names` are equal under `equal`.
template <typename equal_type = std::equal_to<>>
[[nodiscard]] consteval bool fixed_names_distinct(
    std::span<const std::string_view> names, equal_type equal = {}) noexcept {
    for (std::size_t left = 0; left < names.size(); ++left) {
        for (std::size_t right = left + 1; right < names.size(); ++right) {
            if (equal(names[left], names[right])) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace detail

}  // namespace ruvia

namespace ruvia::detail {

// Trivial request/runtime view. Never allocates or extends the callable's
// lifetime. Signature policy changes invocation, not storage or binding logic.
template <typename result_type, bool is_noexcept, typename... args_types>
class callback_ref_view final {
public:
    constexpr callback_ref_view() noexcept = default;
    constexpr callback_ref_view(std::nullptr_t) noexcept {}
    // Select concrete pointer types so MSVC resolves the exception specification.
    using invoke_type = std::conditional_t<is_noexcept,
        result_type (*)(void*, args_types...) noexcept,
        result_type (*)(void*, args_types...)>;
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] result_type operator()(args_types... args) const noexcept(is_noexcept) {
        return invoke_target(std::forward<args_types>(args)...);
    }

    [[nodiscard]] friend constexpr bool operator==(callback_ref_view, callback_ref_view) noexcept = default;

private:
    friend struct callback_access;

    constexpr callback_ref_view(void* target, invoke_type invoke) noexcept
        : target_(target),
          invoke_(invoke) {}

    template <typename callable_type>
        requires(std::is_invocable_r_v<result_type, callable_type&, args_types...> &&
                 (!is_noexcept || std::is_nothrow_invocable_r_v<result_type, callable_type&, args_types...>))
    [[nodiscard]] static constexpr callback_ref_view bind(callable_type& callable) noexcept {
        return callback_ref_view(const_cast<void*>(static_cast<const void*>(std::addressof(callable))),
            [](void* target, args_types... args) noexcept(is_noexcept) -> result_type {
                return (*static_cast<callable_type*>(target))(std::forward<args_types>(args)...);
            });
    }

    template <typename... invocation_args>
    [[nodiscard]] result_type invoke_target(invocation_args&&... args) const noexcept(is_noexcept) {
        return invoke_(target_, std::forward<invocation_args>(args)...);
    }

    void* target_{nullptr};
    invoke_type invoke_{nullptr};
};

// Signature decomposition selects the borrowed view's noexcept policy.
template <typename signature_type>
struct callback_signature;

template <typename result_type, typename... args_types>
struct callback_signature<result_type(args_types...)> final {
    template <template <typename, bool, typename...> typename family_type>
    using apply = family_type<result_type, false, args_types...>;
};

template <typename result_type, typename... args_types>
struct callback_signature<result_type(args_types...) noexcept> final {
    template <template <typename, bool, typename...> typename family_type>
    using apply = family_type<result_type, true, args_types...>;
};

template <typename signature_type>
using callback_ref = typename callback_signature<signature_type>::template apply<callback_ref_view>;

struct callback_access final {
    template <typename signature_type>
    [[nodiscard]] static constexpr callback_ref<signature_type> make(
        void* target, typename callback_ref<signature_type>::invoke_type invoke) noexcept {
        return callback_ref<signature_type>(target, invoke);
    }

    template <typename signature_type>
    [[nodiscard]] static callback_ref<signature_type> ref(const move_only_function<signature_type>& owner_value) noexcept {
        return owner_value ? callback_ref<signature_type>::bind(owner_value) : callback_ref<signature_type>{};
    }

    template <typename callback_type, typename deleter_type>
    [[nodiscard]] static auto ref(const std::unique_ptr<callback_type, deleter_type>& owner_value) noexcept {
        using ref_type = decltype(ref(*owner_value));
        return owner_value ? ref(*owner_value) : ref_type{};
    }

    template <typename signature_type, typename callable_type>
    [[nodiscard]] static constexpr callback_ref<signature_type> bind(callable_type& callable) noexcept {
        return callback_ref<signature_type>::bind(callable);
    }
    template <typename signature_type, typename callable_type>
        requires(!std::is_lvalue_reference_v<callable_type>)
    static void bind(callable_type&&) = delete;

    template <typename ref_type>
    [[nodiscard]] static constexpr void* target(ref_type value) noexcept {
        return value.target_;
    }

    template <typename ref_type>
    [[nodiscard]] static constexpr typename ref_type::invoke_type invoke(ref_type value) noexcept {
        return value.invoke_;
    }
};

static_assert(sizeof(callback_ref<void()>) == 2 * sizeof(void*));
static_assert(std::is_trivially_copyable_v<callback_ref<void()>>);
}  // namespace ruvia::detail

// The three non-owning callable shapes a route runs: a buffered handler, a
// streaming (response-stream / websocket / tunnel) handler, and one middleware
// frame. Each binds a worker-local instance to a process-invariant invoke thunk.

namespace ruvia {

class context;
class next;

namespace detail {

enum class request_body_mode : std::uint8_t { buffered,
    stream };

using route_handler = callback_ref<task<http_response>(context&)>;
using route_stream_handler = callback_ref<task<void>(context&)>;
using route_middleware = callback_ref<task<void>(context&, next&)>;

}  // namespace detail

class validator;

inline constexpr std::size_t max_validation_text_bytes = 1024;

namespace detail {
struct validation_issue_access;

[[nodiscard]] inline std::string_view bounded_validation_text(std::string_view value) noexcept {
    auto count = std::min(value.size(), max_validation_text_bytes);
    // Keep a UTF-8 prefix complete when the last code point crosses the bound.
    while (count < value.size() && count > 0 &&
           (static_cast<unsigned char>(value[count]) & 0xc0U) == 0x80U) {
        --count;
    }
    return value.substr(0, count);
}
}  // namespace detail

struct validation_issue_options final {
    borrowed_text field_{};
    borrowed_text code_{};
    borrowed_text message_{};
};

class validation_issue final {
public:
    validation_issue(const validation_issue&) = default;
    validation_issue(validation_issue&&) noexcept = default;
    validation_issue& operator=(const validation_issue& other);
    validation_issue& operator=(validation_issue&& other) noexcept;

    [[nodiscard]] std::string_view field() const& noexcept RUVIA_LIFETIMEBOUND {
        return field_;
    }
    [[nodiscard]] std::string_view field() const&& = delete;

    [[nodiscard]] std::string_view code() const& noexcept RUVIA_LIFETIMEBOUND {
        return code_;
    }
    [[nodiscard]] std::string_view code() const&& = delete;

    [[nodiscard]] std::string_view message() const& noexcept RUVIA_LIFETIMEBOUND {
        return message_;
    }
    [[nodiscard]] std::string_view message() const&& = delete;

private:
    friend class validator;
    friend struct detail::validation_issue_access;

    void swap(validation_issue& other) noexcept;

    explicit validation_issue(validation_issue_options options)
        : field_(detail::bounded_validation_text(options.field_.view())),
          code_(detail::bounded_validation_text(options.code_.view())),
          message_(detail::bounded_validation_text(options.message_.view())) {}

    std::string field_;
    std::string code_;
    std::string message_;
};
using borrowed_validation_issues = borrowed_span<validation_issue>;

struct http_error_info_options final {
    http_status_code status_{http_status::internal_server_error};
    borrowed_text code_{};
    borrowed_text message_{};
    borrowed_text status_text_{};
    borrowed_validation_issues validation_issues_{};
};

// Non-owning Web application error metadata used by context and custom error
// handlers. Every borrowed value must outlive this view; basic_string rvalues
// and temporary owning validation-issue ranges are rejected. The JSON error
// envelope is a framework product concern, not an HTTP protocol primitive, so
// this type belongs to ruvia-web.
class http_error_info final {
public:
    constexpr explicit http_error_info(http_error_info_options options = {}) noexcept
        : status_(options.status_),
          status_text_(options.status_text_.view()),
          code_(options.code_.view()),
          message_(options.message_.view()),
          validation_issues_(options.validation_issues_.view()) {}

    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr std::string_view status_text() const noexcept {
        return status_text_;
    }

    [[nodiscard]] constexpr std::string_view code() const noexcept {
        return code_;
    }

    [[nodiscard]] constexpr std::string_view message() const noexcept {
        return message_;
    }

    [[nodiscard]] constexpr std::span<const validation_issue> validation_issues() const noexcept {
        return validation_issues_;
    }

private:
    http_status_code status_{http_status::internal_server_error};
    std::string_view status_text_{};
    std::string_view code_{};
    std::string_view message_{};
    std::span<const validation_issue> validation_issues_{};
};

// The owning Web application error: status, code, message, status text and
// validation issues are deep-copied from the options.
class http_error final : public std::exception {
public:
    explicit http_error(http_error_info_options options);
    http_error(const http_error&) = default;
    http_error& operator=(const http_error& other);
    http_error(http_error&&) noexcept = default;
    http_error& operator=(http_error&&) noexcept = default;
    [[nodiscard]] const char* what() const noexcept override;
    [[nodiscard]] http_error_info info() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] http_error_info info() const&& = delete;

    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] std::string_view status_text() const& noexcept RUVIA_LIFETIMEBOUND {
        return status_text_;
    }
    [[nodiscard]] std::string_view status_text() const&& = delete;

    [[nodiscard]] std::string_view code() const& noexcept RUVIA_LIFETIMEBOUND {
        return code_;
    }
    [[nodiscard]] std::string_view code() const&& = delete;

    [[nodiscard]] std::string_view message() const& noexcept RUVIA_LIFETIMEBOUND {
        return message_;
    }
    [[nodiscard]] std::string_view message() const&& = delete;

    [[nodiscard]] std::span<const validation_issue> validation_issues() const& noexcept RUVIA_LIFETIMEBOUND {
        return validation_issues_;
    }
    [[nodiscard]] std::span<const validation_issue> validation_issues() const&& = delete;

private:
    friend class validator;
    http_error(http_status_code status, std::string_view code, std::string_view message,
        std::string_view status_text, std::vector<validation_issue>&& issues);
    http_status_code status_{http_status::internal_server_error};
    std::string status_text_;
    std::string code_;
    std::string message_;
    std::vector<validation_issue> validation_issues_;
};

[[nodiscard]] std::string_view default_error_code(http_status_code status) noexcept;

// UTF-8 text describing any exception, for logging a failure such as the one
// application::run rethrows. On Windows, what() of a std::system_error in the
// system or Asio category carries the OS message in the ANSI code page; that
// message is replaced with its UTF-8 form. Any other std::exception yields
// what(), a non-standard exception a fixed description, and null an empty
// string.
[[nodiscard]] std::string exception_text(const std::exception_ptr& exception);

namespace detail {
using http_error_handler_ref = callback_ref<task<http_response>(context&, http_error_info)>;
using http_not_found_handler_ref = callback_ref<task<http_response>(context&)>;
}  // namespace detail

// Answers a request that failed. Accepts a plain function -- on_error(&handler)
// -- and equally a lambda or any other callable, including one that captures
// the logger, config, or metrics sink the handler needs. A self-contained
// callable is owned by the handler value; references captured by that callable
// must still outlive the registered handler.
// 空函数指针构造空处理器；未配置时不得调用。所有权与退役语义同 move_only_function。
using http_error_handler = move_only_function<task<http_response>(context&, http_error_info)>;

// Answers a request that matched no route.
// 空函数指针与目标析构重入的契约同 http_error_handler。
using http_not_found_handler = move_only_function<task<http_response>(context&)>;

struct scoped_error_handler_options final {
    borrowed_text prefix_{};
    http_error_handler handler_{nullptr};
};

struct scoped_not_found_handler_options final {
    borrowed_text prefix_{};
    http_not_found_handler handler_{nullptr};
};

}  // namespace ruvia

namespace ruvia::detail {

// Converts a decimal whose grammar the caller has already validated (optional
// '-', digits with an optional non-empty fraction, optional exponent; no '+',
// whitespace or non-finite spellings). An all-zero significand is a signed
// zero whatever its exponent; otherwise overflow or underflow is an error.
template <std::floating_point t_type>
    requires std::same_as<t_type, std::remove_cv_t<t_type>>
[[nodiscard]] std::optional<t_type> convert_decimal_number(
    std::string_view validated, bool nonzero_significand) noexcept {
    if (!nonzero_significand) {
        return validated.starts_with('-') ? -t_type{0} : t_type{0};
    }
    t_type value = 0;
    const auto* end = validated.data() + validated.size();
    const auto converted = std::from_chars(validated.data(), end, value, std::chars_format::general);
    if (converted.ec != std::errc{} || converted.ptr != end) {
        return std::nullopt;
    }
    return value;
}

// Overflow or underflow of a finite decimal is an error.
template <std::floating_point t_type = double>
    requires std::same_as<t_type, std::remove_cv_t<t_type>>
[[nodiscard]] std::optional<t_type> parse_decimal_number(std::string_view text) noexcept {
    // Keep the accepted grammar narrower than from_chars: no leading '+',
    // whitespace, empty fraction, NaN payloads, or other non-finite spellings.
    const bool negative = text.starts_with('-');
    std::size_t index = negative ? 1 : 0;
    bool saw_non_zero = false;
    const auto consume_digits = [&] {
        const auto begin = index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            saw_non_zero = saw_non_zero || text[index] != '0';
            ++index;
        }
        return index - begin;
    };
    const auto integral_digits = consume_digits();
    std::size_t fractional_digits = 0;
    if (index < text.size() && text[index] == '.') {
        ++index;
        fractional_digits = consume_digits();
        if (fractional_digits == 0) {
            return std::nullopt;
        }
    }
    if (integral_digits == 0 && fractional_digits == 0) {
        return std::nullopt;
    }
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
            ++index;
        }
        const auto exponent_begin = index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            ++index;
        }
        if (index == exponent_begin) {
            return std::nullopt;
        }
    }
    if (index != text.size()) {
        return std::nullopt;
    }
    return convert_decimal_number<t_type>(text, saw_non_zero);
}

inline constexpr std::size_t max_json_depth = 64;

inline void skip_json_whitespace(std::string_view& input) noexcept {
    while (!input.empty()) {
        const char c = input.front();
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return;
        }
        input.remove_prefix(1);
    }
}

[[nodiscard]] inline bool consume_json_char(std::string_view& input, char expected) noexcept {
    skip_json_whitespace(input);
    if (input.empty() || input.front() != expected) {
        return false;
    }
    input.remove_prefix(1);
    return true;
}

[[nodiscard]] inline bool consume_json_literal(
    std::string_view& input, std::string_view literal) noexcept {
    skip_json_whitespace(input);
    if (!input.starts_with(literal)) {
        return false;
    }
    input.remove_prefix(literal.size());
    return true;
}

[[nodiscard]] inline std::size_t find_json_string_token_byte(
    std::string_view input, std::size_t offset) noexcept {
    if (offset >= input.size()) {
        return std::string_view::npos;
    }

#if defined(RUVIA_JSON_SCAN_SSE2)
    const auto quote = _mm_set1_epi8('"');
    const auto backslash = _mm_set1_epi8('\\');
    const auto sign_bit = _mm_set1_epi8(static_cast<char>(-128));
    const auto control_limit = _mm_set1_epi8(static_cast<char>(-96));
    while (offset + 16 <= input.size()) {
        const auto bytes_value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input.data() + offset));
        const auto unsigned_bytes = _mm_xor_si128(bytes_value, sign_bit);
        auto special = _mm_or_si128(_mm_cmpeq_epi8(bytes_value, quote), _mm_cmpeq_epi8(bytes_value, backslash));
        special = _mm_or_si128(special, _mm_cmplt_epi8(unsigned_bytes, control_limit));
        const auto mask =
            static_cast<unsigned>(_mm_movemask_epi8(special)) | static_cast<unsigned>(_mm_movemask_epi8(bytes_value));
        if (mask != 0) {
            return offset + std::countr_zero(mask);
        }
        offset += 16;
    }
#elif defined(RUVIA_JSON_SCAN_NEON)
    const auto quote = vdupq_n_u8(static_cast<std::uint8_t>('"'));
    const auto backslash = vdupq_n_u8(static_cast<std::uint8_t>('\\'));
    const auto control_limit = vdupq_n_u8(0x20);
    const auto high_bit_limit = vdupq_n_u8(0x80);
    while (offset + 16 <= input.size()) {
        const auto bytes_value = vld1q_u8(reinterpret_cast<const std::uint8_t*>(input.data() + offset));
        auto special = vorrq_u8(vceqq_u8(bytes_value, quote), vceqq_u8(bytes_value, backslash));
        special = vorrq_u8(special, vcltq_u8(bytes_value, control_limit));
        special = vorrq_u8(special, vcgeq_u8(bytes_value, high_bit_limit));
        if (vmaxvq_u8(special) != 0) {
            for (std::size_t index = 0; index < 16; ++index) {
                const auto byte = static_cast<unsigned char>(input[offset + index]);
                if (byte == '"' || byte == '\\' || byte < 0x20 || byte >= 0x80) {
                    return offset + index;
                }
            }
        }
        offset += 16;
    }
#endif

    for (; offset < input.size(); ++offset) {
        const auto byte = static_cast<unsigned char>(input[offset]);
        if (byte == '"' || byte == '\\' || byte < 0x20 || byte >= 0x80) {
            return offset;
        }
    }
    return std::string_view::npos;
}

// O(1) reservation hint for append_json_string, avoiding a pre-scan pass over
// the string before escaping.
[[nodiscard]] constexpr std::size_t json_string_size_hint(std::string_view value) noexcept {
    return value.size() + 2;
}

// Whether RFC 8259 §7 forbids this ASCII byte unescaped inside a string.
[[nodiscard]] constexpr bool json_byte_needs_escape(unsigned char byte) noexcept {
    return byte < 0x20 || byte == '"' || byte == '\\';
}

// Appends the escape of one byte for which json_byte_needs_escape holds: the
// two-character form where RFC 8259 §7 defines one, otherwise lowercase \u00xx
// (as ECMAScript JSON.stringify). The single owner of JSON escape spelling for
// both runtime strings and compile-time model keys.
template <typename output_type>
constexpr void append_json_escape(output_type& output, unsigned char byte) {
    switch (byte) {
        case '"':
            output.append(std::string_view("\\\""));
            break;
        case '\\':
            output.append(std::string_view("\\\\"));
            break;
        case '\b':
            output.append(std::string_view("\\b"));
            break;
        case '\f':
            output.append(std::string_view("\\f"));
            break;
        case '\n':
            output.append(std::string_view("\\n"));
            break;
        case '\r':
            output.append(std::string_view("\\r"));
            break;
        case '\t':
            output.append(std::string_view("\\t"));
            break;
        default:
            output.append(std::string_view("\\u00"));
            output.push_back(http_lower_hex_digit(byte >> 4));
            output.push_back(http_lower_hex_digit(byte));
            break;
    }
}

// Escapes and appends a JSON string. RFC 8259 §8.1 requires interchanged JSON
// to be UTF-8, so the output is well-formed UTF-8 whatever the input bytes are:
// well-formed multibyte sequences are copied verbatim, and each maximal subpart
// of an ill-formed sequence (a stray continuation byte, a truncated or overlong
// sequence, a surrogate, a value above U+10FFFF, or text in another encoding
// such as a Windows ANSI code page) becomes one U+FFFD REPLACEMENT CHARACTER
// (Unicode 3.9 "U+FFFD substitution of maximal subparts"). ASCII runs are still
// scanned 16 bytes at a time.
template <typename string_t_type>
inline void append_json_string(string_t_type& output, std::string_view value) {
    constexpr std::string_view replacement_character = "\xEF\xBF\xBD";
    output.push_back('"');
    std::size_t chunk_begin = 0;
    std::size_t i = 0;
    while ((i = find_json_string_token_byte(value, i)) != std::string_view::npos) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c >= 0x80) {
            const auto sequence = ::ruvia::scan_utf8_sequence(value, i);
            if (!sequence.well_formed_) {
                if (i > chunk_begin) {
                    output.append(value.data() + chunk_begin, i - chunk_begin);
                }
                output.append(replacement_character.data(), replacement_character.size());
                chunk_begin = i + sequence.length_;
            }
            i += sequence.length_;
            continue;
        }
        if (i > chunk_begin) {
            output.append(value.data() + chunk_begin, i - chunk_begin);
        }
        append_json_escape(output, c);
        chunk_begin = i + 1;
        ++i;
    }
    if (chunk_begin < value.size()) {
        output.append(value.data() + chunk_begin, value.size() - chunk_begin);
    }
    output.push_back('"');
}

// Appends `,"<name>":` with `name` JSON-escaped, for object member keys known
// at compile time. Returns false when `name` is not well-formed UTF-8: the
// runtime writer would substitute U+FFFD, so the parser, which compares decoded
// keys with the declared name, could never read such a member back.
template <typename output_type>
[[nodiscard]] constexpr bool append_json_member_key(output_type& output, std::string_view name) {
    output.push_back(',');
    output.push_back('"');
    for (std::size_t i = 0; i < name.size();) {
        const auto byte = static_cast<unsigned char>(name[i]);
        if (byte >= 0x80) {
            const auto sequence = ::ruvia::scan_utf8_sequence(name, i);
            if (!sequence.well_formed_) {
                return false;
            }
            output.append(name.substr(i, sequence.length_));
            i += sequence.length_;
            continue;
        }
        if (json_byte_needs_escape(byte)) {
            append_json_escape(output, byte);
        } else {
            output.push_back(name[i]);
        }
        ++i;
    }
    output.push_back('"');
    output.push_back(':');
    return true;
}

// Read exactly four hex digits as a UTF-16 code unit.
[[nodiscard]] inline bool read_json_hex4(std::string_view input, std::uint32_t& value) noexcept {
    if (input.size() < 4) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto hex = http_decode_hex_nibble(input[i]);
        if (hex < 0) {
            return false;
        }
        value = (value << 4) | static_cast<std::uint32_t>(hex);
    }
    return true;
}

[[nodiscard]] inline bool is_json_high_surrogate(std::uint32_t code_unit) noexcept {
    return code_unit >= 0xD800 && code_unit <= 0xDBFF;
}

[[nodiscard]] inline bool is_json_low_surrogate(std::uint32_t code_unit) noexcept {
    return code_unit >= 0xDC00 && code_unit <= 0xDFFF;
}

template <typename output_t_type>
void append_utf8(output_t_type& output, std::uint32_t code_point) {
    if (code_point <= 0x7F) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

enum class json_string_encoding : std::uint8_t { literal,
    escaped };

class json_string_token final {
public:
    [[nodiscard]] std::string_view raw() const noexcept {
        return raw_;
    }

    [[nodiscard]] json_string_encoding encoding() const noexcept {
        return encoding_;
    }

private:
    template <typename escaped_budget_type>
    friend std::optional<json_string_token> parse_json_string(
        std::string_view& input, std::string* decoded, escaped_budget_type&& consume_escaped_bytes);
    json_string_token(std::string_view raw, json_string_encoding encoding) noexcept
        : raw_(raw),
          encoding_(encoding) {}

    std::string_view raw_;
    json_string_encoding encoding_;
};

// Scans one JSON string and commits the input cursor only after the closing
// quote and all encoded bytes have been validated. The token owns both pieces
// of scan state, so callers cannot observe a raw view without its encoding.
//
// A literal string is read through raw() and never touches `decoded`. When
// `decoded` is non-null and the string contains an escape, the same pass
// replaces its contents with the decoded text, starting at the first escape,
// so escaped input is neither rescanned nor copied before it is known to need
// decoding. After a failed scan `decoded` holds unspecified contents.
template <typename escaped_budget_type>
[[nodiscard]] inline std::optional<json_string_token> parse_json_string(
    std::string_view& input, std::string* decoded, escaped_budget_type&& consume_escaped_bytes) {
    auto remaining = input;
    skip_json_whitespace(remaining);
    if (remaining.empty() || remaining.front() != '"') {
        return std::nullopt;
    }
    remaining.remove_prefix(1);

    auto encoding = json_string_encoding::literal;
    // Raw bytes before this offset are already represented in *decoded.
    std::size_t decoded_until = 0;
    for (std::size_t i = 0; i < remaining.size();) {
        i = find_json_string_token_byte(remaining, i);
        if (i == std::string_view::npos) {
            return std::nullopt;
        }
        const char c = remaining[i];
        if (c == '\\') {
            if (encoding == json_string_encoding::literal && decoded != nullptr) {
                decoded->clear();
            }
            encoding = json_string_encoding::escaped;
            if (i + 1 >= remaining.size()) {
                return std::nullopt;
            }
            std::uint32_t code_point = 0;
            std::size_t escape_length = 2;
            switch (const char escape = remaining[i + 1]) {
                case '"':
                case '\\':
                case '/':
                    code_point = static_cast<unsigned char>(escape);
                    break;
                case 'b':
                    code_point = '\b';
                    break;
                case 'f':
                    code_point = '\f';
                    break;
                case 'n':
                    code_point = '\n';
                    break;
                case 'r':
                    code_point = '\r';
                    break;
                case 't':
                    code_point = '\t';
                    break;
                case 'u': {
                    if (i + 5 >= remaining.size() || !read_json_hex4(remaining.substr(i + 2), code_point)) {
                        return std::nullopt;
                    }
                    escape_length = 6;
                    if (is_json_high_surrogate(code_point)) {
                        std::uint32_t low = 0;
                        if (i + 11 >= remaining.size() || remaining[i + 6] != '\\' ||
                            remaining[i + 7] != 'u' || !read_json_hex4(remaining.substr(i + 8), low) ||
                            !is_json_low_surrogate(low)) {
                            return std::nullopt;
                        }
                        code_point = 0x10000 + (((code_point - 0xD800) << 10) | (low - 0xDC00));
                        escape_length = 12;
                    } else if (is_json_low_surrogate(code_point)) {
                        return std::nullopt;
                    }
                    break;
                }
                default:
                    return std::nullopt;
            }
            const auto next_until = i + escape_length;
            if (decoded != nullptr) {
                if (!consume_escaped_bytes(next_until - decoded_until)) {
                    return std::nullopt;
                }
                decoded->append(remaining.data() + decoded_until, i - decoded_until);
                append_utf8(*decoded, code_point);
            }
            i = next_until;
            decoded_until = i;
            continue;
        }
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20) {
            return std::nullopt;
        }
        if (c == '"') {
            if (encoding == json_string_encoding::escaped && decoded != nullptr) {
                if (!consume_escaped_bytes(i - decoded_until)) {
                    return std::nullopt;
                }
                decoded->append(remaining.data() + decoded_until, i - decoded_until);
            }
            const json_string_token token(remaining.substr(0, i), encoding);
            remaining.remove_prefix(i + 1);
            input = remaining;
            return token;
        }
        if (uc >= 0x80) {
            const auto sequence = ::ruvia::scan_utf8_sequence(remaining, i);
            if (!sequence.well_formed_) {
                return std::nullopt;
            }
            i += sequence.length_;
            continue;
        }
        ++i;
    }

    return std::nullopt;
}

[[nodiscard]] inline std::optional<json_string_token> parse_json_string(
    std::string_view& input, std::string* decoded = nullptr) {
    return parse_json_string(input, decoded, [](std::size_t) noexcept { return true; });
}

// The text of a string scanned by parse_json_string(input, &decoded).
[[nodiscard]] inline std::string_view json_string_text(
    const json_string_token& token, const std::string& decoded) noexcept {
    return token.encoding() == json_string_encoding::literal ? token.raw() : std::string_view(decoded);
}

struct json_number_token final {
    // Bytes of the RFC 8259 number at the start of the input; 0 when none.
    std::size_t length_{0};
    // Whether an integer or fraction digit is nonzero.
    bool nonzero_significand_{false};
};

[[nodiscard]] inline json_number_token scan_json_number_token(std::string_view input) noexcept {
    json_number_token token;
    std::size_t index = 0;
    const auto consume_digits = [&] {
        const auto begin = index;
        while (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            token.nonzero_significand_ = token.nonzero_significand_ || input[index] != '0';
            ++index;
        }
        return index - begin;
    };
    if (index < input.size() && input[index] == '-') {
        ++index;
    }
    if (index >= input.size()) {
        return {};
    }
    if (input[index] == '0') {
        ++index;
        if (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            return {};
        }
    } else if (input[index] >= '1' && input[index] <= '9') {
        (void)consume_digits();
    } else {
        return {};
    }

    if (index < input.size() && input[index] == '.') {
        ++index;
        if (consume_digits() == 0) {
            return {};
        }
    }
    if (index < input.size() && (input[index] == 'e' || input[index] == 'E')) {
        ++index;
        if (index < input.size() && (input[index] == '+' || input[index] == '-')) {
            ++index;
        }
        const auto exponent_begin = index;
        while (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            ++index;
        }
        if (index == exponent_begin) {
            return {};
        }
    }
    token.length_ = index;
    return token;
}

[[nodiscard]] inline bool skip_json_number_token(std::string_view& input) noexcept {
    const auto length = scan_json_number_token(input).length_;
    if (length == 0) {
        return false;
    }
    input.remove_prefix(length);
    return true;
}

template <typename number_t_type>
[[nodiscard]] bool parse_json_number_value(std::string_view& input, number_t_type& value) {
    skip_json_whitespace(input);
    const auto token = scan_json_number_token(input);
    if (token.length_ == 0) {
        return false;
    }

    const auto number = input.substr(0, token.length_);
    if constexpr (std::is_floating_point_v<number_t_type>) {
        // The scan already enforced the RFC 8259 grammar, which excludes the
        // non-finite spellings; only range errors remain.
        const auto parsed_value = convert_decimal_number<number_t_type>(number, token.nonzero_significand_);
        if (!parsed_value || !std::isfinite(*parsed_value)) {
            return false;
        }
        value = *parsed_value;
    } else {
        const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (ec != std::errc{} || ptr != number.data() + number.size()) {
            return false;
        }
    }
    input.remove_prefix(token.length_);
    return true;
}

}  // namespace ruvia::detail

#if defined(RUVIA_JSON_SCAN_SSE2)
#undef RUVIA_JSON_SCAN_SSE2
#elif defined(RUVIA_JSON_SCAN_NEON)
#undef RUVIA_JSON_SCAN_NEON
#endif

// The single owner of the JSON array and object grammars, structural value
// skipping, and object field traversal. Skipping, typed model parsing and
// dynamic token traversal all walk containers through these functions.

namespace ruvia::detail {

// Consumes one array directly from input and lets the visitor consume each
// element from the same cursor, positioned at the element's first byte. The
// visitor returns false to reject the array. input advances past ']' only when
// the whole array is accepted; nested values visited by the caller belong at
// depth + 1.
template <typename visitor_type>
[[nodiscard]] bool consume_json_array_elements(std::string_view& input, std::size_t depth, visitor_type&& visitor) {
    if (depth > max_json_depth) {
        return false;
    }

    auto remaining = input;
    if (!consume_json_char(remaining, '[')) {
        return false;
    }
    skip_json_whitespace(remaining);
    if (!remaining.empty() && remaining.front() == ']') {
        remaining.remove_prefix(1);
        input = remaining;
        return true;
    }

    auto& visitor_ref = visitor;
    while (true) {
        skip_json_whitespace(remaining);
        if (!static_cast<bool>(visitor_ref(remaining))) {
            return false;
        }

        skip_json_whitespace(remaining);
        if (!remaining.empty() && remaining.front() == ']') {
            remaining.remove_prefix(1);
            input = remaining;
            return true;
        }
        if (!consume_json_char(remaining, ',')) {
            return false;
        }
    }
}

// Consumes one object directly from input. For each member the visitor
// receives the scanned key token and consumes the value from the same cursor
// (positioned after ':'); it returns false to reject the object. When
// `decoded_key` is non-null, escaped keys are decoded into it by the key scan
// itself (see parse_json_string). input advances past '}' only when the whole
// object is accepted; nested values visited by the caller belong at depth + 1.
template <typename visitor_type, typename key_budget_type>
[[nodiscard]] bool consume_json_object_members(std::string_view& input, std::size_t depth,
    std::string* decoded_key, visitor_type&& visitor, key_budget_type&& consume_key_bytes) {
    if (depth > max_json_depth) {
        return false;
    }

    auto remaining = input;
    if (!consume_json_char(remaining, '{')) {
        return false;
    }
    skip_json_whitespace(remaining);
    if (!remaining.empty() && remaining.front() == '}') {
        remaining.remove_prefix(1);
        input = remaining;
        return true;
    }

    auto& visitor_ref = visitor;
    while (true) {
        const auto key = parse_json_string(remaining, decoded_key, consume_key_bytes);
        if (!key.has_value() || !consume_json_char(remaining, ':')) {
            return false;
        }
        if (!static_cast<bool>(visitor_ref(*key, remaining))) {
            return false;
        }

        skip_json_whitespace(remaining);
        if (!remaining.empty() && remaining.front() == '}') {
            remaining.remove_prefix(1);
            input = remaining;
            return true;
        }
        if (!consume_json_char(remaining, ',')) {
            return false;
        }
    }
}

template <typename visitor_type>
[[nodiscard]] bool consume_json_object_members(
    std::string_view& input, std::size_t depth, std::string* decoded_key, visitor_type&& visitor) {
    return consume_json_object_members(
        input, depth, decoded_key, std::forward<visitor_type>(visitor), [](std::size_t) noexcept { return true; });
}

[[nodiscard]] inline bool skip_json_value(std::string_view& input, std::size_t depth) noexcept;

template <typename element_budget_type>
[[nodiscard]] bool skip_json_value(std::string_view& input, std::size_t depth,
    element_budget_type& consume_element) noexcept;

template <typename element_budget_type>
[[nodiscard]] bool skip_json_array(std::string_view& input, std::size_t depth,
    element_budget_type& consume_element) noexcept {
    return consume_json_array_elements(input, depth, [depth, &consume_element](std::string_view& element) noexcept {
        return consume_element() && skip_json_value(element, depth + 1, consume_element);
    });
}

template <typename element_budget_type>
[[nodiscard]] bool skip_json_object(std::string_view& input, std::size_t depth,
    element_budget_type& consume_element) noexcept {
    return consume_json_object_members(input, depth, nullptr,
        [depth, &consume_element](const json_string_token&, std::string_view& value) noexcept {
            return skip_json_value(value, depth + 1, consume_element);
        });
}

template <typename element_budget_type>
[[nodiscard]] bool skip_json_value(std::string_view& input, std::size_t depth,
    element_budget_type& consume_element) noexcept {
    if (depth > max_json_depth) {
        return false;
    }
    skip_json_whitespace(input);
    if (input.empty()) {
        return false;
    }

    switch (input.front()) {
        case '"':
            return parse_json_string(input).has_value();
        case '{':
            // skip_json_object/skip_json_array already add depth+1 for their child values; passing
            // depth+1 here too would double-count and enforce half the documented max_json_depth.
            return skip_json_object(input, depth, consume_element);
        case '[':
            return skip_json_array(input, depth, consume_element);
        case 't':
            return consume_json_literal(input, "true");
        case 'f':
            return consume_json_literal(input, "false");
        case 'n':
            return consume_json_literal(input, "null");
        default:
            return skip_json_number_token(input);
    }
}

[[nodiscard]] inline bool skip_json_value(std::string_view& input, std::size_t depth) noexcept {
    auto consume_element = []() noexcept { return true; };
    return skip_json_value(input, depth, consume_element);
}

enum class json_object_visit_result : unsigned char { complete,
    stopped,
    invalid };

// Consumes one object directly from input and lets the visitor consume each
// value from the same cursor, under the member's decoded key. RUVIA_MODEL uses
// this On-Demand-style traversal so a known value is parsed into its typed
// field without first scanning it to discover a raw slice and then parsing
// that slice again. consume_key_allocation is charged with the raw size of
// every escaped key before the visitor sees it.
template <typename visitor_type, typename key_allocation_type>
[[nodiscard]] bool consume_json_object_fields(std::string_view& input, std::size_t depth,
    visitor_type&& visitor, key_allocation_type&& consume_key_allocation) {
    auto& visitor_ref = visitor;
    std::string decoded_key;
    bool key_started = false;
    return consume_json_object_members(
        input, depth, &decoded_key,
        [&](const json_string_token& key, std::string_view& value_input) {
            key_started = false;
            if (key.encoding() == json_string_encoding::escaped) {
                return static_cast<bool>(visitor_ref(std::string_view(decoded_key), value_input));
            }
            return static_cast<bool>(visitor_ref(key.raw(), value_input));
        },
        [&](std::size_t bytes_value) noexcept {
            const bool first = !std::exchange(key_started, true);
            return consume_key_allocation(bytes_value, first);
        });
}

template <typename visitor_type>
[[nodiscard]] bool consume_json_object_fields(
    std::string_view& input, std::size_t depth, visitor_type&& visitor) {
    return consume_json_object_fields(input, depth,
        std::forward<visitor_type>(visitor), [](std::size_t, bool) noexcept { return true; });
}

// Visits every field of one complete JSON object body as a raw value slice
// (which may carry leading whitespace). Values are skipped at depth 1, so the
// whole body honours max_json_depth. The visitor returns false to stop.
template <typename visitor_type>
    requires std::predicate<visitor_type&, std::string_view, std::string_view>
[[nodiscard]] json_object_visit_result visit_json_object_fields(
    std::string_view body, visitor_type&& visitor) {
    auto input = body;
    auto& visitor_ref = visitor;
    bool stopped = false;
    const bool consumed = consume_json_object_fields(input, 0,
        [&visitor_ref, &stopped](std::string_view key, std::string_view& value_input) {
            const auto value_start = value_input;
            if (!skip_json_value(value_input, 1)) {
                return false;
            }
            const auto value = value_start.substr(0, value_start.size() - value_input.size());
            stopped = !visitor_ref(key, value);
            return !stopped;
        });
    if (stopped) {
        return json_object_visit_result::stopped;
    }
    if (!consumed) {
        return json_object_visit_result::invalid;
    }
    skip_json_whitespace(input);
    return input.empty() ? json_object_visit_result::complete : json_object_visit_result::invalid;
}

}  // namespace ruvia::detail

namespace ruvia {
namespace detail {
struct request_name_value_list_access;
}  // namespace detail

class request_name_value_list final {
public:
    using value_type = http_header_view;
    using const_iterator = const http_header_view*;

    request_name_value_list(const request_name_value_list&) = delete;
    request_name_value_list& operator=(const request_name_value_list&) = delete;
    request_name_value_list(request_name_value_list&& other) noexcept
        : owned_(std::move(other.owned_)),
          case_insensitive_(other.case_insensitive_) {
        items_ = owned_.empty() ? other.items_ : std::span<const http_header_view>(owned_);
        other.items_ = {};
    }
    request_name_value_list& operator=(request_name_value_list&&) = delete;

    [[nodiscard]] const_iterator begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.data();
    }
    [[nodiscard]] const_iterator begin() const&& = delete;

    [[nodiscard]] const_iterator cbegin() const& noexcept RUVIA_LIFETIMEBOUND {
        return begin();
    }
    [[nodiscard]] const_iterator cbegin() const&& = delete;

    [[nodiscard]] const_iterator end() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.data() + items_.size();
    }
    [[nodiscard]] const_iterator end() const&& = delete;

    [[nodiscard]] const_iterator cend() const& noexcept RUVIA_LIFETIMEBOUND {
        return end();
    }
    [[nodiscard]] const_iterator cend() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] const http_header_view* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.data();
    }
    [[nodiscard]] const http_header_view* data() const&& = delete;

    [[nodiscard]] const http_header_view& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return items_[index];
    }
    [[nodiscard]] const http_header_view& operator[](std::size_t) const&& = delete;

    // Duplicate fields are preserved in materialization order; scalar lookup uses
    // the last occurrence. Header lists compare names case-insensitively;
    // other field sources compare exactly. Enumeration preserves name spelling.
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept {
        for (std::size_t i = items_.size(); i > 0; --i) {
            if (names_equal(items_[i - 1].name(), name)) {
                return items_[i - 1].value();
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
        std::size_t result_value = 0;
        for (const auto& item : items_) {
            if (names_equal(item.name(), name)) {
                ++result_value;
            }
        }
        return result_value;
    }

    [[nodiscard]] std::span<const http_header_view> entries() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_;
    }
    [[nodiscard]] std::span<const http_header_view> entries() const&& = delete;

private:
    friend struct detail::request_name_value_list_access;

    explicit request_name_value_list(bool case_insensitive = false) noexcept
        : items_(owned_),
          case_insensitive_(case_insensitive) {}
    explicit request_name_value_list(std::span<const http_header_view> headers) noexcept
        : items_(headers),
          case_insensitive_(true) {}

    [[nodiscard]] bool case_insensitive() const noexcept {
        return case_insensitive_;
    }

    [[nodiscard]] bool names_equal(std::string_view left, std::string_view right) const noexcept {
        return case_insensitive_ ? http_ascii_equals_ignore_case(left, right) : left == right;
    }

    void reserve(std::size_t count) {
        owned_.reserve(count);
        items_ = owned_;
    }

    void push_back(http_header_view value) {
        owned_.push_back(value);
        items_ = owned_;
    }

    std::vector<http_header_view> owned_{};
    std::span<const http_header_view> items_{};
    bool case_insensitive_{false};
};

}  // namespace ruvia

namespace ruvia::detail {
struct request_name_value_list_access final {
    [[nodiscard]] static request_name_value_list make() noexcept {
        return request_name_value_list();
    }
    [[nodiscard]] static request_name_value_list borrow_headers(
        std::span<const http_header_view> headers) noexcept {
        return request_name_value_list(headers);
    }

    [[nodiscard]] static bool case_insensitive(const request_name_value_list& list) noexcept {
        return list.case_insensitive();
    }

    [[nodiscard]] static bool names_equal(const request_name_value_list& list,
        std::string_view left, std::string_view right) noexcept {
        return list.names_equal(left, right);
    }

    static void reserve(request_name_value_list& list, std::size_t count) {
        list.reserve(count);
    }

    static void push_back(request_name_value_list& list, http_header_view value) {
        list.push_back(value);
    }
};

}  // namespace ruvia::detail

namespace ruvia {
class bytes;
}  // namespace ruvia

namespace ruvia::detail {

// Text semantics (ordinary string or JSON token) belong to the facade. This
// value owns the common borrow/clone/transfer and strong publication rules.
class model_text_storage final {
public:
    explicit model_text_storage(std::string_view borrowed = {}) noexcept
        : value_(std::in_place_type<std::string_view>, borrowed) {}

    model_text_storage(const model_text_storage&) = delete;
    model_text_storage& operator=(const model_text_storage&) = delete;
    model_text_storage(model_text_storage&& other) noexcept
        : value_(std::move(other.value_)) {
        other.assign_borrowed({});
    }
    model_text_storage& operator=(model_text_storage&&) = delete;

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* borrowed = std::get_if<std::string_view>(&value_)) {
            return *borrowed;
        }
        return std::get<std::string>(value_);
    }
    std::string_view view() const&& = delete;

    void assign_borrowed(std::string_view value) noexcept {
        value_.emplace<std::string_view>(value);
    }

    void assign_owned(std::string_view value) {
        std::string owned(value);
        value_.emplace<std::string>(std::move(owned));
    }

    void assign_owned(std::string&& value) noexcept {
        value_.emplace<std::string>(std::move(value));
    }

    [[nodiscard]] model_text_storage rebind() const& {
        return model_text_storage(std::string(view()));
    }

    [[nodiscard]] model_text_storage rebind() && {
        if (auto* owned = std::get_if<std::string>(&value_)) {
            model_text_storage rebound(std::move(*owned));
            assign_borrowed({});
            return rebound;
        }
        auto rebound = static_cast<const model_text_storage&>(*this).rebind();
        assign_borrowed({});
        return rebound;
    }

    void assign_from(model_text_storage&& source_value) {
        if (this == &source_value) {
            return;
        }
        auto rebound = std::move(source_value).rebind();
        // Rebinding may throw; replacing with an already-owned variant cannot.
        std::destroy_at(&value_);
        std::construct_at(&value_, std::move(rebound.value_));
    }

private:
    explicit model_text_storage(std::string&& owned) noexcept
        : value_(std::in_place_type<std::string>, std::move(owned)) {}

    std::variant<std::string_view, std::string> value_;
};

template <typename bytes_type = ::ruvia::bytes>
inline void append_model_binary(std::string& output, const bytes_type& value) {
    const auto encoded_size = http_base64_encoded_size(value.size());
    const auto start = output.size();
    output.resize(start + encoded_size + 2);
    output[start] = '"';
    http_base64_encode(output.data() + start + 1, value.view());
    output[start + encoded_size + 1] = '"';
}

template <typename bytes_type = ::ruvia::bytes>
[[nodiscard]] inline std::optional<bytes_type> decode_model_binary(std::string_view encoded) {
    if (encoded.empty()) {
        return bytes_type();
    }
    if (encoded.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> decoded(http_base64_decoded_size_bound(encoded.size()));
    const auto decoded_size = http_base64_decode(
        encoded, decoded, http_base64_charset::standard, http_base64_padding::required);
    if (!decoded_size) {
        return std::nullopt;
    }
    decoded.resize(*decoded_size);
    bytes_type result_value;
    result_value.assign_owned(std::move(decoded));
    return result_value;
}

}  // namespace ruvia::detail

namespace ruvia {

namespace detail {

template <typename t_type>
inline constexpr bool is_model_integer =
    std::is_integral_v<t_type> && !std::is_same_v<t_type, bool> && !std::is_same_v<t_type, char> &&
    !std::is_same_v<t_type, wchar_t> && !std::is_same_v<t_type, char8_t> &&
    !std::is_same_v<t_type, char16_t> && !std::is_same_v<t_type, char32_t>;

template <typename target_type, typename input_type>
[[nodiscard]] constexpr target_type checked_model_integer(input_type input) {
    if (!std::in_range<target_type>(input)) {
        throw std::out_of_range("model integer value is out of range");
    }
    return static_cast<target_type>(input);
}

template <typename target_type, std::floating_point input_type>
[[nodiscard]] constexpr target_type checked_model_float(input_type input) noexcept(
    (std::numeric_limits<input_type>::max)() <= (std::numeric_limits<target_type>::max)()) {
    constexpr auto source_max = (std::numeric_limits<input_type>::max)();
    constexpr auto target_max = (std::numeric_limits<target_type>::max)();
    if constexpr (source_max > target_max) {
        if ((input > target_max || input < -target_max) && input <= source_max && input >= -source_max) {
            throw std::out_of_range("model floating value is out of range");
        }
    }
    return static_cast<target_type>(input);
}

struct model_value_factory;
struct model_value_rebind_access;
struct borrowed_model_token_tag final {};

enum class model_string_storage : std::uint8_t {
    borrowed,
    owned,
};

[[nodiscard]] inline bool model_address_within(
    const void* address, const void* begin, std::size_t bytes_value) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto first = reinterpret_cast<std::uintptr_t>(begin);
    return value >= first && value - first < bytes_value;
}

[[noreturn]] inline void throw_model_move_source_owns_destination() {
    throw std::invalid_argument("model move source owns its destination");
}

struct model_value_rebind_access final {
    template <typename t_type>
    [[nodiscard]] static consteval bool has_rebind_for_model() {
        using value_type = std::remove_cvref_t<t_type>;
        if constexpr (requires { typename value_type::ruvia_model_schema_type; }) {
            return true;
        } else {
            return requires(const value_type& source_value) {
                source_value.rebind_for_model();
            };
        }
    }

    template <typename t_type>
    [[nodiscard]] static std::remove_cvref_t<t_type> own(t_type&& value) {
        using value_type = std::remove_cvref_t<t_type>;
        if constexpr (requires { typename value_type::ruvia_model_schema_type; }) {
            return value_type(std::forward<t_type>(value).fields_.rebind());
        } else if constexpr (requires(value_type& source_value) {
                                 source_value.rebind_for_model();
                             }) {
            if constexpr (std::is_lvalue_reference_v<t_type&&> ||
                          std::is_const_v<std::remove_reference_t<t_type>>) {
                return value.rebind_for_model();
            } else {
                return std::move(value).rebind_for_model();
            }
        } else {
            return std::forward<t_type>(value);
        }
    }

    template <typename t_type>
    [[nodiscard]] static consteval bool can_own() {
        using value_type = std::remove_cvref_t<t_type>;
        if constexpr (requires { typename value_type::ruvia_model_schema_type; }) {
            return true;
        } else {
            return requires(const value_type& source_value, const void* address) {
                source_value.owns_for_model(address);
            };
        }
    }

    // Whether `address` lies in storage that `value` owns, excluding its own
    // inline bytes. Consuming a value destroys everything it owns.
    template <typename t_type>
    [[nodiscard]] static bool owns(const t_type& value, const void* address) noexcept {
        if constexpr (requires { typename t_type::ruvia_model_schema_type; }) {
            return value.fields_.owns(address);
        } else if constexpr (can_own<t_type>()) {
            return value.owns_for_model(address);
        } else {
            return false;
        }
    }

    // Whether consuming `value` would destroy the object at `address`.
    template <typename t_type>
    [[nodiscard]] static bool contains(const t_type& value, const void* address) noexcept {
        return model_address_within(address, std::addressof(value), sizeof(t_type)) || owns(value, address);
    }
};

template <typename t_type>
[[nodiscard]] std::remove_cvref_t<t_type> rebind_model_value(t_type&& value) {
    return model_value_rebind_access::own(std::forward<t_type>(value));
}

template <typename value_type, typename factory_type>
struct model_value_construction final {
    factory_type factory_;

    operator value_type() && {
        return factory_();
    }
};

// Borrowed/move-only values are normalized only after the container has allocated
// their final storage. In particular, a failed growth must not consume an input
// that aliases an existing element. The captured arguments are consumed inline.
template <typename value_type, typename... argument_types>
auto prepare_model_value(const void* destination, argument_types&&... arguments) {
    if constexpr (sizeof...(argument_types) == 1 &&
                  ((std::same_as<std::remove_cvref_t<argument_types>, value_type> &&
                       !std::is_lvalue_reference_v<argument_types> &&
                       !std::is_const_v<std::remove_reference_t<argument_types>>) &&
                      ...)) {
        if ((model_value_rebind_access::contains(arguments, destination) || ...)) {
            throw_model_move_source_owns_destination();
        }
    }
    auto construct = [&arguments...]() -> value_type {
        if constexpr (sizeof...(argument_types) == 1 &&
                      (std::same_as<std::remove_cvref_t<argument_types>, value_type> && ...)) {
            return [&]<typename input_type>(input_type&& value) -> value_type {
                if constexpr (std::is_lvalue_reference_v<input_type&&> ||
                              std::is_const_v<std::remove_reference_t<input_type>>) {
                    return rebind_model_value(static_cast<const value_type&>(value));
                } else {
                    return rebind_model_value(std::move(value));
                }
            }(std::forward<argument_types>(arguments)...);
        } else if constexpr (sizeof...(argument_types) == 0 && std::default_initializable<value_type>) {
            return value_type();
        } else {
            return value_type(std::forward<argument_types>(arguments)...);
        }
    };
    if constexpr (model_value_rebind_access::has_rebind_for_model<value_type>()) {
        return model_value_construction<value_type, decltype(construct)>{std::move(construct)};
    } else {
        // Scalar constructors intentionally reject non-numeric conversions.
        return construct();
    }
}

}  // namespace detail

struct model_parse_options final {
    // Totals across the complete typed or dynamic JSON document, including nested arrays.
    std::size_t max_array_elements_{64 * 1024};
    std::size_t max_representation_bytes_{16 * 1024 * 1024};
};

class string final {
public:
    string() noexcept = default;

    string(std::string_view value) {
        storage_.assign_owned(value);
    }

    string(const string&) = delete;
    string& operator=(const string&) = delete;

    string(string&& other) noexcept = default;

    string& operator=(string&& other) {
        storage_.assign_from(std::move(other.storage_));
        return *this;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return storage_.view();
    }
    [[nodiscard]] std::string_view view() const&& = delete;

    [[nodiscard]] const char* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return view().data();
    }
    [[nodiscard]] const char* data() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return view().size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return view().empty();
    }

    operator std::string_view() const& noexcept RUVIA_LIFETIMEBOUND {
        return view();
    }
    operator std::string_view() const&& = delete;

    friend bool operator==(const string& left, const string& right) noexcept {
        return left.view() == right.view();
    }

    void assign_owned(std::string_view value) {
        storage_.assign_owned(value);
    }

    void assign_owned(std::string&& value) noexcept {
        storage_.assign_owned(std::move(value));
    }

private:
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;

    string(detail::borrowed_model_token_tag, std::string_view value) noexcept
        : storage_(value) {}

    explicit string(detail::model_text_storage&& storage) noexcept
        : storage_(std::move(storage)) {}

    [[nodiscard]] string rebind_for_model() const& {
        return string(storage_.rebind());
    }

    [[nodiscard]] string rebind_for_model() && {
        return string(std::move(storage_).rebind());
    }

    detail::model_text_storage storage_;
};

struct bool_value final {
    bool value_{false};
    constexpr bool_value() noexcept = default;
    constexpr bool_value(bool input) noexcept
        : value_(input) {}
    [[nodiscard]] constexpr operator bool() const noexcept {
        return value_;
    }
};

struct double_value;

// Finite floating inputs are checked before narrowing; explicit nonfinite
// inputs and the conversion semantics of user-defined inputs are preserved.
struct float_value final {
    float value_{0};
    constexpr float_value() noexcept = default;
    constexpr float_value(float input) noexcept
        : value_(input) {}
    template <std::floating_point input_type>
        requires(!std::same_as<input_type, float>)
    constexpr float_value(input_type input) noexcept(noexcept(detail::checked_model_float<float>(input)))
        : value_(detail::checked_model_float<float>(input)) {}
    constexpr float_value(const double_value& input);
    [[nodiscard]] constexpr operator float() const noexcept {
        return value_;
    }
};

struct double_value final {
    double value_{0};
    constexpr double_value() noexcept = default;
    constexpr double_value(double input) noexcept
        : value_(input) {}
    template <std::floating_point input_type>
        requires(!std::same_as<input_type, double>)
    constexpr double_value(input_type input) noexcept(noexcept(detail::checked_model_float<double>(input)))
        : value_(detail::checked_model_float<double>(input)) {}
    [[nodiscard]] constexpr operator double() const noexcept {
        return value_;
    }
};

constexpr float_value::float_value(const double_value& input)
    : value_(detail::checked_model_float<float>(input.value_)) {}

// Accepts its exact integer type unchecked, other integers after a range
// check, and rejects every non-integer type at compile time.
template <typename t_type>
struct integer_value final {
    t_type value_{0};
    constexpr integer_value() noexcept = default;
    constexpr integer_value(t_type input) noexcept
        : value_(input) {}
    template <typename input_type>
        requires(detail::is_model_integer<input_type> &&
                 !std::is_same_v<std::remove_cv_t<input_type>, t_type>)
    constexpr integer_value(input_type input)
        : value_(detail::checked_model_integer<t_type>(input)) {}
    template <typename input_type>
        requires(!detail::is_model_integer<input_type>)
    integer_value(input_type) = delete;
    [[nodiscard]] constexpr operator t_type() const noexcept {
        return value_;
    }
};

using int8 = integer_value<std::int8_t>;
using uint8 = integer_value<std::uint8_t>;
using int16 = integer_value<std::int16_t>;
using uint16 = integer_value<std::uint16_t>;
using int32 = integer_value<std::int32_t>;
using uint32 = integer_value<std::uint32_t>;
using int64 = integer_value<std::int64_t>;
using uint64 = integer_value<std::uint64_t>;

class bytes final {
public:
    bytes() noexcept = default;

    bytes(std::span<const std::uint8_t> value)
        : items_(value.begin(), value.end()) {}

    bytes(const bytes&) = delete;
    bytes& operator=(const bytes&) = delete;

    bytes(bytes&& other) noexcept
        : items_(std::move(other.items_)) {
        other.items_.clear();
    }

    bytes& operator=(bytes&& other) noexcept {
        if (this != &other) {
            items_ = std::move(other.items_);
            other.items_.clear();
        }
        return *this;
    }

    [[nodiscard]] std::span<const std::uint8_t> view() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_;
    }
    [[nodiscard]] std::span<const std::uint8_t> view() const&& = delete;
    [[nodiscard]] const std::uint8_t* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.data();
    }
    [[nodiscard]] const std::uint8_t* data() const&& = delete;
    operator std::span<const std::uint8_t>() const& noexcept RUVIA_LIFETIMEBOUND {
        return view();
    }
    operator std::span<const std::uint8_t>() const&& = delete;
    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    void assign_owned(std::span<const std::uint8_t> value) {
        std::vector<std::uint8_t> owned(value.begin(), value.end());
        items_ = std::move(owned);
    }

    void assign_owned(std::vector<std::uint8_t>&& value) noexcept {
        items_ = std::move(value);
    }

    friend bool operator==(const bytes& left, const bytes& right) noexcept {
        return left.size() == right.size() &&
               std::equal(left.view().begin(), left.view().end(), right.view().begin());
    }

private:
    friend struct detail::model_value_rebind_access;

    [[nodiscard]] bytes rebind_for_model() const& {
        return bytes(view());
    }

    [[nodiscard]] bytes rebind_for_model() && noexcept {
        bytes rebound;
        rebound.items_ = std::move(items_);
        items_.clear();
        return rebound;
    }

    std::vector<std::uint8_t> items_;
};

// array and boxed_array leave a distinct source empty on move assignment.
// Strings and models are rebound to own their data. A source that owns the
// destination is rejected; failed rebinding preserves the destination.
template <typename t_type>
class array final {
public:
    using value_type = t_type;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = t_type&;
    using const_reference = const t_type&;
    using iterator = typename std::vector<t_type>::iterator;
    using const_iterator = typename std::vector<t_type>::const_iterator;

    array() noexcept = default;

    array(const array&) = delete;
    array& operator=(const array&) = delete;

    array(array&& other) noexcept
        : items_(std::move(other.items_)) {
        other.items_.clear();
    }

    array& operator=(array&& other) {
        if (this == &other) {
            return *this;
        }
        // Consuming a source that owns this array would destroy the destination.
        if (other.owns_for_model(this)) {
            detail::throw_model_move_source_owns_destination();
        }

        auto rebound = std::move(other).rebind_for_model();
        items_ = std::move(rebound.items_);
        return *this;
    }

    // User-provided, not defaulted: a defaulted destructor would be constexpr
    // and clang instantiates the vector destructor at first use, while a
    // forward-declared element type may still be incomplete.
    ~array() {}

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] size_type size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] size_type capacity() const noexcept {
        return items_.capacity();
    }

    [[nodiscard]] reference operator[](size_type index) & noexcept RUVIA_LIFETIMEBOUND {
        return items_[index];
    }
    [[nodiscard]] const_reference operator[](size_type index) const& noexcept RUVIA_LIFETIMEBOUND {
        return items_[index];
    }
    [[nodiscard]] const_reference operator[](size_type) const&& = delete;

    [[nodiscard]] reference front() & noexcept RUVIA_LIFETIMEBOUND {
        return items_.front();
    }
    [[nodiscard]] const_reference front() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.front();
    }
    [[nodiscard]] const_reference front() const&& = delete;

    [[nodiscard]] reference back() & noexcept RUVIA_LIFETIMEBOUND {
        return items_.back();
    }
    [[nodiscard]] const_reference back() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.back();
    }
    [[nodiscard]] const_reference back() const&& = delete;

    [[nodiscard]] iterator begin() & noexcept RUVIA_LIFETIMEBOUND {
        return items_.begin();
    }
    [[nodiscard]] const_iterator begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.begin();
    }
    iterator begin() const&& = delete;

    [[nodiscard]] iterator end() & noexcept RUVIA_LIFETIMEBOUND {
        return items_.end();
    }
    [[nodiscard]] const_iterator end() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.end();
    }
    iterator end() const&& = delete;

    void reserve(size_type count) {
        items_.reserve(count);
    }

    void clear() noexcept {
        items_.clear();
    }

    void resize(size_type count) {
        const auto old_size = size();
        if (count < old_size) {
            items_.resize(count);
            return;
        }
        reserve(count);
        try {
            while (size() < count) {
                emplace_back();
            }
        } catch (...) {
            items_.resize(old_size);
            throw;
        }
    }

    void resize(size_type count, const t_type& value) {
        const auto old_size = size();
        if (count <= old_size) {
            if (count < old_size) {
                items_.resize(count);
            }
            return;
        }
        t_type stable = detail::rebind_model_value(value);
        reserve(count);
        try {
            while (size() < count) {
                push_back(stable);
            }
        } catch (...) {
            items_.resize(old_size);
            throw;
        }
    }

    template <typename... args_type>
    t_type& emplace_back(args_type&&... args) & RUVIA_LIFETIMEBOUND {
        return items_.emplace_back(detail::prepare_model_value<t_type>(this, std::forward<args_type>(args)...));
    }
    template <typename... args_type>
    t_type& emplace_back(args_type&&...) && = delete;

    void push_back(const t_type& value) & {
        (void)emplace_back(value);
    }
    void push_back(const t_type&) && = delete;

    void push_back(t_type&& value) & {
        (void)emplace_back(std::move(value));
    }
    void push_back(t_type&&) && = delete;

    friend bool operator==(const array& left, const array& right) {
        return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
    }

private:
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;

    t_type& emplace_parsed(t_type&& value) {
        return items_.emplace_back(std::move(value));
    }

    [[nodiscard]] array rebind_for_model() const& {
        array rebound;
        rebound.reserve(size());
        for (const auto& value : items_) {
            (void)rebound.emplace_parsed(detail::rebind_model_value(value));
        }
        return rebound;
    }

    [[nodiscard]] array rebind_for_model() && {
        if constexpr (detail::model_value_rebind_access::has_rebind_for_model<t_type>()) {
            // A later element may still allocate; retain every source until the
            // complete candidate exists, including when assigning a field to itself.
            auto rebound = std::as_const(*this).rebind_for_model();
            clear();
            return rebound;
        } else {
            array rebound;
            rebound.items_ = std::move(items_);
            clear();
            return rebound;
        }
    }

    [[nodiscard]] bool owns_for_model(const void* address) const noexcept {
        if (detail::model_address_within(address, items_.data(), items_.size() * sizeof(t_type))) {
            return true;
        }
        if constexpr (detail::model_value_rebind_access::can_own<t_type>()) {
            for (const auto& value : items_) {
                if (detail::model_value_rebind_access::owns(value, address)) {
                    return true;
                }
            }
        }
        return false;
    }

    std::vector<t_type> items_;
};

template <typename t_type>
class boxed_array final {
    template <bool const_value>
    class iterator_type;

public:
    using value_type = t_type;

    boxed_array() noexcept = default;

    boxed_array(const boxed_array&) = delete;
    boxed_array& operator=(const boxed_array&) = delete;

    boxed_array(boxed_array&& other) noexcept
        : items_(std::move(other.items_)) {
        other.items_.clear();
    }

    boxed_array& operator=(boxed_array&& other) {
        if (this == &other) {
            return *this;
        }
        // Consuming a source that owns this array would destroy the destination.
        if (other.owns_for_model(this)) {
            detail::throw_model_move_source_owns_destination();
        }

        auto rebound = std::move(other).rebind_for_model();
        clear();
        items_ = std::move(rebound.items_);
        return *this;
    }

    ~boxed_array() {
        clear();
    }

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] const t_type& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return *items_[index];
    }
    [[nodiscard]] t_type& operator[](std::size_t index) & noexcept RUVIA_LIFETIMEBOUND {
        return *items_[index];
    }
    [[nodiscard]] const t_type& operator[](std::size_t) const&& = delete;

    [[nodiscard]] const t_type& front() const& noexcept RUVIA_LIFETIMEBOUND {
        return *items_.front();
    }
    [[nodiscard]] t_type& front() & noexcept RUVIA_LIFETIMEBOUND {
        return *items_.front();
    }
    [[nodiscard]] const t_type& front() const&& = delete;

    [[nodiscard]] const t_type& back() const& noexcept RUVIA_LIFETIMEBOUND {
        return *items_.back();
    }
    [[nodiscard]] t_type& back() & noexcept RUVIA_LIFETIMEBOUND {
        return *items_.back();
    }
    [[nodiscard]] const t_type& back() const&& = delete;

    [[nodiscard]] auto begin() & noexcept RUVIA_LIFETIMEBOUND {
        return iterator_type<false>(items_.begin());
    }
    [[nodiscard]] auto begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return iterator_type<true>(items_.begin());
    }
    void begin() const&& = delete;

    [[nodiscard]] auto end() & noexcept RUVIA_LIFETIMEBOUND {
        return iterator_type<false>(items_.end());
    }
    [[nodiscard]] auto end() const& noexcept RUVIA_LIFETIMEBOUND {
        return iterator_type<true>(items_.end());
    }
    void end() const&& = delete;

    void clear() noexcept {
        for (auto* value : items_) {
            delete value;
        }
        items_.clear();
    }

    void reserve(std::size_t count) {
        items_.reserve(count);
    }

    void resize(std::size_t count) {
        const auto old_size = size();
        if (count < old_size) {
            while (size() > count) {
                auto* value = items_.back();
                items_.pop_back();
                delete value;
            }
            return;
        }
        reserve(count);
        try {
            while (size() < count) {
                emplace_back();
            }
        } catch (...) {
            while (size() > old_size) {
                auto* value = items_.back();
                items_.pop_back();
                delete value;
            }
            throw;
        }
    }

    template <typename... args_type>
    t_type& emplace_back(args_type&&... args) & RUVIA_LIFETIMEBOUND {
        return emplace_parsed(detail::prepare_model_value<t_type>(this, std::forward<args_type>(args)...));
    }
    template <typename... args_type>
    t_type& emplace_back(args_type&&...) && = delete;

    void push_back(const t_type& value) & {
        (void)emplace_back(value);
    }
    void push_back(const t_type&) && = delete;

    void push_back(t_type&& value) & {
        (void)emplace_back(std::move(value));
    }
    void push_back(t_type&&) && = delete;

    friend bool operator==(const boxed_array& left, const boxed_array& right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (!(left[i] == right[i])) {
                return false;
            }
        }
        return true;
    }

private:
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;

    template <bool const_value>
    class iterator_type final {
    public:
        using inner_iterator_type = std::conditional_t<const_value,
            typename std::vector<t_type*>::const_iterator,
            typename std::vector<t_type*>::iterator>;
        using difference_type = typename inner_iterator_type::difference_type;
        using value_type = t_type;
        using reference = std::conditional_t<const_value, const t_type&, t_type&>;
        using pointer = std::conditional_t<const_value, const t_type*, t_type*>;
        using iterator_category = std::forward_iterator_tag;

        iterator_type() noexcept = default;

        explicit iterator_type(inner_iterator_type current) noexcept
            : current_(current) {}

        reference operator*() const noexcept {
            return **current_;
        }

        pointer operator->() const noexcept {
            return *current_;
        }

        iterator_type& operator++() noexcept {
            ++current_;
            return *this;
        }

        iterator_type operator++(int) noexcept {
            auto copy = *this;
            ++current_;
            return copy;
        }

        friend bool operator==(const iterator_type& left, const iterator_type& right) noexcept {
            return left.current_ == right.current_;
        }

    private:
        inner_iterator_type current_{};
    };

    template <typename input_type>
    t_type& emplace_parsed(input_type&& value) {
        // Admit the pointer slot before constructing a box can consume its input.
        if (size() == items_.capacity()) {
            if (size() == items_.max_size()) {
                throw std::length_error("model boxed array is too large");
            }
            const auto growth = std::min(std::max(size(), std::size_t{1}), items_.max_size() - size());
            items_.reserve(size() + growth);
        }
        auto* const stored = new t_type(std::forward<input_type>(value));
        items_.push_back(stored);
        return *stored;
    }

    [[nodiscard]] boxed_array rebind_for_model() const& {
        boxed_array rebound;
        rebound.reserve(size());
        for (const auto& value : *this) {
            rebound.emplace_parsed(detail::rebind_model_value(value));
        }
        return rebound;
    }

    [[nodiscard]] boxed_array rebind_for_model() && {
        if constexpr (detail::model_value_rebind_access::has_rebind_for_model<t_type>()) {
            // Prepare both nested values and their boxes before consuming the source.
            auto rebound = std::as_const(*this).rebind_for_model();
            clear();
            return rebound;
        } else {
            boxed_array rebound;
            rebound.items_ = std::move(items_);
            items_.clear();
            return rebound;
        }
    }

    [[nodiscard]] bool owns_for_model(const void* address) const noexcept {
        for (const auto* value : items_) {
            if (detail::model_address_within(address, value, sizeof(t_type))) {
                return true;
            }
            if constexpr (detail::model_value_rebind_access::can_own<t_type>()) {
                if (detail::model_value_rebind_access::owns(*value, address)) {
                    return true;
                }
            }
        }
        return false;
    }

    std::vector<t_type*> items_;
};

namespace detail {

struct model_value_factory final {
    [[nodiscard]] static string make_borrowed_string(std::string_view value) noexcept {
        return string(borrowed_model_token_tag{}, value);
    }

    template <typename target_type>
    static void emplace_parsed(target_type& target, typename target_type::value_type&& value) {
        target.emplace_parsed(std::move(value));
    }
};

}  // namespace detail

class json_object;
class json_value;

namespace detail {

enum class model_field_state : std::uint8_t {
    missing,
    parsed,
    null,
    invalid_type,
    duplicate
};

template <typename>
inline constexpr bool always_false = false;

template <typename t_type>
inline constexpr bool is_ruvia_string = std::is_same_v<std::remove_cvref_t<t_type>, string>;

template <typename t_type>
inline constexpr bool is_ruvia_bytes = std::is_same_v<std::remove_cvref_t<t_type>, bytes>;

template <typename t_type>
inline constexpr bool is_ruvia_json_value = std::is_same_v<std::remove_cvref_t<t_type>, json_value>;

template <typename t_type>
inline constexpr bool is_ruvia_json_object = std::is_same_v<std::remove_cvref_t<t_type>, json_object>;

template <typename t_type>
struct ruvia_array_traits : std::false_type {};

template <typename element_type>
struct ruvia_array_traits<array<element_type>> : std::true_type {
    using value_type = element_type;
};
template <typename t_type>
inline constexpr bool is_ruvia_array = ruvia_array_traits<std::remove_cvref_t<t_type>>::value;

template <typename t_type>
struct ruvia_boxed_array_traits : std::false_type {};

template <typename element_type>
struct ruvia_boxed_array_traits<boxed_array<element_type>> : std::true_type {
    using value_type = element_type;
};
template <typename t_type>
inline constexpr bool is_ruvia_boxed_array = ruvia_boxed_array_traits<std::remove_cvref_t<t_type>>::value;

template <typename t_type>
struct ruvia_scalar_traits : std::false_type {};

template <>
struct ruvia_scalar_traits<bool_value> : std::true_type {
    using value_type = bool;
};
template <typename integer_type>
struct ruvia_scalar_traits<integer_value<integer_type>> : std::true_type {
    using value_type = integer_type;
};
template <>
struct ruvia_scalar_traits<float_value> : std::true_type {
    using value_type = float;
};
template <>
struct ruvia_scalar_traits<double_value> : std::true_type {
    using value_type = double;
};

template <typename t_type>
inline constexpr bool is_ruvia_scalar = ruvia_scalar_traits<std::remove_cvref_t<t_type>>::value;

template <typename t_type>
using model_scalar_value_type = typename ruvia_scalar_traits<std::remove_cvref_t<t_type>>::value_type;

template <typename t_type>
inline constexpr bool is_form_field = is_ruvia_string<t_type> || is_ruvia_scalar<t_type>;

template <typename t_type>
inline constexpr bool is_model =
    requires { typename std::remove_cvref_t<t_type>::ruvia_model_schema_type; };

template <typename t_type>
inline constexpr bool model_field_value = is_ruvia_string<t_type> || is_ruvia_bytes<t_type> || is_ruvia_scalar<t_type> ||
                                          is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type> || is_model<t_type>;

template <typename value_type>
inline constexpr bool model_field_value<array<value_type>> = model_field_value<std::remove_cvref_t<value_type>>;

template <typename value_type>
inline constexpr bool model_field_value<boxed_array<value_type>> = model_field_value<std::remove_cvref_t<value_type>>;

template <typename t_type>
inline constexpr bool is_model_field = model_field_value<std::remove_cvref_t<t_type>>;

template <typename t_type>
inline constexpr bool model_json_value =
    is_ruvia_string<t_type> || is_ruvia_bytes<t_type> || is_ruvia_scalar<t_type> || is_model<t_type>;

template <typename value_type>
inline constexpr bool model_json_value<array<value_type>> = model_json_value<std::remove_cvref_t<value_type>>;

template <typename value_type>
inline constexpr bool model_json_value<boxed_array<value_type>> = model_json_value<std::remove_cvref_t<value_type>>;

template <typename t_type>
inline constexpr bool is_model_json_value = model_json_value<std::remove_cvref_t<t_type>>;

template <typename t_type>
inline constexpr bool is_validation_number =
    std::is_arithmetic_v<std::remove_cvref_t<t_type>> || is_ruvia_scalar<t_type>;

template <typename integer_type, typename floating_type>
[[nodiscard]] std::partial_ordering compare_validation_integer_floating(
    integer_type integer, floating_type floating) noexcept {
    if (std::isnan(floating)) {
        return std::partial_ordering::unordered;
    }

    // The upper bound is exclusive. Unlike max(), this power of two is
    // exactly representable even when the float cannot represent every integer.
    const auto upper = std::ldexp(floating_type{1}, std::numeric_limits<integer_type>::digits);
    if (floating >= upper) {
        return std::partial_ordering::less;
    }
    const auto lower = std::is_signed_v<integer_type> ? -upper : floating_type{0};
    if (floating < lower) {
        return std::partial_ordering::greater;
    }

    // NaN, infinity and out-of-range values have all returned before the cast.
    const auto truncated = static_cast<integer_type>(floating);
    if (std::cmp_less(integer, truncated)) {
        return std::partial_ordering::less;
    }
    if (std::cmp_greater(integer, truncated)) {
        return std::partial_ordering::greater;
    }
    const auto whole = std::trunc(floating);
    if (whole < floating) {
        return std::partial_ordering::less;
    }
    if (whole > floating) {
        return std::partial_ordering::greater;
    }
    return std::partial_ordering::equivalent;
}

template <typename left_type, typename right_type>
[[nodiscard]] bool validation_less(const left_type& left, const right_type& right) {
    if constexpr (!is_validation_number<left_type> || !is_validation_number<right_type>) {
        return left < right;
    } else if constexpr (is_ruvia_scalar<left_type>) {
        return validation_less(left.value_, right);
    } else if constexpr (is_ruvia_scalar<right_type>) {
        return validation_less(left, right.value_);
    } else if constexpr (std::is_integral_v<left_type> && std::is_integral_v<right_type>) {
        return std::cmp_less(+left, +right);
    } else if constexpr (std::is_integral_v<left_type> && std::is_floating_point_v<right_type>) {
        return compare_validation_integer_floating(+left, right) == std::partial_ordering::less;
    } else if constexpr (std::is_floating_point_v<left_type> && std::is_integral_v<right_type>) {
        return compare_validation_integer_floating(+right, left) == std::partial_ordering::greater;
    } else {
        return left < right;
    }
}

template <typename left_type, typename right_type>
[[nodiscard]] bool validation_greater(const left_type& left, const right_type& right) {
    if constexpr (is_validation_number<left_type> && is_validation_number<right_type>) {
        return validation_less(right, left);
    } else {
        return left > right;
    }
}

}  // namespace detail

}  // namespace ruvia

namespace ruvia::detail::model {

// Kinds a single pattern atom or class member decodes to; compilation folds
// each into the byte set it matches.
enum class pattern_atom_kind : unsigned char { literal,
    digit,
    word,
    space };

enum class pattern_quantifier : unsigned char { one,
    zero_or_one,
    zero_or_more,
    one_or_more };

// The bytes one compiled atom matches, as a 256-bit set.
class pattern_byte_set final {
public:
    constexpr void insert(unsigned char byte) noexcept {
        words_[byte >> 6] |= std::uint64_t{1} << (byte & 63U);
    }

    constexpr void invert() noexcept {
        for (auto& word : words_) {
            word = ~word;
        }
    }

    [[nodiscard]] constexpr bool contains(char value) const noexcept {
        const auto byte = static_cast<unsigned char>(value);
        return ((words_[byte >> 6] >> (byte & 63U)) & 1U) != 0;
    }

private:
    std::array<std::uint64_t, 4> words_{};
};

struct pattern_atom final {
    pattern_byte_set bytes_{};
    pattern_quantifier quantifier_{pattern_quantifier::one};
};

template <std::size_t capacity>
struct pattern_plan final {
    bool valid_{false};
    std::size_t count_{0};
    std::array<pattern_atom, capacity> atoms_{};
};

[[nodiscard]] constexpr bool is_pattern_meta(char c) noexcept {
    switch (c) {
        case '^':
        case '$':
        case '[':
        case ']':
        case '(':
        case ')':
        case '{':
        case '}':
        case '|':
        case '+':
        case '*':
        case '?':
        case '.':
        case '\\':
            return true;
        default:
            return false;
    }  // namespace ruvia::detail::model
}

[[nodiscard]] constexpr bool is_pattern_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr bool is_pattern_word(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] constexpr bool is_pattern_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

struct pattern_class_member final {
    pattern_atom_kind kind_{pattern_atom_kind::literal};
    unsigned char first_{0};
    unsigned char last_{0};
};

// Only \d, \w and \s are escape classes. Other ASCII letters and digits are
// rejected rather than read as literals, because regex escapes such as \r, \n,
// \D or \S would otherwise silently change meaning. Remaining escapes are literal.
[[nodiscard]] constexpr bool decode_pattern_escape(char escape, pattern_atom_kind& kind) noexcept {
    switch (escape) {
        case 'd':
            kind = pattern_atom_kind::digit;
            return true;
        case 'w':
            kind = pattern_atom_kind::word;
            return true;
        case 's':
            kind = pattern_atom_kind::space;
            return true;
        default:
            kind = pattern_atom_kind::literal;
            return escape == '_' || !is_pattern_word(escape);
    }
}

[[nodiscard]] constexpr bool consume_pattern_class_atom(std::string_view pattern, std::size_t end,
    std::size_t& cursor, pattern_class_member& member) noexcept {
    if (cursor >= end) {
        return false;
    }
    char value = pattern[cursor++];
    member.kind_ = pattern_atom_kind::literal;
    if (value == '\\') {
        if (cursor >= end) {
            return false;
        }
        value = pattern[cursor++];
        if (!decode_pattern_escape(value, member.kind_)) {
            return false;
        }
    }
    member.first_ = static_cast<unsigned char>(value);
    member.last_ = member.first_;
    return true;
}

// Decode escaped atoms before recognizing a range; a final '-' is a literal,
// and only single literal atoms may be range endpoints.
[[nodiscard]] constexpr bool consume_pattern_class_member(std::string_view pattern, std::size_t end,
    std::size_t& cursor, pattern_class_member& member) noexcept {
    if (!consume_pattern_class_atom(pattern, end, cursor, member)) {
        return false;
    }
    if (cursor + 1 < end && pattern[cursor] == '-') {
        ++cursor;
        pattern_class_member last;
        if (!consume_pattern_class_atom(pattern, end, cursor, last) ||
            member.kind_ != pattern_atom_kind::literal || last.kind_ != pattern_atom_kind::literal ||
            member.first_ > last.first_) {
            return false;
        }
        member.last_ = last.first_;
    }
    return true;
}

constexpr void insert_pattern_member(pattern_byte_set& bytes, const pattern_class_member& member) noexcept {
    for (unsigned byte = 0; byte < 256; ++byte) {
        const auto value = static_cast<char>(byte);
        bool matches = false;
        switch (member.kind_) {
            case pattern_atom_kind::literal:
                matches = byte >= member.first_ && byte <= member.last_;
                break;
            case pattern_atom_kind::digit:
                matches = is_pattern_digit(value);
                break;
            case pattern_atom_kind::word:
                matches = is_pattern_word(value);
                break;
            case pattern_atom_kind::space:
                matches = is_pattern_space(value);
                break;
        }
        if (matches) {
            bytes.insert(static_cast<unsigned char>(byte));
        }
    }
}

// Folds the class body in [begin, end) into `bytes`. Negation is applied by
// the caller: a leading '[^' is stripped before the body, so any '^' here is an
// ordinary member (e.g. "[^^]" excludes only the caret).
[[nodiscard]] constexpr bool compile_pattern_class(std::string_view pattern, std::size_t begin,
    std::size_t end, pattern_byte_set& bytes) noexcept {
    if (begin >= end) {
        return false;
    }
    for (std::size_t cursor = begin; cursor < end;) {
        pattern_class_member member;
        if (!consume_pattern_class_member(pattern, end, cursor, member)) {
            return false;
        }
        insert_pattern_member(bytes, member);
    }
    return true;
}

inline constexpr std::size_t max_pattern_bytes = 256;

template <std::size_t capacity>
[[nodiscard]] constexpr bool append_pattern_atom(std::string_view pattern, std::size_t end,
    std::size_t& cursor_value, pattern_plan<capacity>& plan) noexcept {
    if (cursor_value >= end || plan.count_ >= capacity) {
        return false;
    }

    pattern_atom atom{};
    const char c = pattern[cursor_value];
    if (c == '[') {
        ++cursor_value;
        bool negate = false;
        if (cursor_value < end && pattern[cursor_value] == '^') {
            negate = true;
            ++cursor_value;
        }
        const auto class_begin = cursor_value;
        for (; cursor_value < end; ++cursor_value) {
            if (pattern[cursor_value] == '\\') {
                ++cursor_value;
                continue;
            }
            if (pattern[cursor_value] == ']') {
                const auto class_end = cursor_value++;
                if (!compile_pattern_class(pattern, class_begin, class_end, atom.bytes_)) {
                    return false;
                }
                if (negate) {
                    atom.bytes_.invert();
                }
                plan.atoms_[plan.count_++] = atom;
                return true;
            }
        }
        return false;
    }

    pattern_class_member member;
    if (c == '\\') {
        if (cursor_value + 1 >= end) {
            return false;
        }
        const char escaped = pattern[++cursor_value];
        if (!decode_pattern_escape(escaped, member.kind_)) {
            return false;
        }
        member.first_ = static_cast<unsigned char>(escaped);
        member.last_ = member.first_;
        insert_pattern_member(atom.bytes_, member);
    } else if (c == '.') {
        atom.bytes_.insert(static_cast<unsigned char>('\n'));
        atom.bytes_.invert();
    } else if (is_pattern_meta(c)) {
        return false;
    } else {
        atom.bytes_.insert(static_cast<unsigned char>(c));
    }
    ++cursor_value;
    plan.atoms_[plan.count_++] = atom;
    return true;
}

template <std::size_t capacity>
[[nodiscard]] constexpr pattern_plan<capacity> compile_pattern_plan(
    std::string_view pattern) noexcept {
    pattern_plan<capacity> plan{};
    if (pattern.size() > max_pattern_bytes || pattern.size() < 2 || pattern.front() != '^' || pattern.back() != '$') {
        return plan;
    }

    const std::size_t pattern_end = pattern.size() - 1;
    std::size_t cursor_value = 1;
    while (cursor_value < pattern_end) {
        const std::size_t index = plan.count_;
        if (!append_pattern_atom(pattern, pattern_end, cursor_value, plan)) {
            return {};
        }

        if (cursor_value < pattern_end &&
            (pattern[cursor_value] == '*' || pattern[cursor_value] == '+' || pattern[cursor_value] == '?')) {
            switch (pattern[cursor_value++]) {
                case '*':
                    plan.atoms_[index].quantifier_ = pattern_quantifier::zero_or_more;
                    break;
                case '+':
                    plan.atoms_[index].quantifier_ = pattern_quantifier::one_or_more;
                    break;
                case '?':
                    plan.atoms_[index].quantifier_ = pattern_quantifier::zero_or_one;
                    break;
                default:
                    return {};
            }
        }
    }

    plan.valid_ = true;
    return plan;
}

template <fixed_string pattern>
struct compiled_pattern_plan final {
    static constexpr std::size_t capacity =
        pattern.view().size() < max_pattern_bytes ? pattern.view().size() : max_pattern_bytes;
    static constexpr auto value = compile_pattern_plan<capacity>(pattern.view());
    static_assert(value.valid_,
        "RUVIA_PATTERN supports only anchored patterns using literals, '.', character classes, \\d, \\w, \\s, and '*', '+', '?'. "
        "Escaped letters and digits other than \\d, \\w, \\s (such as \\n or \\D) are unsupported. "
        "Ranges require ordered single-character endpoints. Use RUVIA_CUSTOM for other matching logic.");
};

// Upper bound on matcher work for one pattern match. It charges recursive
// states and each input byte inspected by a quantifier scan; atoms match in
// O(1) against their compiled byte sets. Exhaustion fails the match closed.
inline constexpr std::size_t max_pattern_match_steps = 1'000'000;

template <std::size_t capacity>
[[nodiscard]] constexpr bool match_pattern_plan_from(const pattern_plan<capacity>& plan,
    std::string_view value, std::size_t atom_index, std::size_t value_index, std::size_t& budget) noexcept {
    if (budget == 0) {
        return false;  // step budget exhausted -> bound catastrophic backtracking
    }
    --budget;
    if (atom_index == plan.count_) {
        return value_index == value.size();
    }

    const auto& atom = plan.atoms_[atom_index];
    const std::size_t min_count = atom.quantifier_ == pattern_quantifier::one ||
                                          atom.quantifier_ == pattern_quantifier::one_or_more
                                      ? 1
                                      : 0;
    // Bound the greedy scan at the most this quantifier can consume. one/zero_or_one
    // take at most one character, so scanning the whole matching run and discarding
    // all but one is wasted O(L) work -- and it is repeated at every backtrack
    // position, an O(n^2) ReDoS on shapes like "^a*a$" that the per-recursion step
    // budget never charges for (each full rescan is a single call). Capping the scan
    // makes each fixed atom O(1), so per-call scan cost stays within the
    // budget-charged recursion; the variable quantifiers already spawn ~L recursions
    // for an L-char scan and so remain bounded.
    const std::size_t scan_limit = atom.quantifier_ == pattern_quantifier::one ||
                                           atom.quantifier_ == pattern_quantifier::zero_or_one
                                       ? std::size_t{1}
                                       : value.size();
    std::size_t max_count = 0;
    while (max_count < scan_limit && value_index + max_count < value.size()) {
        if (budget == 0) {
            return false;
        }
        --budget;
        if (!atom.bytes_.contains(value[value_index + max_count])) {
            break;
        }
        ++max_count;
    }

    if (max_count < min_count) {
        return false;
    }

    for (std::size_t count = max_count + 1; count-- > min_count;) {
        if (match_pattern_plan_from(plan, value, atom_index + 1, value_index + count, budget)) {
            return true;
        }
        if (count == 0) {
            break;
        }
    }
    return false;
}

template <fixed_string pattern>
[[nodiscard]] constexpr bool match_pattern_plan(std::string_view value) noexcept {
    std::size_t budget = max_pattern_match_steps;
    return match_pattern_plan_from(compiled_pattern_plan<pattern>::value, value, 0, 0, budget);
}

template <auto bound, fixed_string error_message>
struct min final {
    static_assert(std::is_arithmetic_v<decltype(bound)> && !std::is_same_v<decltype(bound), bool>,
        "RUVIA_MIN requires a numeric bound");
    using ruvia_validation_rule_marker_type = void;
};

template <auto bound, fixed_string error_message>
struct max final {
    static_assert(std::is_arithmetic_v<decltype(bound)> && !std::is_same_v<decltype(bound), bool>,
        "RUVIA_MAX requires a numeric bound");
    using ruvia_validation_rule_marker_type = void;
};

template <fixed_string error_message, fixed_string... values>
struct one_of final {
    using ruvia_validation_rule_marker_type = void;
};

template <fixed_string error_message>
struct email final {
    using ruvia_validation_rule_marker_type = void;
};

template <auto validation_predicate, fixed_string error_message>
struct custom final {
    using ruvia_validation_rule_marker_type = void;
};

template <auto provider>
struct default_value final {
    using ruvia_default_rule_marker_type = void;
    using ruvia_model_option_marker_type = void;

    // Metadata is stateless. Evaluate only when consuming a missing optional
    // field, never while constructing or moving a model.
    [[nodiscard]] static constexpr decltype(auto) value() {
        return provider();
    }
};

struct nullable final {
    using ruvia_model_option_marker_type = void;
};

struct omit_empty final {
    using ruvia_model_option_marker_type = void;
};

struct emit_null final {
    using ruvia_model_option_marker_type = void;
};

template <fixed_string pattern, fixed_string error_message>
struct pattern_rule final {
    using ruvia_validation_rule_marker_type = void;

    static_assert(compiled_pattern_plan<pattern>::value.valid_, "RUVIA_PATTERN requires a valid pattern");
};

template <typename t_type>
[[nodiscard]] std::size_t model_size(const t_type& value) noexcept {
    if constexpr (requires { value.view(); }) {
        return value.view().size();
    } else {
        return value.size();
    }
}

template <typename t_type>
[[nodiscard]] std::string_view model_string(const t_type& value) noexcept {
    if constexpr (requires { value.view(); }) {
        return value.view();
    } else {
        return std::string_view(value);
    }
}

[[nodiscard]] inline bool is_email_like(std::string_view value) noexcept {
    const auto at = value.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= value.size() ||
        value.find('@', at + 1) != std::string_view::npos) {
        return false;
    }
    const auto domain = value.substr(at + 1);
    if (domain.front() == '.' || domain.back() == '.' ||
        domain.find('.') == std::string_view::npos ||
        domain.find("..") != std::string_view::npos) {
        return false;
    }
    for (const char c : value) {
        // Compare as unsigned: `char` is signed on most targets, so a UTF-8 byte
        // (>= 0x80) is negative and would satisfy `c <= 0x20`, wrongly rejecting an
        // internationalized address (RFC 6531) as if it held a control byte. The
        // guard only means to reject controls, SP, and DEL -- match the codebase's
        // other byte checks (e.g. is_valid_cookie_value) by using an unsigned byte.
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7F) {
            return false;
        }
    }
    return true;
}

template <typename t_type>
[[nodiscard]] bool is_empty_value(const t_type& value) noexcept {
    if constexpr (detail::is_ruvia_string<t_type> || detail::is_ruvia_bytes<t_type> ||
                  detail::is_ruvia_array<t_type> || detail::is_ruvia_boxed_array<t_type>) {
        return value.empty();
    } else {
        return false;
    }
}

template <typename t_type>
[[nodiscard]] constexpr std::string_view expected_type_name() noexcept {
    using value_type = std::remove_cvref_t<t_type>;
    if constexpr (detail::is_ruvia_bytes<value_type>) {
        return "must be a padded base64 string";
    } else if constexpr (detail::is_ruvia_string<value_type>) {
        return "must be a string";
    } else if constexpr (detail::is_ruvia_array<value_type> || detail::is_ruvia_boxed_array<value_type>) {
        return "must be an array";
    } else if constexpr (detail::is_ruvia_json_object<value_type> || is_model<value_type>) {
        return "must be an object";
    } else if constexpr (detail::is_ruvia_json_value<value_type>) {
        return "must be a JSON value";
    } else if constexpr (detail::is_ruvia_scalar<value_type>) {
        if constexpr (std::is_same_v<detail::model_scalar_value_type<value_type>, bool>) {
            return "must be a boolean";
        } else {
            return "must be a number";
        }
    } else {
        return "has invalid type";
    }
}

template <typename t_type>
[[nodiscard]] constexpr bool model_has_size_rule() noexcept {
    using value_type = std::remove_cvref_t<t_type>;
    return detail::is_ruvia_string<value_type> || detail::is_ruvia_bytes<value_type> ||
           detail::is_ruvia_array<value_type> || detail::is_ruvia_boxed_array<value_type>;
}

template <typename t_type>
[[nodiscard]] constexpr bool model_has_number_rule() noexcept {
    using value_type = std::remove_cvref_t<t_type>;
    if constexpr (detail::is_ruvia_scalar<value_type>) {
        using scalar_type = detail::model_scalar_value_type<value_type>;
        return std::is_arithmetic_v<scalar_type> && !std::is_same_v<scalar_type, bool>;
    } else {
        return false;
    }
}

// A validation path as segments on the traversal stack. Text is rendered only
// when an issue is emitted, so successful validation builds no path strings.
class model_path final {
public:
    constexpr model_path() noexcept = default;

    constexpr model_path(const model_path& parent, std::string_view field) noexcept
        : parent_(&parent),
          field_(field) {}

    constexpr model_path(const model_path& parent, std::size_t index) noexcept
        : parent_(&parent),
          index_(index),
          indexed_(true) {}

    model_path(const model_path&) = delete;
    model_path& operator=(const model_path&) = delete;

    [[nodiscard]] constexpr std::optional<std::string_view> single_field() const noexcept {
        if (parent_ != nullptr && parent_->parent_ == nullptr && !indexed_) {
            return field_;
        }
        return std::nullopt;
    }

    [[nodiscard]] constexpr std::size_t size_hint() const noexcept {
        if (parent_ == nullptr) {
            return 0;
        }
        const auto parent_size = parent_->size_hint();
        if (indexed_) {
            return parent_size + 2U + http_unsigned_decimal_size(index_);
        }
        return parent_size + (parent_size == 0 ? 0U : 1U) + field_.size();
    }

    [[nodiscard]] std::string_view render_to(std::span<char> buffer) const noexcept {
        std::size_t length = 0;
        append_to_buffer(buffer, length);
        return std::string_view(buffer.data(), length);
    }

    // Cold path for paths beyond the issue sink's stack buffer; size_hint() is exact.
    void render(std::string& output) const {
        output.resize(size_hint());
        output.resize(render_to(output).size());
    }

private:
    void append_to_buffer(std::span<char> buffer, std::size_t& length) const noexcept {
        if (parent_ == nullptr) {
            return;
        }
        parent_->append_to_buffer(buffer, length);
        if (indexed_) {
            buffer[length++] = '[';
            std::array<char, http_max_unsigned_decimal_size> digits;
            const auto formatted = http_format_unsigned(digits, index_);
            std::memcpy(buffer.data() + length, formatted.data(), formatted.size());
            length += formatted.size();
            buffer[length++] = ']';
            return;
        }
        if (length != 0) {
            buffer[length++] = '.';
        }
        if (!field_.empty()) {
            std::memcpy(buffer.data() + length, field_.data(), field_.size());
            length += field_.size();
        }
    }

    const model_path* parent_{nullptr};
    std::string_view field_{};
    std::size_t index_{0};
    bool indexed_{false};
};

// Forwards issues to a validator, rendering typical paths into a stack buffer
// and spilling only oversized paths to a reusable string.
template <typename validator_type>
class model_issue_sink final {
public:
    explicit model_issue_sink(validator_type& validator_value) noexcept
        : validator_(validator_value) {}

    void add(const model_path& path, std::string_view code, std::string_view message) {
        if (const auto single = path.single_field()) {
            validator_.add(*single, code, message);
            return;
        }
        static constexpr std::size_t inline_capacity = 128;
        if (path.size_hint() <= inline_capacity) {
            std::array<char, inline_capacity> buffer;
            validator_.add(path.render_to(buffer), code, message);
            return;
        }
        if (!path_) {
            path_.emplace();
        }
        path.render(*path_);
        validator_.add(*path_, code, message);
    }

private:
    validator_type& validator_;
    std::optional<std::string> path_;
};

template <typename rule_type>
[[nodiscard]] constexpr bool is_default_rule() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_default_rule_marker_type; };
}

template <auto provider>
struct initial final {
    using ruvia_initial_rule_marker_type = void;
    using ruvia_model_option_marker_type = void;

    [[nodiscard]] static constexpr decltype(auto) value() {
        return provider();
    }
};

template <typename rule_type>
[[nodiscard]] constexpr bool is_initial_rule() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_initial_rule_marker_type; };
}

template <typename rule_type>
[[nodiscard]] constexpr bool is_model_option() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_model_option_marker_type; };
}

template <typename rule_type>
[[nodiscard]] constexpr bool is_validation_rule() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_validation_rule_marker_type; };
}

template <auto bound_type>
[[nodiscard]] consteval bool valid_model_size_bound() {
    if constexpr (std::is_integral_v<decltype(bound_type)>) {
        return std::in_range<std::size_t>(bound_type);
    } else {
        return false;
    }
}

// Whether `value` (its size, for strings, bytes and arrays) lies beyond the
// bound in the direction `beyond` tests.
template <auto bound_type, typename value_type, typename compare_type>
[[nodiscard]] bool beyond_model_bound(const value_type& value, compare_type beyond) {
    if constexpr (model_has_size_rule<value_type>()) {
        static_assert(valid_model_size_bound<bound_type>(),
            "size rules require a nonnegative integral bound representable as size_t");
        return beyond(model_size(value), bound_type);
    } else if constexpr (model_has_number_rule<value_type>()) {
        using number_type = detail::model_scalar_value_type<value_type>;
        if constexpr (std::is_integral_v<number_type>) {
            static_assert(std::is_integral_v<decltype(bound_type)>,
                "integer fields require an integral bound");
        }
        return beyond(value, bound_type);
    } else {
        static_assert(detail::always_false<value_type>,
            "RUVIA_MIN/MAX require a numeric, string, bytes, or array field");
    }
}

template <typename value_type, typename validator_type, auto bound_type, fixed_string message>
void validate_rule(
    const value_type& value, const model_path& path, validator_type& validator_value, const min<bound_type, message>&) {
    if (beyond_model_bound<bound_type>(value, [](const auto& left, const auto& right) {
            return detail::validation_less(left, right);
        })) {
        validator_value.add(path, "too_small", message.view());
    }
}

template <typename value_type, typename validator_type, auto bound_type, fixed_string message>
void validate_rule(
    const value_type& value, const model_path& path, validator_type& validator_value, const max<bound_type, message>&) {
    if (beyond_model_bound<bound_type>(value, [](const auto& left, const auto& right) {
            return detail::validation_greater(left, right);
        })) {
        validator_value.add(path, "too_big", message.view());
    }
}

template <typename value_type, typename validator_type, fixed_string message, fixed_string... values>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value,
    const one_of<message, values...>&) {
    const auto actual = model_string(value);
    if (!((actual == values.view()) || ...)) {
        validator_value.add(path, "one_of", message.view());
    }
}

template <typename value_type, typename validator_type, fixed_string message>
void validate_rule(
    const value_type& value, const model_path& path, validator_type& validator_value, const email<message>&) {
    if (!is_email_like(model_string(value))) {
        validator_value.add(path, "email", message.view());
    }
}

template <typename value_type, typename validator_type, fixed_string pattern, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value,
    const pattern_rule<pattern, message>&) {
    const auto actual = model_string(value);
    if (!match_pattern_plan<pattern>(actual)) {
        validator_value.add(path, "pattern", message.view());
    }
}

template <typename value_type, typename validator_type, auto predicate, fixed_string message>
void validate_rule(const value_type& value, const model_path& path, validator_type& validator_value,
    const custom<predicate, message>&) {
    if (!predicate(value)) {
        validator_value.add(path, "custom", message.view());
    }
}

template <typename... rule_types>
class rules final {
public:
    constexpr rules() noexcept {
        static_assert((is_validation_rule<rule_types>() && ... && true),
            "field rules must be RUVIA_MIN, RUVIA_MAX, RUVIA_ONE_OF, RUVIA_EMAIL, RUVIA_PATTERN, "
            "or RUVIA_CUSTOM");
    }

    template <typename value_type, typename validator_type>
    void validate(model_field_state state_value, const std::optional<value_type>& value,
        [[maybe_unused]] const model_path& path, [[maybe_unused]] validator_type& validator_value) const {
        if (state_value != model_field_state::parsed || !value) {
            return;
        }
        (validate_rule(*value, path, validator_value, rule_types{}), ...);
    }
};

template <typename... descriptor_types>
struct model_schema final {};

struct empty_model_tag final {};

struct model_access final {
    template <typename model_type>
    [[nodiscard]] static constexpr auto schema() noexcept {
        return typename std::remove_cvref_t<model_type>::ruvia_model_schema_type{};
    }

    template <std::size_t index, typename model_type>
    [[nodiscard]] static decltype(auto) slot(model_type& value) noexcept {
        return value.fields_.template slot<index>();
    }

    template <typename model_type>
    [[nodiscard]] static model_type empty() {
        return model_type(empty_model_tag{});
    }
};

template <typename model_type, typename... descriptor_types, typename visitor_type, std::size_t... indices>
constexpr void visit_model_fields_impl(model_type& model, model_schema<descriptor_types...>, visitor_type&& visitor,
    std::index_sequence<indices...>) {
    auto& visitor_ref = visitor;
    (visitor_ref(descriptor_types{}, model_access::slot<indices>(model)), ...);
}

template <typename model_type, typename... descriptor_types, typename visitor_type>
constexpr void visit_model_fields(
    model_type& model, model_schema<descriptor_types...> schema, visitor_type&& visitor) {
    visit_model_fields_impl(
        model, schema, std::forward<visitor_type>(visitor), std::index_sequence_for<descriptor_types...>{});
}

}  // namespace ruvia::detail::model

namespace ruvia::detail {

struct model_validation_access final {
    template <typename model_type>
    [[nodiscard]] static bool structure_valid(const model_type& model) {
        bool valid = true;
        model::visit_model_fields(model, model::model_access::schema<model_type>(), [&](const auto&, const auto& slot) {
            using slot_type = std::remove_cvref_t<decltype(slot)>;
            const auto state_value = slot.state();
            if (state_value == model_field_state::duplicate || state_value == model_field_state::invalid_type ||
                (slot_type::required && state_value == model_field_state::missing)) {
                valid = false;
                return;
            }
            if constexpr (contains_model<typename slot_type::value_type>()) {
                if (const auto& value = slot.value(); value && !value_structure_valid(*value)) {
                    valid = false;
                }
            }
        });
        return valid;
    }

    template <typename model_type, typename validator_type>
    static void validate_model(const model_type& model_value, validator_type& validator_value) {
        model::model_issue_sink sink(validator_value);
        const model::model_path root;
        validate_structure(model_value, root, sink);
        validate_field_rules(model_value, root, sink);
    }

    template <typename value_type>
    [[nodiscard]] static consteval bool contains_model() noexcept {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            return true;
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            return contains_model<typename t_type::value_type>();
        } else {
            return false;
        }
    }

    template <typename value_type, typename validator_type>
    static void validate_value(const value_type& value, validator_type& validator_value) {
        model::model_issue_sink sink(validator_value);
        const model::model_path root;
        validate_value_structure(value, root, sink);
        validate_nested_field_rules(value, root, sink);
    }
    template <typename value_type>
    [[nodiscard]] static bool value_rules_valid(const value_type& value) {
        struct boolean_rule_sink final {
            bool valid_{true};
            void add(const model::model_path&, std::string_view, std::string_view) noexcept {
                valid_ = false;
            }
        } sink;
        const model::model_path root;
        validate_nested_field_rules(value, root, sink);
        return sink.valid_;
    }
    template <typename value_type>
    [[nodiscard]] static bool value_structure_valid(const value_type& value) {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            return structure_valid(value);
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_type = typename t_type::value_type;
            if constexpr (contains_model<element_type>()) {
                for (const auto& element : value) {
                    if (!value_structure_valid(element)) {
                        return false;
                    }
                }
            }
            return true;
        } else {
            return true;
        }
    }

private:
    template <typename model_type, typename sink_type>
    static void validate_structure(
        const model_type& model_value, const model::model_path& prefix, sink_type& sink) {
        model::visit_model_fields(
            model_value, model::model_access::schema<model_type>(), [&](const auto&, const auto& slot) {
                using slot_type = std::remove_cvref_t<decltype(slot)>;
                const model::model_path path(prefix, slot.wire_name());

                switch (slot.state()) {
                    case model_field_state::duplicate:
                        sink.add(path, "duplicate", "is duplicated");
                        return;
                    case model_field_state::invalid_type:
                        sink.add(path, "invalid_type", model::expected_type_name<typename slot_type::value_type>());
                        return;
                    case model_field_state::missing:
                        if constexpr (slot_type::required) {
                            sink.add(path, "required", "is required");
                        }
                        return;
                    case model_field_state::null:
                        return;
                    case model_field_state::parsed:
                        break;
                }

                if constexpr (contains_model<typename slot_type::value_type>()) {
                    validate_value_structure(*slot.value(), path, sink);
                }
            });
    }

    template <typename model_type, typename sink_type>
    static void validate_field_rules(
        const model_type& model_value, const model::model_path& prefix, sink_type& sink) {
        model::visit_model_fields(
            model_value, model::model_access::schema<model_type>(), [&](const auto& descriptor, const auto& slot) {
                using descriptor_type = std::remove_cvref_t<decltype(descriptor)>;
                using value_type = typename descriptor_type::value_type;
                if constexpr (descriptor_type::has_field_rules || contains_model<value_type>()) {
                    const model::model_path path(prefix, slot.wire_name());
                    if constexpr (descriptor_type::has_field_rules) {
                        typename descriptor_type::rules_type{}.validate(slot.state(), slot.value(), path, sink);
                    }
                    if constexpr (contains_model<value_type>()) {
                        if (slot.state() == model_field_state::parsed && slot.value()) {
                            validate_nested_field_rules(*slot.value(), path, sink);
                        }
                    }
                }
            });
    }

    template <typename value_type, typename sink_type>
    static void validate_value_structure(
        const value_type& value, const model::model_path& path, sink_type& sink) {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            validate_structure(value, path, sink);
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_type = typename t_type::value_type;
            if constexpr (contains_model<element_type>()) {
                std::size_t index = 0;
                for (const auto& element : value) {
                    const model::model_path item_path(path, index++);
                    validate_value_structure(element, item_path, sink);
                }
            }
        }
    }

    template <typename value_type, typename sink_type>
    static void validate_nested_field_rules(
        const value_type& value, const model::model_path& path, sink_type& sink) {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            validate_field_rules(value, path, sink);
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_type = typename t_type::value_type;
            if constexpr (contains_model<element_type>()) {
                std::size_t index = 0;
                for (const auto& element : value) {
                    const model::model_path item_path(path, index++);
                    validate_nested_field_rules(element, item_path, sink);
                }
            }
        }
    }
};

}  // namespace ruvia::detail

namespace ruvia::detail::model {

// Field options are type-level metadata, not per-model runtime objects.
template <typename... option_types>
class model_options final {
    static_assert((is_model_option<option_types>() && ... && true),
        "model field options must be RUVIA_DEFAULT, RUVIA_INITIAL, RUVIA_NULLABLE, RUVIA_OMIT_EMPTY, or RUVIA_EMIT_NULL");
    static_assert(((is_default_rule<option_types>() ? 1 : 0) + ... + 0) <= 1,
        "a model field may have at most one RUVIA_DEFAULT");
    static_assert(((is_initial_rule<option_types>() ? 1 : 0) + ... + 0) <= 1,
        "a model field may have at most one RUVIA_INITIAL");

public:
    static constexpr bool nullable = (std::is_same_v<option_types, ::ruvia::detail::model::nullable> || ... || false);

    [[nodiscard]] static constexpr bool emit_null() noexcept {
        return contains_option<::ruvia::detail::model::emit_null>();
    }

    [[nodiscard]] static constexpr bool omit_empty() noexcept {
        return contains_option<::ruvia::detail::model::omit_empty>();
    }

    static constexpr bool has_initial = (is_initial_rule<option_types>() || ... || false);
    static constexpr bool has_default = (is_default_rule<option_types>() || ... || false);

    // The field owns assignment, nullability and storage normalization.
    template <typename consumer_type>
    static void apply_default(consumer_type&& consume) {
        if constexpr (sizeof...(option_types) != 0) {
            (apply_default_option<option_types>(consume), ...);
        } else {
            (void)consume;
        }
    }

    template <typename consumer_type>
    static void apply_initial(consumer_type&& consume) {
        if constexpr (sizeof...(option_types) != 0) {
            (apply_initial_option<option_types>(consume), ...);
        } else {
            (void)consume;
        }
    }

private:
    template <typename option_type>
    [[nodiscard]] static constexpr bool contains_option() noexcept {
        return (std::is_same_v<std::remove_cvref_t<option_types>, option_type> || ... || false);
    }

    template <typename option_type, typename consumer_type>
    static void apply_initial_option(consumer_type& consume) {
        if constexpr (is_initial_rule<option_type>()) {
            consume(option_type::value());
        } else {
            (void)consume;
        }
    }

    template <typename option_type, typename consumer_type>
    static void apply_default_option(consumer_type& consume) {
        if constexpr (is_default_rule<option_type>()) {
            consume(option_type::value());
        } else {
            (void)consume;
        }
    }
};

template <typename field_type, typename value_type>
void assign_field_value(std::optional<field_type>& target, value_type&& value) {
    if constexpr (detail::is_ruvia_scalar<field_type>) {
        // Validate narrowing before replacing an existing field value.
        const auto field = [&] {
            if constexpr (detail::is_ruvia_scalar<value_type>) {
                return field_type(value.value_);
            } else {
                return field_type(std::forward<value_type>(value));
            }
        }();
        target.emplace(field);
    } else if constexpr (detail::is_ruvia_string<field_type> &&
                         std::is_same_v<std::remove_cvref_t<value_type>, std::string> &&
                         std::is_rvalue_reference_v<value_type&&>) {
        field_type field;
        field.assign_owned(std::forward<value_type>(value));
        target.emplace(std::move(field));
    } else if constexpr (detail::is_ruvia_string<field_type> &&
                         std::is_convertible_v<value_type&&, std::string_view> &&
                         !std::is_same_v<std::remove_cvref_t<value_type>, field_type>) {
        field_type field;
        field.assign_owned(std::string_view(std::forward<value_type>(value)));
        target.emplace(std::move(field));
    } else if constexpr (detail::is_ruvia_bytes<field_type> &&
                         !std::is_same_v<std::remove_cvref_t<value_type>, field_type>) {
        field_type field;
        if constexpr (std::is_same_v<std::remove_cvref_t<value_type>, std::vector<std::uint8_t>> &&
                      std::is_rvalue_reference_v<value_type&&>) {
            field.assign_owned(std::forward<value_type>(value));
        } else {
            field.assign_owned(std::span<const std::uint8_t>(std::forward<value_type>(value)));
        }
        target.emplace(std::move(field));
    } else {
        if constexpr (!std::is_lvalue_reference_v<value_type&&> &&
                      !std::is_const_v<std::remove_reference_t<value_type>>) {
            // Consuming a source that owns this field would destroy the destination.
            if (model_value_rebind_access::owns(value, std::addressof(target))) {
                throw_model_move_source_owns_destination();
            }
        }
        target.emplace(detail::rebind_model_value(std::forward<value_type>(value)));
    }
}

template <typename... descriptor_types>
class model_storage;

// Byte sinks for append_json_member_key: the first measures, the second fills
// an exactly sized compile-time buffer.
struct json_member_key_size_sink final {
    std::size_t size_{0};

    constexpr void push_back(char) noexcept {
        ++size_;
    }
    constexpr void append(std::string_view text) noexcept {
        size_ += text.size();
    }
};

template <std::size_t capacity>
struct json_member_key_buffer final {
    std::array<char, capacity> bytes_{};
    std::size_t size_{0};

    constexpr void push_back(char value) noexcept {
        bytes_[size_++] = value;
    }
    constexpr void append(std::string_view text) noexcept {
        for (const char value : text) {
            push_back(value);
        }
    }
};

// Size of `,"<escaped wire name>":`, or 0 when the name is not well-formed UTF-8.
template <fixed_string wire_name>
[[nodiscard]] consteval std::size_t json_member_key_size() {
    json_member_key_size_sink sink;
    return append_json_member_key(sink, wire_name.view()) ? sink.size_ : 0;
}

template <fixed_string wire_name>
[[nodiscard]] consteval std::array<char, json_member_key_size<wire_name>()> make_json_member_key() {
    json_member_key_buffer<json_member_key_size<wire_name>()> key;
    (void)append_json_member_key(key, wire_name.view());
    return key.bytes_;
}

template <typename field_value_type, bool is_required, typename options_type, fixed_string field_wire_name>
class model_field final {
    static_assert(std::is_empty_v<options_type>, "model field options must be stateless metadata");
    static_assert(json_member_key_size<field_wire_name>() != 0, "Ruvia model wire names must be well-formed UTF-8");
    // A default never satisfies a required input field, so accepting one would
    // silently ignore it.
    static_assert(!(is_required && options_type::has_default),
        "RUVIA_DEFAULT applies only to a missing optional field; declare it with RUVIA_OPTIONAL_FIELD");

public:
    using value_type = field_value_type;
    static constexpr bool required = is_required;
    static constexpr bool nullable = options_type::nullable;
    constexpr model_field() noexcept = default;

    [[nodiscard]] constexpr std::string_view wire_name() const noexcept {
        return field_wire_name.view();
    }

    // `,"<wire name>":` escaped at compile time; the first emitted member drops the comma.
    [[nodiscard]] static constexpr std::string_view json_member_key(bool leading_comma) noexcept {
        const std::string_view key(json_member_key_.data(), json_member_key_.size());
        return leading_comma ? key : key.substr(1);
    }

    [[nodiscard]] detail::model_field_state state() const noexcept {
        return state_;
    }

    // Input provenance survives defaults and later application mutations.
    [[nodiscard]] bool is_present() const noexcept {
        return present_;
    }

    [[nodiscard]] bool is_null() const noexcept {
        return state_ == detail::model_field_state::null;
    }

    [[nodiscard]] bool has_value() const noexcept {
        return value_.has_value();
    }

    [[nodiscard]] const std::optional<field_value_type>& value() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }

    [[nodiscard]] const std::optional<field_value_type>& value() const&& = delete;

    [[nodiscard]] const field_value_type& required_value() const& RUVIA_LIFETIMEBOUND
        requires required
    {
        if (!value_) {
            throw std::logic_error("required model field has no value");
        }
        return *value_;
    }

    [[nodiscard]] const field_value_type& required_value() const&&
        requires required
    = delete;

    // Dynamic JSON tokens require whole-value assignment to keep null state
    // synchronized; only values that cannot become JSON null expose mutation.
    [[nodiscard]] field_value_type& ensure() & RUVIA_LIFETIMEBOUND
        requires(!detail::is_ruvia_json_value<field_value_type>)
    {
        if (!value_) {
            value_.emplace();
        }
        state_ = detail::model_field_state::parsed;
        return *value_;
    }

    field_value_type& ensure() &&
        requires(!detail::is_ruvia_json_value<field_value_type>)
    = delete;

    template <typename input_type>
    void assign(input_type&& input) {
        if constexpr (std::is_null_pointer_v<std::remove_cvref_t<input_type>>) {
            static_assert(nullable, "setting null requires RUVIA_NULLABLE");
            assign_null();
        } else {
            if constexpr (detail::is_ruvia_json_value<input_type>) {
                if (input.is_null()) {
                    if constexpr (nullable) {
                        assign_null();
                        return;
                    } else {
                        throw std::invalid_argument("setting JSON null requires RUVIA_NULLABLE");
                    }
                }
            }
            assign_field_value(value_, std::forward<input_type>(input));
            state_ = detail::model_field_state::parsed;
        }
    }

    void set_null() noexcept
        requires nullable
    {
        assign_null();
    }

    void reset() noexcept {
        state_ = detail::model_field_state::missing;
        value_.reset();
    }

    void apply_initial() {
        options_type::apply_initial([this]<typename input_type>(input_type&& value) {
            this->assign(std::forward<input_type>(value));
        });
    }

    void apply_default() {
        if constexpr (options_type::has_default) {
            if (state_ != detail::model_field_state::missing) {
                return;
            }
            options_type::apply_default([this]<typename input_type>(input_type&& value) {
                this->assign(std::forward<input_type>(value));
            });
        }
    }

    [[nodiscard]] constexpr bool emit_null() const noexcept {
        return options_type::emit_null();
    }

    [[nodiscard]] constexpr bool omit_empty() const noexcept {
        return options_type::omit_empty();
    }

    friend bool operator==(const model_field& left, const model_field& right) {
        if (left.state_ != right.state_) {
            return false;
        }
        if (left.state_ == detail::model_field_state::parsed) {
            return left.value_ == right.value_;
        }
        return true;
    }

    void mark_duplicate() noexcept {
        present_ = true;
        state_ = detail::model_field_state::duplicate;
    }

    void mark_invalid_type() noexcept {
        present_ = true;
        state_ = detail::model_field_state::invalid_type;
    }

    void mark_null() noexcept {
        present_ = true;
        assign_null();
    }

private:
    template <typename... descriptor_types>
    friend class model_storage;
    friend struct ::ruvia::detail::model_value_factory;

    void assign_null() noexcept {
        value_.reset();
        state_ = detail::model_field_state::null;
    }

    void emplace_parsed(field_value_type&& value) {
        value_.emplace(std::move(value));
        present_ = true;
        state_ = detail::model_field_state::parsed;
    }

    [[nodiscard]] model_field rebind() const {
        model_field rebound;
        rebound.state_ = state_;
        rebound.present_ = present_;
        if (value_) {
            rebound.value_.emplace(detail::rebind_model_value(*value_));
        }
        return rebound;
    }

    [[nodiscard]] bool owns(const void* address) const noexcept {
        return value_ && model_value_rebind_access::owns(*value_, address);
    }

    void reset_after_move() noexcept {
        present_ = false;
        value_.reset();
        state_ = detail::model_field_state::missing;
    }

    static constexpr auto json_member_key_ = make_json_member_key<field_wire_name>();
    detail::model_field_state state_{detail::model_field_state::missing};
    bool present_{false};
    std::optional<field_value_type> value_;
};

template <typename t_type>
using field_option_tuple_type =
    std::conditional_t<is_model_option<t_type>(), std::tuple<t_type>, std::tuple<>>;

template <typename t_type>
using field_rule_tuple_type =
    std::conditional_t<is_validation_rule<t_type>(), std::tuple<t_type>, std::tuple<>>;

template <typename tuple_type>
struct tuple_model_options;

template <typename... ts_type>
struct tuple_model_options<std::tuple<ts_type...>> {
    using type = model_options<ts_type...>;
};

template <typename tuple_type>
struct tuple_rules;

template <typename... ts_type>
struct tuple_rules<std::tuple<ts_type...>> {
    using type = rules<ts_type...>;
};

template <fixed_string field_source_name, fixed_string field_wire_name, typename field_value_type, bool is_required,
    typename... arg_types>
struct model_field_descriptor final {
    static_assert(((is_model_option<arg_types>() || is_validation_rule<arg_types>()) && ... && true),
        "RUVIA_REQUIRED_FIELD/RUVIA_OPTIONAL_FIELD accept model options (RUVIA_DEFAULT, "
        "RUVIA_INITIAL, RUVIA_NULLABLE, RUVIA_OMIT_EMPTY, RUVIA_EMIT_NULL) and validation rules "
        "(RUVIA_MIN, RUVIA_EMAIL, ...)");

    using value_type = field_value_type;
    using options_type = typename tuple_model_options<decltype(std::tuple_cat(std::tuple<>{},
        std::declval<field_option_tuple_type<arg_types>>()...))>::type;
    using rules_type = typename tuple_rules<decltype(std::tuple_cat(std::tuple<>{},
        std::declval<field_rule_tuple_type<arg_types>>()...))>::type;
    using field_type = model_field<field_value_type, is_required, options_type, field_wire_name>;

    static constexpr auto source_name = field_source_name;
    static constexpr auto wire_name = field_wire_name;
    static constexpr bool required = is_required;
    static constexpr bool nullable = options_type::nullable;
    static constexpr bool has_field_rules = !std::is_same_v<rules_type, rules<>>;

    static_assert(!has_field_rules ||
                      !(detail::is_ruvia_json_value<field_value_type> || detail::is_ruvia_json_object<field_value_type>),
        "json_value/json_object are dynamic content, not typed field validation schemas");
};
template <typename... descriptor_types>
consteval void validate_model_schema(model_schema<descriptor_types...>) {
    static_assert((detail::is_model_field<typename descriptor_types::value_type> && ...),
        "model fields must use Ruvia values or nested models");
}

template <fixed_string field, typename... descriptor_types>
[[nodiscard]] consteval std::size_t model_field_index() {
    static_assert((std::size_t{field == descriptor_types::source_name} + ... + 0) == 1,
        "unknown or duplicate Ruvia model field");
    const std::array<std::string_view, sizeof...(descriptor_types)> names{descriptor_types::source_name.view()...};
    return fixed_name_index(names, field.view());
}

template <typename... descriptor_types>
[[nodiscard]] consteval bool unique_model_field_names() {
    const std::array<std::string_view, sizeof...(descriptor_types)> names{descriptor_types::source_name.view()...};
    return fixed_names_distinct(names);
}

template <typename... descriptor_types>
[[nodiscard]] consteval bool unique_model_wire_names() {
    const std::array<std::string_view, sizeof...(descriptor_types)> names{descriptor_types::wire_name.view()...};
    return fixed_names_distinct(names);
}

// Case-insensitive sources (HTTP headers) bind at most one field per name, so
// their schemas must not declare ASCII-case-equivalent wire names.
template <typename... descriptor_types>
[[nodiscard]] consteval bool unique_model_wire_names_ignore_ascii_case(model_schema<descriptor_types...>) {
    const std::array<std::string_view, sizeof...(descriptor_types)> names{descriptor_types::wire_name.view()...};
    return fixed_names_distinct(names, [](std::string_view left, std::string_view right) {
        return http_ascii_equals_ignore_case(left, right);
    });
}
template <std::size_t index, typename descriptor_type, typename... remaining_types, typename model_type,
    typename visitor_type>
[[nodiscard]] bool visit_model_field_by_wire_name_impl(model_type& model,
    std::string_view wire_name, bool& visit_result, visitor_type& visitor) {
    if (wire_name == descriptor_type::wire_name.view()) {
        visit_result = visitor(model_access::slot<index>(model));
        return true;
    }
    if constexpr (sizeof...(remaining_types) > 0) {
        return visit_model_field_by_wire_name_impl<index + 1, remaining_types...>(
            model, wire_name, visit_result, visitor);
    } else {
        return false;
    }
}

template <typename model_type, typename... descriptor_types, typename visitor_type>
[[nodiscard]] bool visit_model_field_by_wire_name(model_type& model, model_schema<descriptor_types...>,
    std::string_view wire_name, bool& visit_result, visitor_type&& visitor) {
    if constexpr (sizeof...(descriptor_types) == 0) {
        return false;
    } else {
        auto& visitor_ref = visitor;
        return visit_model_field_by_wire_name_impl<0, descriptor_types...>(
            model, wire_name, visit_result, visitor_ref);
    }
}

template <typename model_type>
void initialize_model(model_type& value) {
    visit_model_fields(value, model_access::schema<model_type>(),
        [](const auto&, auto& slot) { slot.apply_initial(); });
}

}  // namespace ruvia::detail::model

namespace ruvia::detail {

struct form_body_input final {
    std::string_view body_;
    model_string_storage string_storage_{model_string_storage::borrowed};
};

using model_input = std::variant<form_body_input, std::reference_wrapper<const request_name_value_list>>;

template <typename visitor_type>
    requires std::invocable<visitor_type&, std::string_view, std::string_view>
void visit_form_body_fields(std::string_view body, visitor_type&& visitor) {
    auto& visitor_ref = visitor;
    std::string decoded_name;
    (void)ruvia::visit_url_encoded_pairs(body, [&](std::string_view name, std::string_view value) {
        visitor_ref(*ruvia::decode_url_component(name, decoded_name, url_decode_mode::form), value);
    });
}

enum class form_value_encoding : std::uint8_t { url_encoded,
    decoded };
[[nodiscard]] inline std::optional<bool> parse_form_bool(std::string_view decoded) noexcept {
    if (decoded == "true" || decoded == "1") {
        return true;
    }
    if (decoded == "false" || decoded == "0") {
        return false;
    }
    return std::nullopt;
}

template <typename number_type>
[[nodiscard]] std::optional<number_type> parse_form_number(std::string_view decoded) {
    if (decoded.empty()) {
        return std::nullopt;
    }
    number_type parsed_value{};
    if constexpr (std::is_floating_point_v<number_type>) {
        const auto value = detail::parse_decimal_number<number_type>(decoded);
        if (!value) {
            return std::nullopt;
        }
        parsed_value = *value;
        if (!std::isfinite(parsed_value)) {
            return std::nullopt;
        }
    } else {
        const auto integer = parse_integer<number_type>(decoded);
        if ((integer.index() != 0)) {
            return std::nullopt;
        }
        parsed_value = std::get<0>(integer);
    }
    return parsed_value;
}

[[nodiscard]] inline std::string_view decode_form_value(
    std::string_view input, form_value_encoding encoding, std::string& storage) {
    if (encoding == form_value_encoding::decoded) {
        return input;
    }
    return *ruvia::decode_url_component(input, storage, url_decode_mode::form);
}

template <typename t_type>
    requires is_form_field<t_type>
[[nodiscard]] std::optional<t_type> parse_form_value(
    std::string_view input, form_value_encoding encoding, model_string_storage string_storage) {
    using field_type = std::remove_cvref_t<t_type>;

    if constexpr (is_ruvia_scalar<field_type>) {
        std::string decoded_storage;
        const auto decoded = decode_form_value(input, encoding, decoded_storage);
        using scalar_type = model_scalar_value_type<field_type>;
        if constexpr (std::is_same_v<scalar_type, bool>) {
            const auto parsed_value = parse_form_bool(decoded);
            if (!parsed_value.has_value()) {
                return std::nullopt;
            }
            return field_type(*parsed_value);
        } else {
            const auto parsed_value = parse_form_number<scalar_type>(decoded);
            if (!parsed_value.has_value()) {
                return std::nullopt;
            }
            return field_type(*parsed_value);
        }
    } else {
        std::string decoded_storage;
        if (decode_form_value(input, encoding, decoded_storage).data() != input.data()) {
            field_type value;
            value.assign_owned(std::move(decoded_storage));
            return value;
        }
        if (string_storage == model_string_storage::owned) {
            return field_type(input);
        }
        return model_value_factory::make_borrowed_string(input);
    }
}

// A parse-local, conservative representation budget.
class json_parse_budget final {
public:
    explicit json_parse_budget(model_parse_options options = {}) noexcept
        : elements_(options.max_array_elements_),
          bytes_(options.max_representation_bytes_) {}

    [[nodiscard]] bool consume_bytes(std::size_t bytes_value) noexcept {
        if (bytes_value > bytes_) {
            exhausted_ = true;
            return false;
        }
        bytes_ -= bytes_value;
        return true;
    }

    // Charges one owned text of `bytes` plus its string object.
    [[nodiscard]] bool consume_owned_text(std::size_t bytes, std::size_t copies = 1) noexcept {
        return consume_owned_text_chunk(bytes, copies, true);
    }

    [[nodiscard]] bool consume_owned_text_chunk(
        std::size_t bytes, std::size_t copies, bool first_chunk) noexcept {
        const auto string_bytes = first_chunk ? sizeof(std::string) : std::size_t{0};
        if (bytes > (std::numeric_limits<std::size_t>::max() - string_bytes) / copies) {
            exhausted_ = true;
            return false;
        }
        return consume_bytes(bytes * copies + string_bytes);
    }
    [[nodiscard]] bool consume_element() noexcept {
        if (elements_ == 0) {
            exhausted_ = true;
            return false;
        }
        --elements_;
        return true;
    }

    [[nodiscard]] bool exhausted() const noexcept {
        return exhausted_;
    }

    [[nodiscard]] std::size_t element_checkpoint() const noexcept {
        return elements_;
    }

    void restore_elements(std::size_t checkpoint) noexcept {
        elements_ = checkpoint;
    }

    [[nodiscard]] bool skip_value(std::string_view& input, std::size_t depth) noexcept {
        auto consume = [this]() noexcept { return consume_element(); };
        return skip_json_value(input, depth, consume);
    }

private:
    std::size_t elements_;
    std::size_t bytes_;
    bool exhausted_{false};
};

template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_json_value(std::string_view& input,
    std::size_t depth, model_string_storage string_storage, json_parse_budget& budget);

// Defined with json_value/json_object in model.h.
template <typename view_type>
[[nodiscard]] std::optional<view_type> parse_json_view_value(std::string_view& input,
    std::size_t depth, model_string_storage string_storage, bool require_object, json_parse_budget& budget);

struct model_parse_access final {
    template <typename model_type>
    [[nodiscard]] static std::optional<model_type> parse_value(std::string_view& input,
        std::size_t depth, model_string_storage string_storage, json_parse_budget& budget) {
        if (depth > max_json_depth) {
            return std::nullopt;
        }
        auto value = model::model_access::empty<model_type>();
        if (!materialize_json(value, input, depth, string_storage, budget)) {
            return std::nullopt;
        }
        return value;
    }

    // Binds every recognized field without checking field structure; the
    // caller validates. Form input carries no syntax that can fail: values a
    // field cannot hold mark that field invalid_type instead.
    template <typename model_type>
    [[nodiscard]] static model_type parse_form(const model_input& input) {
        auto value = model::model_access::empty<model_type>();
        materialize_form(value, input);
        return value;
    }

    template <typename model_type>
    [[nodiscard]] static std::optional<model_type> parse_form_owned(std::string_view body) {
        auto value = parse_form<model_type>(
            form_body_input{.body_ = body, .string_storage_ = model_string_storage::owned});
        if (!model_validation_access::structure_valid(value)) {
            return std::nullopt;
        }
        return value;
    }

private:
    template <typename model_type>
    static void apply_defaults(model_type& value) {
        model::visit_model_fields(value, model::model_access::schema<model_type>(),
            [](const auto&, auto& slot) { slot.apply_default(); });
    }

    template <typename slot_type>
    [[nodiscard]] static bool parse_json_field_slot(slot_type& slot, std::string_view& value_input,
        std::size_t depth, model_string_storage string_storage, json_parse_budget& budget) {
        if (slot.is_present()) {
            slot.mark_duplicate();
            return budget.skip_value(value_input, depth + 1);
        }
        using unwrapped_slot_type = std::remove_cvref_t<slot_type>;
        using value_type = typename unwrapped_slot_type::value_type;
        auto null_input = value_input;
        if (consume_json_literal(null_input, "null")) {
            value_input = null_input;
            if constexpr (unwrapped_slot_type::nullable) {
                slot.mark_null();
            } else {
                slot.mark_invalid_type();
            }
            return true;
        }
        const auto element_checkpoint = budget.element_checkpoint();
        if (auto parsed_value =
                parse_json_value<value_type>(value_input, depth + 1, string_storage, budget);
            parsed_value) {
            model_value_factory::emplace_parsed(slot, std::move(*parsed_value));
            return true;
        }
        if (budget.exhausted()) {
            return false;
        }
        // A failed value parse leaves the cursor at the value, so the
        // wrong-type value is skipped structurally. The skip rescans the whole
        // value: count its elements once, while retaining byte charges.
        budget.restore_elements(element_checkpoint);
        if (!budget.skip_value(value_input, depth + 1)) {
            return false;
        }
        slot.mark_invalid_type();
        return true;
    }

    template <typename model_type>
    [[nodiscard]] static bool visit_json_field(model_type& value, std::string_view key,
        std::string_view& value_input, std::size_t depth,
        model_string_storage string_storage, json_parse_budget& budget) {
        bool field_result = true;
        const bool matched = model::visit_model_field_by_wire_name(value,
            model::model_access::schema<model_type>(), key, field_result,
            [&](auto& slot) -> bool {
                return parse_json_field_slot(slot, value_input, depth, string_storage, budget);
            });
        return matched ? field_result : budget.skip_value(value_input, depth + 1);
    }

    template <typename model_type>
    static bool materialize_json(model_type& value, std::string_view& input, std::size_t depth,
        model_string_storage string_storage, json_parse_budget& budget) {
        const auto visit_field = [&value, depth, string_storage, &budget](
                                     std::string_view key, std::string_view& value_input) -> bool {
            return visit_json_field(value, key, value_input, depth, string_storage, budget);
        };
        const auto consume_key_allocation = [&budget](std::size_t bytes_value, bool first_chunk) noexcept {
            return budget.consume_owned_text_chunk(bytes_value, 1, first_chunk);
        };
        const bool valid = consume_json_object_fields(input, depth, visit_field, consume_key_allocation);
        if (valid) {
            apply_defaults(value);
        }
        return valid;
    }

    template <typename slot_type>
    static void bind_form_field_slot(slot_type& slot, std::string_view field_value,
        form_value_encoding encoding, model_string_storage string_storage) {
        if (slot.is_present()) {
            slot.mark_duplicate();
            return;
        }
        using value_type = typename std::remove_cvref_t<slot_type>::value_type;
        // Name/value inputs only carry strings and scalars. A value for any
        // other field is a type error, never an absence that a default fills.
        if constexpr (is_form_field<value_type>) {
            auto parsed_value = parse_form_value<value_type>(field_value, encoding, string_storage);
            if (parsed_value) {
                model_value_factory::emplace_parsed(slot, std::move(*parsed_value));
                return;
            }
        }
        slot.mark_invalid_type();
    }

    template <typename model_type>
    static void bind_form_field(model_type& value, std::string_view key, std::string_view field_value,
        form_value_encoding encoding, model_string_storage string_storage) {
        bool field_result = true;
        (void)model::visit_model_field_by_wire_name(value, model::model_access::schema<model_type>(),
            key, field_result, [&](auto& slot) -> bool {
                bind_form_field_slot(slot, field_value, encoding, string_storage);
                return true;
            });
    }

    // Case-insensitive sources (HTTP headers) bind the first field whose wire
    // name equals the name under the source's comparison.
    template <typename model_type>
    static void bind_form_field_ignore_case(model_type& value, const request_name_value_list& fields_value,
        std::string_view key, std::string_view field_value) {
        bool matched = false;
        model::visit_model_fields(value, model::model_access::schema<model_type>(),
            [&](const auto&, auto& slot) {
                if (matched || !request_name_value_list_access::names_equal(fields_value, key, slot.wire_name())) {
                    return;
                }
                matched = true;
                bind_form_field_slot(slot, field_value, form_value_encoding::decoded,
                    model_string_storage::borrowed);
            });
    }

    template <typename model_type>
    static void materialize_form(model_type& value, const model_input& input) {
        if (const auto* body = std::get_if<form_body_input>(&input)) {
            visit_form_body_fields(body->body_,
                [&value, string_storage = body->string_storage_](std::string_view key, std::string_view field_value) {
                    bind_form_field(value, key, field_value, form_value_encoding::url_encoded, string_storage);
                });
        } else {
            const request_name_value_list& fields_value = std::get<1>(input);
            const bool case_insensitive = request_name_value_list_access::case_insensitive(fields_value);
            for (const auto& field : fields_value) {
                if (case_insensitive) {
                    bind_form_field_ignore_case(value, fields_value, field.name(), field.value());
                } else {
                    bind_form_field(value, field.name(), field.value(), form_value_encoding::decoded,
                        model_string_storage::borrowed);
                }
            }
        }
        apply_defaults(value);
    }
};

template <typename sequence_type>
struct json_sequence_value_traits;

template <typename element_type>
struct json_sequence_value_traits<array<element_type>> {
    using value_type = element_type;

    static void emplace(array<element_type>& value, element_type&& element) {
        model_value_factory::emplace_parsed(value, std::move(element));
    }
};

template <typename element_type>
struct json_sequence_value_traits<boxed_array<element_type>> {
    using value_type = element_type;

    static void emplace(boxed_array<element_type>& value, element_type&& element) {
        model_value_factory::emplace_parsed(value, std::move(element));
    }
};

template <typename sequence_type>
[[nodiscard]] std::optional<sequence_type> parse_json_sequence_value(std::string_view& input,
    std::size_t depth, model_string_storage string_storage, json_parse_budget& budget) {
    using traits_type = json_sequence_value_traits<std::remove_cvref_t<sequence_type>>;
    using element_type = typename traits_type::value_type;

    sequence_type value;
    const bool consumed = consume_json_array_elements(input, depth,
        [&value, depth, string_storage, &budget](std::string_view& element_input) {
            if (!budget.consume_element()) {
                return false;
            }
            // Four element slots cover geometric vector allocations. Boxed
            // values additionally own pointers.
            constexpr auto element_bytes = 4 * (sizeof(element_type) + sizeof(element_type*));
            if (!budget.consume_bytes(element_bytes)) {
                return false;
            }
            auto element =
                parse_json_value<element_type>(element_input, depth + 1, string_storage, budget);
            if (!element.has_value()) {
                return false;
            }
            traits_type::emplace(value, std::move(*element));
            return true;
        });
    if (!consumed) {
        return std::nullopt;
    }
    return value;
}

// Every branch commits `input` only when it returns a value.
template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_json_value(std::string_view& input,
    std::size_t depth, model_string_storage string_storage, json_parse_budget& budget) {
    using field_type = std::remove_cvref_t<t_type>;
    if (depth > max_json_depth || budget.exhausted()) {
        return std::nullopt;
    }
    auto remaining = input;
    if constexpr (is_ruvia_string<field_type>) {
        std::string decoded;
        bool started = false;
        const auto token = parse_json_string(remaining, &decoded, [&](std::size_t chunk_bytes) noexcept {
            const bool first = !std::exchange(started, true);
            return budget.consume_owned_text_chunk(chunk_bytes, 2, first);
        });
        if (!token.has_value()) {
            return std::nullopt;
        }
        if (token->encoding() == json_string_encoding::escaped) {
            input = remaining;
            field_type value;
            value.assign_owned(std::move(decoded));
            return value;
        }
        if (string_storage == model_string_storage::borrowed) {
            input = remaining;
            return model_value_factory::make_borrowed_string(token->raw());
        }
        if (!budget.consume_owned_text(token->raw().size())) {
            return std::nullopt;
        }
        input = remaining;
        return field_type(token->raw());
    } else if constexpr (is_ruvia_bytes<field_type>) {
        std::string decoded;
        bool started = false;
        const auto token = parse_json_string(remaining, &decoded, [&](std::size_t chunk_bytes) noexcept {
            // Escaped base64 may retain both decoded text and the byte value.
            const bool first = !std::exchange(started, true);
            return budget.consume_owned_text_chunk(chunk_bytes, 2, first);
        });
        if (!token ||
            (token->encoding() == json_string_encoding::literal &&
                !budget.consume_owned_text(token->raw().size(), 2))) {
            return std::nullopt;
        }
        auto value = decode_model_binary(json_string_text(*token, decoded));
        if (!value) {
            return std::nullopt;
        }
        input = remaining;
        return value;
    } else if constexpr (is_ruvia_json_value<field_type> || is_ruvia_json_object<field_type>) {
        return parse_json_view_value<field_type>(
            input, depth, string_storage, is_ruvia_json_object<field_type>, budget);
    } else if constexpr (is_ruvia_array<field_type> || is_ruvia_boxed_array<field_type>) {
        auto parsed_value = parse_json_sequence_value<field_type>(remaining, depth, string_storage, budget);
        if (!parsed_value.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return parsed_value;
    } else if constexpr (is_ruvia_scalar<field_type>) {
        using scalar_type = model_scalar_value_type<field_type>;
        scalar_type parsed_value{};
        if constexpr (std::is_same_v<scalar_type, bool>) {
            if (consume_json_literal(remaining, "true")) {
                parsed_value = true;
            } else if (consume_json_literal(remaining, "false")) {
                parsed_value = false;
            } else {
                return std::nullopt;
            }
        } else {
            if (!parse_json_number_value(remaining, parsed_value)) {
                return std::nullopt;
            }
        }
        input = remaining;
        return field_type(parsed_value);
    } else if constexpr (is_model<field_type>) {
        auto nested =
            model_parse_access::parse_value<field_type>(remaining, depth, string_storage, budget);
        if (!nested.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return nested;
    } else {
        static_assert(always_false<field_type>, "RUVIA_MODEL JSON field type is not supported");
    }
}

// Exactly one JSON value (with surrounding whitespace) as `t_type`, within the
// representation budget of `options`. Field structure inside the value is not
// checked; see parse_json_document.
template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_complete_json_value(std::string_view input,
    model_string_storage string_storage, model_parse_options options = {}) {
    json_parse_budget budget(options);
    if (!budget.consume_bytes(sizeof(t_type))) {
        return std::nullopt;
    }
    auto value = parse_json_value<t_type>(input, 0, string_storage, budget);
    skip_json_whitespace(input);
    if (!value || !input.empty()) {
        return std::nullopt;
    }
    return value;
}

// One complete typed JSON value, including structural checks inside arrays.
// Borrowed reads are used by JSON views; standalone codecs request owned data.
template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_json_document(std::string_view input,
    model_string_storage string_storage, model_parse_options options = {}) {
    auto value = parse_complete_json_value<t_type>(input, string_storage, options);
    if (!value || !model_validation_access::value_structure_valid(*value)) {
        return std::nullopt;
    }
    return value;
}

// Whether `body` is exactly one JSON value (an object when require_object),
// with surrounding whitespace, within the limits of `options`. Dynamic tokens
// validate their whole body this way and then borrow it unchanged.
[[nodiscard]] inline bool is_complete_json_value(
    std::string_view body, model_parse_options options, bool require_object) noexcept {
    json_parse_budget budget(options);
    skip_json_whitespace(body);
    if (require_object && (body.empty() || body.front() != '{')) {
        return false;
    }
    if (!budget.skip_value(body, 0)) {
        return false;
    }
    skip_json_whitespace(body);
    return body.empty();
}

struct model_json_access final {
    template <typename model_type>
    [[nodiscard]] static std::size_t size_hint(const model_type& model);

    template <typename model_type>
    static void append(std::string& output, const model_type& model);
};

template <typename value_type>
consteval std::size_t json_fixed_scalar_size_hint() noexcept {
    using t_type = std::remove_cvref_t<value_type>;
    if constexpr (is_ruvia_scalar<t_type>) {
        return json_fixed_scalar_size_hint<decltype(std::declval<t_type>().value_)>();
    } else if constexpr (std::is_same_v<t_type, bool>) {
        return 5;
    } else if constexpr (std::is_integral_v<t_type>) {
        return static_cast<std::size_t>(std::numeric_limits<t_type>::digits10) + 3;
    } else if constexpr (std::is_floating_point_v<t_type>) {
        return 32;
    } else {
        return 0;
    }
}

template <typename value_type>
[[nodiscard]] std::size_t json_size_hint_value(const value_type& value) {
    using t_type = std::remove_cvref_t<value_type>;
    if constexpr (json_fixed_scalar_size_hint<t_type>() != 0) {
        return json_fixed_scalar_size_hint<t_type>();
    } else if constexpr (is_ruvia_bytes<t_type>) {
        return http_base64_encoded_size(value.size()) + 2;
    } else if constexpr (is_ruvia_string<t_type>) {
        return json_string_size_hint(value.view());
    } else if constexpr (is_model<t_type>) {
        return model_json_access::size_hint(value);
    } else if constexpr (is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type>) {
        return value.view().size();
    } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
        constexpr std::size_t item_hint = [] {
            if constexpr (is_ruvia_array<t_type>) {
                return json_fixed_scalar_size_hint<typename t_type::value_type>();
            } else {
                return std::size_t{0};
            }
        }();
        if constexpr (item_hint != 0) {
            return value.empty() ? 2 : 2 + value.size() * item_hint + (value.size() - 1);
        } else {
            std::size_t size = 2;
            bool first = true;
            for (const auto& item : value) {
                if (!first) {
                    ++size;
                }
                first = false;
                size += json_size_hint_value(item);
            }
            return size;
        }
    } else {
        static_assert(
            always_false<t_type>, "JSON output must use Ruvia scalar types or RUVIA_MODEL");
    }
}

template <typename value_type>
void append_json_value(std::string& output, const value_type& value);

template <typename sequence_type>
void append_json_sequence(std::string& output, const sequence_type& value) {
    output.push_back('[');
    bool first = true;
    for (const auto& item : value) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        append_json_value(output, item);
    }
    output.push_back(']');
}

template <typename value_type>
void append_json_value(std::string& output, const value_type& value) {
    using t_type = std::remove_cvref_t<value_type>;
    if constexpr (is_ruvia_scalar<t_type>) {
        append_json_value(output, value.value_);
    } else if constexpr (std::is_same_v<t_type, bool>) {
        output.append(value ? "true" : "false");
    } else if constexpr (std::is_unsigned_v<t_type>) {
        http_append_unsigned(output, value);
    } else if constexpr (std::is_integral_v<t_type>) {
        http_append_signed(output, value);
    } else if constexpr (std::is_floating_point_v<t_type>) {
        // JSON (RFC 8259) has no representation for infinity or NaN, and
        // std::to_chars would emit the bare tokens "inf"/"nan" — invalid JSON.
        // Serialize non-finite values as null, matching JSON.stringify.
        if (!std::isfinite(value)) {
            output.append("null");
            return;
        }
        http_append_formatted_number(output, value, "JSON number cannot be formatted");
    } else if constexpr (is_ruvia_bytes<t_type>) {
        append_model_binary(output, value);
    } else if constexpr (is_ruvia_string<t_type>) {
        append_json_string(output, value.view());
    } else if constexpr (is_model<t_type>) {
        model_json_access::append(output, value);
    } else if constexpr (is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type>) {
        output.append(value.view());
    } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
        append_json_sequence(output, value);
    } else {
        static_assert(
            always_false<t_type>, "JSON output must use Ruvia scalar types or RUVIA_MODEL");
    }
}

namespace model {

enum class serialized_field_kind { value,
    null };

// Both serialization passes share emission and comma decisions, with no
// intermediate representation or runtime field lookup.
template <typename model_type, typename visitor_type>
void visit_serialized_fields(const model_type& value, visitor_type&& visitor) {
    bool first = true;
    visit_model_fields(value, model_access::schema<model_type>(),
        [&](const auto&, const auto& slot) {
            const auto& field_value = slot.value();
            serialized_field_kind kind;
            if (field_value && !(slot.omit_empty() && is_empty_value(*field_value))) {
                kind = serialized_field_kind::value;
            } else if (!field_value && (slot.is_null() || slot.emit_null())) {
                kind = serialized_field_kind::null;
            } else {
                return;
            }
            visitor(slot, kind, !first);
            first = false;
        });
}

}  // namespace model

template <typename model_type>
std::size_t model_json_access::size_hint(const model_type& value) {
    std::size_t size = 2;
    model::visit_serialized_fields(value, [&size](const auto& slot, auto kind, bool comma) {
        size += slot.json_member_key(comma).size();
        size += kind == model::serialized_field_kind::value
                    ? json_size_hint_value(*slot.value())
                    : 4;
    });
    return size;
}

template <typename model_type>
void model_json_access::append(std::string& output, const model_type& value) {
    output.push_back('{');
    model::visit_serialized_fields(value, [&output](const auto& slot, auto kind, bool comma) {
        output.append(slot.json_member_key(comma));
        if (kind == model::serialized_field_kind::value) {
            append_json_value(output, *slot.value());
        } else {
            output.append("null");
        }
    });
    output.push_back('}');
}

}  // namespace ruvia::detail

namespace ruvia::detail::model {

template <typename... descriptor_types>
class model_storage final {
public:
    static constexpr bool has_initial =
        (descriptor_types::options_type::has_initial || ... || false);

    model_storage() noexcept {
        static_assert(unique_model_field_names<descriptor_types...>(),
            "Ruvia model source field names must be unique");
        static_assert(
            unique_model_wire_names<descriptor_types...>(), "Ruvia model JSON field names must be unique");
    }

    model_storage(const model_storage&) = delete;
    model_storage& operator=(const model_storage&) = delete;

    model_storage(model_storage&& other) noexcept
        : fields_(std::move(other.fields_)) {
        reset_moved_fields(other.fields_);
    }

    friend bool operator==(const model_storage& left, const model_storage& right) {
        return left.fields_ == right.fields_;
    }

    model_storage& operator=(model_storage&& other) {
        if (this == &other) {
            return *this;
        }
        // Consuming a source that owns this model would destroy the destination.
        if (::ruvia::detail::model_address_within(this, std::addressof(other), sizeof(model_storage)) ||
            other.owns(this)) {
            ::ruvia::detail::throw_model_move_source_owns_destination();
        }

        auto rebound = rebind_fields(other.fields_);
        reset_moved_fields(other.fields_);
        replace_fields(fields_, std::move(rebound));
        return *this;
    }

    template <fixed_string field>
    [[nodiscard]] decltype(auto) get() const& RUVIA_LIFETIMEBOUND {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        using descriptor_type = std::tuple_element_t<index, std::tuple<descriptor_types...>>;
        const auto& slot = std::get<index>(fields_);
        if constexpr (descriptor_type::required && !descriptor_type::nullable) {
            return slot.required_value();
        } else {
            return slot.value();
        }
    }

    template <fixed_string field>
    [[nodiscard]] decltype(auto) get() const&& = delete;

    // Whether the original parsed input contained this field. Defaults and
    // set/ensure/reset do not rewrite input provenance; unparsed models have
    // no input presence. Use get()/is_null() for the current value state.
    template <fixed_string field>
    [[nodiscard]] bool is_present() const noexcept {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        return std::get<index>(fields_).is_present();
    }

    template <fixed_string field>
    [[nodiscard]] bool is_null() const noexcept {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        return std::get<index>(fields_).is_null();
    }

    template <fixed_string field>
    [[nodiscard]] bool has_value() const noexcept {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        return std::get<index>(fields_).has_value();
    }

    template <fixed_string field, typename value_type>
    void set(value_type&& value) & {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        std::get<index>(fields_).assign(std::forward<value_type>(value));
    }

    template <fixed_string field>
        requires(std::tuple_element_t<model_field_index<field, descriptor_types...>(),
            std::tuple<descriptor_types...>>::nullable)
    void set_null() & noexcept {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        std::get<index>(fields_).set_null();
    }

    template <fixed_string field>
        requires requires(typename std::tuple_element_t<model_field_index<field, descriptor_types...>(),
            std::tuple<descriptor_types...>>::field_type& slot) {
            slot.ensure();
        }
    [[nodiscard]] decltype(auto) ensure() & RUVIA_LIFETIMEBOUND {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        return std::get<index>(fields_).ensure();
    }

    template <fixed_string field>
        requires(!std::tuple_element_t<model_field_index<field, descriptor_types...>(),
            std::tuple<descriptor_types...>>::required)
    void reset() & noexcept {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        std::get<index>(fields_).reset();
    }

    template <std::size_t index>
    [[nodiscard]] decltype(auto) slot() & noexcept {
        return std::get<index>(fields_);
    }

    template <std::size_t index>
    [[nodiscard]] decltype(auto) slot() const& noexcept {
        return std::get<index>(fields_);
    }

    [[nodiscard]] model_storage rebind() const& {
        model_storage rebound;
        auto fields_value = rebind_fields(fields_);
        replace_fields(rebound.fields_, std::move(fields_value));
        return rebound;
    }

    [[nodiscard]] model_storage rebind() && {
        auto rebound = std::as_const(*this).rebind();
        reset_moved_fields(fields_);
        return rebound;
    }

    // Whether `address` lies in storage owned by these fields beyond their
    // inline tuple.
    [[nodiscard]] bool owns(const void* address) const noexcept {
        return std::apply(
            [address](const auto&... field) { return (field.owns(address) || ... || false); }, fields_);
    }

private:
    using field_tuple = std::tuple<typename descriptor_types::field_type...>;

    template <std::size_t... indices>
    [[nodiscard]] static field_tuple rebind_fields(
        const field_tuple& source_value, std::index_sequence<indices...>) {
        return field_tuple{std::get<indices>(source_value).rebind()...};
    }

    [[nodiscard]] static field_tuple rebind_fields(const field_tuple& source_value) {
        return rebind_fields(source_value, std::index_sequence_for<descriptor_types...>{});
    }

    static void replace_fields(field_tuple& destination, field_tuple&& source_value) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<field_tuple>);
        std::destroy_at(std::addressof(destination));
        std::construct_at(std::addressof(destination), std::move(source_value));
    }

    template <std::size_t... indices>
    static void reset_moved_fields(field_tuple& fields_value, std::index_sequence<indices...>) noexcept {
        (std::get<indices>(fields_value).reset_after_move(), ...);
    }

    static void reset_moved_fields(field_tuple& fields_value) noexcept {
        reset_moved_fields(fields_value, std::index_sequence_for<descriptor_types...>{});
    }

    field_tuple fields_;
};

}  // namespace ruvia::detail::model

// The closure is a structural NTTP even when invoking its factory needs runtime
// state. Let the compiler infer constexpr only for expressions that support it.
#define RUVIA_DEFAULT(value) ::ruvia::detail::model::default_value<[]() { return value; }>
#define RUVIA_INITIAL(value) ::ruvia::detail::model::initial<[]() { return value; }>
#define RUVIA_NULLABLE ::ruvia::detail::model::nullable
#define RUVIA_OMIT_EMPTY ::ruvia::detail::model::omit_empty
#define RUVIA_EMIT_NULL ::ruvia::detail::model::emit_null

#define RUVIA_REQUIRED_FIELD(field, type, ...)                                    \
    ::ruvia::detail::model::model_field_descriptor<::ruvia::fixed_string{#field}, \
        ::ruvia::fixed_string{#field}, type, true __VA_OPT__(, ) __VA_ARGS__>

#define RUVIA_REQUIRED_FIELD_NAME(wire_name, field, type, ...)                    \
    ::ruvia::detail::model::model_field_descriptor<::ruvia::fixed_string{#field}, \
        ::ruvia::fixed_string{wire_name}, type, true __VA_OPT__(, ) __VA_ARGS__>

#define RUVIA_OPTIONAL_FIELD(field, type, ...)                                    \
    ::ruvia::detail::model::model_field_descriptor<::ruvia::fixed_string{#field}, \
        ::ruvia::fixed_string{#field}, type, false __VA_OPT__(, ) __VA_ARGS__>

#define RUVIA_OPTIONAL_FIELD_NAME(wire_name, field, type, ...)                    \
    ::ruvia::detail::model::model_field_descriptor<::ruvia::fixed_string{#field}, \
        ::ruvia::fixed_string{wire_name}, type, false __VA_OPT__(, ) __VA_ARGS__>

// Field descriptors are forwarded directly into a C++ variadic template. No
// preprocessor argument counter or FOR_EACH expansion participates in model
// registration, so the framework does not impose a fixed field-count limit.
// Each declaration is a nominal type, including forward-declared recursive
// schemas. Its inline field owner does not know the enclosing model type.

#define RUVIA_MODEL(t_type, ...)                                                                           \
    struct t_type final {                                                                                  \
    private:                                                                                               \
        using storage_type = ::ruvia::detail::model::model_storage<__VA_ARGS__>;                           \
                                                                                                           \
    public:                                                                                                \
        using ruvia_model_schema_type = ::ruvia::detail::model::model_schema<__VA_ARGS__>;                 \
        template <typename model_type = t_type>                                                            \
        t_type() noexcept(!storage_type::has_initial) {                                                    \
            ::ruvia::detail::model::validate_model_schema(typename model_type::ruvia_model_schema_type{}); \
            if constexpr (storage_type::has_initial) {                                                     \
                ::ruvia::detail::model::initialize_model(*this);                                           \
            }                                                                                              \
        }                                                                                                  \
        t_type(const t_type&) = delete;                                                                    \
        t_type& operator=(const t_type&) = delete;                                                         \
        t_type(t_type&&) noexcept = default;                                                               \
        t_type& operator=(t_type&&) = default;                                                             \
        [[maybe_unused]] friend bool operator==(const t_type&, const t_type&) = default;                   \
        template <::ruvia::fixed_string field>                                                             \
        [[nodiscard]] decltype(auto) get() const& RUVIA_LIFETIMEBOUND {                                    \
            return fields_.template get<field>();                                                          \
        }                                                                                                  \
        template <::ruvia::fixed_string field>                                                             \
        [[nodiscard]] decltype(auto) get() const&& = delete;                                               \
        template <::ruvia::fixed_string field>                                                             \
        [[nodiscard]] bool is_present() const noexcept {                                                   \
            return fields_.template is_present<field>();                                                   \
        }                                                                                                  \
        template <::ruvia::fixed_string field>                                                             \
        [[nodiscard]] bool is_null() const noexcept {                                                      \
            return fields_.template is_null<field>();                                                      \
        }                                                                                                  \
        template <::ruvia::fixed_string field>                                                             \
        [[nodiscard]] bool has_value() const noexcept {                                                    \
            return fields_.template has_value<field>();                                                    \
        }                                                                                                  \
        template <::ruvia::fixed_string field, typename value_type>                                        \
        t_type& set(value_type&& value) & {                                                                \
            fields_.template set<field>(std::forward<value_type>(value));                                  \
            return *this;                                                                                  \
        }                                                                                                  \
        template <::ruvia::fixed_string field, typename value_type>                                        \
        t_type& set(value_type&&) && = delete;                                                             \
        template <::ruvia::fixed_string field>                                                             \
            requires requires(storage_type& storage) { storage.template set_null<field>(); }               \
        t_type& set_null() & noexcept {                                                                    \
            fields_.template set_null<field>();                                                            \
            return *this;                                                                                  \
        }                                                                                                  \
        template <::ruvia::fixed_string field>                                                             \
            requires requires(storage_type& storage) { storage.template set_null<field>(); }               \
        t_type& set_null() && = delete;                                                                    \
        template <::ruvia::fixed_string field>                                                             \
            requires requires(storage_type& storage) { storage.template ensure<field>(); }                 \
        [[nodiscard]] decltype(auto) ensure() & RUVIA_LIFETIMEBOUND {                                      \
            return fields_.template ensure<field>();                                                       \
        }                                                                                                  \
        template <::ruvia::fixed_string field>                                                             \
            requires requires(storage_type& storage) { storage.template ensure<field>(); }                 \
        [[nodiscard]] decltype(auto) ensure() && = delete;                                                 \
        template <::ruvia::fixed_string field>                                                             \
            requires requires(storage_type& storage) { storage.template reset<field>(); }                  \
        t_type& reset() & noexcept {                                                                       \
            fields_.template reset<field>();                                                               \
            return *this;                                                                                  \
        }                                                                                                  \
        template <::ruvia::fixed_string field>                                                             \
            requires requires(storage_type& storage) { storage.template reset<field>(); }                  \
        t_type& reset() && = delete;                                                                       \
                                                                                                           \
    private:                                                                                               \
        friend struct ::ruvia::detail::model::model_access;                                                \
        friend struct ::ruvia::detail::model_value_rebind_access;                                          \
        template <typename model_type = t_type>                                                            \
        explicit t_type(::ruvia::detail::model::empty_model_tag) noexcept {                                \
            ::ruvia::detail::model::validate_model_schema(typename model_type::ruvia_model_schema_type{}); \
        }                                                                                                  \
        explicit t_type(storage_type&& fields) noexcept                                                    \
            : fields_(std::move(fields)) {}                                                                \
        storage_type fields_;                                                                              \
    }

namespace ruvia {

// Outcome of looking up one name in a collection that permits repeated names
// (JSON object members, SQL result columns). A repeated name never resolves
// to one of its occurrences.
enum class unique_lookup_status : unsigned char { absent,
    found,
    repeated };

// Result of json_object::get<T>(name) / json_value::get<T>(name). value() is
// engaged only when status() is found and the member converts to (and, for a
// model type, validates as) t_type; found with an empty value means the member
// exists but is not a t_type. The value may borrow the queried token's bytes and
// must not outlive that token. An rvalue lookup yields its value by move, so no
// reference into a temporary lookup is handed out.
template <typename t_type>
class json_field_lookup final {
public:
    [[nodiscard]] unique_lookup_status status() const noexcept {
        return status_;
    }

    [[nodiscard]] std::optional<t_type>& value() & noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }

    [[nodiscard]] const std::optional<t_type>& value() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }

    [[nodiscard]] std::optional<t_type> value() && noexcept(std::is_nothrow_move_constructible_v<t_type>) {
        return std::move(value_);
    }

    std::optional<t_type> value() const&& = delete;

private:
    friend class json_object;
    friend class json_value;

    explicit json_field_lookup(unique_lookup_status status) noexcept
        : status_(status) {}

    explicit json_field_lookup(std::optional<t_type>&& value) noexcept(std::is_nothrow_move_constructible_v<t_type>)
        : status_(unique_lookup_status::found),
          value_(std::move(value)) {}

    unique_lookup_status status_;
    std::optional<t_type> value_;
};

// json_value and json_object borrow their complete input body by default. An
// owning string passed to parse() must therefore outlive the parsed view;
// basic_string rvalues are rejected before a dangling view can be created.
// Model fields may copy the JSON token when the parse owns its strings.
// Dynamic JSON token, not an ordinary field-validation schema. parse() borrows
// the complete token; from_json<Model>() owns it instead.
// Move construction preserves borrowing/ownership. Move assignment owns the
// result (copying borrowed storage). A successful move or rvalue field transfer
// resets the source to its default token without allocation. Failed rebinding
// preserves both tokens; self-move assignment is a no-op. Existing views must
// be refreshed after either changes. Views and potentially borrowed get<T>()
// results must not outlive their token. Default construction represents JSON null.
namespace detail {

template <typename derived_type, fixed_string default_token>
class json_token_base {
public:
    json_token_base(const json_token_base&) = delete;
    json_token_base& operator=(const json_token_base&) = delete;

    json_token_base(json_token_base&& other) noexcept
        : storage_(std::move(other.storage_)) {
        other.storage_.assign_borrowed(default_token.view());
    }

    json_token_base& operator=(json_token_base&& other) {
        if (this != &other) {
            storage_.assign_from(std::move(other.storage_));
            other.storage_.assign_borrowed(default_token.view());
        }
        return *this;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return storage_.view();
    }
    [[nodiscard]] std::string_view view() const&& = delete;

    // The callback receives a field name and json_value borrowing this object's
    // token; both are valid only for the duration of the callback.
    template <typename visitor_type>
    [[nodiscard]] bool for_each_field(visitor_type&& visitor) const;

    // Compare retained token bytes exactly, ignoring JSON normalization, to
    // preserve the zero-cost view semantics.
    [[nodiscard]] friend bool operator==(const derived_type& left, const derived_type& right) noexcept {
        return left.view() == right.view();
    }

protected:
    friend struct model_value_factory;
    friend struct model_value_rebind_access;
    template <typename view_type>
    friend std::optional<view_type> parse_json_view_value(
        std::string_view&, std::size_t, model_string_storage, bool, json_parse_budget&);

    json_token_base(borrowed_model_token_tag, std::string_view body) noexcept
        : storage_(body) {}

    explicit json_token_base(model_text_storage&& storage) noexcept
        : storage_(std::move(storage)) {}

    [[nodiscard]] static derived_type parse_token(
        std::string_view body, model_parse_options options, bool require_object, const char* error_message) {
        if (!is_complete_json_value(body, options, require_object)) {
            throw std::invalid_argument(error_message);
        }
        return derived_type(borrowed_model_token_tag{}, body);
    }

    void assign_owned(std::string_view value) {
        storage_.assign_owned(value);
    }

    [[nodiscard]] derived_type rebind_for_model() const& {
        return derived_type(storage_.rebind());
    }

    [[nodiscard]] derived_type rebind_for_model() && {
        auto rebound = derived_type(std::move(storage_).rebind());
        storage_.assign_borrowed(default_token.view());
        return rebound;
    }

    model_text_storage storage_;
};

}  // namespace detail

class json_value final : public detail::json_token_base<json_value, "null"> {
    using base_type = detail::json_token_base<json_value, "null">;

public:
    enum class kind_type : unsigned char { object,
        array,
        string,
        number,
        boolean,
        null };

    json_value() noexcept
        : base_type(detail::borrowed_model_token_tag{}, "null") {}

    [[nodiscard]] static json_value parse(std::string_view body, model_parse_options options = {}) {
        return parse_token(body, options, false, "invalid JSON value");
    }

    template <typename traits_type, typename allocator_type>
    static json_value parse(
        std::basic_string<char, traits_type, allocator_type>&&, model_parse_options = {}) = delete;

    template <typename traits_type, typename allocator_type>
    static json_value parse(
        const std::basic_string<char, traits_type, allocator_type>&&, model_parse_options = {}) = delete;

    json_value(const json_value&) = delete;
    json_value& operator=(const json_value&) = delete;
    json_value(json_value&&) noexcept = default;
    json_value& operator=(json_value&&) = default;

    [[nodiscard]] kind_type kind() const noexcept {
        auto input = view();
        detail::skip_json_whitespace(input);
        if (input.empty()) {
            return kind_type::null;
        }
        switch (input.front()) {
            case '{':
                return kind_type::object;
            case '[':
                return kind_type::array;
            case '"':
                return kind_type::string;
            case 't':
            case 'f':
                return kind_type::boolean;
            case 'n':
                return kind_type::null;
            default:
                return kind_type::number;
        }
    }

    [[nodiscard]] bool is_object() const noexcept {
        return kind() == kind_type::object;
    }

    [[nodiscard]] bool is_array() const noexcept {
        return kind() == kind_type::array;
    }

    [[nodiscard]] bool is_string() const noexcept {
        return kind() == kind_type::string;
    }

    [[nodiscard]] bool is_number() const noexcept {
        return kind() == kind_type::number;
    }

    [[nodiscard]] bool is_boolean() const noexcept {
        return kind() == kind_type::boolean;
    }

    [[nodiscard]] bool is_null() const noexcept {
        return kind() == kind_type::null;
    }

    // get<T>() results may borrow this token's bytes, so rvalue tokens are
    // rejected rather than left to dangle.
    template <typename t_type>
        requires detail::is_model_field<t_type>
    [[nodiscard]] std::optional<t_type> get() const&;
    template <typename t_type>
        requires detail::is_model_field<t_type>
    std::optional<t_type> get() const&& = delete;

    // Same contract as json_object::get(field); a non-object token has no
    // members, so every name is absent.
    template <typename t_type>
        requires detail::is_model_field<t_type>
    [[nodiscard]] json_field_lookup<t_type> get(std::string_view field) const&;
    template <typename t_type>
        requires detail::is_model_field<t_type>
    json_field_lookup<t_type> get(std::string_view field) const&& = delete;

    // The callback receives a json_value borrowing this object's token; it is
    // valid only for the duration of the callback.
    template <typename visitor_type>
    [[nodiscard]] bool for_each_element(visitor_type&& visitor) const;

private:
    friend class json_object;
    template <typename, fixed_string>
    friend class detail::json_token_base;
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;
    template <typename view_type>
    friend std::optional<view_type> detail::parse_json_view_value(
        std::string_view&, std::size_t, detail::model_string_storage, bool, detail::json_parse_budget&);

    using base_type::base_type;
};

// Object-only dynamic token with the same borrowing, ownership and move
// contract as json_value. Default construction represents an empty JSON object.
class json_object final : public detail::json_token_base<json_object, "{}"> {
    using base_type = detail::json_token_base<json_object, "{}">;

public:
    json_object() noexcept
        : base_type(detail::borrowed_model_token_tag{}, "{}") {}

    [[nodiscard]] static json_object parse(std::string_view body, model_parse_options options = {}) {
        return parse_token(body, options, true, "invalid JSON object");
    }

    template <typename traits_type, typename allocator_type>
    static json_object parse(
        std::basic_string<char, traits_type, allocator_type>&&, model_parse_options = {}) = delete;

    template <typename traits_type, typename allocator_type>
    static json_object parse(
        const std::basic_string<char, traits_type, allocator_type>&&, model_parse_options = {}) = delete;

    json_object(const json_object&) = delete;
    json_object& operator=(const json_object&) = delete;
    json_object(json_object&&) noexcept = default;
    json_object& operator=(json_object&&) = default;

    // Looks up the one member whose decoded name equals `field`: absent when
    // no member has it, repeated when two or more do (no occurrence is chosen,
    // and the scan stops at the second), otherwise found with the converted
    // value. The value may borrow this token's bytes, so rvalue tokens are
    // rejected rather than left to dangle.
    template <typename t_type>
        requires detail::is_model_field<t_type>
    [[nodiscard]] json_field_lookup<t_type> get(std::string_view field) const&;
    template <typename t_type>
        requires detail::is_model_field<t_type>
    json_field_lookup<t_type> get(std::string_view field) const&& = delete;

private:
    friend class json_value;
    template <typename, fixed_string>
    friend class detail::json_token_base;
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;
    template <typename view_type>
    friend std::optional<view_type> detail::parse_json_view_value(
        std::string_view&, std::size_t, detail::model_string_storage, bool, detail::json_parse_budget&);

    using base_type::base_type;
};

template <typename visitor_type>
[[nodiscard]] inline bool json_value::for_each_element(visitor_type&& visitor) const {
    if (!is_array()) {
        return false;
    }
    auto input = view();
    if (!detail::consume_json_array_elements(input, 0, [&visitor](std::string_view& element_input) {
            const auto start = element_input;
            if (!detail::skip_json_value(element_input, 1)) {
                return false;
            }
            json_value value(detail::borrowed_model_token_tag{},
                start.substr(0, start.size() - element_input.size()));
            return static_cast<bool>(visitor(value));
        })) {
        return false;
    }
    detail::skip_json_whitespace(input);
    return input.empty();
}

namespace detail {

template <typename derived_type, fixed_string default_token>
template <typename visitor_type>
[[nodiscard]] inline bool json_token_base<derived_type, default_token>::for_each_field(
    visitor_type&& visitor) const {
    if constexpr (std::is_same_v<derived_type, json_value>) {
        if (!static_cast<const json_value*>(this)->is_object()) {
            return false;
        }
    }
    return visit_json_object_fields(view(), [&](std::string_view key, std::string_view value_view) {
        skip_json_whitespace(value_view);
        json_value value(borrowed_model_token_tag{}, value_view);
        return static_cast<bool>(visitor(key, value));
    }) == json_object_visit_result::complete;
}

}  // namespace detail

}  // namespace ruvia

namespace ruvia::detail {

template <typename view_type>
[[nodiscard]] std::optional<view_type> parse_json_view_value(std::string_view& input,
    std::size_t depth, model_string_storage string_storage, bool require_object, json_parse_budget& budget) {
    auto remaining = input;
    skip_json_whitespace(remaining);
    if (require_object && (remaining.empty() || remaining.front() != '{')) {
        return std::nullopt;
    }
    const auto start = remaining;
    if (!budget.skip_value(remaining, depth)) {
        return std::nullopt;
    }
    const auto token = start.substr(0, start.size() - remaining.size());
    view_type value;
    if (string_storage == model_string_storage::owned) {
        if (!budget.consume_owned_text(token.size())) {
            return std::nullopt;
        }
        value.assign_owned(token);
    } else {
        value.storage_.assign_borrowed(token);
    }
    input = remaining;
    return value;
}

}  // namespace ruvia::detail

namespace ruvia {

// Serializes current values without invoking field rules or initializers.
// The output is always well-formed UTF-8 (RFC 8259 §8.1): a string value holding
// ill-formed UTF-8 has each maximal ill-formed subpart replaced by U+FFFD.
template <typename t_type>
    requires detail::is_model_json_value<t_type>
[[nodiscard]] inline std::string to_json(const t_type& value) {
    std::string output;
    if constexpr (!detail::is_ruvia_scalar<t_type>) {
        // MSVC reserve(n) can leave no room for the trailing NUL, so resize(n)
        // allocates again. One extra byte stays in the same allocation.
        output.reserve(detail::json_size_hint_value(value) + 1);
    }
    detail::append_json_value(output, value);
    return output;
}

}  // namespace ruvia

namespace ruvia::detail {

enum class text_scalar_parse_error {
    invalid_format,
    out_of_range,
};

template <typename t_type>
concept text_scalar = std::is_same_v<t_type, std::remove_cv_t<t_type>> &&
                      (std::is_same_v<t_type, bool> || is_model_integer<t_type> ||
                          std::is_floating_point_v<t_type> || std::is_same_v<t_type, std::string> ||
                          std::is_same_v<t_type, string> || std::is_same_v<t_type, std::string_view>);

// The one text-to-scalar syntax shared by database results and Redis replies:
// boolean tokens 1/t/true/TRUE and 0/f/false/FALSE, strict decimal integers
// (parse_integer), and finite floating-point values. std::string_view borrows
// `text`. A float that is syntactically complete but not representable is
// out_of_range; any other rejection, including a non-finite value, is invalid_format.
template <text_scalar t_type>
[[nodiscard]] std::variant<t_type, text_scalar_parse_error> parse_text_scalar(std::string_view text) {
    using result_type = std::variant<t_type, text_scalar_parse_error>;
    if constexpr (std::is_same_v<t_type, std::string_view>) {
        return result_type(std::in_place_index<0>, text);
    } else if constexpr (std::is_same_v<t_type, std::string>) {
        return result_type(std::in_place_index<0>, text);
    } else if constexpr (std::is_same_v<t_type, string>) {
        return result_type(std::in_place_index<0>, text);
    } else if constexpr (std::is_same_v<t_type, bool>) {
        if (text == "1" || text == "t" || text == "true" || text == "TRUE") {
            return result_type(std::in_place_index<0>, true);
        }
        if (text == "0" || text == "f" || text == "false" || text == "FALSE") {
            return result_type(std::in_place_index<0>, false);
        }
        return text_scalar_parse_error::invalid_format;
    } else if constexpr (std::is_floating_point_v<t_type>) {
        t_type converted{};
        const auto* const last = text.data() + text.size();
        const auto [end, error] = std::from_chars(text.data(), last, converted);
        if (error == std::errc::result_out_of_range && end == last) {
            return text_scalar_parse_error::out_of_range;
        }
        if (error != std::errc{} || end != last || !std::isfinite(converted)) {
            return text_scalar_parse_error::invalid_format;
        }
        return result_type(std::in_place_index<0>, converted);
    } else {
        const auto parsed = parse_integer<t_type>(text);
        if (const auto* error = std::get_if<integer_parse_error>(&parsed)) {
            return *error == integer_parse_error::out_of_range ? text_scalar_parse_error::out_of_range
                                                               : text_scalar_parse_error::invalid_format;
        }
        return result_type(std::in_place_index<0>, std::get<t_type>(parsed));
    }
}

// Shared owning value storage. Backend descriptors and relations stay with
// their backend.
template <typename t_type>
struct is_std_vector : std::false_type {};
template <typename t_type>
struct is_std_vector<std::vector<t_type>> : std::true_type {
    using value_type = t_type;
};

template <typename value_type>
void commit_entity_value(value_type& target, value_type&& owned) noexcept {
    if constexpr (std::is_same_v<value_type, std::string> || is_std_vector<value_type>::value) {
        target.swap(owned);
    } else {
        static_assert(std::is_nothrow_move_constructible_v<value_type>);
        target.~value_type();
        std::construct_at(&target, std::move(owned));
    }
}
template <typename t_type>
struct entity_value_slot {
    enum class state_type : unsigned char { unset,
        null,
        value };
    entity_value_slot() = default;
    static t_type make_value() {
        return t_type{};
    }
    void clear() {
        auto empty = make_value();
        commit_entity_value(value_, std::move(empty));
    }
    state_type state_{state_type::unset};
    t_type value_{};
};

// Position of the descriptor named `name` in a descriptor tuple (entity
// columns or relations), or the tuple size when it is absent.
template <typename tuple_type, fixed_string name>
consteval std::size_t descriptor_index() {
    return []<std::size_t... i>(std::index_sequence<i...>) consteval {
        const std::array<std::string_view, sizeof...(i)> names{std::tuple_element_t<i, tuple_type>::name.view()...};
        return fixed_name_index(names, name.view());
    }(std::make_index_sequence<std::tuple_size_v<tuple_type>>{});
}
template <typename... column_types>
consteval bool unique_entity_columns() {
    const std::array<std::string_view, sizeof...(column_types)> names{column_types::name.view()...};
    return fixed_names_distinct(names);
}

// Visits every element of a descriptor tuple (entity columns, relations, join
// columns) in declaration order as function.template operator()<element, index>().
template <typename tuple_type, typename function_type>
constexpr void for_each_descriptor(function_type&& function) {
    [&]<std::size_t... i>(std::index_sequence<i...>) {
        (function.template operator()<std::tuple_element_t<i, tuple_type>, i>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<tuple_type>>{});
}

template <typename t_type>
struct is_optional : std::false_type {};
template <typename t_type>
struct is_optional<std::optional<t_type>> : std::true_type {
    using value_type = t_type;
};

template <typename range_type, typename element_type>
consteval bool is_exact_element_range() noexcept {
    using decayed_type = std::remove_cvref_t<range_type>;
    if constexpr (requires { typename decayed_type::value_type; }) {
        return std::is_same_v<typename decayed_type::value_type, element_type>;
    } else {
        return std::is_same_v<
            std::remove_cvref_t<decltype(*std::declval<range_type&>().begin())>, element_type>;
    }
}

template <typename t_type, typename v_type>
void assign_entity_value(t_type& out, v_type&& value) {
    constexpr bool movable_rvalue =
        !std::is_lvalue_reference_v<v_type> && !std::is_const_v<std::remove_reference_t<v_type>>;
    if constexpr (std::is_arithmetic_v<t_type> && is_ruvia_scalar<v_type>) {
        assign_entity_value(out, value.value_);
    } else if constexpr (is_model_integer<t_type> && std::is_arithmetic_v<std::remove_cvref_t<v_type>>) {
        // 整数输入先检查范围；浮点输入须由调用方显式选择整数转换策略。
        out = integer_value<t_type>(value).value_;
    } else if constexpr (std::is_floating_point_v<t_type> &&
                         std::is_arithmetic_v<std::remove_cvref_t<v_type>>) {
        if constexpr (std::is_floating_point_v<std::remove_cvref_t<v_type>>) {
            out = checked_model_float<t_type>(value);
        } else {
            out = static_cast<t_type>(value);
        }
    } else if constexpr (std::is_same_v<t_type, string>) {
        if constexpr (movable_rvalue && std::is_same_v<std::remove_cvref_t<v_type>, string>) {
            commit_entity_value(out, rebind_model_value(std::move(value)));
        } else {
            auto owned = string(std::string_view(value));
            commit_entity_value(out, std::move(owned));
        }
    } else if constexpr (std::is_same_v<t_type, std::string>) {
        if constexpr (movable_rvalue && std::is_same_v<std::remove_cvref_t<v_type>, std::string>) {
            std::string owned(std::move(value));
            commit_entity_value(out, std::move(owned));
            return;
        }
        std::string owned{std::string_view(value)};
        commit_entity_value(out, std::move(owned));
    } else if constexpr (is_optional<t_type>::value) {
        using element_type = typename is_optional<t_type>::value_type;
        if constexpr (is_optional<std::remove_cvref_t<v_type>>::value) {
            if (!value) {
                out.reset();
            } else {
                auto owned = entity_value_slot<element_type>::make_value();
                if constexpr (movable_rvalue) {
                    assign_entity_value(owned, std::move(*value));
                } else {
                    assign_entity_value(owned, *value);
                }
                out.emplace(std::move(owned));
            }
        } else {
            auto owned = entity_value_slot<element_type>::make_value();
            assign_entity_value(owned, std::forward<v_type>(value));
            out.emplace(std::move(owned));
        }
    } else if constexpr (is_std_vector<t_type>::value) {
        using element_type = typename is_std_vector<t_type>::value_type;
        if constexpr (movable_rvalue && std::is_same_v<std::remove_cvref_t<v_type>, t_type>) {
            t_type owned(std::move(value));
            commit_entity_value(out, std::move(owned));
            return;
        }
        if constexpr (std::is_arithmetic_v<element_type> &&
                      is_exact_element_range<v_type, element_type>()) {
            t_type owned(value.begin(), value.end());
            commit_entity_value(out, std::move(owned));
        } else if constexpr (std::is_same_v<element_type, std::string>) {
            auto owned = entity_value_slot<t_type>::make_value();
            owned.reserve(value.size());
            if constexpr (movable_rvalue &&
                          std::is_same_v<std::remove_cvref_t<decltype(*value.begin())>, std::string>) {
                for (auto& item : value) {
                    owned.emplace_back(std::move(item));
                }
                commit_entity_value(out, std::move(owned));
                return;
            }
            for (const auto& item : value) {
                owned.emplace_back(std::string_view(item));
            }
            commit_entity_value(out, std::move(owned));
        } else {
            auto owned = entity_value_slot<t_type>::make_value();
            owned.reserve(value.size());
            for (auto& item : value) {
                auto element = entity_value_slot<element_type>::make_value();
                if constexpr (movable_rvalue) {
                    assign_entity_value(element, std::move(item));
                } else {
                    assign_entity_value(element, item);
                }
                owned.push_back(std::move(element));
            }
            commit_entity_value(out, std::move(owned));
        }
    } else {
        if constexpr (std::is_nothrow_assignable_v<t_type&, v_type&&>) {
            out = std::forward<v_type>(value);
        } else {
            auto owned = entity_value_slot<t_type>::make_value();
            owned = std::forward<v_type>(value);
            commit_entity_value(out, std::move(owned));
        }
    }
}

template <typename... columns>
class entity_value_storage final {
    static_assert(unique_entity_columns<columns...>(), "duplicate entity column name");
    using slots_type = std::tuple<entity_value_slot<typename columns::value_type>...>;
    template <fixed_string name>
    auto& slot() {
        return std::get<column_index<name>()>(slots_);
    }
    template <fixed_string name>
    const auto& slot() const {
        return std::get<column_index<name>()>(slots_);
    }

public:
    using columns_type = std::tuple<columns...>;
    template <fixed_string name>
    static consteval std::size_t column_index() {
        constexpr auto result_value = descriptor_index<columns_type, name>();
        static_assert(result_value < sizeof...(columns), "unknown entity column");
        return result_value;
    }
    template <fixed_string name>
    static consteval std::string_view column_name() noexcept {
        return std::tuple_element_t<column_index<name>(), columns_type>::name.view();
    }
    entity_value_storage() = default;
    entity_value_storage(const entity_value_storage&) = delete;
    entity_value_storage& operator=(const entity_value_storage&) = delete;
    entity_value_storage(entity_value_storage&&) noexcept = default;
    entity_value_storage& operator=(entity_value_storage&&) = delete;
    template <fixed_string name>
    auto& get() & RUVIA_LIFETIMEBOUND {
        auto& value = slot<name>();
        if (value.state_ != decltype(value.state_)::value) {
            throw std::logic_error("entity value is not set");
        }
        return value.value_;
    }
    template <fixed_string name>
    auto& get() && = delete;
    template <fixed_string name>
    const auto& get() const& RUVIA_LIFETIMEBOUND {
        return const_cast<entity_value_storage*>(this)->template get<name>();
    }
    template <fixed_string name>
    const auto& get() const&& = delete;
    template <fixed_string name, typename value_type>
    void set(value_type&& value) {
        auto& target = slot<name>();
        assign_entity_value(target.value_, std::forward<value_type>(value));
        target.state_ = decltype(target.state_)::value;
    }
    template <fixed_string name>
    void set_null()
        requires(std::tuple_element_t<column_index<name>(), columns_type>::options.nullable_)
    {
        auto& value = slot<name>();
        value.clear();
        value.state_ = decltype(value.state_)::null;
    }
    template <fixed_string name>
    void reset() {
        auto& value = slot<name>();
        value.clear();
        value.state_ = decltype(value.state_)::unset;
    }
    template <fixed_string name>
    bool is_set() const {
        const auto& value = slot<name>();
        return value.state_ != decltype(value.state_)::unset;
    }
    template <fixed_string name>
    bool is_null() const {
        const auto& value = slot<name>();
        return value.state_ == decltype(value.state_)::null;
    }
    slots_type slots_;
};

template <typename tuple>
struct entity_storage_from_tuple;
template <typename... columns>
struct entity_storage_from_tuple<std::tuple<columns...>> {
    using type = entity_value_storage<columns...>;
};

}  // namespace ruvia::detail

// Construction, row rebinding and column lookup shared by every entity owner
// macro (SQL entity, SQL projection, Redis entity). Leaves access public.
#define RUVIA_DETAIL_ENTITY_OWNER_API(owner, storage_type, member) \
private:                                                           \
    template <typename>                                            \
    friend class ::ruvia::entity_rows;                             \
    explicit owner(storage_type&& storage) noexcept                \
        : member(std::move(storage)) {}                            \
                                                                   \
public:                                                            \
    owner() = default;                                             \
    template <::ruvia::fixed_string field_name>                    \
    static consteval std::size_t column_index() {                  \
        return storage_type::template column_index<field_name>();  \
    }                                                              \
    template <::ruvia::fixed_string field_name>                    \
    static consteval std::string_view column_name() noexcept {     \
        return storage_type::template column_name<field_name>();   \
    }

// Public value facades only forward; state transitions belong to the owner.
#define RUVIA_DETAIL_ENTITY_VALUE_API(member)                       \
    template <::ruvia::fixed_string name>                           \
    auto& get() & RUVIA_LIFETIMEBOUND {                             \
        return member.template get<name>();                         \
    }                                                               \
    template <::ruvia::fixed_string name>                           \
    auto& get() && = delete;                                        \
    template <::ruvia::fixed_string name>                           \
    const auto& get() const& RUVIA_LIFETIMEBOUND {                  \
        return member.template get<name>();                         \
    }                                                               \
    template <::ruvia::fixed_string name>                           \
    const auto& get() const&& = delete;                             \
    template <::ruvia::fixed_string name, typename value_type>      \
    auto& set(value_type&& value) & RUVIA_LIFETIMEBOUND {           \
        member.template set<name>(std::forward<value_type>(value)); \
        return *this;                                               \
    }                                                               \
    template <::ruvia::fixed_string name, typename value_type>      \
    auto& set(value_type&&) && = delete;                            \
    template <::ruvia::fixed_string name>                           \
    auto& set_null() & RUVIA_LIFETIMEBOUND                          \
        requires requires { member.template set_null<name>(); }     \
    {                                                               \
        member.template set_null<name>();                           \
        return *this;                                               \
    }                                                               \
    template <::ruvia::fixed_string name>                           \
    auto& set_null() &&                                             \
        requires requires { member.template set_null<name>(); }     \
    = delete;                                                       \
    template <::ruvia::fixed_string name>                           \
    auto& reset() & RUVIA_LIFETIMEBOUND {                           \
        member.template reset<name>();                              \
        return *this;                                               \
    }                                                               \
    template <::ruvia::fixed_string name>                           \
    auto& reset() && = delete;                                      \
    template <::ruvia::fixed_string name>                           \
    bool is_set() const {                                           \
        return member.template is_set<name>();                      \
    }                                                               \
    template <::ruvia::fixed_string name>                           \
    bool is_null() const {                                          \
        return member.template is_null<name>();                     \
    }

namespace ruvia {
// Non-negative row count or offset in SQL and RediSearch 64-bit signed range.
// Checked before narrowing so negative integers cannot wrap into huge counts.
class query_row_count final {
public:
    constexpr query_row_count() noexcept = default;

    template <std::integral integer_type>
    constexpr query_row_count(integer_type value)
        : value_(is_valid_value(value)
                     ? static_cast<std::uint64_t>(value)
                     : throw std::invalid_argument("query row count must be in 0..9223372036854775807")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::cmp_greater_equal(+value, 0) && std::in_range<std::int64_t>(+value);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<query_row_count> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return query_row_count(static_cast<std::uint64_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(query_row_count, query_row_count) = default;

private:
    struct validated_tag_type {};

    constexpr query_row_count(std::uint64_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint64_t value_{0};
};

static_assert(std::is_trivially_copyable_v<query_row_count>);
static_assert(sizeof(query_row_count) == sizeof(std::uint64_t));

// A move-only entity collection.
template <typename entity_type>
class entity_rows final {
public:
    using value_type = entity_type;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = entity_type&;
    using const_reference = const entity_type&;
    using pointer = entity_type*;
    using const_pointer = const entity_type*;
    using iterator = entity_type*;
    using const_iterator = const entity_type*;

    entity_rows() noexcept = default;
    entity_rows(const entity_rows&) = delete;
    entity_rows& operator=(const entity_rows&) = delete;
    entity_rows(entity_rows&&) noexcept = default;
    entity_rows& operator=(entity_rows&& other) noexcept {
        if (this != &other) {
            rows_ = std::move(other.rows_);
            other.rows_.clear();
        }
        return *this;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return rows_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return rows_.empty();
    }
    [[nodiscard]] const entity_type& operator[](std::size_t i) const& noexcept RUVIA_LIFETIMEBOUND {
        return rows_[i];
    }
    [[nodiscard]] entity_type& operator[](std::size_t i) & noexcept RUVIA_LIFETIMEBOUND {
        return rows_[i];
    }
    const entity_type& operator[](std::size_t) const&& = delete;
    [[nodiscard]] const entity_type& front() const& noexcept RUVIA_LIFETIMEBOUND {
        return rows_.front();
    }
    [[nodiscard]] entity_type& front() & noexcept RUVIA_LIFETIMEBOUND {
        return rows_.front();
    }
    const entity_type& front() const&& = delete;
    [[nodiscard]] const entity_type& back() const& noexcept RUVIA_LIFETIMEBOUND {
        return rows_.back();
    }
    [[nodiscard]] entity_type& back() & noexcept RUVIA_LIFETIMEBOUND {
        return rows_.back();
    }
    const entity_type& back() const&& = delete;
    [[nodiscard]] entity_type* data() & noexcept RUVIA_LIFETIMEBOUND {
        return rows_.data();
    }
    [[nodiscard]] const entity_type* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return rows_.data();
    }
    const entity_type* data() const&& = delete;
    [[nodiscard]] entity_type* begin() & noexcept RUVIA_LIFETIMEBOUND {
        return rows_.data();
    }
    [[nodiscard]] const entity_type* begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return rows_.data();
    }
    [[nodiscard]] entity_type* end() & noexcept RUVIA_LIFETIMEBOUND {
        return rows_.data() + rows_.size();
    }
    [[nodiscard]] const entity_type* end() const& noexcept RUVIA_LIFETIMEBOUND {
        return rows_.data() + rows_.size();
    }
    const entity_type* begin() const&& = delete;
    const entity_type* end() const&& = delete;
    void reserve(std::size_t count) {
        rows_.reserve(count);
    }
    void push_back(entity_type&& entity) {
        rows_.push_back(std::move(entity));
    }

private:
    std::vector<entity_type> rows_;
};

enum class validation_target : std::uint8_t { json,
    form,
    query,
    param,
    header,
    cookie };
namespace detail {

template <typename t_type>
[[nodiscard]] std::string_view validation_string_view(const t_type& value) noexcept {
    if constexpr (requires { value.view(); }) {
        return value.view();
    } else {
        return std::string_view(value);
    }
}

}  // namespace detail

// How validator::throw_if_invalid presents its issues: the http_error it throws
// carries these values together with the collected validation issues.
struct validation_failure_options final {
    http_status_code status_{http_status::bad_request};
    borrowed_text code_{"validation_failed"};
    borrowed_text message_{"request validation failed"};
};

// Allocation failure during copy assignment leaves both validators unchanged.
// Successful moves leave an empty source that can be reused.
class validator final {
public:
    using issue_list_type = std::vector<validation_issue>;

    validator() noexcept = default;

    validator(const validator&) = default;

    validator& operator=(const validator& other) {
        if (this != &other) {
            auto copied = other.issues_;
            issues_.swap(copied);
        }
        return *this;
    }

    validator(validator&& other) noexcept
        : issues_(std::move(other.issues_)) {
        other.issues_.clear();
    }

    validator& operator=(validator&& other) noexcept {
        if (this != &other) {
            issues_ = std::move(other.issues_);
            other.issues_.clear();
        }
        return *this;
    }

    ~validator() = default;

    validator& add(std::string_view field, std::string_view code, std::string_view message) & RUVIA_LIFETIMEBOUND {
        issues_.push_back(validation_issue({.field_ = field, .code_ = code, .message_ = message}));
        return *this;
    }
    validator& add(std::string_view, std::string_view, std::string_view) && = delete;

    template <typename optional_type>
    validator& required(const optional_type& value, std::string_view field,
        std::string_view message = "is required") & RUVIA_LIFETIMEBOUND {
        if (!value) {
            add(field, "required", message);
        }
        return *this;
    }
    template <typename optional_type>
    validator& required(const optional_type&, std::string_view, std::string_view = "is required") && = delete;

    template <typename optional_type>
    validator& min_length(const optional_type& value, std::string_view field, std::size_t min_value,
        std::string_view message = "is too short") & RUVIA_LIFETIMEBOUND {
        if (value && detail::validation_string_view(*value).size() < min_value) {
            add(field, "too_small", message);
        }
        return *this;
    }
    template <typename optional_type>
    validator& min_length(
        const optional_type&, std::string_view, std::size_t, std::string_view = "is too short") && = delete;

    template <typename optional_type>
    validator& max_length(const optional_type& value, std::string_view field, std::size_t max_value,
        std::string_view message = "is too long") & RUVIA_LIFETIMEBOUND {
        if (value && detail::validation_string_view(*value).size() > max_value) {
            add(field, "too_big", message);
        }
        return *this;
    }
    template <typename optional_type>
    validator& max_length(
        const optional_type&, std::string_view, std::size_t, std::string_view = "is too long") && = delete;

    template <typename optional_type, typename min_type, typename max_type>
    validator& range(const optional_type& value, std::string_view field, min_type min_value, max_type max_value,
        std::string_view message = "is out of range") & RUVIA_LIFETIMEBOUND {
        if (value) {
            if (detail::validation_less(*value, min_value)) {
                add(field, "too_small", message);
            } else if (detail::validation_greater(*value, max_value)) {
                add(field, "too_big", message);
            }
        }
        return *this;
    }
    template <typename optional_type, typename min_type, typename max_type>
    validator& range(const optional_type&, std::string_view, min_type, max_type,
        std::string_view = "is out of range") && = delete;

    template <typename optional_type>
    validator& one_of(const optional_type& value, std::string_view field,
        std::initializer_list<std::string_view> allowed,
        std::string_view message = "is not allowed") & RUVIA_LIFETIMEBOUND {
        if (!value) {
            return *this;
        }

        const auto actual = detail::validation_string_view(*value);
        for (const auto option : allowed) {
            if (actual == option) {
                return *this;
            }
        }

        add(field, "one_of", message);
        return *this;
    }
    template <typename optional_type>
    validator& one_of(const optional_type&, std::string_view, std::initializer_list<std::string_view>,
        std::string_view = "is not allowed") && = delete;

    [[nodiscard]] bool ok() const noexcept {
        return issues_.empty();
    }

    [[nodiscard]] bool valid() const noexcept {
        return ok();
    }

    [[nodiscard]] const issue_list_type& issues() const& noexcept RUVIA_LIFETIMEBOUND {
        return issues_;
    }
    [[nodiscard]] const issue_list_type& issues() const&& = delete;

    void throw_if_invalid(validation_failure_options options = {}) const& {
        if (!ok()) {
            throw http_error({.status_ = options.status_,
                .code_ = options.code_,
                .message_ = options.message_,
                .validation_issues_ = issues_});
        }
    }

    void throw_if_invalid(validation_failure_options options = {}) && {
        if (!ok()) {
            auto moved_issues = std::move(issues_);
            issues_.clear();
            throw http_error(
                options.status_, options.code_.view(), options.message_.view(), {}, std::move(moved_issues));
        }
    }

private:
    issue_list_type issues_;
};

template <typename model_type>
    requires detail::is_model<model_type>
void validate_model(const model_type& model, validator& out) {
    detail::model_validation_access::validate_model(model, out);
}

template <typename t_type>
    requires detail::is_model<t_type>
[[nodiscard]] t_type from_form(std::string_view body) {
    auto parsed_value = detail::model_parse_access::parse_form_owned<t_type>(body);
    if (!parsed_value) {
        throw std::invalid_argument("invalid form value");
    }
    validator validator_value;
    validate_model(*parsed_value, validator_value);
    std::move(validator_value).throw_if_invalid();
    return std::move(*parsed_value);
}

// Supports models, Ruvia scalar values, strings, bytes and arrays of these
// types. Model types also evaluate their declared field validation rules.
template <typename t_type>
    requires detail::is_model_json_value<t_type>
[[nodiscard]] t_type from_json(std::string_view body, model_parse_options options = {}) {
    auto parsed_value = detail::parse_json_document<t_type>(
        body, detail::model_string_storage::owned, options);
    if (!parsed_value) {
        throw std::invalid_argument("invalid JSON value");
    }
    if constexpr (detail::model_validation_access::contains_model<t_type>()) {
        validator validator_value;
        detail::model_validation_access::validate_value(*parsed_value, validator_value);
        std::move(validator_value).throw_if_invalid();
    }
    return std::move(*parsed_value);
}

template <typename t_type>
    requires detail::is_model_field<t_type>
[[nodiscard]] inline std::optional<t_type> json_value::get() const& {
    auto parsed_value = detail::parse_json_document<t_type>(view(), detail::model_string_storage::borrowed);
    if (!parsed_value) {
        return std::nullopt;
    }
    if constexpr (detail::model_validation_access::contains_model<t_type>()) {
        if (!detail::model_validation_access::value_rules_valid(*parsed_value)) {
            return std::nullopt;
        }
    }
    return parsed_value;
}

// A repeated name makes the member ambiguous (RFC 8259 section 4 leaves its
// meaning unpredictable), so the lookup reports it instead of choosing an
// occurrence, as typed model parsing rejects it: the scan stops at the second
// occurrence and neither value is parsed. A unique match is converted by
// json_value::get<T>(), the single parse-and-validate path for dynamic tokens.
template <typename t_type>
    requires detail::is_model_field<t_type>
[[nodiscard]] inline json_field_lookup<t_type> json_object::get(std::string_view field) const& {
    std::optional<std::string_view> match;
    const auto visited = detail::visit_json_object_fields(view(),
        [&match, field](std::string_view key, std::string_view value_view) {
            if (key != field) {
                return true;
            }
            if (match) {
                return false;
            }
            match = value_view;
            return true;
        });
    switch (visited) {
        case detail::json_object_visit_result::stopped:
            return json_field_lookup<t_type>(unique_lookup_status::repeated);
        case detail::json_object_visit_result::invalid:
            // Every construction path validates the complete object.
            throw std::logic_error("json_object token is not a complete JSON object");
        case detail::json_object_visit_result::complete:
            break;
    }
    if (!match) {
        return json_field_lookup<t_type>(unique_lookup_status::absent);
    }
    detail::skip_json_whitespace(*match);
    const json_value value(detail::borrowed_model_token_tag{}, *match);
    return json_field_lookup<t_type>(value.get<t_type>());
}

template <typename t_type>
    requires detail::is_model_field<t_type>
[[nodiscard]] inline json_field_lookup<t_type> json_value::get(std::string_view field) const& {
    if (!is_object()) {
        return json_field_lookup<t_type>(unique_lookup_status::absent);
    }
    // Borrows this token's bytes, so results stay valid as long as *this.
    const json_object object(detail::borrowed_model_token_tag{}, view());
    return object.get<t_type>(field);
}
}  // namespace ruvia

// RUVIA_MIN / RUVIA_MAX constrain a field's magnitude. For a number field this
// is its value; for a string field it is the UTF-8 BYTE length, not the
// codepoint count -- a three-emoji string is 12 bytes -- so choose bounds with
// multibyte input in mind (for example a minimum-length rule on free text).
#define RUVIA_MIN(value, message) \
    ::ruvia::detail::model::min<value, ::ruvia::fixed_string{message}>
#define RUVIA_MAX(value, message) \
    ::ruvia::detail::model::max<value, ::ruvia::fixed_string{message}>
#define RUVIA_ONE_OF(message, ...) \
    ::ruvia::detail::model::one_of<::ruvia::fixed_string{message}, __VA_ARGS__>
#define RUVIA_EMAIL(message) ::ruvia::detail::model::email<::ruvia::fixed_string{message}>
// Pattern rules use anchored (^...$) byte patterns: literals, '.', positive
// or negative character classes, literal ranges, \d/\w/\s, and '*', '+', '?'.
// Escaped literals may be range endpoints; descending ranges and range endpoints
// such as \d are rejected at compile time. A trailing class '-' is a literal.
// Patterns are limited to 256 bytes and bounded matching work. Exceeding a
// match limit fails validation. Use RUVIA_CUSTOM for matching syntax outside
// this dialect.
#define RUVIA_PATTERN(pattern, message) \
    ::ruvia::detail::model::pattern_rule<pattern, ::ruvia::fixed_string{message}>
#define RUVIA_CUSTOM(predicate, message) \
    ::ruvia::detail::model::custom<predicate, ::ruvia::fixed_string{message}>

namespace ruvia {

namespace detail {

[[noreturn]] void throw_invalid_json_body();
[[noreturn]] void throw_invalid_json_content_type();
[[noreturn]] void throw_invalid_form_content_type();

template <validation_target target>
    requires(target == validation_target::json || target == validation_target::form)
inline constexpr std::string_view request_body_media_type =
    target == validation_target::json ? "application/json" : "application/x-www-form-urlencoded";

// Media-type parameters, including charset, do not select the request model.
template <validation_target target>
    requires(target == validation_target::json || target == validation_target::form)
[[nodiscard]] bool request_has_body_media_type(std::optional<std::string_view> content_type) {
    return http_ascii_equals_ignore_case(
        http_media_type_only(content_type.value_or(std::string_view{})), request_body_media_type<target>);
}

template <validation_target target>
    requires(target == validation_target::json || target == validation_target::form)
[[noreturn]] inline void throw_invalid_content_type() {
    if constexpr (target == validation_target::json) {
        throw_invalid_json_content_type();
    } else {
        throw_invalid_form_content_type();
    }
}

template <validation_target target, typename model_type>
[[nodiscard]] model_type parse_and_validate_request_model(
    std::conditional_t<target == validation_target::json || target == validation_target::form,
        std::string_view, std::reference_wrapper<const request_name_value_list>>
        input) {
    auto model = [&] {
        if constexpr (target == validation_target::json) {
            auto parsed_value = parse_complete_json_value<model_type>(input, model_string_storage::borrowed);
            if (!parsed_value) {
                // Selecting JSON makes malformed input an error, not an optional
                // format mismatch that json_if<T>() may silently skip.
                throw_invalid_json_body();
            }
            return std::move(*parsed_value);
        } else if constexpr (target == validation_target::form) {
            return model_parse_access::parse_form<model_type>(form_body_input{.body_ = input});
        } else {
            return model_parse_access::parse_form<model_type>(input);
        }
    }();
    validator validator_value;
    model_validation_access::validate_model(model, validator_value);
    std::move(validator_value).throw_if_invalid();
    return model;
}

// One validating middleware per request source: parses the model from `target`,
// runs its declared rules, binds it for the handler, then continues the chain.
template <validation_target target, typename body_type>
class model_validation_middleware final {
public:
    static_assert(is_model<body_type>, "request validation middleware requires a RUVIA_MODEL");
    static_assert(target != validation_target::header ||
                      model::unique_model_wire_names_ignore_ascii_case(model::model_access::schema<body_type>()),
        "header_model field names must be unique ignoring ASCII case");
    using ruvia_validation_body_type = body_type;

    [[nodiscard]] task<void> invoke(context& c, next& next_value);
};

}  // namespace detail

template <typename body_type>
using json_body = detail::model_validation_middleware<validation_target::json, body_type>;

template <typename body_type>
using form_body = detail::model_validation_middleware<validation_target::form, body_type>;

template <typename body_type>
using query_model = detail::model_validation_middleware<validation_target::query, body_type>;

template <typename body_type>
using path_model = detail::model_validation_middleware<validation_target::param, body_type>;

template <typename body_type>
using header_model = detail::model_validation_middleware<validation_target::header, body_type>;

template <typename body_type>
using cookie_model = detail::model_validation_middleware<validation_target::cookie, body_type>;
}  // namespace ruvia
