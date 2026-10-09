#pragma once

#include <cstdint>
#include <variant>

namespace ruvia::detail {

enum class http2_connect_form : std::uint8_t { standard,
    extended };

class http2_tunnel_state;

class http2_not_connect final {
private:
    friend class http2_tunnel_state;

    constexpr http2_not_connect() noexcept = default;
};

// The CONNECT form matters while validating/dispatching the request and choosing
// its dedicated acceptance path. After a final response, :protocol on the retained
// request state is the authoritative Extended CONNECT signal; rejected responses
// resume ordinary HTTP response-body semantics. Consequently, only pending owns a
// form payload.
class http2_connect_pending final {
public:
    [[nodiscard]] constexpr http2_connect_form form() const noexcept {
        return form_;
    }

private:
    friend class http2_tunnel_state;

    explicit constexpr http2_connect_pending(http2_connect_form form) noexcept
        : form_(form) {}

    http2_connect_form form_;
};

class http2_tunnel_open final {
private:
    friend class http2_tunnel_state;

    constexpr http2_tunnel_open() noexcept = default;
};

class http2_connect_rejected final {
private:
    friend class http2_tunnel_state;

    constexpr http2_connect_rejected() noexcept = default;
};

// CONNECT is exactly one of four protocol phases. This representation cannot form
// the former kind=None/phase=Open or kind=Extended/phase=None combinations.
class http2_tunnel_state final {
public:
    constexpr http2_tunnel_state() noexcept
        : state_(http2_not_connect()) {}

    [[nodiscard]] bool begin(http2_connect_form form) noexcept {
        if ((form != http2_connect_form::standard && form != http2_connect_form::extended) ||
            not_connect() == nullptr) {
            return false;
        }
        state_ = state_type(http2_connect_pending(form));
        return true;
    }

    [[nodiscard]] bool accept() noexcept {
        if (pending() == nullptr) {
            return false;
        }
        state_ = state_type(http2_tunnel_open());
        return true;
    }

    [[nodiscard]] bool reject() noexcept {
        if (pending() == nullptr) {
            return false;
        }
        state_ = state_type(http2_connect_rejected());
        return true;
    }

    [[nodiscard]] constexpr const http2_not_connect* not_connect() const& noexcept {
        return std::get_if<http2_not_connect>(&state_);
    }
    [[nodiscard]] constexpr const http2_not_connect* not_connect() const&& = delete;

    [[nodiscard]] constexpr const http2_connect_pending* pending() const& noexcept {
        return std::get_if<http2_connect_pending>(&state_);
    }
    [[nodiscard]] constexpr const http2_connect_pending* pending() const&& = delete;

    [[nodiscard]] constexpr const http2_tunnel_open* open() const& noexcept {
        return std::get_if<http2_tunnel_open>(&state_);
    }
    [[nodiscard]] constexpr const http2_tunnel_open* open() const&& = delete;

    [[nodiscard]] constexpr const http2_connect_rejected* rejected() const& noexcept {
        return std::get_if<http2_connect_rejected>(&state_);
    }
    [[nodiscard]] constexpr const http2_connect_rejected* rejected() const&& = delete;

private:
    using state_type =
        std::variant<http2_not_connect, http2_connect_pending, http2_tunnel_open, http2_connect_rejected>;

    state_type state_;
};

}  // namespace ruvia::detail
