#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
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

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_field_syntax.h"

namespace ruvia {

// Protocol version is HTTP message control data, not a borrowed wire token.
// HTTP/1 carries it in the start-line; HTTP/2 and HTTP/3 establish it from
// connection framing even though no textual version appears in a field block.
enum class http_protocol_version : std::uint8_t {
    http10,
    http11,
    http2,
    http3,
};

}  // namespace ruvia

namespace ruvia {
enum class http_scheme : std::uint8_t {
    http,
    https,
};

enum class http_known_method : std::uint8_t {
    get,
    post,
    put,
    delete_value,
    patch,
    head,
    options,
    connect,
    unknown
};

[[nodiscard]] constexpr std::uint32_t http_known_method_bit(http_known_method method) noexcept {
    return method == http_known_method::unknown
               ? 0U
               : (std::uint32_t{1} << static_cast<std::uint32_t>(method));
}

// HTTP methods are an extensible, case-sensitive token space. http_known_method is
// only the framework's fixed semantic classification; it is never the wire value.
[[nodiscard]] http_known_method classify_http_method(std::string_view method) noexcept;
[[nodiscard]] std::string_view known_http_method_token(http_known_method method) noexcept;
[[nodiscard]] bool is_valid_http_method_token(std::string_view method) noexcept;

// RFC 9110 method properties over the exact, case-sensitive wire token. Unknown
// extension methods are conservatively neither safe nor idempotent; TRACE is
// recognized even though it is intentionally not a routable fixed enum member.
[[nodiscard]] bool is_http_method_safe(std::string_view method) noexcept;
[[nodiscard]] bool is_http_method_idempotent(std::string_view method) noexcept;

}  // namespace ruvia

namespace ruvia {

// An owned field returned by protocol parsers and events. The same value type
// represents initial fields and trailers for requests and responses.
class http_header final {
public:
    http_header(std::string_view name, std::string_view value)
        : name_size_(name.size()),
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

    http_header(const http_header& other)
        : bytes_(!other.owned_ ? other.bytes_ : nullptr),
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

    http_header(http_header&& other) noexcept
        : bytes_(std::exchange(other.bytes_, nullptr)),
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
    [[nodiscard]] static http_header copy_with_lowercase_name(
        std::string_view name, std::string_view value) {
        auto result = http_header(name, value);
        for (std::size_t index = 0; index < result.name_size_; ++index) {
            result.bytes_[index] =
                static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(result.bytes_[index])));
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

    constexpr http_header(
        const char* static_bytes, std::size_t name_size, std::size_t value_size) noexcept
        : bytes_(const_cast<char*>(static_bytes)),
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

    constexpr http_header_view(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    template <typename name_type, typename value_type>
        requires(detail::http_temporary_owning_char_string<name_type> ||
                    detail::http_temporary_owning_char_string<value_type>)
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
concept http_header_field_range =
    std::ranges::contiguous_range<const std::remove_cvref_t<range_type>> &&
    http_header_field<std::ranges::range_reference_t<const std::remove_cvref_t<range_type>>>;

namespace detail {
class http_header_range_view;
}  // namespace detail

template <typename source_type>
concept http_header_field_source =
    std::same_as<std::remove_cvref_t<source_type>, detail::http_header_range_view> ||
    http_header_field_range<source_type> ||
    requires(const std::remove_cvref_t<source_type>& source) {
        { source.headers() } -> http_header_field_range;
    };
namespace detail {

class http_header_range_view final {
public:
    using accessor_type = http_header_view (*)(const void*, std::size_t) noexcept;

    constexpr http_header_range_view() noexcept = default;

    template <http_header_field_source source_type>
        requires(!std::same_as<std::remove_cvref_t<source_type>, http_header_range_view>)
    constexpr http_header_range_view(const source_type& source) noexcept {
        if constexpr (http_header_field_range<source_type>) {
            using value_type = std::remove_cv_t<std::ranges::range_value_t<const source_type>>;
            data_ = std::ranges::data(source);
            size_ = std::ranges::size(source);
            at_ = +[](const void* base, std::size_t index) noexcept -> http_header_view {
                const auto& field = static_cast<const value_type*>(base)[index];
                return {field.name(), field.value()};
            };
        } else {
            *this = http_header_range_view(source.headers());
        }
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] constexpr http_header_view operator[](std::size_t index) const noexcept {
        return at_(data_, index);
    }

    class iterator final {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using value_type = http_header_view;
        using difference_type = std::ptrdiff_t;

        constexpr iterator() noexcept = default;
        constexpr iterator(const void* data, accessor_type at, std::size_t index) noexcept
            : data_(data),
              at_(at),
              index_(index) {}

        [[nodiscard]] constexpr http_header_view operator*() const noexcept {
            return at_(data_, index_);
        }
        constexpr iterator& operator++() noexcept {
            ++index_;
            return *this;
        }
        constexpr iterator operator++(int) noexcept {
            auto copy = *this;
            ++index_;
            return copy;
        }
        [[nodiscard]] friend constexpr bool operator==(
            const iterator& left, const iterator& right) noexcept {
            return left.data_ == right.data_ && left.index_ == right.index_;
        }

    private:
        const void* data_{nullptr};
        accessor_type at_{nullptr};
        std::size_t index_{0};
    };

    [[nodiscard]] constexpr iterator begin() const noexcept {
        return {data_, at_, 0};
    }

    [[nodiscard]] constexpr iterator end() const noexcept {
        return {data_, at_, size_};
    }

private:
    const void* data_{nullptr};
    std::size_t size_{0};
    accessor_type at_{nullptr};
};
}  // namespace detail

}  // namespace ruvia

namespace ruvia {

// RFC 9218 section 4.1: urgency is 0..7, defaulting to 3. Local numeric
// inputs are checked before narrowing. Peer fields instead use the tolerant
// parser below, which ignores out-of-range values as required by section 4.
class http_priority_urgency final {
public:
    constexpr http_priority_urgency() noexcept = default;

    template <std::integral integer_type>
    constexpr http_priority_urgency(integer_type value)
        : value_(is_valid_value(value)
                     ? static_cast<std::uint8_t>(value)
                     : throw std::invalid_argument("HTTP priority urgency must be between 0 and 7")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::cmp_greater_equal(+value, 0) && std::cmp_less_equal(+value, 7);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<http_priority_urgency> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return http_priority_urgency(static_cast<std::uint8_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint8_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(http_priority_urgency, http_priority_urgency) = default;

private:
    struct validated_tag_type {};

    constexpr http_priority_urgency(std::uint8_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint8_t value_{3};
};

static_assert(std::is_trivially_copyable_v<http_priority_urgency>);
static_assert(sizeof(http_priority_urgency) == sizeof(std::uint8_t));

struct http_priority final {
    http_priority_urgency urgency_{};
    bool incremental_{false};
};
struct http_priority_fields final {
    std::optional<http_priority_urgency> urgency_{};
    std::optional<bool> incremental_{};
    [[nodiscard]] constexpr http_priority request_priority() const noexcept {
        return {urgency_.value_or(http_priority_urgency{}), incremental_.value_or(false)};
    }
};
enum class http_priority_error : std::uint8_t { invalid_syntax,
    invalid_value,
    invalid_frame,
    output_too_small };
struct http_priority_update final {
    std::uint64_t element_id_{0};
    bool push_{false};
    http_priority_fields fields_{};
};

// RFC 9218 / RFC 8941 Dictionary parsing. Unknown members, invalid parameter
// types and out-of-range values are ignored; malformed structured syntax fails.
// Missing response parameters remain absent for intermediary merging.
[[nodiscard]] std::variant<http_priority_fields, http_priority_error> parse_http_priority(std::string_view value) noexcept;
namespace detail {
[[nodiscard]] std::variant<http_priority_fields, http_priority_error> parse_http_priority_headers(
    http_header_range_view headers) noexcept;
}  // namespace detail

// Reads all Priority field lines in wire order without allocating. A later
// dictionary member replaces the earlier member, including an invalid value.
template <http_header_field_source headers_type>
[[nodiscard]] std::variant<http_priority_fields, http_priority_error> parse_http_priority(
    const headers_type& headers) noexcept {
    return detail::parse_http_priority_headers(headers);
}
[[nodiscard]] std::variant<std::size_t, http_priority_error> encode_http_priority(std::span<char> output, http_priority_fields fields_value) noexcept;
[[nodiscard]] std::variant<http_priority_update, http_priority_error> decode_http2_priority_update(std::span<const char> payload_value) noexcept;
[[nodiscard]] std::variant<std::size_t, http_priority_error> encode_http2_priority_update(std::span<char> output, std::uint32_t stream_id, http_priority_fields fields_value) noexcept;

}  // namespace ruvia

namespace ruvia {

struct http_push_request_view final {
    borrowed_text method_{"GET"};
    borrowed_text scheme_{"https"};
    borrowed_text authority_{};
    borrowed_text path_{"/"};
    borrowed_http_headers headers_{};
};

// Detached promise metadata owns its decoded bytes independently of feed input.
// Accepting a promise does
// not establish transport authority: the driver validates the URI origin using
// its authenticated connection before consuming the pushed response.
struct http_push_request final {
    http_push_request() noexcept = default;
    http_push_request(const http_push_request&) = delete;
    http_push_request& operator=(const http_push_request&) = delete;
    http_push_request(http_push_request&&) noexcept = default;
    http_push_request& operator=(http_push_request&&) = delete;
    std::string method_;
    std::string scheme_;
    std::string authority_;
    std::string path_;
    std::vector<http_header> headers_;
};

struct http2_push_promise_event final {
    std::uint32_t associated_stream_id_;
    std::uint32_t promised_stream_id_;
    http_push_request request_;
};

enum class http2_push_submit_error : std::uint8_t {
    invalid_state,
    push_disabled,
    stream_limit,
    invalid_request,
};

}  // namespace ruvia

namespace ruvia {
// Encoders validate limits before allocation and write into the final owned result.
struct http_origin_advertisement final {
    std::vector<std::string> origins_;
    http_origin_advertisement() noexcept = default;
    http_origin_advertisement(const http_origin_advertisement&) = default;
    http_origin_advertisement(http_origin_advertisement&&) noexcept = default;
    http_origin_advertisement& operator=(const http_origin_advertisement&) = default;
    http_origin_advertisement& operator=(http_origin_advertisement&&) noexcept = default;
};
struct http_alternative_service_advertisement final {
    std::uint32_t stream_id_{0};
    std::string origin_;
    std::string field_value_;
    http_alternative_service_advertisement() noexcept = default;
    http_alternative_service_advertisement(const http_alternative_service_advertisement&) = default;
    http_alternative_service_advertisement(http_alternative_service_advertisement&&) noexcept = default;
    http_alternative_service_advertisement& operator=(const http_alternative_service_advertisement&) = default;
    http_alternative_service_advertisement& operator=(http_alternative_service_advertisement&&) noexcept = default;
};
enum class http_connection_advertisement_error : std::uint8_t {
    malformed_payload,
    invalid_origin,
    invalid_field,
    invalid_stream,
    limit,
};
// RFC 8336 ORIGIN payload decoding. Invalid serialized
// origins are ignored; truncated length-delimited entries reject the payload.
// Origin authority and connection coalescing require the runtime's TLS identity.
[[nodiscard]] std::variant<http_origin_advertisement, http_connection_advertisement_error>
decode_http_origin_advertisement(std::span<const char> payload_value);
[[nodiscard]] std::variant<std::vector<char>, http_connection_advertisement_error>
encode_http2_origin_frame(std::span<const std::string_view> origins,
    std::uint32_t max_frame_size = 16384);
// RFC 7838 ALTSVC. Stream 0 carries an explicit origin; a request stream
// carries an empty origin and inherits the request origin. The value is an
// Alt-Svc field value; transport authority and cache policy are external.
[[nodiscard]] std::variant<http_alternative_service_advertisement, http_connection_advertisement_error>
decode_http2_alternative_service(std::uint32_t stream_id, std::span<const char> payload_value);
[[nodiscard]] std::variant<std::vector<char>, http_connection_advertisement_error>
encode_http2_alternative_service_frame(std::uint32_t stream_id, std::string_view origin, std::string_view field_value,
    std::uint32_t max_frame_size = 16384);
}  // namespace ruvia
