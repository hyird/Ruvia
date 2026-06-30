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

[[nodiscard]] std::size_t delimitedFieldCount(std::string_view input, char delimiter) noexcept {
    if (input.empty()) {
        return 0;
    }

    std::size_t count = 1;
    for (const char c : input) {
        if (c == delimiter) {
            ++count;
        }
    }
    return count;
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

RequestValue HttpRequest::decodedPath() const noexcept {
    return RequestValue(path_, resource(), RequestValue::DecodeMode::kPercent);
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
    return QueryValue(
        detail::findUrlEncodedValue(queryString_, name, detail::UrlDecodeMode::kForm),
        resource(),
        RequestValue::DecodeMode::kForm);
}

RequestNameValueList HttpRequest::query() const {
    RequestNameValueList params(resource());
    params.reserve(delimitedFieldCount(queryString_, '&'));
    (void)detail::visitUrlEncodedPairs(
        queryString_,
        [&params](std::string_view key, std::string_view value) {
            params.push_back(RequestNameValueView{.name = key, .value = value});
        });
    return params;
}

std::pmr::vector<QueryValue> HttpRequest::queries(std::string_view name) const {
    std::pmr::vector<QueryValue> result(resource());
    (void)detail::visitUrlEncodedPairs(
        queryString_,
        [&](std::string_view key, std::string_view value) {
            if (detail::urlComponentEquals(key, name, detail::UrlDecodeMode::kForm)) {
                result.emplace_back(
                    std::optional<std::string_view>(value),
                    resource(),
                    RequestValue::DecodeMode::kForm);
            }
        });
    return result;
}

std::optional<std::string_view> HttpRequest::cookie(std::string_view name) const noexcept {
    return detail::httpFindSemicolonParameter(
        detail::requestKnownHeader(*this, detail::RequestKnownHeader::kCookie),
        name);
}

RequestNameValueList HttpRequest::cookies() const {
    RequestNameValueList params(resource());
    const auto input = detail::requestKnownHeader(*this, detail::RequestKnownHeader::kCookie);
    params.reserve(delimitedFieldCount(input, ';'));
    detail::httpVisitSemicolonParameters(
        input,
        [&params](std::string_view key, std::string_view value) {
            params.push_back(RequestNameValueView{.name = key, .value = value});
            return true;
        });
    return params;
}

std::pmr::memory_resource* HttpRequest::resource() const noexcept {
    return detail::pmrResourceOrDefault(resource_);
}

}  // namespace ruvia
