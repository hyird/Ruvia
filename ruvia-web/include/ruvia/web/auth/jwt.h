#pragma once

#ifdef RUVIA_ENABLE_JWT

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"
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

struct jwt_claim_options final {
    borrowed_text name_{};
    borrowed_text value_{};
};

class jwt_claim final {
public:
    using allocator_type = std::pmr::polymorphic_allocator<std::byte>;
    jwt_claim(const jwt_claim& other, allocator_type allocator);
    jwt_claim(jwt_claim&& other, allocator_type allocator);
    jwt_claim(const jwt_claim&) = default;
    jwt_claim& operator=(const jwt_claim&) = default;
    jwt_claim(jwt_claim&&) noexcept = default;
    jwt_claim& operator=(jwt_claim&&) = default;

    explicit jwt_claim(jwt_claim_options options)
        : name_(std::in_place_type<std::string>, options.name_.view()),
          value_(std::in_place_type<std::string>, options.value_.view()) {}

    [[nodiscard]] std::string_view name() const& noexcept {
        return text(name_);
    }
    [[nodiscard]] std::string_view name() const&& = delete;

    [[nodiscard]] std::string_view value() const& noexcept {
        return text(value_);
    }
    [[nodiscard]] std::string_view value() const&& = delete;

private:
    friend struct detail::jwt_payload_access;

    struct owned_tag_type final {};

    jwt_claim(owned_tag_type, std::pmr::string name, std::pmr::string value) noexcept
        : name_(std::in_place_type<std::pmr::string>, std::move(name)),
          value_(std::in_place_type<std::pmr::string>, std::move(value)) {}

    using text_type = std::variant<std::string, std::pmr::string>;

    [[nodiscard]] static std::string_view text(const text_type& value) noexcept {
        return std::visit(
            [](const auto& stored) noexcept { return std::string_view(stored); }, value);
    }

    text_type name_;
    text_type value_;
};

struct jwt_sign_options final {
    jwt_algorithm algorithm_{jwt_algorithm::hs256};
    // Raw key bytes: at least 32/48/64 bytes for HS256/HS384/HS512 (RFC 7518).
    // Use cryptographically random keys; byte length alone is not entropy.
    borrowed_text secret_{};
    std::string issuer_{};
    std::string subject_{};
    std::string audience_{};
    std::string id_{};
    std::optional<std::chrono::seconds> expires_in_{std::chrono::hours(1)};
    std::optional<std::chrono::seconds> not_before_delay_{};
    std::vector<jwt_claim> claims_{};
    std::pmr::memory_resource* resource_{nullptr};
};

struct jwt_verify_options final {
    borrowed_text token_{};
    jwt_algorithm algorithm_{jwt_algorithm::hs256};
    // Same raw-key requirements as jwt_sign_options::secret.
    borrowed_text secret_{};
    std::string issuer_{};
    std::string subject_{};
    // The audience this verifier identifies itself with. A token that carries
    // "aud" is rejected unless this value is one of its audiences (RFC 7519
    // §4.1.3); when empty, any token that carries "aud" is rejected.
    std::string audience_{};
    std::chrono::seconds leeway_{0};
    jwt_expiration_claim_policy expiration_claim_{jwt_expiration_claim_policy::require};
    std::pmr::memory_resource* resource_{nullptr};
};

struct jwt_decode_unverified_options final {
    borrowed_text token_{};
    std::pmr::memory_resource* resource_{nullptr};
};

struct jwt_payload_options final {
    std::pmr::memory_resource* resource_{nullptr};
};

class jwt_payload final {
public:
    jwt_payload(const jwt_payload& other);
    jwt_payload& operator=(const jwt_payload& other);
    jwt_payload(jwt_payload&&) noexcept = default;
    // Assignment keeps the destination resource, including all nested claims.
    // Rebinding may allocate; failure leaves both payloads unchanged.
    jwt_payload& operator=(jwt_payload&& other);

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
    // Custom claims are string-valued: a token whose custom claim holds any
    // other JSON value is rejected, like a malformed registered claim.
    [[nodiscard]] std::span<const jwt_claim> claims() const& noexcept;
    [[nodiscard]] std::span<const jwt_claim> claims() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> claim(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> claim(std::string_view name) const&& = delete;

private:
    friend struct detail::jwt_payload_access;
    friend jwt_payload jwt_verify(const jwt_verify_options&);

    explicit jwt_payload(jwt_payload_options options);
    jwt_payload(const jwt_payload& other, std::pmr::memory_resource* resource);
    jwt_payload(jwt_payload&& other, std::pmr::memory_resource* resource);
    void swap_values(jwt_payload& other) noexcept;

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
[[nodiscard]] jwt_payload jwt_decode_unverified(jwt_decode_unverified_options options);
[[nodiscard]] std::optional<std::string_view> jwt_bearer_token(
    std::string_view authorization) noexcept;

template <typename authorization_type>
    requires(std::convertible_to<authorization_type &&, std::string_view> &&
                !std::constructible_from<borrowed_text, authorization_type &&>)
std::optional<std::string_view> jwt_bearer_token(authorization_type&&) = delete;

}  // namespace ruvia

#endif  // RUVIA_ENABLE_JWT
