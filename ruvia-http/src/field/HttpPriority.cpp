#include "ruvia/http/HttpPriority.h"

#include <algorithm>
#include <array>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3PeerStreams.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/field/HttpStructuredFields.h"
#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"

namespace ruvia {
namespace {
using Item = detail::HttpStructuredItem;
using Parser = detail::HttpStructuredParser;
std::expected<void, HttpPriorityError> parsePriorityMembers(std::string_view value, HttpPriorityFields& fields) noexcept {
    Parser parser{value};
    parser.spaces();
    while (parser.at < value.size()) {
        const auto key = parser.key();
        if (key.empty()) {
            return std::unexpected(HttpPriorityError::kInvalidSyntax);
        }
        Item item{.kind = Item::Kind::kBoolean, .boolean = true};
        if (parser.take('=')) {
            item = {};
            if (!parser.member(item)) {
                return std::unexpected(HttpPriorityError::kInvalidSyntax);
            }
        } else if (!parser.parameters()) {
            return std::unexpected(HttpPriorityError::kInvalidSyntax);
        }
        if (key == "u") {
            fields.urgency = item.kind == Item::Kind::kInteger && item.integer >= 0 && item.integer <= 7
                                 ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(item.integer))
                                 : std::nullopt;
        }
        if (key == "i") {
            fields.incremental = item.kind == Item::Kind::kBoolean ? std::optional(item.boolean) : std::nullopt;
        }
        parser.ows();
        if (parser.at == value.size()) {
            break;
        }
        if (!parser.take(',')) {
            return std::unexpected(HttpPriorityError::kInvalidSyntax);
        }
        parser.ows();
        if (parser.at == value.size()) {
            return std::unexpected(HttpPriorityError::kInvalidSyntax);
        }
    }
    return {};
}
}  // namespace

std::expected<HttpPriorityFields, HttpPriorityError> parseHttpPriority(std::string_view value) noexcept {
    HttpPriorityFields fields;
    if (auto parsed = parsePriorityMembers(value, fields); !parsed) {
        return std::unexpected(parsed.error());
    }
    return fields;
}
std::expected<HttpPriorityFields, HttpPriorityError> parseHttpPriority(std::span<const HttpHeaderView> headers) noexcept {
    HttpPriorityFields fields;
    bool seen = false;
    bool empty = false;
    for (const auto& header : headers) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "priority")) {
            continue;
        }
        const auto value = header.value();
        const bool thisEmpty = value.find_first_not_of(" \t") == std::string_view::npos;
        if (seen && (empty || thisEmpty)) {
            return std::unexpected(HttpPriorityError::kInvalidSyntax);
        }
        if (auto parsed = parsePriorityMembers(value, fields); !parsed) {
            return std::unexpected(parsed.error());
        }
        seen = true;
        empty = thisEmpty;
    }
    return fields;
}
std::expected<std::size_t, HttpPriorityError> encodeHttpPriority(std::span<char> output, HttpPriorityFields fields) noexcept {
    if (fields.urgency && *fields.urgency > 7) {
        return std::unexpected(HttpPriorityError::kInvalidValue);
    }
    std::array<char, 12> bytes{};
    std::size_t count = 0;
    if (fields.urgency) {
        bytes[count++] = 'u';
        bytes[count++] = '=';
        bytes[count++] = static_cast<char>('0' + *fields.urgency);
    }
    if (fields.incremental) {
        if (count) {
            bytes[count++] = ',';
            bytes[count++] = ' ';
        }
        bytes[count++] = 'i';
        bytes[count++] = '=';
        bytes[count++] = '?';
        bytes[count++] = *fields.incremental ? '1' : '0';
    }
    if (output.size() < count) {
        return std::unexpected(HttpPriorityError::kOutputTooSmall);
    }
    std::copy_n(bytes.begin(), count, output.begin());
    return count;
}
std::expected<HttpPriorityUpdate, HttpPriorityError> decodeHttp2PriorityUpdate(std::span<const char> payload) noexcept {
    if (payload.size() < 4) {
        return std::unexpected(HttpPriorityError::kInvalidFrame);
    }
    const auto id = detail::http2Read31(reinterpret_cast<const unsigned char*>(payload.data()));
    if (!id) {
        return std::unexpected(HttpPriorityError::kInvalidValue);
    }
    auto fields = parseHttpPriority({payload.data() + 4, payload.size() - 4});
    if (!fields) {
        return std::unexpected(fields.error());
    }
    return HttpPriorityUpdate{id, (id & 1U) == 0, *fields};
}
std::expected<std::size_t, HttpPriorityError> encodeHttp2PriorityUpdate(std::span<char> output, std::uint32_t id, HttpPriorityFields fields) noexcept {
    if (!id || id > 0x7fffffffU) {
        return std::unexpected(HttpPriorityError::kInvalidValue);
    }
    std::array<char, 12> value{};
    const auto length = encodeHttpPriority(value, fields);
    if (!length) {
        return std::unexpected(length.error());
    }
    if (output.size() < 13 + *length) {
        return std::unexpected(HttpPriorityError::kOutputTooSmall);
    }
    detail::http2EncodeFrameHeader(output.data(), static_cast<std::uint32_t>(4 + *length), static_cast<detail::Http2FrameType>(0x10), 0, 0);
    detail::http2Write32(output.data() + 9, id);
    std::copy_n(value.begin(), *length, output.begin() + 13);
    return 13 + *length;
}
std::expected<HttpPriorityUpdate, HttpPriorityError> decodeHttp3PriorityUpdate(std::uint64_t type, std::span<const char> payload) noexcept {
    if (type != 0xf0700 && type != 0xf0701) {
        return std::unexpected(HttpPriorityError::kInvalidFrame);
    }
    const auto id = decodeHttp3VarInt(payload);
    if (!id || (type == 0xf0700 && !isHttp3RequestStreamId(id->value))) {
        return std::unexpected(HttpPriorityError::kInvalidValue);
    }
    const auto value = payload.subspan(id->encodedBytes);
    auto fields = parseHttpPriority({value.data(), value.size()});
    if (!fields) {
        return std::unexpected(fields.error());
    }
    return HttpPriorityUpdate{id->value, type == 0xf0701, *fields};
}
std::expected<std::size_t, HttpPriorityError> encodeHttp3PriorityUpdate(std::span<char> output, HttpPriorityUpdate update) noexcept {
    if (update.elementId > kHttp3VarIntMax || (!update.push && !isHttp3RequestStreamId(update.elementId))) {
        return std::unexpected(HttpPriorityError::kInvalidValue);
    }
    std::array<char, 20> payload{};
    const auto id = encodeHttp3VarInt(payload, update.elementId);
    const auto value = encodeHttpPriority(std::span(payload).subspan(*id), update.fields);
    if (!value) {
        return std::unexpected(value.error());
    }
    const auto size = encodeHttp3Frame(output, update.push ? 0xf0701 : 0xf0700, std::span(payload).first(*id + *value));
    if (!size) {
        return std::unexpected(HttpPriorityError::kOutputTooSmall);
    }
    return *size;
}
}  // namespace ruvia
