#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace ruvia::detail {

// Allocation-free RFC 6455 subprotocol set. Semantic callers append one token
// at a time; wire parsers use appendList(), which permits RFC list null
// elements while still requiring every actual element to be a unique token.
class WebSocketSubprotocolSet final {
public:
    [[nodiscard]] bool append(std::string_view protocol) noexcept {
        if (protocol.empty()) {
            return false;
        }
        for (const char character : protocol) {
            if (!isHttpTokenChar(static_cast<unsigned char>(character))) {
                return false;
            }
        }
        if (size_ == protocols_.size() || contains(protocol)) {
            return false;
        }
        protocols_[size_++] = protocol;
        return true;
    }

    [[nodiscard]] bool appendList(std::string_view value) noexcept {
        while (true) {
            const auto comma = value.find(',');
            const auto protocol = httpTrimOws(comma == std::string_view::npos ? value : value.substr(0, comma));
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
    // The request parser already caps the complete header field count at 64.
    // Reusing that bound keeps uniqueness validation deterministic and stack-
    // only for both incoming offers and configured client preferences.
    std::array<std::string_view, kMaxHttpHeaderFields> protocols_{};
    std::size_t size_{0};
};

}  // namespace ruvia::detail
