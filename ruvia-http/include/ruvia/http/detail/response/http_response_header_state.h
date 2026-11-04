#pragma once

#include <cstdint>

#include "ruvia/http/http_response.h"

namespace ruvia::detail {

struct http_response_header_state_access final {
    [[nodiscard]] static std::uint32_t known_bits(const http_response_headers& headers) noexcept {
        return headers.known_header_bits_;
    }

    [[nodiscard]] static std::string_view known_header_value(
        const http_response_headers& headers, std::uint32_t bit) noexcept {
        return headers.known_header_value(bit);
    }
};

struct http_response_header_access final {
    [[nodiscard]] static constexpr http_response_header make(const char* bytes_value, std::uint32_t name_size,
        std::uint32_t value_size, std::uint32_t known_bit, bool owned) noexcept {
        http_response_header header_value{};
        header_value.bytes_ = bytes_value;
        header_value.name_size_ = name_size;
        header_value.value_size_ = value_size;
        header_value.known_bit_ = known_bit;
        header_value.owned_ = owned;
        header_value.append_ = false;
        return header_value;
    }

    [[nodiscard]] static char* value_begin(http_response_header& header_value) noexcept {
        if (header_value.bytes_ == nullptr) {
            return nullptr;
        }
        return const_cast<char*>(header_value.bytes_) + header_value.name_size_;
    }

    [[nodiscard]] static char* value_end(http_response_header& header_value) noexcept {
        auto* const begin = value_begin(header_value);
        return begin == nullptr ? nullptr : begin + header_value.value_size_;
    }

    [[nodiscard]] static std::uint32_t known_bit(const http_response_header& header_value) noexcept {
        return header_value.known_bit_;
    }

    [[nodiscard]] static bool append(const http_response_header& header_value) noexcept {
        return header_value.append_;
    }

    static void set_append(http_response_header& header_value) noexcept {
        header_value.append_ = true;
    }
};

[[nodiscard]] inline std::uint32_t response_known_header_bits(const http_response& response) noexcept {
    return http_response_header_state_access::known_bits(response.headers());
}

[[nodiscard]] inline bool response_has_known_header(
    const http_response_headers& headers, std::uint32_t bit) noexcept {
    return (http_response_header_state_access::known_bits(headers) & bit) != 0;
}

[[nodiscard]] inline bool response_has_known_header(
    const http_response& response, std::uint32_t bit) noexcept {
    return response_has_known_header(response.headers(), bit);
}

[[nodiscard]] inline std::string_view response_known_header_value(
    const http_response_headers& headers, std::uint32_t bit) noexcept {
    return http_response_header_state_access::known_header_value(headers, bit);
}

[[nodiscard]] inline std::string_view response_known_header_value(
    const http_response& response, std::uint32_t bit) noexcept {
    return response_known_header_value(response.headers(), bit);
}

}  // namespace ruvia::detail
