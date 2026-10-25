#pragma once

#ifdef RUVIA_ENABLE_JWT

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/borrowed_text.h"

namespace ruvia {

namespace detail {
struct jwt_payload_access;
}  // namespace detail

enum class jwt_algorithm : std::uint8_t { hs256,
    hs384,
    hs512 };

enum class jwt_expiration_claim_policy : std::uint8_t {
    require,
    allow_missing,
};

class jwt_error final : public std::runtime_error {
public:
    enum class code_type : std::uint8_t {
        malformed_token,
        invalid_header,
        invalid_payload,
        signature_mismatch,
        algorithm_mismatch,
        expired,
        not_yet_valid,
        missing_claim,
        claim_mismatch
    };

    jwt_error(code_type code, std::string_view message)
        : std::runtime_error(std::string(message)),
          code_(code) {}

    [[nodiscard]] code_type code() const noexcept {
        return code_;
    }

private:
    code_type code_;
};

struct jwt_claim_view final {
    borrowed_text name_{};
    borrowed_text value_{};
};

class jwt_claim final {
public:
    jwt_claim(const jwt_claim&) = delete;
    jwt_claim& operator=(const jwt_claim&) = delete;
    jwt_claim(jwt_claim&&) noexcept = default;
    jwt_claim& operator=(jwt_claim&&) = delete;

    [[nodiscard]] std::string_view name() const& noexcept {
        return name_;
    }
    [[nodiscard]] std::string_view name() const&& = delete;

    [[nodiscard]] std::string_view value() const& noexcept {
        return value_;
    }
    [[nodiscard]] std::string_view value() const&& = delete;

private:
    friend struct detail::jwt_payload_access;

    jwt_claim(std::pmr::string name, std::pmr::string value) noexcept
        : name_(std::move(name)),
          value_(std::move(value)) {}

    std::pmr::string name_;
    std::pmr::string value_;
};

struct jwt_sign_options final {
    jwt_algorithm algorithm_{jwt_algorithm::hs256};
    // Raw key bytes: at least 32/48/64 bytes for HS256/HS384/HS512 (RFC 7518).
    // Use cryptographically random keys; byte length alone is not entropy.
    borrowed_text secret_{};
    borrowed_text issuer_{};
    borrowed_text subject_{};
    borrowed_text audience_{};
    borrowed_text id_{};
    std::optional<std::chrono::seconds> expires_in_{std::chrono::hours(1)};
    std::optional<std::chrono::seconds> not_before_delay_{};
    std::span<const jwt_claim_view> claims_{};
    std::pmr::memory_resource* resource_{nullptr};
};

struct jwt_verify_options final {
    borrowed_text token_{};
    jwt_algorithm algorithm_{jwt_algorithm::hs256};
    // Same raw-key requirements as jwt_sign_options::secret.
    borrowed_text secret_{};
    borrowed_text issuer_{};
    borrowed_text subject_{};
    // The audience this verifier identifies itself with. A token that carries
    // "aud" is rejected unless this value is one of its audiences (RFC 7519
    // §4.1.3); when empty, any token that carries "aud" is rejected.
    borrowed_text audience_{};
    std::chrono::seconds leeway_{0};
    jwt_expiration_claim_policy expiration_claim_{jwt_expiration_claim_policy::require};
    std::pmr::memory_resource* resource_{nullptr};
};

struct jwt_decode_unverified_options final {
    borrowed_text token_{};
    std::pmr::memory_resource* resource_{nullptr};
};

class jwt_payload final {
public:
    jwt_payload(const jwt_payload&) = delete;
    jwt_payload& operator=(const jwt_payload&) = delete;
    jwt_payload(jwt_payload&&) noexcept = default;
    jwt_payload& operator=(jwt_payload&&) = delete;
    [[nodiscard]] std::string_view issuer() const& noexcept;
    [[nodiscard]] std::string_view issuer() const&& = delete;
    [[nodiscard]] std::string_view subject() const& noexcept;
    [[nodiscard]] std::string_view subject() const&& = delete;
    // The first "aud" value, or empty if none. A JWT may carry multiple audiences
    // (RFC 7519 §4.1.3); use has_audience to test membership across all of them.
    [[nodiscard]] std::string_view audience() const& noexcept;
    [[nodiscard]] std::string_view audience() const&& = delete;
    [[nodiscard]] bool has_audience(std::string_view audience) const noexcept;
    [[nodiscard]] std::string_view id() const& noexcept;
    [[nodiscard]] std::string_view id() const&& = delete;
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> expires_at() const noexcept;
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> not_before() const noexcept;
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> issued_at() const noexcept;
    // String-valued custom claims, decoded. A claim value may be any JSON
    // value (RFC 7519 §2) and unknown claims are ignored (§4): custom claims
    // holding numbers, booleans, null, arrays or objects are syntax- and
    // uniqueness-checked with the rest of the payload, then not exposed here.
    [[nodiscard]] std::span<const jwt_claim> claims() const& noexcept;
    [[nodiscard]] std::span<const jwt_claim> claims() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> claim(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> claim(std::string_view name) const&& = delete;

private:
    friend struct detail::jwt_payload_access;
    friend jwt_payload jwt_verify(const jwt_verify_options&);

    explicit jwt_payload(std::pmr::memory_resource* resource);

    std::pmr::string issuer_;
    std::pmr::string subject_;
    std::pmr::vector<std::pmr::string> audiences_;
    std::pmr::string id_;
    std::optional<std::chrono::system_clock::time_point> expires_at_;
    std::optional<std::chrono::system_clock::time_point> not_before_;
    std::optional<std::chrono::system_clock::time_point> issued_at_;
    std::pmr::vector<jwt_claim> claims_;
};

[[nodiscard]] std::pmr::string jwt_sign(const jwt_sign_options& options);
[[nodiscard]] jwt_payload jwt_verify(const jwt_verify_options& options);
// Decodes and validates the three-part token structure, JOSE header (requiring
// a non-empty "alg" string and rejecting duplicate keys / "crit" / "b64"), and
// payload claims without verifying the signature or time-based claims.
[[nodiscard]] jwt_payload jwt_decode_unverified(jwt_decode_unverified_options options);
// Returns the b64token of an "Authorization: Bearer <token>" field value
// (RFC 6750 §2.1: 1*( ALPHA / DIGIT / "-" / "." / "_" / "~" / "+" / "/" )
// *"="), or nullopt for another scheme or a malformed token. The scheme name
// is case-insensitive and is followed by one or more SP.
[[nodiscard]] std::optional<std::string_view> jwt_bearer_token(
    std::string_view authorization) noexcept;

template <typename authorization_type>
    requires(std::convertible_to<authorization_type&&, std::string_view> &&
                !std::constructible_from<borrowed_text, authorization_type&&>)
std::optional<std::string_view> jwt_bearer_token(authorization_type&&) = delete;

}  // namespace ruvia

#endif  // RUVIA_ENABLE_JWT
