#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/core/Task.h"
#include "ruvia/web/Context.h"
#include "ruvia/http/detail/util/BorrowedView.h"
#include "ruvia/web/Middleware.h"
#include "ruvia/web/Next.h"

namespace ruvia {

struct SecurityHeader final {
    // Security header definitions and options are commonly retained by custom
    // middleware. Keep their text zero-copy while rejecting owning-string
    // rvalues that would leave a saved definition with dangling views.
    class BorrowedText final {
    public:
        constexpr BorrowedText() noexcept = default;

        constexpr BorrowedText(std::string_view value) noexcept
            : value_(value) {}

        constexpr BorrowedText(const char* value) noexcept
            : value_(value) {}

        template <typename Traits, typename Allocator>
        constexpr BorrowedText(const std::basic_string<char, Traits, Allocator>& value) noexcept
            : value_(value) {}

        template <detail::HttpTemporaryOwningCharString String>
        BorrowedText(String&&) = delete;

        constexpr BorrowedText& operator=(std::string_view value) noexcept {
            value_ = value;
            return *this;
        }

        constexpr BorrowedText& operator=(const char* value) noexcept {
            value_ = std::string_view(value);
            return *this;
        }

        template <typename Traits, typename Allocator>
        constexpr BorrowedText& operator=(const std::basic_string<char, Traits, Allocator>& value) noexcept {
            value_ = std::string_view(value);
            return *this;
        }

        template <detail::HttpTemporaryOwningCharString String>
        BorrowedText& operator=(String&&) = delete;

        [[nodiscard]] constexpr std::string_view view() const noexcept {
            return value_;
        }

        [[nodiscard]] constexpr operator std::string_view() const noexcept {
            return value_;
        }

        [[nodiscard]] constexpr bool empty() const noexcept {
            return value_.empty();
        }

        [[nodiscard]] constexpr const char* data() const noexcept {
            return value_.data();
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return value_.size();
        }

        friend constexpr bool operator==(BorrowedText left, BorrowedText right) noexcept {
            return left.value_ == right.value_;
        }

        friend constexpr bool operator==(BorrowedText left, std::string_view right) noexcept {
            return left.value_ == right;
        }

        friend constexpr bool operator==(BorrowedText left, const char* right) noexcept {
            return left.value_ == right;
        }

        template <typename Traits, typename Allocator>
        friend constexpr bool operator==(BorrowedText left, const std::basic_string<char, Traits, Allocator>& right) noexcept {
            return left.value_ == std::string_view(right);
        }

    private:
        std::string_view value_;
    };

    BorrowedText name;
    BorrowedText value;
};

static_assert(sizeof(SecurityHeader::BorrowedText) == sizeof(std::string_view));

namespace detail {

template <typename Range>
concept SecurityHeaderRange = std::ranges::contiguous_range<Range> && std::same_as<std::remove_cv_t<std::ranges::range_value_t<Range>>, SecurityHeader>;

}  // namespace detail

// Legacy browser XSS filters can introduce vulnerabilities in otherwise safe
// pages. Modern policy explicitly disables them with X-XSS-Protection: 0;
// applications targeting only browsers that ignore the obsolete header may
// choose not to emit it.
enum class LegacyXssFilterPolicy : std::uint8_t {
    kDisable,
    kOmitHeader,
};

struct SecurityHeadersOptions final {
    // The collection itself is borrowed alongside the text in each element.
    // Borrowed ranges and owning-range lvalues remain valid inputs; temporary
    // owning ranges are rejected before their storage can disappear.
    class HeaderInit final {
    public:
        constexpr HeaderInit() noexcept = default;

        template <detail::SecurityHeaderRange Range>
            requires(std::is_lvalue_reference_v<Range &&> || std::ranges::borrowed_range<Range>)
        constexpr HeaderInit(Range&& headers) noexcept
            : headers_(std::ranges::data(headers), std::ranges::size(headers)) {}

        template <detail::SecurityHeaderRange Range>
            requires(!std::is_lvalue_reference_v<Range &&> && !std::ranges::borrowed_range<Range>)
        HeaderInit(Range&&) = delete;

        constexpr HeaderInit(std::initializer_list<SecurityHeader>) = delete;

        template <detail::SecurityHeaderRange Range>
            requires(std::is_lvalue_reference_v<Range &&> || std::ranges::borrowed_range<Range>)
        constexpr HeaderInit& operator=(Range&& headers) noexcept {
            headers_ = std::span<const SecurityHeader>(std::ranges::data(headers), std::ranges::size(headers));
            return *this;
        }

        template <detail::SecurityHeaderRange Range>
            requires(!std::is_lvalue_reference_v<Range &&> && !std::ranges::borrowed_range<Range>)
        HeaderInit& operator=(Range&&) = delete;

        HeaderInit& operator=(std::initializer_list<SecurityHeader>) = delete;

        [[nodiscard]] constexpr operator std::span<const SecurityHeader>() const noexcept {
            return headers_;
        }

        [[nodiscard]] constexpr auto begin() const noexcept {
            return headers_.begin();
        }

        [[nodiscard]] constexpr auto end() const noexcept {
            return headers_.end();
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return headers_.size();
        }

        [[nodiscard]] constexpr bool empty() const noexcept {
            return headers_.empty();
        }

    private:
        std::span<const SecurityHeader> headers_{};
    };

    bool contentTypeOptions = true;
    bool frameOptions = true;
    // Emitted only for requests received over TLS. Plain HTTP responses must
    // never carry Strict-Transport-Security.
    bool strictTransportSecurity = true;
    LegacyXssFilterPolicy legacyXssFilter = LegacyXssFilterPolicy::kDisable;

    SecurityHeader::BorrowedText contentSecurityPolicy = "default-src 'self'";
    SecurityHeader::BorrowedText referrerPolicy = "strict-origin-when-cross-origin";
    SecurityHeader::BorrowedText permissionsPolicy = "geolocation=(), microphone=(), camera=()";

    HeaderInit customHeaders{};
    bool overwriteExisting = false;
};

static_assert(sizeof(SecurityHeadersOptions::HeaderInit) == sizeof(std::span<const SecurityHeader>));

void applySecurityHeaders(Context& context, const SecurityHeadersOptions& options = {});

class SecurityHeadersMiddleware final : public Middleware<SecurityHeadersMiddleware> {
public:
    Task<void> handle(Context& context, Next& next);
};

}  // namespace ruvia
