#include "ruvia/core/detail/io/IpAddress.h"

#include <array>
#include <cstring>
#include <memory_resource>
#include <string>

namespace ruvia::detail {

std::expected<asio::ip::address, std::error_code> parseIpAddress(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    std::error_code error;
    std::array<char, 64> terminated;
    const auto address = [&] {
        if (text.size() < terminated.size()) {
            if (!text.empty()) {
                std::memcpy(terminated.data(), text.data(), text.size());
            }
            terminated[text.size()] = '\0';
            return asio::ip::make_address(terminated.data(), error);
        }
        const std::pmr::string owned(text);
        return asio::ip::make_address(owned.c_str(), error);
    }();
    if (error) {
        return std::unexpected(error);
    }
    return address;
}

}  // namespace ruvia::detail
