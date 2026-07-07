#include "ruvia/http/HttpResponse.h"

#include "HttpResponseHeaderAccess.h"
#include "HttpResponseStaticHeaders.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace ruvia {
namespace {

[[nodiscard]] bool overlapsHeaderStorage(
    const HttpResponseHeader& header,
    std::string_view value) noexcept {
    const auto name = header.name();
    if (value.empty() || name.data() == nullptr) {
        return false;
    }
    const auto storageBegin = reinterpret_cast<std::uintptr_t>(name.data());
    const auto storageEnd = storageBegin + name.size() + header.value().size();
    const auto valueBegin = reinterpret_cast<std::uintptr_t>(value.data());
    const auto valueEnd = valueBegin + value.size();
    return valueBegin < storageEnd && storageBegin < valueEnd;
}

}  // namespace

HttpResponseHeader HttpResponseHeaders::makeOwnedHeader(
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit) {
    const auto total = name.size() + value.size();
    char* bytes = nullptr;
    if (total > 0) {
        bytes = static_cast<char*>(resource_->allocate(total, 1));
        std::memcpy(bytes, name.data(), name.size());
        std::memcpy(bytes + name.size(), value.data(), value.size());
    }
    return detail::makeResponseHeader(
        bytes,
        static_cast<std::uint32_t>(name.size()),
        static_cast<std::uint32_t>(value.size()),
        knownBit,
        true);
}

HttpResponseHeader HttpResponseHeaders::makeUninitializedHeader(
    std::string_view name,
    std::size_t valueSize,
    std::uint32_t knownBit) {
    const auto total = name.size() + valueSize;
    char* bytes = nullptr;
    if (total > 0) {
        bytes = static_cast<char*>(resource_->allocate(total, 1));
        std::memcpy(bytes, name.data(), name.size());
    }
    return detail::makeResponseHeader(
        bytes,
        static_cast<std::uint32_t>(name.size()),
        static_cast<std::uint32_t>(valueSize),
        knownBit,
        true);
}

std::optional<HttpResponseHeader> HttpResponseHeaders::makeStaticHeader(
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit) noexcept {
    if (knownBit == 0) {
        return std::nullopt;
    }
    auto header = detail::builtinStaticResponseHeader(knownBit, value);
    if (!header || header->name() != name) {
        return std::nullopt;
    }
    return header;
}

void HttpResponseHeaders::releaseHeader(HttpResponseHeader& header) noexcept {
    if (header.owned && header.bytes != nullptr) {
        resource_->deallocate(
            const_cast<char*>(header.bytes),
            static_cast<std::size_t>(header.nameSize) + header.valueSize,
            1);
    }
    header.bytes = nullptr;
    header.nameSize = 0;
    header.valueSize = 0;
    header.knownBit = 0;
    header.owned = false;
    header.append = false;
}

HttpResponseHeader& HttpResponseHeaders::appendHeader(HttpResponseHeader header) {
    if (!spilled_ && size_ == kInlineCapacity) {
        spill(size_ + 1);
    }
    if (!spilled_) {
        auto* target = inlineData() + size_;
        *target = header;
        ++size_;
        return *target;
    }
    heap_.push_back(header);
    return heap_.back();
}

HttpResponseHeader& HttpResponseHeaders::add(
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit) {
    return appendHeader(makeOwnedHeader(name, value, knownBit));
}

HttpResponseHeader& HttpResponseHeaders::addStableView(
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit) {
    const auto staticHeader = makeStaticHeader(name, value, knownBit);
    return appendHeader(staticHeader ? *staticHeader : makeOwnedHeader(name, value, knownBit));
}

HttpResponseHeader& HttpResponseHeaders::addUninitializedValue(
    std::string_view name,
    std::size_t valueSize,
    std::uint32_t knownBit) {
    return appendHeader(makeUninitializedHeader(name, valueSize, knownBit));
}

HttpResponseHeader& HttpResponseHeaders::assignUninitializedValue(
    HttpResponseHeader& header,
    std::string_view name,
    std::size_t valueSize,
    std::uint32_t knownBit) {
    const auto total = name.size() + valueSize;
    if (header.owned &&
        header.bytes != nullptr &&
        !overlapsHeaderStorage(header, name) &&
        total == static_cast<std::size_t>(header.nameSize) + header.valueSize) {
        auto* const bytes = const_cast<char*>(header.bytes);
        std::memcpy(bytes, name.data(), name.size());
        header.nameSize = static_cast<std::uint32_t>(name.size());
        header.valueSize = static_cast<std::uint32_t>(valueSize);
        header.knownBit = knownBit;
        header.append = false;
        return header;
    }

    const auto replacement = makeUninitializedHeader(name, valueSize, knownBit);
    releaseHeader(header);
    header = replacement;
    return header;
}

bool HttpResponseHeaders::tryAssignOwnedInPlace(
    HttpResponseHeader& header,
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit) noexcept {
    const auto total = name.size() + value.size();
    if (!header.owned ||
        header.bytes == nullptr ||
        overlapsHeaderStorage(header, name) ||
        overlapsHeaderStorage(header, value) ||
        total != static_cast<std::size_t>(header.nameSize) + header.valueSize) {
        return false;
    }
    auto* const bytes = const_cast<char*>(header.bytes);
    std::memcpy(bytes, name.data(), name.size());
    std::memcpy(bytes + name.size(), value.data(), value.size());
    header.nameSize = static_cast<std::uint32_t>(name.size());
    header.valueSize = static_cast<std::uint32_t>(value.size());
    header.knownBit = knownBit;
    header.append = false;
    return true;
}

void HttpResponseHeaders::assign(
    HttpResponseHeader& header,
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit) {
    if (tryAssignOwnedInPlace(header, name, value, knownBit)) {
        return;
    }
    const auto replacement = makeOwnedHeader(name, value, knownBit);
    releaseHeader(header);
    header = replacement;
}

void HttpResponseHeaders::assignStableView(
    HttpResponseHeader& header,
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit) {
    const auto staticHeader = makeStaticHeader(name, value, knownBit);
    if (staticHeader) {
        releaseHeader(header);
        header = *staticHeader;
        return;
    }

    if (tryAssignOwnedInPlace(header, name, value, knownBit)) {
        return;
    }
    const auto replacement = makeOwnedHeader(name, value, knownBit);
    releaseHeader(header);
    header = replacement;
}

}  // namespace ruvia
