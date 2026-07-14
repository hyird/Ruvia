#pragma once

namespace ruvia::detail {

// One chain-owned bit records that `next()` reached the stream/WebSocket
// handler even when it emitted no bytes. RouteTable is the sole mutator; Next
// carries only a typed pointer to this state, so continuation dispatch has no
// erased outcome channel or extra request-time storage.
class StreamMiddlewareChainState final {
public:
    [[nodiscard]] constexpr bool handlerInvoked() const noexcept {
        return handlerInvoked_;
    }

private:
    friend class RouteTable;

    constexpr StreamMiddlewareChainState() noexcept = default;

    constexpr void markHandlerInvoked() noexcept {
        handlerInvoked_ = true;
    }

    bool handlerInvoked_{false};
};

static_assert(sizeof(StreamMiddlewareChainState) == sizeof(bool));

}  // namespace ruvia::detail
