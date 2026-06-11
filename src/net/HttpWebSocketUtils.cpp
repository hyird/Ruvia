#include "HttpWebSocketUtils.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>

#include "ruvia/http/HeaderUtils.h"
#include "ruvia/http/HttpParser.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

namespace {

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

[[nodiscard]] bool webSocketHeaderEquals(std::string_view value, std::string_view expected) noexcept {
    return detail::httpAsciiEqualsIgnoreCase(detail::httpTrimOws(value), expected);
}

[[nodiscard]] std::optional<std::uint8_t> base64Value(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<std::uint8_t>(c - 'A');
    }
    if (c >= 'a' && c <= 'z') {
        return static_cast<std::uint8_t>(26 + c - 'a');
    }
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(52 + c - '0');
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> decodeWebSocketKey(std::string_view key) noexcept {
    key = detail::httpTrimOws(key);
    if (key.size() != 24) {
        return std::nullopt;
    }

    std::array<std::uint8_t, 18> decoded{};
    std::size_t out = 0;
    for (std::size_t i = 0; i < key.size(); i += 4) {
        std::array<std::uint8_t, 4> values{};
        std::size_t padding = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const auto ch = key[i + j];
            if (ch == '=') {
                values[j] = 0;
                ++padding;
                continue;
            }
            if (padding != 0) {
                return std::nullopt;
            }
            const auto value = base64Value(ch);
            if (!value) {
                return std::nullopt;
            }
            values[j] = *value;
        }
        if (padding > 2 || (padding != 0 && i + 4 != key.size())) {
            return std::nullopt;
        }
        const auto triple =
            (static_cast<std::uint32_t>(values[0]) << 18) |
            (static_cast<std::uint32_t>(values[1]) << 12) |
            (static_cast<std::uint32_t>(values[2]) << 6) |
            static_cast<std::uint32_t>(values[3]);
        if (out < decoded.size()) {
            decoded[out++] = static_cast<std::uint8_t>((triple >> 16) & 0xFF);
        }
        if (padding < 2 && out < decoded.size()) {
            decoded[out++] = static_cast<std::uint8_t>((triple >> 8) & 0xFF);
        }
        if (padding < 1 && out < decoded.size()) {
            decoded[out++] = static_cast<std::uint8_t>(triple & 0xFF);
        }
    }
    if (out != 16) {
        return std::nullopt;
    }

    std::array<std::uint8_t, 16> nonce{};
    std::copy_n(decoded.begin(), nonce.size(), nonce.begin());
    return nonce;
}

void appendBase64(std::pmr::string& output, std::span<const std::uint8_t> input) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::size_t i = 0;
    while (i + 3 <= input.size()) {
        const auto value =
            (static_cast<std::uint32_t>(input[i]) << 16) |
            (static_cast<std::uint32_t>(input[i + 1]) << 8) |
            static_cast<std::uint32_t>(input[i + 2]);
        output.push_back(table[(value >> 18) & 0x3F]);
        output.push_back(table[(value >> 12) & 0x3F]);
        output.push_back(table[(value >> 6) & 0x3F]);
        output.push_back(table[value & 0x3F]);
        i += 3;
    }
    if (i == input.size()) {
        return;
    }
    const auto remaining = input.size() - i;
    const auto value = static_cast<std::uint32_t>(input[i]) << 16 |
        (remaining == 2 ? static_cast<std::uint32_t>(input[i + 1]) << 8 : 0U);
    output.push_back(table[(value >> 18) & 0x3F]);
    output.push_back(table[(value >> 12) & 0x3F]);
    output.push_back(remaining == 2 ? table[(value >> 6) & 0x3F] : '=');
    output.push_back('=');
}

[[nodiscard]] std::uint32_t sha1RotateLeft(std::uint32_t value, std::uint32_t bits) noexcept {
    return (value << bits) | (value >> (32 - bits));
}

[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::string_view input) noexcept {
    std::uint32_t h0 = 0x67452301U;
    std::uint32_t h1 = 0xEFCDAB89U;
    std::uint32_t h2 = 0x98BADCFEU;
    std::uint32_t h3 = 0x10325476U;
    std::uint32_t h4 = 0xC3D2E1F0U;
    std::array<std::uint8_t, 64> block{};
    std::uint64_t totalBits = static_cast<std::uint64_t>(input.size()) * 8U;
    std::size_t offset = 0;

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

    while (input.size() - offset >= block.size()) {
        for (std::size_t i = 0; i < block.size(); ++i) {
            block[i] = static_cast<std::uint8_t>(input[offset + i]);
        }
        process(block);
        offset += block.size();
    }
    block.fill(0);
    const auto remaining = input.size() - offset;
    for (std::size_t i = 0; i < remaining; ++i) {
        block[i] = static_cast<std::uint8_t>(input[offset + i]);
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

[[nodiscard]] bool listContainsToken(std::string_view list, std::string_view token) noexcept {
    while (!list.empty()) {
        const auto comma = list.find(',');
        auto item = detail::httpTrimOws(comma == std::string_view::npos ? list : list.substr(0, comma));
        if (item == token) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }
    return false;
}

[[nodiscard]] bool protocolOffered(
    const HttpRequest& request,
    const HttpRequestFlags& flags,
    std::string_view protocol) noexcept {
    if (listContainsToken(request.header(HttpRequest::KnownHeader::kSecWebSocketProtocol), protocol)) {
        return true;
    }
    if (flags.secWebSocketProtocolCount <= 1) {
        return false;
    }

    for (const auto& header : request.headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name, "Sec-WebSocket-Protocol") &&
            listContainsToken(header.value, protocol)) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::pmr::string webSocketAccept(std::string_view key, std::pmr::memory_resource* resource) {
    std::pmr::string material(resource);
    material.append(detail::httpTrimOws(key));
    material.append(kWebSocketGuid);
    const auto digest = sha1(material);
    std::pmr::string accept(resource);
    appendBase64(accept, std::span<const std::uint8_t>(digest.data(), digest.size()));
    return accept;
}

bool isValidWebSocketRequest(const HttpRequest& request, const HttpRequestFlags& flags) noexcept {
    return request.method() == HttpMethod::kGet &&
        request.httpVersion() == "HTTP/1.1" &&
        webSocketHeaderEquals(request.header(HttpRequest::KnownHeader::kUpgrade), "websocket") &&
        flags.upgrade &&
        flags.secWebSocketKeyCount == 1 &&
        flags.secWebSocketVersionCount == 1 &&
        webSocketHeaderEquals(request.header(HttpRequest::KnownHeader::kSecWebSocketVersion), "13") &&
        decodeWebSocketKey(request.header(HttpRequest::KnownHeader::kSecWebSocketKey)).has_value();
}

bool isValidWebSocketCloseCode(std::uint16_t code) noexcept {
    if (code == 1000 || code == 1001 || code == 1002 || code == 1003 ||
        code == 1007 || code == 1008 || code == 1009 || code == 1010 || code == 1011) {
        return true;
    }
    return code >= 3000 && code <= 4999;
}

bool isValidUtf8(std::string_view value) noexcept {
    std::uint32_t codepoint = 0;
    std::uint32_t remaining = 0;
    std::uint32_t minValue = 0;
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (remaining == 0) {
            if (byte <= 0x7F) {
                continue;
            }
            if (byte >= 0xC2 && byte <= 0xDF) {
                codepoint = byte & 0x1FU;
                remaining = 1;
                minValue = 0x80;
            } else if (byte >= 0xE0 && byte <= 0xEF) {
                codepoint = byte & 0x0FU;
                remaining = 2;
                minValue = 0x800;
            } else if (byte >= 0xF0 && byte <= 0xF4) {
                codepoint = byte & 0x07U;
                remaining = 3;
                minValue = 0x10000;
            } else {
                return false;
            }
        } else {
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6) | (byte & 0x3FU);
            --remaining;
            if (remaining == 0 &&
                (codepoint < minValue || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))) {
                return false;
            }
        }
    }
    return remaining == 0;
}

std::string_view chooseWebSocketSubprotocol(
    const HttpRequest& request,
    const HttpRequestFlags& flags,
    std::string_view supported) noexcept {
    while (!supported.empty()) {
        const auto comma = supported.find(',');
        auto token = detail::httpTrimOws(comma == std::string_view::npos ? supported : supported.substr(0, comma));
        if (!token.empty() && protocolOffered(request, flags, token)) {
            return token;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        supported.remove_prefix(comma + 1);
    }
    return {};
}

}  // namespace ruvia::detail
