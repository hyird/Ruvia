#pragma once

#include <span>
#include <string_view>
#include <variant>

#include "ruvia/core/constant_time.h"

namespace ruvia::detail {

class secure_token_ready final {
public:
    [[nodiscard]] std::string_view value() const noexcept {
        return value_;
    }

private:
    friend class secure_token_result;

    explicit secure_token_ready(std::string_view value) noexcept
        : value_(value) {}

    std::string_view value_;
};

struct secure_token_failure final {};

class secure_token_result final {
public:
    [[nodiscard]] const secure_token_ready* ready() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    [[nodiscard]] const secure_token_ready* ready() const&& = delete;

    [[nodiscard]] const secure_token_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    [[nodiscard]] const secure_token_failure* failure() const&& = delete;

private:
    friend secure_token_result generate_secure_token(std::span<char> buffer) noexcept;

    [[nodiscard]] static secure_token_result make_ready(std::string_view value) noexcept {
        return secure_token_result(secure_token_ready(value));
    }
    [[nodiscard]] static secure_token_result make_failure() noexcept {
        return secure_token_result(secure_token_failure{});
    }

    explicit secure_token_result(secure_token_ready value) noexcept
        : value_(value) {}
    explicit secure_token_result(secure_token_failure value) noexcept
        : value_(value) {}
    std::variant<secure_token_ready, secure_token_failure> value_;
};

// Fills `buffer` (which must hold at least 48 bytes) with a cryptographically
// random hex token. Failure is explicit and cannot be mistaken for a token.
[[nodiscard]] secure_token_result generate_secure_token(std::span<char> buffer) noexcept;

// Length-checked constant-time compare of the double-submit CSRF token; see
// ruvia::constant_time_bytes_equal for the timing-safety rationale.
[[nodiscard]] inline bool csrf_tokens_equal(std::string_view left, std::string_view right) noexcept {
    return ruvia::constant_time_bytes_equal(left, right);
}

}  // namespace ruvia::detail
