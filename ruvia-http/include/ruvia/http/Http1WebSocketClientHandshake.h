#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/Http1ClientRequestWriter.h"

namespace ruvia {

class Http1ParsedClientResponseHead;

struct Http1WebSocketClientHandshakeConfigView final {
    // Supply fresh cryptographically random bytes for each opening handshake.
    std::span<const std::uint8_t, 16> nonce;
    std::span<const HttpHeaderView> headers{};
    std::span<const std::string_view> subprotocols{};
    std::string_view userAgent{};
};

enum class Http1WebSocketClientHandshakeError : std::uint8_t {
    kResponseStatus,
    kAccept,
    kUpgrade,
    kConnection,
    kSubprotocol,
    kExtensions,
};

struct Http1WebSocketClientHandshakeResultView final {
    // Borrows the parsed response head passed to validateResponse().
    std::string_view selectedSubprotocol{};
};

// Owns one opening handshake's key and configured fields in the supplied
// resource; configuration views need only survive construction. The resource
// must outlive this object and every prepared request's exchange state.
class Http1WebSocketClientHandshake final {
public:
    Http1WebSocketClientHandshake(Http1WebSocketClientHandshakeConfigView options,
        std::pmr::memory_resource* resource = nullptr);

    Http1WebSocketClientHandshake(const Http1WebSocketClientHandshake&) = delete;
    Http1WebSocketClientHandshake& operator=(const Http1WebSocketClientHandshake&) = delete;
    Http1WebSocketClientHandshake(Http1WebSocketClientHandshake&&) noexcept = default;
    Http1WebSocketClientHandshake& operator=(Http1WebSocketClientHandshake&&) = delete;

    static void validateConfiguration(
        std::span<const HttpHeaderView> headers,
        std::span<const std::string_view> subprotocols,
        std::string_view userAgent = {});

    [[nodiscard]] Http1ClientRequestPrepareResult prepareRequest(
        const HttpOriginView& origin, std::string_view target, std::span<char> headBuffer) const;

    [[nodiscard]] std::expected<Http1WebSocketClientHandshakeResultView,
        Http1WebSocketClientHandshakeError>
    validateResponse(const Http1ParsedClientResponseHead& response) const;

private:
    std::pmr::memory_resource* resource_;
    std::pmr::string key_;
    std::pmr::string subprotocolHeader_;
    std::pmr::vector<std::pair<std::size_t, std::size_t>> subprotocolRanges_;
    struct StoredHeader final {
        std::pmr::string name;
        std::pmr::string value;
        StoredHeader(std::string_view n, std::string_view v, std::pmr::memory_resource* r)
            : name(n, r),
              value(v, r) {}
    };
    std::pmr::vector<StoredHeader> headers_;
    std::pmr::string userAgent_;
};

}  // namespace ruvia
