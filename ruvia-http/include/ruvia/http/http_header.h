#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

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
