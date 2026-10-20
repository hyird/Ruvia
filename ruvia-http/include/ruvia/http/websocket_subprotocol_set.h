#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_field_whitespace.h"
#include "ruvia/http/http_limits.h"

namespace ruvia {

// Allocation-free validated RFC 6455 subprotocol list, suitable for server
// route configuration and parsing offered protocol lists. Capacity is a product
// limit of max_http_header_fields distinct protocols across all list elements;
// a handshake offering more is rejected as invalid rather than truncated.
class websocket_subprotocol_set final {
public:
    [[nodiscard]] bool append(std::string_view protocol) noexcept {
        if (!is_valid_http_header_name(protocol) || size_ == protocols_.size() || contains(protocol)) {
            return false;
        }
        protocols_[size_++] = protocol;
        return true;
    }

    [[nodiscard]] bool append_list(std::string_view value) noexcept {
        while (true) {
            const auto comma = value.find(',');
            const auto protocol = http_trim_ows(
                comma == std::string_view::npos ? value : value.substr(0, comma));
            if (!protocol.empty() && !append(protocol)) {
                return false;
            }
            if (comma == std::string_view::npos) {
                return true;
            }
            value.remove_prefix(comma + 1);
        }
    }

    [[nodiscard]] bool contains(std::string_view protocol) const noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            if (protocols_[i] == protocol) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

private:
    std::array<std::string_view, max_http_header_fields> protocols_{};
    std::size_t size_{0};
};

}  // namespace ruvia
