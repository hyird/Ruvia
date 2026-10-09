#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "http2/http2_local_settings.h"

namespace ruvia::detail {

class http2_stream_flow_control final {
public:
    void set_send_window(std::int32_t window) noexcept {
        send_window_ = window;
    }

    [[nodiscard]] std::int32_t send_window() const noexcept {
        return send_window_;
    }

    [[nodiscard]] bool add_send_window(std::int64_t delta) noexcept {
        const auto updated = static_cast<std::int64_t>(send_window_) + delta;
        if (!std::in_range<std::int32_t>(updated)) {
            return false;
        }
        send_window_ = static_cast<std::int32_t>(updated);
        return true;
    }

    void consume_send(std::size_t bytes_value) noexcept {
        send_window_ -= static_cast<std::int32_t>(bytes_value);
    }

    [[nodiscard]] bool consume_receive(std::int32_t bytes_value) noexcept {
        if (bytes_value > receive_window_) {
            return false;
        }
        receive_window_ -= bytes_value;
        return true;
    }

    [[nodiscard]] std::int32_t receive_window() const noexcept {
        return receive_window_;
    }

    void restore_receive(std::int32_t bytes_value) noexcept {
        receive_window_ += bytes_value;
    }

private:
    std::int32_t send_window_{http2_default_initial_window_size};
    std::int32_t receive_window_{static_cast<std::int32_t>(http2_local_settings::initial_window_size)};
};

}  // namespace ruvia::detail
