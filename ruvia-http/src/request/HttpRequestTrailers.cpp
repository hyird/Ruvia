#include "ruvia/http/HttpRequestTrailers.h"

#include <algorithm>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/field/HttpTrailerFields.h"
#include "ruvia/http/detail/parser/HttpChunkParser.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace ruvia {
HttpRequestTrailers::HttpRequestTrailers(std::pmr::memory_resource* resource)
    : fields_(resource != nullptr ? resource : std::pmr::get_default_resource()) {}

std::expected<void, HttpRequestTrailerError> HttpRequestTrailers::append(std::string_view name, std::string_view value) {
    if (!detail::isValidHttpHeaderName(name) || !std::ranges::all_of(value, [](unsigned char ch) { return detail::isHttpFieldValueChar(ch); }) || detail::httpTrimOws(value) != value) {
        return std::unexpected(HttpRequestTrailerError::kInvalidField);
    }
    if (detail::isForbiddenHttpRequestTrailerName(name)) {
        return std::unexpected(HttpRequestTrailerError::kForbiddenField);
    }
    if (fields_.size() == kMaxHttpHeaderFields || bytes_ > kMaxHttpHeaderBytes - 4 || name.size() > kMaxHttpHeaderBytes - bytes_ - 4 ||
        value.size() > kMaxHttpHeaderBytes - bytes_ - 4 - name.size()) {
        return std::unexpected(HttpRequestTrailerError::kSectionTooLarge);
    }
    fields_.push_back(HttpHeader::copyOf(name, value, fields_.get_allocator().resource()));
    bytes_ += name.size() + value.size() + 4;
    return {};
}

std::expected<void, HttpRequestTrailerError> HttpRequestTrailers::appendHttp1(std::string_view block) {
    if (block.ends_with("\r\n\r\n")) {
        block.remove_suffix(4);
    } else if (block.ends_with("\r\n")) {
        block.remove_suffix(2);
    }
    detail::HttpChunkTrailerParser parser(block);
    for (;;) {
        const auto result = parser.next();
        if (const auto* failure = result.failure()) {
            return std::unexpected(failure->error() == detail::HttpChunkScanError::kTooLarge ? HttpRequestTrailerError::kSectionTooLarge : HttpRequestTrailerError::kInvalidField);
        }
        if (result.end()) {
            return {};
        }
        const auto* field = result.field();
        const auto appended = append(field->name(), field->value());
        if (!appended) {
            return appended;
        }
    }
}

std::optional<std::string_view> HttpRequestTrailers::field(std::string_view name) const& noexcept {
    for (auto field = fields_.rbegin(); field != fields_.rend(); ++field) {
        if (httpAsciiEqualsIgnoreCase(field->name(), name)) {
            return field->value();
        }
    }
    return std::nullopt;
}
}  // namespace ruvia
