#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"

namespace ruvia {

// Immutable, bodyless response-head view for 1xx progress messages that precede
// a final response. 101 is deliberately excluded: switching protocols transfers
// connection ownership and therefore requires its dedicated protocol driver.
// Header elements and their strings are borrowed and must remain stable through
// the synchronous sans-I/O submit call.
class http_interim_response_head final {
public:
    class header_init_type final {
    public:
        constexpr header_init_type() noexcept
            : headers_() {}

        constexpr header_init_type(std::span<const http_header_view> headers) noexcept
            : headers_(headers) {}

        template <std::size_t n>
        constexpr header_init_type(const http_header_view (&headers)[n]) noexcept
            : headers_(headers, n) {}

        template <std::size_t n>
        constexpr header_init_type(const std::array<http_header_view, n>& headers) noexcept
            : headers_(headers) {}

        template <std::size_t n>
        header_init_type(std::array<http_header_view, n>&&) = delete;

        template <typename allocator_type>
        header_init_type(const std::vector<http_header_view, allocator_type>&) = delete;

        constexpr header_init_type(std::initializer_list<http_header_view>) = delete;

        [[nodiscard]] constexpr operator std::span<const http_header_view>() const noexcept {
            return headers_;
        }

    private:
        std::span<const http_header_view> headers_;
    };

    explicit http_interim_response_head(http_status_code status_code, header_init_type headers = {});

    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_code_;
    }

    [[nodiscard]] constexpr std::span<const http_header_view> headers() const noexcept {
        return headers_;
    }

private:
    http_status_code status_code_;
    std::span<const http_header_view> headers_;
};

}  // namespace ruvia
