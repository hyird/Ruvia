#pragma once

#include <chrono>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/memory/PmrResource.h"

namespace ruvia {

namespace detail {
struct JwtPayloadAccess;
}  // namespace detail

enum class JwtAlgorithm {
    kHs256,
    kHs384,
    kHs512
};

class JwtClaim final {
public:
    JwtClaim(
        std::string_view name,
        std::string_view value)
        : name_(name, detail::pmrResourceOrDefault(nullptr)),
          value_(value, name_.get_allocator().resource()) {}

    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return value_;
    }

private:
    friend struct detail::JwtPayloadAccess;

    struct OwnedTag final {};

    JwtClaim(
        OwnedTag,
        std::pmr::string name,
        std::pmr::string value) noexcept
        : name_(std::move(name)),
          value_(std::move(value)) {}

    std::pmr::string name_;
    std::pmr::string value_;
};

struct JwtSignOptions final {
    JwtAlgorithm algorithm{JwtAlgorithm::kHs256};
    std::pmr::string secret;
    std::pmr::string issuer;
    std::pmr::string subject;
    std::pmr::string audience;
    std::pmr::string id;
    std::chrono::seconds expiresIn{std::chrono::hours(1)};
    std::chrono::seconds notBeforeDelay{0};
    std::pmr::vector<JwtClaim> claims{detail::pmrResourceOrDefault(nullptr)};
};

struct JwtVerifyOptions final {
    JwtAlgorithm algorithm{JwtAlgorithm::kHs256};
    std::pmr::string secret;
    std::pmr::string issuer;
    std::pmr::string subject;
    std::pmr::string audience;
    std::chrono::seconds leeway{0};
    bool requireExpiration{true};
};

class JwtPayload final {
public:
    [[nodiscard]] std::string_view issuer() const noexcept;
    [[nodiscard]] std::string_view subject() const noexcept;
    [[nodiscard]] std::string_view audience() const noexcept;
    [[nodiscard]] std::string_view id() const noexcept;
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> expiresAt() const noexcept;
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> notBefore() const noexcept;
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> issuedAt() const noexcept;
    [[nodiscard]] std::span<const JwtClaim> claims() const noexcept;
    [[nodiscard]] std::optional<std::string_view> claim(std::string_view name) const noexcept;

private:
    friend struct detail::JwtPayloadAccess;
    friend JwtPayload jwtDecodeUnverified(std::string_view, std::pmr::memory_resource*);
    friend JwtPayload jwtVerify(std::string_view, const JwtVerifyOptions&, std::pmr::memory_resource*);

    explicit JwtPayload(std::pmr::memory_resource* resource = nullptr);

    std::pmr::string issuer_;
    std::pmr::string subject_;
    std::pmr::string audience_;
    std::pmr::string id_;
    std::optional<std::chrono::system_clock::time_point> expiresAt_;
    std::optional<std::chrono::system_clock::time_point> notBefore_;
    std::optional<std::chrono::system_clock::time_point> issuedAt_;
    std::pmr::vector<JwtClaim> claims_;
};

[[nodiscard]] std::pmr::string jwtSign(
    const JwtSignOptions& options,
    std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] JwtPayload jwtVerify(
    std::string_view token,
    const JwtVerifyOptions& options,
    std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] JwtPayload jwtDecodeUnverified(
    std::string_view token,
    std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] std::optional<std::string_view> jwtBearerToken(std::string_view authorization) noexcept;

}  // namespace ruvia
