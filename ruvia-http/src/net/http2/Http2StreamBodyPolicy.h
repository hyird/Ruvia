#pragma once

#include <cstddef>
#include <cstdint>

namespace ruvia::detail {

enum class HttpRequestBodyMode : std::uint8_t {
    kBuffered,
    kStream
};

[[nodiscard]] inline std::size_t httpRequestBodyByteLimit(
    HttpRequestBodyMode bodyMode,
    std::size_t maxStreamBodyBytes,
    std::size_t maxBufferedBodyBytes) noexcept {
    return bodyMode == HttpRequestBodyMode::kStream ? maxStreamBodyBytes : maxBufferedBodyBytes;
}

class Http2StreamBodyPolicy final {
public:
    [[nodiscard]] HttpRequestBodyMode bodyMode() const noexcept {
        return bodyMode_;
    }

    [[nodiscard]] bool usesStreamRequestBody() const noexcept {
        return bodyMode_ == HttpRequestBodyMode::kStream;
    }

    void resetToBuffered() noexcept {
        bodyMode_ = HttpRequestBodyMode::kBuffered;
    }

    void setBodyMode(HttpRequestBodyMode bodyMode) noexcept {
        bodyMode_ = bodyMode;
    }

private:
    HttpRequestBodyMode bodyMode_{HttpRequestBodyMode::kBuffered};
};

}  // namespace ruvia::detail
