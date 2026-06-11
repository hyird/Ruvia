#pragma once

#include <chrono>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia {

enum class JwtAlgorithm {
    kHs256,
    kHs384,
    kHs512
};

struct JwtClaim final {
    std::pmr::string name;
    std::pmr::string value;
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
    std::pmr::vector<JwtClaim> claims{std::pmr::get_default_resource()};
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
    explicit JwtPayload(std::pmr::memory_resource* resource = std::pmr::get_default_resource());

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
    friend struct JwtPayloadAccess;
    friend JwtPayload jwtDecodeUnverified(std::string_view, std::pmr::memory_resource*);
    friend JwtPayload jwtVerify(std::string_view, const JwtVerifyOptions&, std::pmr::memory_resource*);

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
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] JwtPayload jwtVerify(
    std::string_view token,
    const JwtVerifyOptions& options,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] JwtPayload jwtDecodeUnverified(
    std::string_view token,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::optional<std::string_view> jwtBearerToken(std::string_view authorization) noexcept;

}  // namespace ruvia
