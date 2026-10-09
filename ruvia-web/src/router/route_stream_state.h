#pragma once

namespace ruvia::detail {

// One chain-owned bit records that `next()` reached the stream/websocket
// handler even when it emitted no bytes. route_table is the sole mutator; Next
// carries only a typed pointer to this state, so continuation dispatch has no
// erased outcome channel or extra request-time storage.
class stream_middleware_chain_state final {
public:
    [[nodiscard]] constexpr bool handler_invoked() const noexcept {
        return handler_invoked_;
    }

private:
    friend class route_table;

    constexpr stream_middleware_chain_state() noexcept = default;

    constexpr void mark_handler_invoked() noexcept {
        handler_invoked_ = true;
    }

    bool handler_invoked_{false};
};

static_assert(sizeof(stream_middleware_chain_state) == sizeof(bool));

}  // namespace ruvia::detail
