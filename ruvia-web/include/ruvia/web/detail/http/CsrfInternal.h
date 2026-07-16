#pragma once

#include <span>
#include <string_view>
#include <variant>

#include "ruvia/core/detail/ConstantTime.h"

namespace ruvia::detail {

class SecureTokenReady final {
public:
    explicit SecureTokenReady(std::string_view value) noexcept : value_(value) {}
    [[nodiscard]] std::string_view value() const noexcept { return value_; }

private:
    std::string_view value_;
};

struct SecureTokenFailure final {};

class SecureTokenResult final {
public:
    [[nodiscard]] static SecureTokenResult makeReady(std::string_view value) noexcept {
        return SecureTokenResult(SecureTokenReady(value));
    }
    [[nodiscard]] static SecureTokenResult makeFailure() noexcept {
        return SecureTokenResult(SecureTokenFailure{});
    }
    [[nodiscard]] const SecureTokenReady* ready() const noexcept {
        return std::get_if<SecureTokenReady>(&value_);
    }
    [[nodiscard]] const SecureTokenFailure* failure() const noexcept {
        return std::get_if<SecureTokenFailure>(&value_);
    }

private:
    explicit SecureTokenResult(SecureTokenReady value) noexcept : value_(value) {}
    explicit SecureTokenResult(SecureTokenFailure value) noexcept : value_(value) {}
    std::variant<SecureTokenReady, SecureTokenFailure> value_;
};

// Fills `buffer` (which must hold at least 48 bytes) with a cryptographically
// random hex token. Failure is explicit and cannot be mistaken for a token.
[[nodiscard]] SecureTokenResult generateSecureToken(std::span<char> buffer) noexcept;

// Length-checked constant-time compare of the double-submit CSRF token; see
// constantTimeBytesEqual for the timing-safety rationale.
[[nodiscard]] inline bool csrfTokensEqual(std::string_view left, std::string_view right) noexcept {
    return constantTimeBytesEqual(left, right);
}

}  // namespace ruvia::detail
