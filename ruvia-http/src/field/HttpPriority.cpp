#include "ruvia/http/HttpPriority.h"

#include <algorithm>
#include <array>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3PeerStreams.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpHeader.h"

#include "field/HttpStructuredFields.h"
#include "http2/Http2FrameCodec.h"

namespace ruvia {
namespace {
using Item = detail::HttpStructuredItem;

class priority_header_input final {
public:
    explicit priority_header_input(std::span<const HttpHeaderView> headers) noexcept
        : headers_(headers) {
        (void)select_next(remaining_);
        normalize();
    }
    [[nodiscard]] bool empty() const noexcept {
        return remaining_.empty();
    }
    [[nodiscard]] char peek() const noexcept {
        return remaining_.front();
    }
    [[nodiscard]] const char* data() const noexcept {
        return remaining_.data();
    }
    void advance() noexcept {
        remaining_.remove_prefix(1);
        normalize();
    }

private:
    [[nodiscard]] bool select_next(std::string_view& value) noexcept {
        while (next_header_ != headers_.size()) {
            const auto& header = headers_[next_header_++];
            if (httpAsciiEqualsIgnoreCase(header.name(), "priority")) {
                value = header.value();
                return true;
            }
        }
        return false;
    }
    void normalize() noexcept {
        while (remaining_.empty()) {
            if (separator_) {
                separator_ = false;
                remaining_ = pending_;
            } else if (select_next(pending_)) {
                // HTTP combines repeated field lines before Structured Field parsing.
                remaining_ = ", ";
                separator_ = true;
            } else {
                return;
            }
        }
    }

    std::span<const HttpHeaderView> headers_;
    std::string_view remaining_;
    std::string_view pending_;
    std::size_t next_header_{0};
    bool separator_{false};
};

template <typename input_type>
std::variant<std::monostate, HttpPriorityError> parsePriorityMembers(detail::HttpStructuredParser<input_type> parser, HttpPriorityFields& fields) noexcept {
    parser.spaces();
    while (!parser.empty()) {
        const auto key = parser.key();
        if (key.empty()) {
            return HttpPriorityError::kInvalidSyntax;
        }
        Item item{.kind = Item::Kind::kBoolean, .boolean = true};
        if (parser.take('=')) {
            item = {};
            if (!parser.member(item)) {
                return HttpPriorityError::kInvalidSyntax;
            }
        } else if (!parser.parameters()) {
            return HttpPriorityError::kInvalidSyntax;
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
        if (parser.empty()) {
            break;
        }
        if (!parser.take(',')) {
            return HttpPriorityError::kInvalidSyntax;
        }
        parser.ows();
        if (parser.empty()) {
            return HttpPriorityError::kInvalidSyntax;
        }
    }
    return {};
}
}  // namespace

std::variant<HttpPriorityFields, HttpPriorityError> parseHttpPriority(std::string_view value) noexcept {
    HttpPriorityFields fields;
    if (auto parsed = parsePriorityMembers(detail::HttpStructuredParser{detail::http_structured_text_input{value}}, fields); parsed.index() != 0) {
        return std::get<1>(parsed);
    }
    return fields;
}
std::variant<HttpPriorityFields, HttpPriorityError> parseHttpPriority(std::span<const HttpHeaderView> headers) noexcept {
    HttpPriorityFields fields;
    if (auto parsed = parsePriorityMembers(detail::HttpStructuredParser{priority_header_input{headers}}, fields); parsed.index() != 0) {
        return std::get<1>(parsed);
    }
    return fields;
}
std::variant<std::size_t, HttpPriorityError> encodeHttpPriority(std::span<char> output, HttpPriorityFields fields) noexcept {
    if (fields.urgency && *fields.urgency > 7) {
        return HttpPriorityError::kInvalidValue;
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
        return HttpPriorityError::kOutputTooSmall;
    }
    std::copy_n(bytes.begin(), count, output.begin());
    return count;
}
std::variant<HttpPriorityUpdate, HttpPriorityError> decodeHttp2PriorityUpdate(std::span<const char> payload) noexcept {
    if (payload.size() < 4) {
        return HttpPriorityError::kInvalidFrame;
    }
    const auto id = detail::http2Read31(reinterpret_cast<const unsigned char*>(payload.data()));
    if (!id) {
        return HttpPriorityError::kInvalidValue;
    }
    auto fields = parseHttpPriority({payload.data() + 4, payload.size() - 4});
    if ((fields.index() != 0)) {
        return std::get<1>(fields);
    }
    return HttpPriorityUpdate{id, (id & 1U) == 0, std::get<0>(fields)};
}
std::variant<std::size_t, HttpPriorityError> encodeHttp2PriorityUpdate(std::span<char> output, std::uint32_t id, HttpPriorityFields fields) noexcept {
    if (!id || id > 0x7fffffffU) {
        return HttpPriorityError::kInvalidValue;
    }
    std::array<char, 12> value{};
    const auto length = encodeHttpPriority(value, fields);
    if ((length.index() != 0)) {
        return std::get<1>(length);
    }
    if (output.size() < 13 + std::get<0>(length)) {
        return HttpPriorityError::kOutputTooSmall;
    }
    detail::http2EncodeFrameHeader(output.data(), static_cast<std::uint32_t>(4 + std::get<0>(length)), static_cast<detail::Http2FrameType>(0x10), 0, 0);
    detail::http2Write32(output.data() + 9, id);
    std::copy_n(value.begin(), std::get<0>(length), output.begin() + 13);
    return 13 + std::get<0>(length);
}
std::variant<HttpPriorityUpdate, HttpPriorityError> decodeHttp3PriorityUpdate(std::uint64_t type, std::span<const char> payload) noexcept {
    if (type != 0xf0700 && type != 0xf0701) {
        return HttpPriorityError::kInvalidFrame;
    }
    const auto id = decodeHttp3VarInt(payload);
    if ((id.index() != 0) || (type == 0xf0700 && !isHttp3RequestStreamId(std::get<0>(id).value))) {
        return HttpPriorityError::kInvalidValue;
    }
    const auto value = payload.subspan(std::get<0>(id).encodedBytes);
    auto fields = parseHttpPriority({value.data(), value.size()});
    if ((fields.index() != 0)) {
        return std::get<1>(fields);
    }
    return HttpPriorityUpdate{std::get<0>(id).value, type == 0xf0701, std::get<0>(fields)};
}
std::variant<std::size_t, HttpPriorityError> encodeHttp3PriorityUpdate(std::span<char> output, HttpPriorityUpdate update) noexcept {
    if (update.elementId > kHttp3VarIntMax || (!update.push && !isHttp3RequestStreamId(update.elementId))) {
        return HttpPriorityError::kInvalidValue;
    }
    std::array<char, 20> payload{};
    const auto id = encodeHttp3VarInt(payload, update.elementId);
    const auto value = encodeHttpPriority(std::span(payload).subspan(std::get<0>(id)), update.fields);
    if ((value.index() != 0)) {
        return std::get<1>(value);
    }
    const auto size = encodeHttp3Frame(output, update.push ? 0xf0701 : 0xf0700, std::span(payload).first(std::get<0>(id) + std::get<0>(value)));
    if ((size.index() != 0)) {
        return HttpPriorityError::kOutputTooSmall;
    }
    return std::get<0>(size);
}
}  // namespace ruvia
