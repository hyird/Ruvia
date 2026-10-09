#pragma once

#include <cstdint>
#include <type_traits>

#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/http_status.h"

namespace ruvia::detail {

class response_write_policy;

class response_normal_write final {
private:
    friend class response_write_policy;
    constexpr response_normal_write() noexcept = default;
};

class response_body_forbidden_write final {
private:
    friend class response_write_policy;
    constexpr response_body_forbidden_write() noexcept = default;
};

class response_zero_length_write final {
private:
    friend class response_write_policy;
    constexpr response_zero_length_write() noexcept = default;
};

class response_not_modified_write final {
private:
    friend class response_write_policy;
    constexpr response_not_modified_write() noexcept = default;
};

// Status-owned response writing semantics. Exactly one RFC state is active;
// body/framing capabilities are derived observations rather than four stored
// booleans that could describe contradictory products.
class response_write_policy final {
public:
    [[nodiscard]] constexpr const response_normal_write* normal() const& noexcept {
        return state_ == state_type::normal ? &normal_value : nullptr;
    }
    const response_normal_write* normal() const&& = delete;

    [[nodiscard]] constexpr const response_body_forbidden_write* body_forbidden() const& noexcept {
        return state_ == state_type::body_forbidden ? &body_forbidden_value : nullptr;
    }
    const response_body_forbidden_write* body_forbidden() const&& = delete;

    [[nodiscard]] constexpr const response_zero_length_write* zero_length() const& noexcept {
        return state_ == state_type::zero_length ? &zero_length_value : nullptr;
    }
    const response_zero_length_write* zero_length() const&& = delete;

    [[nodiscard]] constexpr const response_not_modified_write* not_modified() const& noexcept {
        return state_ == state_type::not_modified ? &not_modified_value : nullptr;
    }
    const response_not_modified_write* not_modified() const&& = delete;

    [[nodiscard]] constexpr bool body_allowed() const noexcept {
        return normal() != nullptr;
    }

    [[nodiscard]] constexpr bool auto_content_length_allowed() const noexcept {
        return normal() != nullptr || zero_length() != nullptr;
    }

    [[nodiscard]] constexpr bool explicit_content_length_allowed() const noexcept {
        return normal() != nullptr || not_modified() != nullptr;
    }

    [[nodiscard]] constexpr bool transfer_encoding_allowed() const noexcept {
        return normal() != nullptr;
    }

private:
    friend response_write_policy get_response_write_policy(http_status_code) noexcept;

    enum class state_type : std::uint8_t { normal,
        body_forbidden,
        zero_length,
        not_modified };

    explicit constexpr response_write_policy(state_type state_value) noexcept
        : state_(state_value) {}

    [[nodiscard]] static constexpr response_write_policy make_normal() noexcept {
        return response_write_policy(state_type::normal);
    }

    [[nodiscard]] static constexpr response_write_policy make_body_forbidden() noexcept {
        return response_write_policy(state_type::body_forbidden);
    }

    [[nodiscard]] static constexpr response_write_policy make_zero_length() noexcept {
        return response_write_policy(state_type::zero_length);
    }

    [[nodiscard]] static constexpr response_write_policy make_not_modified() noexcept {
        return response_write_policy(state_type::not_modified);
    }

    static inline constexpr response_normal_write normal_value{};
    static inline constexpr response_body_forbidden_write body_forbidden_value{};
    static inline constexpr response_zero_length_write zero_length_value{};
    static inline constexpr response_not_modified_write not_modified_value{};

    state_type state_;
};

static_assert(std::is_trivially_copyable_v<response_write_policy>);
static_assert(sizeof(response_write_policy) <= 2);

[[nodiscard]] inline response_write_policy get_response_write_policy(http_status_code status_code) noexcept {
    if (status_code.is_informational()) {
        return response_write_policy::make_body_forbidden();
    }
    if (status_code == http_status::no_content) {
        return response_write_policy::make_body_forbidden();
    }
    if (status_code == http_status::reset_content) {
        // RFC 9110 15.3.6 forbids content in a 205 response. Unlike 1xx/204/304,
        // HTTP/1.1 message framing does not make 205 self-delimiting from the
        // status alone, so the writer owns a canonical Content-Length: 0. A
        // caller-provided length and Transfer-Encoding are filtered instead of
        // creating a second, potentially contradictory framing declaration.
        return response_write_policy::make_zero_length();
    }
    if (status_code == http_status::not_modified) {
        return response_write_policy::make_not_modified();
    }
    return response_write_policy::make_normal();
}

}  // namespace ruvia::detail
