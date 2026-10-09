#include "ruvia/http/Http3FieldSection.h"

#include <array>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/Http3Qpack.h"

#include "http3/qpack_static_table.h"

namespace ruvia {
namespace {

Http3FieldSectionError mapError(Http3QpackError error) noexcept {
    switch (error) {
        case Http3QpackError::kNeedMoreData:
            return Http3FieldSectionError::kNeedMoreData;
        case Http3QpackError::kIntegerOverflow:
            return Http3FieldSectionError::kIntegerOverflow;
        case Http3QpackError::kInvalidIndex:
            return Http3FieldSectionError::kInvalidIndex;
        case Http3QpackError::kInvalidHuffman:
            return Http3FieldSectionError::kInvalidHuffman;
        case Http3QpackError::kOutputTooSmall:
            return Http3FieldSectionError::kOutputTooSmall;
    }
    return Http3FieldSectionError::kInvalidPrefix;
}

std::variant<std::size_t, Http3FieldSectionError> appendInteger(std::pmr::vector<char>& output,
    std::uint8_t prefixBits, std::uint8_t prefix, std::uint64_t value) {
    std::array<char, 11> bytes{};
    const auto encoded = encodeHttp3QpackInteger(bytes, prefixBits, prefix, value);
    if ((encoded.index() != 0)) {
        return mapError(std::get<1>(encoded));
    }
    output.insert(output.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(std::get<0>(encoded)));
    return std::get<0>(encoded);
}

std::variant<std::monostate, Http3FieldSectionError> appendLiteral(std::pmr::vector<char>& output,
    std::string_view value, std::uint8_t prefixBits, std::uint8_t firstByteFlags) {
    const auto length = appendInteger(output, prefixBits, firstByteFlags, value.size());
    if ((length.index() != 0)) {
        return std::get<1>(length);
    }
    output.insert(output.end(), value.begin(), value.end());
    return {};
}

}  // namespace

std::variant<std::size_t, Http3FieldSectionError> decodeHttp3FieldSection(
    std::span<const char> input, Http3FieldSectionCallback callback, void* context,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    if (input.size() > limits.maxEncodedBytes) {
        return Http3FieldSectionError::kFieldSectionTooLarge;
    }
    if (input.size() < 2) {
        return Http3FieldSectionError::kNeedMoreData;
    }
    if (static_cast<std::uint8_t>(input[0]) != 0) {
        return Http3FieldSectionError::kNonzeroRequiredInsertCount;
    }
    if (static_cast<std::uint8_t>(input[1]) != 0) {
        return Http3FieldSectionError::kInvalidBase;
    }

    std::size_t offset = 2;
    std::size_t decodedBytes = 0;
    std::size_t fieldCount = 0;
    std::pmr::string name(resource != nullptr ? resource : std::pmr::get_default_resource());
    std::pmr::string value(resource != nullptr ? resource : std::pmr::get_default_resource());
    const auto remaining = [&] { return input.subspan(offset); };
    const auto addField = [&](Http3FieldSectionFieldView field) -> std::variant<std::monostate, Http3FieldSectionError> {
        if (fieldCount >= limits.maxFields) {
            return Http3FieldSectionError::kTooManyFields;
        }
        constexpr std::size_t kFieldOverhead = 32;
        const auto remainingBudget = limits.maxDecodedBytes - decodedBytes;
        if (remainingBudget < kFieldOverhead ||
            field.name.size() > remainingBudget - kFieldOverhead ||
            field.value.size() > remainingBudget - kFieldOverhead - field.name.size()) {
            return Http3FieldSectionError::kFieldListTooLarge;
        }
        decodedBytes += kFieldOverhead + field.name.size() + field.value.size();
        ++fieldCount;
        if (callback != nullptr && !callback(context, field)) {
            return Http3FieldSectionError::kCallbackStopped;
        }
        return {};
    };

    while (offset < input.size()) {
        const auto first = static_cast<std::uint8_t>(input[offset]);
        Http3FieldSectionFieldView field{};
        if ((first & 0x80U) != 0) {
            if ((first & 0x40U) == 0) {
                return Http3FieldSectionError::kDynamicReference;
            }
            const auto index = decodeHttp3QpackInteger(remaining(), 6);
            if ((index.index() != 0)) {
                return mapError(std::get<1>(index));
            }
            const auto entry = http3QpackStaticEntry(std::get<0>(index).value);
            if ((entry.index() != 0)) {
                return Http3FieldSectionError::kInvalidIndex;
            }
            offset += std::get<0>(index).encodedBytes;
            field = {std::get<0>(entry).name, std::get<0>(entry).value, false};
        } else if ((first & 0xc0U) == 0x40U) {
            const bool neverIndexed = (first & 0x20U) != 0;
            if ((first & 0x10U) == 0) {
                return Http3FieldSectionError::kDynamicReference;
            }
            const auto index = decodeHttp3QpackInteger(remaining(), 4);
            if ((index.index() != 0)) {
                return mapError(std::get<1>(index));
            }
            const auto entry = http3QpackStaticEntry(std::get<0>(index).value);
            if ((entry.index() != 0)) {
                return Http3FieldSectionError::kInvalidIndex;
            }
            offset += std::get<0>(index).encodedBytes;
            const auto consumed = decodeHttp3QpackString(remaining(), value);
            if ((consumed.index() != 0)) {
                return mapError(std::get<1>(consumed));
            }
            offset += std::get<0>(consumed);
            field = {std::get<0>(entry).name, value, neverIndexed};
        } else if ((first & 0xe0U) == 0x20U) {
            const bool neverIndexed = (first & 0x10U) != 0;
            const auto nameSize = decodeHttp3QpackString(remaining(), 3, name);
            if ((nameSize.index() != 0)) {
                return mapError(std::get<1>(nameSize));
            }
            offset += std::get<0>(nameSize);
            const auto valueSize = decodeHttp3QpackString(remaining(), value);
            if ((valueSize.index() != 0)) {
                return mapError(std::get<1>(valueSize));
            }
            offset += std::get<0>(valueSize);
            field = {name, value, neverIndexed};
        } else {
            return Http3FieldSectionError::kInvalidPrefix;
        }
        const auto accepted = addField(field);
        if ((accepted.index() != 0)) {
            return std::get<1>(accepted);
        }
    }
    return fieldCount;
}

std::variant<std::pmr::vector<char>, Http3FieldSectionError> encodeHttp3FieldSection(
    std::span<const Http3FieldSectionFieldView> fields, std::pmr::memory_resource* resource) {
    std::pmr::vector<char> output(2, '\0', resource != nullptr ? resource : std::pmr::get_default_resource());
    for (const auto& field : fields) {
        const auto match = detail::qpack_static_fields.find(field.name,
            field.neverIndexed ? std::nullopt : std::optional(field.value));
        if (match && match->exact_index) {
            const auto encoded = appendInteger(output, 6, 0xc0, *match->exact_index);
            if ((encoded.index() != 0)) {
                return std::get<1>(encoded);
            }
        } else if (match) {
            const auto encoded = appendInteger(output, 4,
                static_cast<std::uint8_t>(0x50U | (field.neverIndexed ? 0x20U : 0U)), match->name_index);
            if ((encoded.index() != 0)) {
                return std::get<1>(encoded);
            }
            const auto value = appendLiteral(output, field.value, 7, 0);
            if ((value.index() != 0)) {
                return std::get<1>(value);
            }
        } else {
            const auto name = appendLiteral(output, field.name, 3,
                static_cast<std::uint8_t>(0x20U | (field.neverIndexed ? 0x10U : 0U)));
            if ((name.index() != 0)) {
                return std::get<1>(name);
            }
            const auto value = appendLiteral(output, field.value, 7, 0);
            if ((value.index() != 0)) {
                return std::get<1>(value);
            }
        }
    }
    return output;
}

}  // namespace ruvia
