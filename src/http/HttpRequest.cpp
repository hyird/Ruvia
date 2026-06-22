#include "ruvia/http/HttpRequest.h"

#include "HttpRequestInternal.h"
#include "parser/HttpParserSyntax.h"
#include "HeaderTokenUtils.h"
#include "ruvia/http/UrlEncoding.h"

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

std::optional<int> RequestValue::toInt() const noexcept {
    return parseInteger<int>(value_);
}

std::optional<unsigned int> RequestValue::toUInt() const noexcept {
    return parseInteger<unsigned int>(value_);
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

std::optional<std::pmr::string> HttpRequest::decodedPath() const {
    if (!detail::hasUrlEncoding(path_, detail::UrlDecodeMode::kPercent)) {
        return std::pmr::string(path_.data(), path_.size(), resource());
    }
    return detail::decodeUrlComponentToString(path_, resource(), detail::UrlDecodeMode::kPercent);
}

std::string_view HttpRequest::header(std::string_view name) const noexcept {
    const auto kind = detail::classifyRequestHeader(name);
    if (kind != detail::RequestHeaderKind::kOther) {
        const auto knownSlot = static_cast<std::size_t>(kind) - 1;
        return detail::requestKnownHeader(*this, static_cast<detail::RequestKnownHeader>(knownSlot));
    }

    for (std::size_t i = 0; i < headerCount_; ++i) {
        if (detail::httpAsciiEqualsIgnoreCase(headers_[i].name, name)) {
            return headers_[i].value;
        }
    }

    return {};
}

QueryValue HttpRequest::query(std::string_view name) const noexcept {
    std::optional<std::string_view> result;
    (void)detail::visitUrlEncodedPairs(queryString_, [&](std::string_view key, std::string_view value) {
        if (detail::urlComponentEquals(key, name, detail::UrlDecodeMode::kForm)) {
            result = value;
            return false;
        }
        return true;
    });

    return QueryValue(result, resource(), RequestValue::DecodeMode::kForm);
}

std::optional<std::string_view> HttpRequest::cookie(std::string_view name) const noexcept {
    auto input = detail::requestKnownHeader(*this, detail::RequestKnownHeader::kCookie);
    while (!input.empty()) {
        const auto semicolon = input.find(';');
        const auto part = semicolon == std::string_view::npos ? input : input.substr(0, semicolon);
        const auto equals = part.find('=');
        if (equals != std::string_view::npos) {
            const auto key = detail::httpTrimOws(part.substr(0, equals));
            if (key == name) {
                return detail::httpTrimOws(part.substr(equals + 1));
            }
        }

        if (semicolon == std::string_view::npos) {
            break;
        }
        input.remove_prefix(semicolon + 1);
    }

    return std::nullopt;
}

std::pmr::memory_resource* HttpRequest::resource() const noexcept {
    return resource_ == nullptr ? std::pmr::get_default_resource() : resource_;
}

}  // namespace ruvia
