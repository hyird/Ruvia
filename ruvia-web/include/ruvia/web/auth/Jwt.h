#pragma once

#ifdef RUVIA_ENABLE_JWT

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/BorrowedText.h"
#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia {

namespace detail {
struct JwtPayloadAccess;
}  // namespace detail

enum class JwtAlgorithm : std::uint8_t { kHs256, kHs384, kHs512 };

enum class JwtExpirationClaimPolicy : std::uint8_t {
    kRequire,
    kAllowMissing,
};

struct JwtClaimOptions final {
    BorrowedText name{};
    BorrowedText value{};
};

class JwtClaim final {
public:
    explicit JwtClaim(JwtClaimOptions options)
        : name_(std::in_place_type<std::string>, options.name.view()),
          value_(std::in_place_type<std::string>, options.value.view()) {}

    [[nodiscard]] std::string_view name() const& noexcept {
        return text(name_);
    }
    [[nodiscard]] std::string_view name() const&& = delete;

    [[nodiscard]] std::string_view value() const& noexcept {
        return text(value_);
    }
    [[nodiscard]] std::string_view value() const&& = delete;

private:
    friend struct detail::JwtPayloadAccess;

    struct OwnedTag final {};

    JwtClaim(OwnedTag, std::pmr::string name, std::pmr::string value) noexcept
        : name_(std::in_place_type<std::pmr::string>, std::move(name)),
          value_(std::in_place_type<std::pmr::string>, std::move(value)) {}

    using Text = std::variant<std::string, std::pmr::string>;

    [[nodiscard]] static std::string_view text(const Text& value) noexcept {
        return std::visit(
            [](const auto& stored) noexcept { return std::string_view(stored); }, value);
    }

    Text name_;
    Text value_;
};

struct JwtSignOptions final {
    JwtAlgorithm algorithm{JwtAlgorithm::kHs256};
    BorrowedText secret{};
    std::string issuer{};
    std::string subject{};
    std::string audience{};
    std::string id{};
    std::optional<std::chrono::seconds> expiresIn{std::chrono::hours(1)};
    std::optional<std::chrono::seconds> notBeforeDelay{};
    std::vector<JwtClaim> claims{};
    std::pmr::memory_resource* resource{nullptr};
};

struct JwtVerifyOptions final {
    BorrowedText token{};
    JwtAlgorithm algorithm{JwtAlgorithm::kHs256};
    BorrowedText secret{};
    std::string issuer{};
    std::string subject{};
    std::string audience{};
    std::chrono::seconds leeway{0};
    JwtExpirationClaimPolicy expirationClaim{JwtExpirationClaimPolicy::kRequire};
    std::pmr::memory_resource* resource{nullptr};
};

struct JwtDecodeUnverifiedOptions final {
    BorrowedText token{};
    std::pmr::memory_resource* resource{nullptr};
};

struct JwtPayloadOptions final {
    std::pmr::memory_resource* resource{nullptr};
};

class JwtPayload final {
public:
    [[nodiscard]] std::string_view issuer() const& noexcept;
    [[nodiscard]] std::string_view issuer() const&& = delete;
    [[nodiscard]] std::string_view subject() const& noexcept;
    [[nodiscard]] std::string_view subject() const&& = delete;
    // The first "aud" value, or empty if none. A JWT may carry multiple audiences
    // (RFC 7519 §4.1.3); use hasAudience to test membership across all of them.
    [[nodiscard]] std::string_view audience() const& noexcept;
    [[nodiscard]] std::string_view audience() const&& = delete;
    [[nodiscard]] bool hasAudience(std::string_view audience) const noexcept;
    [[nodiscard]] std::string_view id() const& noexcept;
    [[nodiscard]] std::string_view id() const&& = delete;
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> expiresAt() const noexcept;
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> notBefore() const noexcept;
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> issuedAt() const noexcept;
    [[nodiscard]] std::span<const JwtClaim> claims() const& noexcept;
    [[nodiscard]] std::span<const JwtClaim> claims() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> claim(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> claim(std::string_view name) const&& = delete;

private:
    friend struct detail::JwtPayloadAccess;
    friend JwtPayload jwtDecodeUnverified(JwtDecodeUnverifiedOptions);
    friend JwtPayload jwtVerify(const JwtVerifyOptions&);

    explicit JwtPayload(JwtPayloadOptions options = {});

    std::pmr::string issuer_;
    std::pmr::string subject_;
    std::pmr::vector<std::pmr::string> audiences_;
    std::pmr::string id_;
    std::optional<std::chrono::system_clock::time_point> expiresAt_;
    std::optional<std::chrono::system_clock::time_point> notBefore_;
    std::optional<std::chrono::system_clock::time_point> issuedAt_;
    std::pmr::vector<JwtClaim> claims_;
};

[[nodiscard]] std::pmr::string jwtSign(const JwtSignOptions& options);
[[nodiscard]] JwtPayload jwtVerify(const JwtVerifyOptions& options);
[[nodiscard]] JwtPayload jwtDecodeUnverified(JwtDecodeUnverifiedOptions options);
[[nodiscard]] std::optional<std::string_view> jwtBearerToken(
    std::string_view authorization) noexcept;

template <detail::HttpTemporaryOwningCharString Authorization>
std::optional<std::string_view> jwtBearerToken(Authorization&&) = delete;

}  // namespace ruvia

#endif  // RUVIA_ENABLE_JWT
