#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string_view>

namespace ruvia {

// An HTTP protocol primitive rejected input with a response status that a
// server driver may map onto its own application error model. The diagnostic is
// copied into fixed exception storage, so reporting never allocates or borrows.
class HttpProtocolError final : public std::exception {
public:
    HttpProtocolError(std::uint16_t status, std::string_view message) noexcept
        : status_(status) {
        const auto size = (std::min)(message.size(), message_.size() - 1);
        if (size != 0) {
            std::memcpy(message_.data(), message.data(), size);
        }
        message_[size] = '\0';
    }

    [[nodiscard]] std::uint16_t status() const noexcept {
        return status_;
    }

    [[nodiscard]] const char* what() const noexcept override {
        return message_.data();
    }

private:
    std::uint16_t status_;
    std::array<char, 128> message_{};
};

}  // namespace ruvia
