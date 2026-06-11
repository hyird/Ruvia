#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia {

class HttpRequest;
struct HttpRequestFlags;

namespace detail {

[[nodiscard]] std::pmr::string webSocketAccept(std::string_view key, std::pmr::memory_resource* resource);
[[nodiscard]] bool isValidWebSocketRequest(const HttpRequest& request, const HttpRequestFlags& flags) noexcept;
[[nodiscard]] bool isValidWebSocketCloseCode(std::uint16_t code) noexcept;
[[nodiscard]] bool isValidUtf8(std::string_view value) noexcept;
[[nodiscard]] std::string_view chooseWebSocketSubprotocol(
    const HttpRequest& request,
    const HttpRequestFlags& flags,
    std::string_view supported) noexcept;

}  // namespace detail
}  // namespace ruvia
