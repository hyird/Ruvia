#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"

#include <array>
#include <span>

#include "ruvia/http/detail/HttpBase64.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"

namespace ruvia::detail {
namespace {

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

[[nodiscard]] std::uint32_t sha1RotateLeft(std::uint32_t value, std::uint32_t bits) noexcept {
    return (value << bits) | (value >> (32 - bits));
}

[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::string_view first, std::string_view second) noexcept {
    std::uint32_t h0 = 0x67452301U;
    std::uint32_t h1 = 0xEFCDAB89U;
    std::uint32_t h2 = 0x98BADCFEU;
    std::uint32_t h3 = 0x10325476U;
    std::uint32_t h4 = 0xC3D2E1F0U;
    std::array<std::uint8_t, 64> block{};
    const auto totalSize = first.size() + second.size();
    const std::uint64_t totalBits = static_cast<std::uint64_t>(totalSize) * 8U;
    std::size_t offset = 0;

    const auto byteAt = [first, second](std::size_t index) noexcept {
        return index < first.size()
            ? static_cast<std::uint8_t>(first[index])
            : static_cast<std::uint8_t>(second[index - first.size()]);
    };

    const auto process = [&](const std::array<std::uint8_t, 64>& data) noexcept {
        std::array<std::uint32_t, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(data[i * 4]) << 24) |
                (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16) |
                (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8) |
                static_cast<std::uint32_t>(data[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = sha1RotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        auto a = h0;
        auto b = h1;
        auto c = h2;
        auto d = h3;
        auto e = h4;
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const auto temp = sha1RotateLeft(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = sha1RotateLeft(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    };

    while (totalSize - offset >= block.size()) {
        for (std::size_t i = 0; i < block.size(); ++i) {
            block[i] = byteAt(offset + i);
        }
        process(block);
        offset += block.size();
    }
    block.fill(0);
    const auto remaining = totalSize - offset;
    for (std::size_t i = 0; i < remaining; ++i) {
        block[i] = byteAt(offset + i);
    }
    block[remaining] = 0x80;
    if (remaining >= 56) {
        process(block);
        block.fill(0);
    }
    for (std::size_t i = 0; i < 8; ++i) {
        block[63 - i] = static_cast<std::uint8_t>((totalBits >> (i * 8)) & 0xFF);
    }
    process(block);

    std::array<std::uint8_t, 20> digest{};
    const std::array words{h0, h1, h2, h3, h4};
    for (std::size_t i = 0; i < words.size(); ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((words[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((words[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((words[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(words[i] & 0xFF);
    }
    return digest;
}

}  // namespace

void encodeWebSocketAccept(WebSocketAcceptKey& output, std::string_view key) {
    key = detail::httpTrimOws(key);
    const auto digest = sha1(key, kWebSocketGuid);
    encodeHttpBase64(output.data(), std::span<const std::uint8_t>(digest.data(), digest.size()));
}

}  // namespace ruvia::detail
