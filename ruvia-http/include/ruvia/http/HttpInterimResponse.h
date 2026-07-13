#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "ruvia/http/HttpHeader.h"

namespace ruvia {

// Immutable, bodyless response-head view for 1xx progress messages that precede
// a final response. 101 is deliberately excluded: switching protocols transfers
// connection ownership and therefore requires its dedicated protocol driver.
// Header elements and their strings are borrowed and must remain stable through
// the synchronous sans-I/O submit call.
class HttpInterimResponseHead final {
public:
    class HeaderInit final {
    public:
        constexpr HeaderInit() noexcept
            : headers_() {}

        constexpr HeaderInit(std::span<const HttpHeaderView> headers) noexcept
            : headers_(headers) {}

        template <std::size_t N>
        constexpr HeaderInit(const HttpHeaderView (&headers)[N]) noexcept
            : headers_(headers, N) {}

        template <std::size_t N>
        constexpr HeaderInit(const std::array<HttpHeaderView, N>& headers) noexcept
            : headers_(headers.data(), headers.size()) {}

        template <std::size_t N>
        HeaderInit(std::array<HttpHeaderView, N>&&) = delete;

        template <typename Allocator>
        HeaderInit(const std::vector<HttpHeaderView, Allocator>&) = delete;

        constexpr HeaderInit(std::initializer_list<HttpHeaderView>) = delete;

        [[nodiscard]] constexpr operator std::span<const HttpHeaderView>() const noexcept {
            return headers_;
        }

    private:
        std::span<const HttpHeaderView> headers_;
    };

    explicit HttpInterimResponseHead(
        std::uint16_t statusCode,
        HeaderInit headers = {});

    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return statusCode_;
    }

    [[nodiscard]] constexpr std::span<const HttpHeaderView> headers() const noexcept {
        return headers_;
    }

private:
    std::uint16_t statusCode_;
    std::span<const HttpHeaderView> headers_;
};

}  // namespace ruvia
