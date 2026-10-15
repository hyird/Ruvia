#pragma once

#include <cstdint>
#include <type_traits>

#include "ruvia/core/task.h"
#include "ruvia/http/http2_connection.h"

#include "http2/http2_sans_io_stream_signal.h"

namespace ruvia::detail {

class http2_send_window_ready final {
private:
    constexpr http2_send_window_ready() noexcept = default;
    friend class http2_send_window_wait_result;
};

class http2_send_window_aborted final {
private:
    constexpr http2_send_window_aborted() noexcept = default;
    friend class http2_send_window_wait_result;
};

class http2_send_window_wait_result final {
public:
    [[nodiscard]] static constexpr http2_send_window_wait_result make_ready() noexcept {
        return http2_send_window_wait_result(state_type::ready);
    }

    [[nodiscard]] static constexpr http2_send_window_wait_result make_aborted() noexcept {
        return http2_send_window_wait_result(state_type::aborted);
    }

    [[nodiscard]] constexpr const http2_send_window_ready* ready() const& noexcept {
        return state_ == state_type::ready ? &ready_value : nullptr;
    }
    const http2_send_window_ready* ready() const&& = delete;

    [[nodiscard]] constexpr const http2_send_window_aborted* aborted() const& noexcept {
        return state_ == state_type::aborted ? &aborted_value : nullptr;
    }
    const http2_send_window_aborted* aborted() const&& = delete;

private:
    enum class state_type : std::uint8_t { ready,
        aborted };

    explicit constexpr http2_send_window_wait_result(state_type state_value) noexcept
        : state_(state_value) {}

    static inline constexpr http2_send_window_ready ready_value{};
    static inline constexpr http2_send_window_aborted aborted_value{};

    state_type state_;
};

static_assert(std::is_trivially_copyable_v<http2_send_window_wait_result>);
static_assert(sizeof(http2_send_window_wait_result) <= 2);

// Wait until the HTTP core no longer owns queued DATA for this stream. The
// Web-owned signal is only a wakeup edge; stream existence, abort, session end,
// and queue state are re-checked together after every wake.
[[nodiscard]] task<http2_send_window_wait_result> await_http2_send_window(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    http2_sans_io_stream_signal* signal);

// Existing Web protocol drivers own the sans-I/O connection directly. Keep this
// HTTP-target-private overload generic over its semantic connection queries so
// those callers need not expose or alias the internal connection type here.
template <typename connection_type>
[[nodiscard]] task<http2_send_window_wait_result> await_http2_send_window(
    connection_type& connection, std::uint32_t stream_id, http2_sans_io_stream_signal* signal) {
    for (;;) {
        const auto status = connection.stream_receive_status(stream_id);
        if (status == ruvia::http2_stream_receive_status::closed || signal == nullptr ||
            signal->terminated()) {
            co_return http2_send_window_wait_result::make_aborted();
        }
        if (!connection.has_queued_data(stream_id)) {
            co_return http2_send_window_wait_result::make_ready();
        }
        co_await signal->wait();
    }
}

}  // namespace ruvia::detail
