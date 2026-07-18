#include "ruvia/http/detail/http2/Http2Hpack.h"
#include "ruvia/http/detail/http2/Http2HpackStaticTable.h"

#include <array>
#include <charconv>

namespace ruvia::detail {

void HpackEncoder::encodeInteger(
    std::pmr::string& out,
    std::uint8_t firstByteMask,
    std::uint8_t prefixBits,
    std::uint32_t value) {
    const auto prefixMax = static_cast<std::uint32_t>((1U << prefixBits) - 1U);
    if (value < prefixMax) {
        out.push_back(static_cast<char>(firstByteMask | static_cast<std::uint8_t>(value)));
        return;
    }

    out.push_back(static_cast<char>(firstByteMask | static_cast<std::uint8_t>(prefixMax)));
    value -= prefixMax;
    while (value >= 128) {
        out.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

void HpackEncoder::encodeString(std::pmr::string& out, std::string_view value) {
    encodeInteger(out, 0, 7, static_cast<std::uint32_t>(value.size()));
    out.append(value.data(), value.size());
}

void HpackEncoder::encodeIndexed(std::pmr::string& out, std::uint32_t index) {
    encodeInteger(out, 0x80, 7, index);
}

void HpackEncoder::encodeDynamicTableSizeUpdate(
    std::pmr::string& out,
    std::uint32_t maximum) {
    // RFC 7541 §6.3: 001xxxxx followed by an HPACK integer with a 5-bit prefix.
    encodeInteger(out, 0x20, 5, maximum);
}

void HpackEncoder::encodeHeader(std::pmr::string& out, std::string_view name, std::string_view value) {
    const auto match = hpackFindStaticHeaderMatch(name, value);
    if (match.exactIndex != 0) {
        // A fully indexed static-table entry carries no field value on the wire, so
        // there is nothing for an intermediary to index; the never-indexed hint does
        // not apply.
        encodeIndexed(out, match.exactIndex);
        return;
    }

    const bool neverIndexed = hpackHeaderNameIsSensitive(name);
    const auto nameIndex = match.nameIndex;
    if (nameIndex != 0) {
        encodeHeaderWithNameIndex(out, nameIndex, value, neverIndexed);
        return;
    }
    encodeInteger(out, neverIndexed ? kHpackLiteralNeverIndexed : kHpackLiteralWithoutIndexing, 4, 0);
    encodeString(out, name);
    encodeString(out, value);
}

void HpackEncoder::encodeHeaderWithNameIndex(
    std::pmr::string& out,
    std::uint32_t nameIndex,
    std::string_view value,
    bool neverIndexed) {
    encodeInteger(out, neverIndexed ? kHpackLiteralNeverIndexed : kHpackLiteralWithoutIndexing, 4, nameIndex);
    encodeString(out, value);
}

void HpackEncoder::encodeStatus(std::pmr::string& out, HttpStatusCode status) {
    switch (status.value()) {
        case http_status::kOk.value():
            encodeIndexed(out, HpackStaticIndex::kStatus200);
            return;
        case http_status::kNoContent.value():
            encodeIndexed(out, HpackStaticIndex::kStatus204);
            return;
        case http_status::kPartialContent.value():
            encodeIndexed(out, HpackStaticIndex::kStatus206);
            return;
        case http_status::kNotModified.value():
            encodeIndexed(out, HpackStaticIndex::kStatus304);
            return;
        case http_status::kBadRequest.value():
            encodeIndexed(out, HpackStaticIndex::kStatus400);
            return;
        case http_status::kNotFound.value():
            encodeIndexed(out, HpackStaticIndex::kStatus404);
            return;
        case http_status::kInternalServerError.value():
            encodeIndexed(out, HpackStaticIndex::kStatus500);
            return;
        default:
            break;
    }

    std::array<char, 3> buffer{};
    const auto [ptr, ec] = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), status.value());
    if (ec != std::errc{}) {
        encodeIndexed(out, HpackStaticIndex::kStatus500);
        return;
    }
    encodeHeaderWithNameIndex(
        out,
        HpackStaticIndex::kStatus200,
        std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())));
}

}  // namespace ruvia::detail
