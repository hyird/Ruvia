#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia::detail {
struct HttpHeaderAccess;
}

namespace ruvia {

inline constexpr std::size_t kMaxHttpHeaderFields = 64;

// An owned field returned by protocol parsers and events. The same value type
// represents initial fields and trailers for requests and responses.
class HttpHeader final {
public:
    [[nodiscard]] std::string_view name() const& noexcept {
        return name_;
    }
    std::string_view name() const&& = delete;
    [[nodiscard]] std::string_view value() const& noexcept {
        return value_;
    }
    std::string_view value() const&& = delete;

private:
    friend struct detail::HttpHeaderAccess;
    HttpHeader(std::pmr::string name, std::pmr::string value)
        : name_(std::move(name)),
          value_(std::move(value)) {}
    HttpHeader(std::string_view name, std::string_view value,
        std::pmr::memory_resource* resource)
        : name_(name, resource),
          value_(value, resource) {}
    std::pmr::string name_;
    std::pmr::string value_;
};

class HttpHeaderView final {
public:
    constexpr HttpHeaderView() noexcept = default;

    constexpr HttpHeaderView(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    template <typename Name, typename Value>
        requires(detail::HttpTemporaryOwningCharString<Name> ||
                    detail::HttpTemporaryOwningCharString<Value>)
    HttpHeaderView(Name&& name, Value&& value) = delete;

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

[[nodiscard]] bool isValidHttpHeaderName(std::string_view name) noexcept;
[[nodiscard]] bool isValidHttpHeaderValue(std::string_view value) noexcept;
[[nodiscard]] bool isValidHttpStatusText(std::string_view value) noexcept;

}  // namespace ruvia
