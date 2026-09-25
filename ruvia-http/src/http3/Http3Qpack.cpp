#include "ruvia/http/Http3Qpack.h"

#include <array>
#include <limits>
#include <string_view>

#include "ruvia/http/detail/http2/hpack/Http2HpackHuffmanTables.h"

namespace ruvia {
namespace {

using Entry = Http3QpackStaticEntry;
constexpr std::array<Entry, 99> kStaticTable{{
    {":authority", ""},
    {":path", "/"},
    {"age", "0"},
    {"content-disposition", ""},
    {"content-length", "0"},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"referer", ""},
    {"set-cookie", ""},
    {":method", "CONNECT"},
    {":method", "DELETE"},
    {":method", "GET"},
    {":method", "HEAD"},
    {":method", "OPTIONS"},
    {":method", "POST"},
    {":method", "PUT"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "103"},
    {":status", "200"},
    {":status", "304"},
    {":status", "404"},
    {":status", "503"},
    {"accept", "*/*"},
    {"accept", "application/dns-message"},
    {"accept-encoding", "gzip, deflate, br"},
    {"accept-ranges", "bytes"},
    {"access-control-allow-headers", "cache-control"},
    {"access-control-allow-headers", "content-type"},
    {"access-control-allow-origin", "*"},
    {"cache-control", "max-age=0"},
    {"cache-control", "max-age=3600"},
    {"cache-control", "no-cache"},
    {"cache-control", "no-store"},
    {"cache-control", "public, max-age=31536000"},
    {"content-encoding", "br"},
    {"content-encoding", "gzip"},
    {"content-type", "application/dns-message"},
    {"content-type", "application/javascript"},
    {"content-type", "application/json"},
    {"content-type", "application/x-www-form-urlencoded"},
    {"content-type", "image/gif"},
    {"content-type", "image/jpeg"},
    {"content-type", "image/png"},
    {"content-type", "text/css"},
    {"content-type", "text/html; charset=utf-8"},
    {"content-type", "text/plain"},
    {"content-type", "text/plain;charset=utf-8"},
    {"range", "bytes=0-"},
    {"strict-transport-security", "max-age=31536000"},
    {"strict-transport-security", "max-age=31536000; includesubdomains"},
    {"strict-transport-security", "max-age=31536000; includesubdomains; preload"},
    {"vary", "accept-encoding"},
    {"vary", "origin"},
    {"x-content-type-options", "nosniff"},
    {"x-xss-protection", "1; mode=block"},
    {":status", "100"},
    {":status", "204"},
    {":status", "206"},
    {":status", "302"},
    {":status", "308"},
    {":status", "400"},
    {":status", "403"},
    {":status", "421"},
    {":status", "425"},
    {":status", "500"},
    {"accept-language", ""},
    {"access-control-allow-credentials", "FALSE"},
    {"access-control-allow-credentials", "TRUE"},
    {"access-control-allow-headers", "*"},
    {"access-control-allow-methods", "get"},
    {"access-control-allow-methods", "get, post, options"},
    {"access-control-allow-methods", "options"},
    {"access-control-expose-headers", "content-length"},
    {"access-control-request-headers", "content-type"},
    {"access-control-request-method", "get"},
    {"access-control-request-method", "post"},
    {"alt-svc", "clear"},
    {"authorization", ""},
    {"content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"},
    {"early-data", "1"},
    {"expect-ct", ""},
    {"forwarded", ""},
    {"if-range", ""},
    {"origin", ""},
    {"purpose", "prefetch"},
    {"server", ""},
    {"timing-allow-origin", "*"},
    {"upgrade-insecure-requests", "1"},
    {"user-agent", ""},
    {"x-forwarded-for", ""},
    {"x-frame-options", "deny"},
    {"x-frame-options", "sameorigin"},
}};

struct HuffmanNode final {
    std::array<int, 2> child{-1, -1};
    int symbol{-1};
};

const auto& huffmanTree() {
    static const auto tree = [] {
        std::array<HuffmanNode, 513> result{};
        std::size_t nextNode = 1;
        for (std::size_t symbol = 0; symbol < detail::kHpackHuffmanCodes.size(); ++symbol) {
            int node = 0;
            const auto code = detail::kHpackHuffmanCodes[symbol];
            const auto length = detail::kHpackHuffmanLengths[symbol];
            for (std::uint8_t i = 0; i < length; ++i) {
                const auto bit = static_cast<std::size_t>((code >> (length - i - 1)) & 1U);
                auto& child = result[static_cast<std::size_t>(node)].child[bit];
                if (child < 0) {
                    child = static_cast<int>(nextNode++);
                }
                node = child;
            }
            result[static_cast<std::size_t>(node)].symbol = static_cast<int>(symbol);
        }
        return result;
    }();
    return tree;
}

bool decodeHuffman(std::string_view input, std::pmr::string& output) {
    output.clear();
    const auto& tree = huffmanTree();
    int node = 0;
    unsigned depth = 0;
    bool allOnes = true;
    for (const char byteValue : input) {
        const auto byte = static_cast<unsigned char>(byteValue);
        for (int bitIndex = 7; bitIndex >= 0; --bitIndex) {
            const auto bit = static_cast<std::size_t>((byte >> bitIndex) & 1U);
            node = tree[static_cast<std::size_t>(node)].child[bit];
            if (node < 0) {
                return false;
            }
            ++depth;
            allOnes = allOnes && bit == 1;
            const auto symbol = tree[static_cast<std::size_t>(node)].symbol;
            if (symbol >= 0) {
                if (symbol == 256) {
                    return false;
                }
                output.push_back(static_cast<char>(symbol));
                node = 0;
                depth = 0;
                allOnes = true;
            }
        }
    }
    return node == 0 || (depth <= 7 && allOnes);
}

}  // namespace

std::expected<Http3QpackStaticEntry, Http3QpackError> http3QpackStaticEntry(
    std::uint64_t index) noexcept {
    if (index >= kStaticTable.size()) {
        return std::unexpected(Http3QpackError::kInvalidIndex);
    }
    return kStaticTable[static_cast<std::size_t>(index)];
}

std::expected<Http3QpackInteger, Http3QpackError> decodeHttp3QpackInteger(
    std::span<const char> input, std::uint8_t prefixBits) noexcept {
    if (prefixBits == 0 || prefixBits > 8) {
        return std::unexpected(Http3QpackError::kIntegerOverflow);
    }
    if (input.empty()) {
        return std::unexpected(Http3QpackError::kNeedMoreData);
    }
    const auto mask = static_cast<std::uint8_t>((1U << prefixBits) - 1U);
    std::uint64_t value = static_cast<std::uint8_t>(input[0]) & mask;
    if (value < mask) {
        return Http3QpackInteger{value, 1};
    }
    unsigned shift = 0;
    std::size_t offset = 1;
    for (;;) {
        if (offset == input.size()) {
            return std::unexpected(Http3QpackError::kNeedMoreData);
        }
        const auto byte = static_cast<std::uint8_t>(input[offset++]);
        const auto payload = static_cast<std::uint64_t>(byte & 0x7fU);
        if (shift >= 64 || payload > (std::numeric_limits<std::uint64_t>::max() - value) >> shift) {
            return std::unexpected(Http3QpackError::kIntegerOverflow);
        }
        value += payload << shift;
        if ((byte & 0x80U) == 0) {
            return Http3QpackInteger{value, offset};
        }
        shift += 7;
    }
}

std::expected<std::size_t, Http3QpackError> encodeHttp3QpackInteger(std::span<char> output,
    std::uint8_t prefixBits, std::uint8_t prefix, std::uint64_t value) noexcept {
    if (prefixBits == 0 || prefixBits > 8) {
        return std::unexpected(Http3QpackError::kIntegerOverflow);
    }
    const auto mask = static_cast<std::uint8_t>((1U << prefixBits) - 1U);
    std::array<char, 11> encoded{};
    std::size_t size = 0;
    if (value < mask) {
        encoded[size++] = static_cast<char>((prefix & ~mask) | static_cast<std::uint8_t>(value));
    } else {
        encoded[size++] = static_cast<char>((prefix & ~mask) | mask);
        value -= mask;
        while (value >= 128) {
            encoded[size++] = static_cast<char>((value & 0x7fU) | 0x80U);
            value >>= 7;
        }
        encoded[size++] = static_cast<char>(value);
    }
    if (output.size() < size) {
        return std::unexpected(Http3QpackError::kOutputTooSmall);
    }
    for (std::size_t i = 0; i < size; ++i) {
        output[i] = encoded[i];
    }
    return size;
}

std::expected<std::size_t, Http3QpackError> decodeHttp3QpackString(
    std::span<const char> input, std::pmr::string& output) {
    if (input.empty()) {
        return std::unexpected(Http3QpackError::kNeedMoreData);
    }
    const bool huffman = (static_cast<std::uint8_t>(input[0]) & 0x80U) != 0;
    const auto length = decodeHttp3QpackInteger(input, 7);
    if (!length) {
        return std::unexpected(length.error());
    }
    const auto prefixBytes = length->encodedBytes;
    if (length->value > input.size() - prefixBytes) {
        return std::unexpected(Http3QpackError::kNeedMoreData);
    }
    const auto encoded = std::string_view(input.data() + prefixBytes, static_cast<std::size_t>(length->value));
    if (huffman) {
        if (!decodeHuffman(encoded, output)) {
            output.clear();
            return std::unexpected(Http3QpackError::kInvalidHuffman);
        }
    } else {
        output.assign(encoded);
    }
    return prefixBytes + static_cast<std::size_t>(length->value);
}

std::expected<std::size_t, Http3QpackError> encodeHttp3QpackString(std::span<char> output,
    std::string_view value) noexcept {
    std::array<char, 11> length{};
    const auto lengthBytes = encodeHttp3QpackInteger(length, 7, 0, value.size());
    if (!lengthBytes) {
        return std::unexpected(lengthBytes.error());
    }
    if (output.size() < *lengthBytes + value.size()) {
        return std::unexpected(Http3QpackError::kOutputTooSmall);
    }
    for (std::size_t i = 0; i < *lengthBytes; ++i) {
        output[i] = length[i];
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        output[*lengthBytes + i] = value[i];
    }
    return *lengthBytes + value.size();
}

}  // namespace ruvia
