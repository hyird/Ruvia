#pragma once

#include <cstdint>
#include <optional>

namespace ruvia::detail {

class Http2StreamRequestState final {
public:
    [[nodiscard]] bool hasProtocol() const noexcept {
        return hasProtocol_;
    }

    void markProtocol() noexcept {
        hasProtocol_ = true;
    }

    [[nodiscard]] bool hasScheme() const noexcept {
        return hasScheme_;
    }

    void markScheme(std::uint16_t defaultPort) noexcept {
        hasScheme_ = true;
        schemeDefaultPort_ = defaultPort;
    }

    [[nodiscard]] std::uint16_t schemeDefaultPort() const noexcept {
        return schemeDefaultPort_;
    }

    [[nodiscard]] bool hasAuthority() const noexcept {
        return hasAuthority_;
    }

    void markAuthority() noexcept {
        hasAuthority_ = true;
    }

    [[nodiscard]] bool hasPath() const noexcept {
        return hasPath_;
    }

    void markPath() noexcept {
        hasPath_ = true;
    }

    [[nodiscard]] bool hasHost() const noexcept {
        return hasHost_;
    }

    void markHost() noexcept {
        hasHost_ = true;
    }

    [[nodiscard]] bool hasCookie() const noexcept {
        return hasCookie_;
    }

    void markCookie() noexcept {
        hasCookie_ = true;
    }

    [[nodiscard]] bool regularHeaderSeen() const noexcept {
        return regularHeaderSeen_;
    }

    void markRegularHeaderSeen() noexcept {
        regularHeaderSeen_ = true;
    }

    [[nodiscard]] bool markSingletonHeader(std::uint32_t bit) noexcept {
        if ((singletonHeaderBits_ & bit) != 0) {
            return false;
        }
        singletonHeaderBits_ |= bit;
        return true;
    }

    // Client role: nullptr until the final response :status is committed once for
    // a stream this endpoint opened. The owner bounds preceding 1xx heads.
    [[nodiscard]] const std::uint16_t* responseStatus() const noexcept {
        return responseStatus_ ? &*responseStatus_ : nullptr;
    }

    [[nodiscard]] bool setResponseStatus(std::uint16_t status) noexcept {
        if (responseStatus_) {
            return false;
        }
        responseStatus_ = status;
        return true;
    }

    [[nodiscard]] std::uint8_t interimResponseCount() const noexcept {
        return interimResponses_;
    }

    void countInterimResponse() noexcept {
        ++interimResponses_;
    }

private:
    bool hasProtocol_ : 1 {false};
    bool hasScheme_ : 1 {false};
    bool hasAuthority_ : 1 {false};
    bool hasPath_ : 1 {false};
    bool hasHost_ : 1 {false};
    bool hasCookie_ : 1 {false};
    bool regularHeaderSeen_ : 1 {false};
    std::uint32_t singletonHeaderBits_{0};
    std::uint16_t schemeDefaultPort_{0};
    std::optional<std::uint16_t> responseStatus_;
    std::uint8_t interimResponses_{0};
};

}  // namespace ruvia::detail
