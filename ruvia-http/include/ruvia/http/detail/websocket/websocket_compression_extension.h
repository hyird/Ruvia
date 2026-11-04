#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "ruvia/http/websocket_protocol.h"

namespace ruvia::detail {

class websocket_compression_extension final {
public:
    explicit constexpr websocket_compression_extension(websocket_compression compression) noexcept {
        if (!compression.enabled_) {
            return;
        }
        append("permessage-deflate");
        if (compression.server_no_context_takeover_) {
            append("; server_no_context_takeover");
        }
        if (compression.client_no_context_takeover_) {
            append("; client_no_context_takeover");
        }
        if (compression.server_max_window_bits_) {
            append("; server_max_window_bits=");
            number(*compression.server_max_window_bits_);
        }
        if (compression.client_max_window_bits_) {
            append("; client_max_window_bits=");
            number(*compression.client_max_window_bits_);
        }
    }
    [[nodiscard]] constexpr std::string_view view() const& noexcept {
        return {bytes_.data(), size_};
    }
    std::string_view view() const&& = delete;
    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }

private:
    constexpr void append(std::string_view text) noexcept {
        for (char ch : text) {
            bytes_[size_++] = ch;
        }
    }
    constexpr void number(int value) noexcept {
        if (value >= 10) {
            bytes_[size_++] = '1';
        }
        bytes_[size_++] = static_cast<char>('0' + value % 10);
    }
    std::array<char, 160> bytes_{};
    std::size_t size_{0};
};

}  // namespace ruvia::detail
