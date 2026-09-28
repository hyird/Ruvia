#include "ruvia/http/Http3FieldSection.h"

#include <array>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/Http3Qpack.h"

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

std::expected<std::size_t, Http3FieldSectionError> appendInteger(std::pmr::vector<char>& output,
    std::uint8_t prefixBits, std::uint8_t prefix, std::uint64_t value) {
    std::array<char, 11> bytes{};
    const auto encoded = encodeHttp3QpackInteger(bytes, prefixBits, prefix, value);
    if (!encoded) {
        return std::unexpected(mapError(encoded.error()));
    }
    output.insert(output.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(*encoded));
    return *encoded;
}

std::expected<void, Http3FieldSectionError> appendLiteral(std::pmr::vector<char>& output,
    std::string_view value, std::uint8_t prefixBits, std::uint8_t firstByteFlags) {
    const auto length = appendInteger(output, prefixBits, firstByteFlags, value.size());
    if (!length) {
        return std::unexpected(length.error());
    }
    output.insert(output.end(), value.begin(), value.end());
    return {};
}

}  // namespace

std::expected<std::size_t, Http3FieldSectionError> decodeHttp3FieldSection(
    std::span<const char> input, Http3FieldSectionCallback callback, void* context,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    if (input.size() > limits.maxEncodedBytes) {
        return std::unexpected(Http3FieldSectionError::kFieldSectionTooLarge);
    }
    if (input.size() < 2) {
        return std::unexpected(Http3FieldSectionError::kNeedMoreData);
    }
    if (static_cast<std::uint8_t>(input[0]) != 0) {
        return std::unexpected(Http3FieldSectionError::kNonzeroRequiredInsertCount);
    }
    if (static_cast<std::uint8_t>(input[1]) != 0) {
        return std::unexpected(Http3FieldSectionError::kInvalidBase);
    }

    std::size_t offset = 2;
    std::size_t decodedBytes = 0;
    std::size_t fieldCount = 0;
    std::pmr::string name(resource != nullptr ? resource : std::pmr::get_default_resource());
    std::pmr::string value(resource != nullptr ? resource : std::pmr::get_default_resource());
    const auto remaining = [&] { return input.subspan(offset); };
    const auto addField = [&](Http3FieldSectionFieldView field) -> std::expected<void, Http3FieldSectionError> {
        if (fieldCount >= limits.maxFields) {
            return std::unexpected(Http3FieldSectionError::kTooManyFields);
        }
        constexpr std::size_t kFieldOverhead = 32;
        const auto remainingBudget = limits.maxDecodedBytes - decodedBytes;
        if (remainingBudget < kFieldOverhead ||
            field.name.size() > remainingBudget - kFieldOverhead ||
            field.value.size() > remainingBudget - kFieldOverhead - field.name.size()) {
            return std::unexpected(Http3FieldSectionError::kFieldListTooLarge);
        }
        decodedBytes += kFieldOverhead + field.name.size() + field.value.size();
        ++fieldCount;
        if (callback != nullptr && !callback(context, field)) {
            return std::unexpected(Http3FieldSectionError::kCallbackStopped);
        }
        return {};
    };

    while (offset < input.size()) {
        const auto first = static_cast<std::uint8_t>(input[offset]);
        Http3FieldSectionFieldView field{};
        if ((first & 0x80U) != 0) {
            if ((first & 0x40U) == 0) {
                return std::unexpected(Http3FieldSectionError::kDynamicReference);
            }
            const auto index = decodeHttp3QpackInteger(remaining(), 6);
            if (!index) {
                return std::unexpected(mapError(index.error()));
            }
            const auto entry = http3QpackStaticEntry(index->value);
            if (!entry) {
                return std::unexpected(Http3FieldSectionError::kInvalidIndex);
            }
            offset += index->encodedBytes;
            field = {entry->name, entry->value, false};
        } else if ((first & 0xc0U) == 0x40U) {
            const bool neverIndexed = (first & 0x20U) != 0;
            if ((first & 0x10U) == 0) {
                return std::unexpected(Http3FieldSectionError::kDynamicReference);
            }
            const auto index = decodeHttp3QpackInteger(remaining(), 4);
            if (!index) {
                return std::unexpected(mapError(index.error()));
            }
            const auto entry = http3QpackStaticEntry(index->value);
            if (!entry) {
                return std::unexpected(Http3FieldSectionError::kInvalidIndex);
            }
            offset += index->encodedBytes;
            const auto consumed = decodeHttp3QpackString(remaining(), value);
            if (!consumed) {
                return std::unexpected(mapError(consumed.error()));
            }
            offset += *consumed;
            field = {entry->name, value, neverIndexed};
        } else if ((first & 0xe0U) == 0x20U) {
            const bool neverIndexed = (first & 0x10U) != 0;
            const auto nameSize = decodeHttp3QpackString(remaining(), 3, name);
            if (!nameSize) {
                return std::unexpected(mapError(nameSize.error()));
            }
            offset += *nameSize;
            const auto valueSize = decodeHttp3QpackString(remaining(), value);
            if (!valueSize) {
                return std::unexpected(mapError(valueSize.error()));
            }
            offset += *valueSize;
            field = {name, value, neverIndexed};
        } else {
            return std::unexpected(Http3FieldSectionError::kInvalidPrefix);
        }
        const auto accepted = addField(field);
        if (!accepted) {
            return std::unexpected(accepted.error());
        }
    }
    return fieldCount;
}

std::expected<std::pmr::vector<char>, Http3FieldSectionError> encodeHttp3FieldSection(
    std::span<const Http3FieldSectionFieldView> fields, std::pmr::memory_resource* resource) {
    std::pmr::vector<char> output(resource != nullptr ? resource : std::pmr::get_default_resource());
    output.push_back('\0');
    output.push_back('\0');
    for (const auto& field : fields) {
        std::size_t exactIndex = 0;
        std::size_t nameIndex = 0;
        bool exactMatch = false;
        bool nameMatch = false;
        for (std::size_t index = 0; index < 99; ++index) {
            const auto entry = http3QpackStaticEntry(index);
            if (entry->name == field.name) {
                if (!nameMatch) {
                    nameIndex = index;
                    nameMatch = true;
                }
                if (entry->value == field.value) {
                    exactIndex = index;
                    exactMatch = true;
                    break;
                }
            }
        }
        if (exactMatch && !field.neverIndexed) {
            const auto encoded = appendInteger(output, 6, 0xc0, exactIndex);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
        } else if (nameMatch) {
            const auto encoded = appendInteger(output, 4,
                static_cast<std::uint8_t>(0x50U | (field.neverIndexed ? 0x20U : 0U)), nameIndex);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            const auto value = appendLiteral(output, field.value, 7, 0);
            if (!value) {
                return std::unexpected(value.error());
            }
        } else {
            const auto name = appendLiteral(output, field.name, 3,
                static_cast<std::uint8_t>(0x20U | (field.neverIndexed ? 0x10U : 0U)));
            if (!name) {
                return std::unexpected(name.error());
            }
            const auto value = appendLiteral(output, field.value, 7, 0);
            if (!value) {
                return std::unexpected(value.error());
            }
        }
    }
    return output;
}

}  // namespace ruvia
