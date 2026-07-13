#pragma once

#include <cstdint>
#include <variant>

namespace ruvia::detail {

enum class Http2ConnectForm : std::uint8_t {
    kStandard,
    kExtended
};

class Http2TunnelState;

class Http2NotConnect final {
private:
    friend class Http2TunnelState;

    constexpr Http2NotConnect() noexcept = default;
};

// The CONNECT form matters while validating/dispatching the request and choosing
// its dedicated acceptance path. After a final response, :protocol on the retained
// request state is the authoritative Extended CONNECT signal; rejected responses
// resume ordinary HTTP response-body semantics. Consequently, only pending owns a
// form payload.
class Http2ConnectPending final {
public:
    [[nodiscard]] constexpr Http2ConnectForm form() const noexcept {
        return form_;
    }

private:
    friend class Http2TunnelState;

    explicit constexpr Http2ConnectPending(Http2ConnectForm form) noexcept
        : form_(form) {}

    Http2ConnectForm form_;
};

class Http2TunnelOpen final {
private:
    friend class Http2TunnelState;

    constexpr Http2TunnelOpen() noexcept = default;
};

class Http2ConnectRejected final {
private:
    friend class Http2TunnelState;

    constexpr Http2ConnectRejected() noexcept = default;
};

// CONNECT is exactly one of four protocol phases. This representation cannot form
// the former kind=None/phase=Open or kind=Extended/phase=None combinations.
class Http2TunnelState final {
public:
    constexpr Http2TunnelState() noexcept
        : state_(Http2NotConnect()) {}

    [[nodiscard]] bool begin(Http2ConnectForm form) noexcept {
        if ((form != Http2ConnectForm::kStandard &&
             form != Http2ConnectForm::kExtended) ||
            notConnect() == nullptr) {
            return false;
        }
        state_ = State(Http2ConnectPending(form));
        return true;
    }

    [[nodiscard]] bool accept() noexcept {
        if (pending() == nullptr) {
            return false;
        }
        state_ = State(Http2TunnelOpen());
        return true;
    }

    [[nodiscard]] bool reject() noexcept {
        if (pending() == nullptr) {
            return false;
        }
        state_ = State(Http2ConnectRejected());
        return true;
    }

    [[nodiscard]] constexpr const Http2NotConnect*
    notConnect() const noexcept {
        return std::get_if<Http2NotConnect>(&state_);
    }

    [[nodiscard]] constexpr const Http2ConnectPending*
    pending() const noexcept {
        return std::get_if<Http2ConnectPending>(&state_);
    }

    [[nodiscard]] constexpr const Http2TunnelOpen*
    open() const noexcept {
        return std::get_if<Http2TunnelOpen>(&state_);
    }

    [[nodiscard]] constexpr const Http2ConnectRejected*
    rejected() const noexcept {
        return std::get_if<Http2ConnectRejected>(&state_);
    }

private:
    using State = std::variant<
        Http2NotConnect,
        Http2ConnectPending,
        Http2TunnelOpen,
        Http2ConnectRejected>;

    State state_;
};

}  // namespace ruvia::detail
