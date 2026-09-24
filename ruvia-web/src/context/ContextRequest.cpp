#include <algorithm>
#include <array>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/Bytes.h"
#include "ruvia/http/HttpAcceptMatch.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/http/HttpCookieFields.h"
#include "ruvia/http/HttpRequestContentDecoding.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/ModelJson.h"
#include "ruvia/web/detail/auth/CookieSignature.h"
#include "ruvia/web/detail/http/context/ContextRequestStorage.h"
#include "ruvia/web/detail/http/request/RequestBodyLoader.h"
#include "ruvia/web/detail/http/request/RequestFieldParsing.h"
#include "ruvia/web/detail/http/request/RequestFieldsAccess.h"
#include "ruvia/web/detail/http/request/RequestQueryValues.h"
#include "ruvia/web/detail/http/request/UnsupportedRequestContentCoding.h"
#include "ruvia/web/detail/model/parse/Parser.h"

namespace ruvia {

namespace detail {

// A media-type mismatch is the client speaking the wrong format at a valid
// endpoint: RFC 9110 15.5.16 assigns that 415, distinct from the 400 a
// malformed body of the RIGHT type earns below.
[[noreturn]] void throwInvalidJsonContentType() {
    throw HttpError({.status = http_status::kUnsupportedMediaType,
        .code = "unsupported_media_type",
        .message = "request body must be application/json"});
}

[[noreturn]] void throwInvalidJsonBody() {
    throw HttpError({.status = http_status::kBadRequest, .message = "invalid json body"});
}

[[noreturn]] void throwInvalidFormContentType() {
    throw HttpError({.status = http_status::kUnsupportedMediaType,
        .code = "unsupported_media_type",
        .message = "request body must be application/x-www-form-urlencoded"});
}

[[noreturn]] void throwInvalidFormBody() {
    throw HttpError({.status = http_status::kBadRequest, .message = "invalid form body"});
}

[[noreturn]] void throwInvalidQuery() {
    throw HttpError({.status = http_status::kBadRequest, .message = "invalid query"});
}

[[noreturn]] void throwInvalidParam() {
    throw HttpError({.status = http_status::kBadRequest, .message = "invalid route parameter"});
}

[[noreturn]] void throwInvalidHeader() {
    throw HttpError({.status = http_status::kBadRequest, .message = "invalid request header"});
}

[[noreturn]] void throwInvalidCookie() {
    throw HttpError({.status = http_status::kBadRequest, .message = "invalid cookie"});
}

}  // namespace detail

const RequestNameValueList& Context::requestHeaders() const {
    auto& cache = requestStorage().headers;
    if (!cache) {
        cache.emplace(detail::RequestNameValueListAccess::borrowHeaders(request_.headers()));
    }
    return *cache;
}

std::optional<std::string_view> Context::requestHeader(std::string_view name) const {
    return request_.header(name);
}

void Context::ensureRequestQuery() const {
    auto& cache = requestStorage().query;
    if (cache) {
        return;
    }
    if (requestStorage_->queryInvalid) {
        detail::throwInvalidQuery();
    }
    // Percent-encoding is validated while decoding each component. Unencoded
    // names and values borrow the request query string instead of copying.
    std::pmr::vector<std::pmr::string> storage(arena());
    auto query = detail::RequestNameValueListAccess::make(arena());
    bool valid = true;
    const bool completed = detail::visitUrlEncodedPairs(request_.queryString(),
        [&storage, &query, &valid](std::string_view key, std::string_view value) {
            const auto name =
                detail::borrowOrDecode(storage, key, detail::UrlDecodeMode::kForm);
            const auto decodedValue =
                detail::borrowOrDecode(storage, value, detail::UrlDecodeMode::kForm);
            if (!name || !decodedValue) {
                valid = false;
                return false;
            }
            detail::RequestNameValueListAccess::pushBack(
                query, detail::RequestNameValueViewAccess::make(*name, *decodedValue));
            return true;
        });
    if (!completed || !valid) {
        requestStorage_->queryInvalid = true;
        detail::throwInvalidQuery();
    }

    struct QueryBuild final {
        std::size_t firstIndex;
        std::size_t begin;
        std::size_t end;
    };

    const auto order = detail::sortedFieldOrder(query, arena());
    std::pmr::vector<QueryBuild> builds(arena());
    builds.reserve(order.size());
    for (std::size_t offset = 0; offset < order.size();) {
        const auto begin = offset;
        const auto firstIndex = order[offset];
        const auto name = query[firstIndex].name();
        do {
            ++offset;
        } while (offset < order.size() && query[order[offset]].name() == name);
        builds.push_back(QueryBuild{.firstIndex = firstIndex, .begin = begin, .end = offset});
    }
    std::ranges::sort(builds, [](const QueryBuild& left, const QueryBuild& right) noexcept {
        return left.firstIndex < right.firstIndex;
    });

    detail::RequestQueryValues groups{arena()};
    groups.reserve(builds.size());
    for (const auto& build : builds) {
        // A duplicated query name resolves to its LAST value, matching every other
        // duplicate-resolution path: Context::requestQuery(name), HttpRequest::query,
        // and RequestNameValueList::get() all take the last occurrence. Keep the
        // public field list duplicate-preserving so model binding can still reject
        // ambiguity; this grouped index only backs requestQueries(name).
        auto& group = groups.append(query[build.firstIndex].name());
        for (std::size_t i = build.begin; i < build.end; ++i) {
            group.add(query[order[i]].value());
        }
    }

    cache.emplace(std::move(storage), std::move(query), std::move(groups));
}

const RequestNameValueList& Context::requestQuery() const {
    ensureRequestQuery();
    return requestStorage_->query->fields();
}

std::optional<std::string_view> Context::requestQuery(std::string_view name) const {
    ensureRequestQuery();
    return requestStorage_->query->fields().get(name);
}

const detail::RequestQueryValues& Context::requestQueries() const {
    ensureRequestQuery();
    return requestStorage_->query->values();
}

std::optional<std::string_view> Context::requestCookie(std::string_view name) const {
    return request_.cookie(name);
}

const RequestNameValueList& Context::requestCookies() const {
    auto& cache = requestStorage().cookies;
    if (!cache) {
        auto cookies = detail::RequestNameValueListAccess::make(arena());
        const auto headers = request_.headers();
        if (request_.header("Cookie").has_value()) {
            detail::RequestNameValueListAccess::reserve(
                cookies, detail::boundedFieldReserve(8));
        }
        for (const auto& header : headers) {
            if (!httpAsciiEqualsIgnoreCase(header.name(), "Cookie")) {
                continue;
            }
            httpVisitCookiePairs(header.value(), [&cookies](std::string_view key,
                                                 std::string_view value) {
                detail::RequestNameValueListAccess::pushBack(
                    cookies, detail::RequestNameValueViewAccess::make(key, value));
                return true;
            });
        }
        cache.emplace(std::move(cookies));
    }
    return *cache;
}

void Context::ensureRouteParams() const {
    auto& cache = requestStorage().routeParams;
    if (cache) {
        return;
    }
    if (requestStorage_->routeParamsInvalid) {
        detail::throwInvalidParam();
    }
    // Route names and unencoded captures already borrow stable route/request
    // storage. Own only decoded values. Invalid percent-escapes fail in decode
    // rather than in a separate pre-scan of the same captures.
    std::pmr::vector<std::pmr::string> storage(arena());
    auto params = detail::RequestNameValueListAccess::make(arena());
    storage.reserve(paramCount_);
    detail::RequestNameValueListAccess::reserve(params, paramCount_);
    for (std::size_t i = 0; i < paramCount_; ++i) {
        auto value = paramValues_[i];
        if (detail::hasUrlEncoding(value, detail::UrlDecodeMode::kPercent)) {
            auto decoded = detail::decodeUrlComponent(
                value, {.mode = detail::UrlDecodeMode::kPercent, .resource = arena()});
            if (!decoded) {
                requestStorage_->routeParamsInvalid = true;
                detail::throwInvalidParam();
            }
            auto& owned = storage.emplace_back(std::move(*decoded));
            value = detail::storedStringView(owned);
        }
        detail::RequestNameValueListAccess::pushBack(
            params, detail::RequestNameValueViewAccess::make(paramNames_[i], value));
    }
    cache.emplace(std::move(storage), std::move(params));
}

const RequestNameValueList& Context::routeParams() const {
    ensureRouteParams();
    return requestStorage_->routeParams->fields;
}

std::optional<std::string_view> Context::routeParam(std::string_view name) const {
    ensureRouteParams();
    return requestStorage_->routeParams->fields.get(name);
}

bool Context::requestAccepts(std::string_view mediaType) const noexcept {
    // RFC 9110 5.3: multiple Accept field lines are equivalent to a single value
    // comma-joining them. requestKnownHeader returns only one stored slot, so a
    // client that sent Accept across several lines had all but one ignored. Fold
    // every Accept line into one best-match accumulator (equivalent to the joined
    // value, and correct for a q=0 exclusion spread across lines) without
    // allocating to concatenate.
    if (!request_.header("Accept").has_value()) {
        return true;
    }
    HttpAcceptMatch match;
    bool sawAccept = false;
    const auto headers = request_.headers();
    for (const auto& header : headers) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "Accept")) {
            continue;
        }
        sawAccept = true;
        if (!header.value().empty()) {
            match.updateMediaType(header.value(), mediaType);
        }
    }
    // Only absence means no preference. A present but empty Accept field is an
    // empty media-range list and therefore matches no representation.
    if (!sawAccept) {
        return true;
    }
    return match.matched();
}

namespace {

[[nodiscard]] std::string_view negotiationHeaderName(ContextRequest::Negotiable field) noexcept {
    switch (field) {
        case ContextRequest::Negotiable::kLanguage:
            return "Accept-Language";
        case ContextRequest::Negotiable::kEncoding:
            return "Accept-Encoding";
        case ContextRequest::Negotiable::kCharset:
            return "Accept-Charset";
        case ContextRequest::Negotiable::kMediaType:
            break;
    }
    return "Accept";
}

}  // namespace

std::optional<std::string_view> Context::requestNegotiate(
    ContextRequest::Negotiable field, std::span<const std::string_view> supported) const noexcept {
    if (supported.empty()) {
        return std::nullopt;
    }

    const auto headerName = negotiationHeaderName(field);
    const bool mediaType = field == ContextRequest::Negotiable::kMediaType;
    // Only Accept-Language does RFC 4647 prefix matching; an encoding or charset
    // either is the offered token or is "*".
    const auto tokenMode = field == ContextRequest::Negotiable::kLanguage
                               ? HttpAcceptTokenMatchMode::kLanguagePrefix
                               : HttpAcceptTokenMatchMode::kExact;

    std::array<std::string_view, kMaxHttpHeaderFields> fieldValues{};
    std::size_t fieldValueCount = 0;
    bool sawField = false;
    const auto headers = request_.headers();
    for (const auto& header : headers) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), headerName)) {
            continue;
        }
        sawField = true;
        if (header.value().empty() || fieldValueCount == fieldValues.size()) {
            continue;
        }
        fieldValues[fieldValueCount++] = header.value();
    }

    std::optional<std::string_view> best;
    int bestQuality = 0;
    for (const auto offered : supported) {
        HttpAcceptMatch match;
        for (std::size_t i = 0; i < fieldValueCount; ++i) {
            if (mediaType) {
                match.updateMediaType(fieldValues[i], offered);
            } else {
                match.updateToken(fieldValues[i], offered, tokenMode);
            }
        }
        if (!match.matched()) {
            continue;
        }
        const auto quality = match.quality();
        // Strictly greater, so `supported` order breaks the client's ties and
        // reads as the server's own preference.
        if (quality > bestQuality) {
            bestQuality = quality;
            best = offered;
        }
    }

    // Absence alone means no preference; a present but empty field is an empty
    // list that matches nothing, exactly as in requestAccepts.
    if (!sawField) {
        return supported.front();
    }
    return best;
}

Task<std::string_view> Context::requestBody() const {
    if (bodyDecoded_) {
        const auto& decoded = *requestStorage_->decodedBody;
        co_return std::string_view(decoded);
    }

    std::string_view raw;
    if (const auto* lazy = requestBodySource().lazy()) {
        raw = co_await lazy->loader().readAll();
    } else if (requestBodySource().streaming() != nullptr) {
        throw std::logic_error("streaming request body cannot be buffered");
    } else {
        raw = asChars(request_.bodyBytes());
    }

    // Transparently decode a request body whose Content-Encoding we understand,
    // so handlers always see the decoded representation (RFC 9110 §8.4).
    const auto parsedCoding = requestContentCoding(request_);
    if (const auto* invalid = parsedCoding.invalid()) {
        throw HttpProtocolError(invalid->status(), "invalid request Content-Encoding");
    }
    if (const auto* unsupported = parsedCoding.unsupported()) {
        throw detail::UnsupportedRequestContentCoding(*unsupported);
    }
    const auto coding = *parsedCoding.coding();
    if (coding == HttpContentCoding::kIdentity) {
        co_return raw;
    }
    auto decodeResult = decodeHttpRequestContent(
        coding, raw, {.maxDecodedBytes = maxDecodedBodyBytes_, .resource = arena()});
    auto* decodedContent = decodeResult.decoded();
    if (decodedContent == nullptr) {
        if (const auto* failure = decodeResult.protocolFailure()) {
            throw failure->protocolError();
        }
        if (decodeResult.decoderFailure() != nullptr) {
            throw std::runtime_error("request content decoder failed");
        }
        throw std::logic_error("unexpected request content decode result");
    }
    auto& decoded = decodedBody();
    decoded = std::move(*decodedContent).takeBytes();
    bodyDecoded_ = true;
    co_return std::string_view(decoded);
}

std::optional<std::string_view> ContextRequest::signedCookie(
    SignedCookieLookupOptions options) const {
    const auto name = options.name.view();
    const auto secret = options.secret.view();
    const auto stored = cookie(name);
    if (!stored.has_value() || stored->size() <= detail::kCookieSignatureSize) {
        return std::nullopt;
    }
    const auto valueSize = stored->size() - detail::kCookieSignatureSize - 1;
    if ((*stored)[valueSize] != '.') {
        return std::nullopt;
    }
    const auto value = stored->substr(0, valueSize);
    const auto signature = stored->substr(valueSize + 1);
    char expected[detail::kCookieSignatureSize];
    detail::writeCookieSignature(expected, secret, name, value);
    if (!detail::cookieSignatureEquals(signature, std::string_view(expected, sizeof(expected)))) {
        return std::nullopt;
    }
    return value;
}

bool Context::requestContentTypeMatches(std::string_view expected) const noexcept {
    return detail::contentTypeMatches(request_.header("Content-Type").value_or(std::string_view{}),
        expected);
}

Task<std::pmr::vector<MultipartPart>> Context::requestMultipart() const {
    const auto boundary = multipartBoundary();
    const auto requestBody = co_await this->requestBody();
    auto parsed = parseMultipartBody(requestBody, {.boundary = boundary, .resource = arena()});
    if (const auto* failure = parsed.failure()) {
        throw failure->protocolError();
    }
    auto* body = parsed.body();
    if (body == nullptr) {
        throw std::logic_error("unexpected multipart body parse result");
    }
    co_return std::move(*body).takeParts();
}

Task<void> Context::requestDiscardBody() const {
    if (const auto* lazy = requestBodySource().lazy()) {
        co_await lazy->loader().discard();
        co_return;
    }
    if (const auto* streaming = requestBodySource().streaming()) {
        while (co_await streaming->reader().read()) {
        }
    }
}

BodyReader& Context::requestBodyReader() const {
    const auto* streaming = requestBodySource().streaming();
    if (streaming == nullptr) {
        throw std::logic_error("request body is not streamable");
    }
    return streaming->reader();
}

MultipartReader Context::requestMultipartReader() const {
    return MultipartReader(
        requestBodyReader(), {.boundary = multipartBoundary(), .resource = arena()});
}

MultipartBoundary Context::multipartBoundary() const {
    const auto boundary = parseMultipartBoundary(
        request_.header("Content-Type").value_or(std::string_view{}));
    if (const auto* parsed = boundary.boundary()) {
        return *parsed;
    }
    if (boundary.notApplicable() != nullptr) {
        throw HttpError({.status = http_status::kUnsupportedMediaType,
            .code = "unsupported_media_type",
            .message = "request body must be multipart/form-data"});
    }
    if (const auto* failure = boundary.failure()) {
        throw failure->protocolError();
    }
    throw std::logic_error("unexpected multipart boundary parse result");
}

}  // namespace ruvia
