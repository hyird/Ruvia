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
#include "ruvia/http/BorrowedText.h"
#include "ruvia/web/Middleware.h"
#include "ruvia/web/Next.h"

namespace ruvia {

struct SecurityHeader final {
    // Security header definitions and options are commonly retained by custom
    // middleware. Keep their text zero-copy while rejecting owning-string
    // rvalues that would leave a saved definition with dangling views.
    ::ruvia::BorrowedText name;
    ::ruvia::BorrowedText value;
};

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

    ::ruvia::BorrowedText contentSecurityPolicy = "default-src 'self'";
    ::ruvia::BorrowedText referrerPolicy = "strict-origin-when-cross-origin";
    ::ruvia::BorrowedText permissionsPolicy = "geolocation=(), microphone=(), camera=()";

    HeaderInit customHeaders{};
    bool overwriteExisting = false;
};

static_assert(sizeof(SecurityHeadersOptions::HeaderInit) == sizeof(std::span<const SecurityHeader>));

void applySecurityHeaders(Context& context, const SecurityHeadersOptions& options = {});

// Registered app-wide with the defaults as `app().use<SecurityHeadersMiddleware>()`,
// or with a policy as `app().use<SecurityHeadersMiddleware>(options)`. The
// options value is borrowed text throughout, and one instance serves the whole
// process, so whatever it points at must outlive App::run() -- the same rule the
// SecurityHeadersOptions members already enforce against owning temporaries.
class SecurityHeadersMiddleware final : public Middleware<SecurityHeadersMiddleware> {
public:
    SecurityHeadersMiddleware() noexcept = default;

    explicit SecurityHeadersMiddleware(SecurityHeadersOptions options) noexcept
        : options_(options) {}

    Task<void> handle(Context& context, Next& next);

private:
    SecurityHeadersOptions options_{};
};

}  // namespace ruvia
