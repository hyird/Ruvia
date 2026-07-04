#include "ruvia/http/HttpRequest.h"

#include "HttpRequestInternal.h"
#include "parser/HttpParserSyntax.h"
#include "HeaderTokenUtils.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/memory/PmrResource.h"

#include <charconv>
#include <system_error>

namespace ruvia {
namespace {

static_assert(
    static_cast<std::size_t>(detail::RequestHeaderKind::kAccept) ==
    static_cast<std::size_t>(detail::RequestKnownHeader::kAccept) + 1);
static_assert(
    static_cast<std::size_t>(detail::RequestHeaderKind::kAuthorization) ==
    static_cast<std::size_t>(detail::RequestKnownHeader::kAuthorization) + 1);
static_assert(
    static_cast<std::size_t>(detail::RequestHeaderKind::kContentEncoding) ==
    static_cast<std::size_t>(detail::RequestKnownHeader::kContentEncoding) + 1);
static_assert(
    static_cast<std::size_t>(detail::RequestHeaderKind::kUserAgent) ==
    static_cast<std::size_t>(detail::RequestKnownHeader::kUserAgent) + 1);
static_assert(
    detail::kRequestHeaderKindCount ==
    static_cast<std::size_t>(detail::RequestKnownHeader::kUserAgent) + 2);

template <typename T>
std::optional<T> parseInteger(std::optional<std::string_view> input) noexcept {
    if (!input || input->empty()) {
        return std::nullopt;
    }

    T value{};
    const auto* begin = input->data();
    const auto* end = begin + input->size();
    const auto [ptr, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || ptr != end) {
        return std::nullopt;
    }

    return value;
}

}  // namespace

std::optional<std::pmr::string> RequestValue::toString() const {
    if (!value_) {
        return std::nullopt;
    }

    if (decodeMode_ != DecodeMode::kNone) {
        const auto mode = decodeMode_ == DecodeMode::kForm
            ? detail::UrlDecodeMode::kForm
            : detail::UrlDecodeMode::kPercent;
        if (detail::hasUrlEncoding(*value_, mode)) {
            return detail::decodeUrlComponentToString(*value_, resource(), mode);
        }
    }

    return std::pmr::string(value_->data(), value_->size(), resource());
}

std::optional<bool> RequestValue::toBool() const noexcept {
    if (!value_) {
        return std::nullopt;
    }

    if (*value_ == "1" || detail::httpAsciiEqualsIgnoreCase(*value_, "true")) {
        return true;
    }
    if (*value_ == "0" || detail::httpAsciiEqualsIgnoreCase(*value_, "false")) {
        return false;
    }
    return std::nullopt;
}

std::optional<std::int32_t> RequestValue::toInt32() const noexcept {
    return parseInteger<std::int32_t>(value_);
}

std::optional<std::uint32_t> RequestValue::toUInt32() const noexcept {
    return parseInteger<std::uint32_t>(value_);
}

std::optional<std::int64_t> RequestValue::toInt64() const noexcept {
    return parseInteger<std::int64_t>(value_);
}

std::optional<std::uint64_t> RequestValue::toUInt64() const noexcept {
    return parseInteger<std::uint64_t>(value_);
}

std::string_view HttpRequest::header(std::string_view name) const noexcept {
    const auto kind = detail::classifyRequestHeader(name);
    if (kind != detail::RequestHeaderKind::kOther) {
        const auto knownSlot = static_cast<std::size_t>(kind) - 1;
        return detail::requestKnownHeader(*this, static_cast<detail::RequestKnownHeader>(knownSlot));
    }

    for (std::size_t i = 0; i < headerCount_; ++i) {
        if (detail::httpAsciiEqualsIgnoreCase(headers_[i].name(), name)) {
            return headers_[i].value();
        }
    }

    return {};
}

std::optional<std::string_view> HttpRequest::query(std::string_view name) const noexcept {
    return detail::findUrlEncodedValue(queryString_, name, detail::UrlDecodeMode::kForm);
}

std::optional<std::string_view> HttpRequest::cookie(std::string_view name) const noexcept {
    return detail::httpFindSemicolonParameter(
        detail::requestKnownHeader(*this, detail::RequestKnownHeader::kCookie),
        name);
}

std::pmr::memory_resource* HttpRequest::resource() const noexcept {
    return detail::pmrResourceOrDefault(resource_);
}

}  // namespace ruvia
