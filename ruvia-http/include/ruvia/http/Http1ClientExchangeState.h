#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/Http1ClosePolicy.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"

namespace ruvia {

class PreparedHttp1ClientRequest;

namespace detail {

struct Http1ClientRequestPrepareResultAccess;

enum class Http1ClientInitialContentState : std::uint8_t {
    kComplete,
    kPending,
    kAwaitingContinue,
};

struct Http1ClientExchangeStateAccess;

}  // namespace detail

// Owning protocol facts for exactly one HTTP/1 response exchange. Preparing a
// request creates this state from the facts that actually entered the wire
// plan; transferring it to the response parser removes every dependency on the
// caller's method and header storage. Only an offered Upgrade value needs owned
// dynamic storage, so ordinary requests remain allocation-free.
class Http1ClientExchangeState final {
public:
    Http1ClientExchangeState(const Http1ClientExchangeState&) = delete;
    Http1ClientExchangeState& operator=(const Http1ClientExchangeState&) = delete;
    Http1ClientExchangeState(Http1ClientExchangeState&&) noexcept = default;
    Http1ClientExchangeState& operator=(Http1ClientExchangeState&&) = delete;

private:
    friend class PreparedHttp1ClientRequest;
    friend struct detail::Http1ClientRequestPrepareResultAccess;
    friend struct detail::Http1ClientExchangeStateAccess;

    Http1ClientExchangeState(
        const Http1ClientExchangeState& other, std::pmr::memory_resource* resource)
        : offeredUpgradeProtocols_(other.offeredUpgradeProtocols_, resource),
          method_(other.method_),
          connectionOptions_(other.connectionOptions_),
          closePolicy_(other.closePolicy_),
          contentState_(other.contentState_) {}

    Http1ClientExchangeState(HttpKnownMethod method,
        detail::HttpConnectionOptions connectionOptions, Http1ClosePolicy closePolicy,
        detail::Http1ClientInitialContentState contentState,
        std::pmr::string offeredUpgradeProtocols) noexcept
        : offeredUpgradeProtocols_(std::move(offeredUpgradeProtocols)),
          method_(method),
          connectionOptions_(connectionOptions),
          closePolicy_(closePolicy),
          contentState_(contentState) {}

    std::pmr::string offeredUpgradeProtocols_;
    HttpKnownMethod method_{HttpKnownMethod::kUnknown};
    detail::HttpConnectionOptions connectionOptions_;
    Http1ClosePolicy closePolicy_{Http1ClosePolicy::kAllowReuse};
    detail::Http1ClientInitialContentState contentState_{
        detail::Http1ClientInitialContentState::kComplete};
};

namespace detail {

struct Http1ClientExchangeStateAccess final {
    [[nodiscard]] static constexpr HttpKnownMethod method(
        const Http1ClientExchangeState& state) noexcept {
        return state.method_;
    }

    [[nodiscard]] static constexpr HttpConnectionOptions connectionOptions(
        const Http1ClientExchangeState& state) noexcept {
        return state.connectionOptions_;
    }

    [[nodiscard]] static constexpr Http1ClosePolicy closePolicy(
        const Http1ClientExchangeState& state) noexcept {
        return state.closePolicy_;
    }

    [[nodiscard]] static constexpr Http1ClientInitialContentState contentState(
        const Http1ClientExchangeState& state) noexcept {
        return state.contentState_;
    }

    [[nodiscard]] static std::string_view offeredUpgradeProtocols(
        const Http1ClientExchangeState& state) noexcept {
        return state.offeredUpgradeProtocols_;
    }
};

}  // namespace detail

}  // namespace ruvia
