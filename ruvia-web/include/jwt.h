#pragma once

#ifdef RUVIA_ENABLE_JWT

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "model.h"

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

enum class jwt_error_code : std::uint8_t { malformed_token,
    invalid_header,
    invalid_payload,
    signature_mismatch,
    algorithm_mismatch,
    expired,
    not_yet_valid,
    missing_claim,
    claim_mismatch };

struct jwt_claim_view final {
    borrowed_text name_{};
    borrowed_text value_{};
};

class jwt_claim final {
public:
    jwt_claim(const jwt_claim&) = delete;
    jwt_claim& operator=(const jwt_claim&) = delete;
    jwt_claim(jwt_claim&&) noexcept = default;
    jwt_claim& operator=(jwt_claim&& other) noexcept;

    [[nodiscard]] std::string_view name() const& noexcept RUVIA_LIFETIMEBOUND {
        return name_;
    }
    [[nodiscard]] std::string_view name() const&& = delete;
    [[nodiscard]] std::string take_name() && noexcept {
        return std::exchange(name_, {});
    }

    [[nodiscard]] std::string_view value() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }
    [[nodiscard]] std::string_view value() const&& = delete;
    [[nodiscard]] std::string take_value() && noexcept {
        return std::exchange(value_, {});
    }

private:
    friend struct detail::jwt_payload_access;

    jwt_claim(std::string name, std::string value) noexcept :
        name_(std::move(name)),
        value_(std::move(value)) {}

    std::string name_;
    std::string value_;
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
    borrowed_span<jwt_claim_view> claims_{};
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
};

struct jwt_decode_unverified_options final {
    borrowed_text token_{};
};

// JWT NumericDate 的精确十进制表示，不受宿主时钟范围或浮点精度限制。
// 移动后源值为 0。
class jwt_numeric_date final {
public:
    jwt_numeric_date(const jwt_numeric_date&) = delete;
    jwt_numeric_date& operator=(const jwt_numeric_date&) = delete;
    jwt_numeric_date(jwt_numeric_date&& other) noexcept;
    jwt_numeric_date& operator=(jwt_numeric_date&& other) noexcept;

    [[nodiscard]] std::string_view text() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view text() const&& = delete;
    // 仅在范围及小数精度都能被宿主时钟精确表示时转换；否则返回 nullopt。
    // JWT 验证直接比较 NumericDate，不依赖此转换。
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> to_time_point() const noexcept;

private:
    friend struct detail::jwt_payload_access;
    explicit jwt_numeric_date(std::string_view text);
    std::string text_;
};

class jwt_payload final {
public:
    jwt_payload(const jwt_payload&) = delete;
    jwt_payload& operator=(const jwt_payload&) = delete;
    jwt_payload(jwt_payload&&) noexcept = default;
    jwt_payload& operator=(jwt_payload&& other) noexcept;
    [[nodiscard]] std::string_view issuer() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view issuer() const&& = delete;
    [[nodiscard]] std::string take_issuer() && noexcept {
        return std::exchange(issuer_, {});
    }
    [[nodiscard]] std::string_view subject() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view subject() const&& = delete;
    [[nodiscard]] std::string take_subject() && noexcept {
        return std::exchange(subject_, {});
    }
    // The first "aud" value, or empty if none. A JWT may carry multiple audiences
    // (RFC 7519 §4.1.3); use has_audience to test membership across all of them.
    [[nodiscard]] std::string_view audience() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view audience() const&& = delete;
    [[nodiscard]] std::span<const std::string> audiences() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const std::string> audiences() const&& = delete;
    [[nodiscard]] std::vector<std::string> take_audiences() && noexcept {
        return std::exchange(audiences_, {});
    }
    [[nodiscard]] bool has_audience(std::string_view audience) const noexcept;
    [[nodiscard]] std::string_view id() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view id() const&& = delete;
    [[nodiscard]] std::string take_id() && noexcept {
        return std::exchange(id_, {});
    }
    // nullptr 表示声明缺失。返回值及 text() 是借用，payload 移动或销毁前须结束使用。
    [[nodiscard]] const jwt_numeric_date* expires_at() const& noexcept RUVIA_LIFETIMEBOUND;
    const jwt_numeric_date* expires_at() const&& = delete;
    [[nodiscard]] const jwt_numeric_date* not_before() const& noexcept RUVIA_LIFETIMEBOUND;
    const jwt_numeric_date* not_before() const&& = delete;
    [[nodiscard]] const jwt_numeric_date* issued_at() const& noexcept RUVIA_LIFETIMEBOUND;
    const jwt_numeric_date* issued_at() const&& = delete;
    // String-valued custom claims, decoded. A claim value may be any JSON
    // value (RFC 7519 §2) and unknown claims are ignored (§4): custom claims
    // holding numbers, booleans, null, arrays or objects are syntax- and
    // uniqueness-checked with the rest of the payload, then not exposed here.
    [[nodiscard]] std::span<const jwt_claim> claims() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const jwt_claim> claims() const&& = delete;
    [[nodiscard]] std::vector<jwt_claim> take_claims() && noexcept {
        return std::exchange(claims_, {});
    }
    [[nodiscard]] std::optional<std::string_view> claim(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> claim(std::string_view name) const&& = delete;

private:
    friend struct detail::jwt_payload_access;

    jwt_payload() noexcept = default;

    std::string issuer_;
    std::string subject_;
    std::vector<std::string> audiences_;
    std::string id_;
    std::optional<jwt_numeric_date> expires_at_;
    std::optional<jwt_numeric_date> not_before_;
    std::optional<jwt_numeric_date> issued_at_;
    std::vector<jwt_claim> claims_;
};

// The outcome of verify_jwt / decode_jwt_unverified. The category is fixed at
// construction: a decoded payload, or a token data error code.
class jwt_result final {
public:
    jwt_result(const jwt_result&) = delete;
    jwt_result& operator=(const jwt_result&) = delete;
    jwt_result(jwt_result&&) noexcept = default;
    jwt_result& operator=(jwt_result&&) = delete;

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<jwt_payload>(state_);
    }
    // nullopt when the result holds a payload.
    [[nodiscard]] std::optional<jwt_error_code> error() const noexcept {
        if (const auto* code = std::get_if<jwt_error_code>(&state_)) {
            return *code;
        }
        return std::nullopt;
    }
    // Throws std::logic_error when the result holds an error.
    [[nodiscard]] const jwt_payload& value() const& RUVIA_LIFETIMEBOUND;
    const jwt_payload& value() const&& = delete;
    [[nodiscard]] jwt_payload take_value() &&;

private:
    friend struct detail::jwt_payload_access;

    explicit jwt_result(jwt_payload payload) noexcept :
        state_(std::in_place_type<jwt_payload>, std::move(payload)) {}
    explicit jwt_result(jwt_error_code code) noexcept :
        state_(std::in_place_type<jwt_error_code>, code) {}

    std::variant<jwt_payload, jwt_error_code> state_;
};

template <typename claims_type>
class jwt_claims_value final {
public:
    jwt_claims_value(jwt_payload payload, claims_type claims) noexcept :
        payload_(std::move(payload)),
        claims_(std::move(claims)) {}
    jwt_claims_value(const jwt_claims_value&) = delete;
    jwt_claims_value& operator=(const jwt_claims_value&) = delete;
    jwt_claims_value(jwt_claims_value&&) noexcept = default;
    jwt_claims_value& operator=(jwt_claims_value&&) = delete;

    [[nodiscard]] const jwt_payload& payload() const& noexcept RUVIA_LIFETIMEBOUND {
        return payload_;
    }
    const jwt_payload& payload() const&& = delete;
    [[nodiscard]] jwt_payload take_payload() && noexcept {
        return std::move(payload_);
    }

    [[nodiscard]] const claims_type& claims() const& noexcept RUVIA_LIFETIMEBOUND {
        return claims_;
    }
    const claims_type& claims() const&& = delete;
    [[nodiscard]] claims_type take_claims() && noexcept {
        return std::move(claims_);
    }

private:
    jwt_payload payload_;
    claims_type claims_;
};

template <typename claims_type>
class jwt_claims_result final {
public:
    using value_type = jwt_claims_value<claims_type>;

    explicit jwt_claims_result(value_type value) noexcept :
        state_(std::in_place_type<value_type>, std::move(value)) {}
    explicit jwt_claims_result(jwt_error_code code) noexcept :
        state_(std::in_place_type<jwt_error_code>, code) {}
    jwt_claims_result(const jwt_claims_result&) = delete;
    jwt_claims_result& operator=(const jwt_claims_result&) = delete;
    jwt_claims_result(jwt_claims_result&&) noexcept = default;
    jwt_claims_result& operator=(jwt_claims_result&&) = delete;

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<value_type>(state_);
    }
    [[nodiscard]] std::optional<jwt_error_code> error() const noexcept {
        if (const auto* code = std::get_if<jwt_error_code>(&state_)) {
            return *code;
        }
        return std::nullopt;
    }
    [[nodiscard]] const value_type& value() const& RUVIA_LIFETIMEBOUND {
        const auto* stored = std::get_if<value_type>(&state_);
        if (stored == nullptr) {
            throw std::logic_error("JWT claims result holds an error, not a value");
        }
        return *stored;
    }
    const value_type& value() const&& = delete;
    [[nodiscard]] value_type take_value() && {
        auto* stored = std::get_if<value_type>(&state_);
        if (stored == nullptr) {
            throw std::logic_error("JWT claims result holds an error, not a value");
        }
        return std::move(*stored);
    }

private:
    std::variant<value_type, jwt_error_code> state_;
};

namespace detail {
[[nodiscard]] std::string sign_jwt_with_custom_json(std::string_view custom_object_json, const jwt_sign_options& options);
[[nodiscard]] std::variant<std::pair<jwt_payload, std::string>, jwt_error_code> verify_jwt_with_raw_payload(const jwt_verify_options& options);
}  // namespace detail

// Option misuse (negative leeway, invalid algorithm/policy, a secret outside the
// RFC 7518 key bounds) throws even for malformed tokens. Allocation and crypto
// initialization failures also throw; token data failures are returned.
[[nodiscard]] std::string sign_jwt(const jwt_sign_options& options);
[[nodiscard]] jwt_result verify_jwt(const jwt_verify_options& options);

// Signs a JWT embedding all present fields of `custom_claims` (a RUVIA_MODEL,
// supporting numbers, booleans, arrays, nested objects, enums, UUIDs, etc.)
// alongside standard registered claims in `options`. Throws std::invalid_argument
// if any field in `custom_claims` uses a reserved JWT claim name or duplicates `options.claims_`.
template <typename claims_type>
    requires detail::is_model<claims_type>
[[nodiscard]] std::string sign_jwt_claims(const claims_type& custom_claims, const jwt_sign_options& options) {
    return detail::sign_jwt_with_custom_json(to_json(custom_claims), options);
}

// Authenticates and verifies `options`, then parses and validates custom claims
// into `claims_type` (`RUVIA_MODEL`). Structural or validation failure on custom
// claims reports `jwt_error_code::invalid_payload` or `missing_claim`.
template <typename claims_type>
    requires detail::is_model<claims_type>
[[nodiscard]] jwt_claims_result<claims_type> verify_jwt_claims(const jwt_verify_options& options) {
    auto verified = detail::verify_jwt_with_raw_payload(options);
    if (const auto* err = std::get_if<jwt_error_code>(&verified)) {
        return jwt_claims_result<claims_type>(*err);
    }
    auto& [payload, raw_json] = std::get<0>(verified);
    auto parsed = parse_json<claims_type>(raw_json);
    if (!parsed.has_value()) {
        for (const auto& issue : parsed.error().issues()) {
            if (issue.code() == "required") {
                return jwt_claims_result<claims_type>(jwt_error_code::missing_claim);
            }
        }
        return jwt_claims_result<claims_type>(jwt_error_code::invalid_payload);
    }
    return jwt_claims_result<claims_type>(jwt_claims_value<claims_type>(std::move(payload), std::move(parsed).take_value()));
}
// Decodes and validates the three-part token structure, JOSE header (requiring
// a non-empty "alg" string and rejecting duplicate keys / "crit" / "b64"), and
// payload claims WITHOUT authenticating the signature or checking time claims.
// The returned claims are untrusted and must never authorize a request.
[[nodiscard]] jwt_result decode_jwt_unverified(const jwt_decode_unverified_options& options);
// Returns the b64token of an "Authorization: Bearer <token>" field value
// (RFC 6750 §2.1: 1*( ALPHA / DIGIT / "-" / "." / "_" / "~" / "+" / "/" )
// *"="), or nullopt for another scheme or a malformed token. The scheme name
// is case-insensitive and is followed by one or more SP.
[[nodiscard]] std::optional<std::string_view> extract_bearer_token(std::string_view authorization RUVIA_LIFETIMEBOUND) noexcept;

template <typename authorization_type>
    requires(std::convertible_to<authorization_type&&, std::string_view> && !std::constructible_from<borrowed_text, authorization_type&&>)
std::optional<std::string_view> extract_bearer_token(authorization_type&&) = delete;

}  // namespace ruvia

#endif  // RUVIA_ENABLE_JWT
