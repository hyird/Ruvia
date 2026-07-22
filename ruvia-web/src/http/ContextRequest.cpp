#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/UnsupportedRequestContentCoding.h"

#include "ruvia/web/detail/CookieSignature.h"
#include "ruvia/web/ModelJson.h"
#include "ruvia/web/ModelObject.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpAcceptMediaType.h"
#include "ruvia/http/detail/HttpContentCoding.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/web/detail/http/ContextRequestInternal.h"
#include "ruvia/web/detail/http/RequestFieldsAccess.h"
#include "ruvia/web/detail/http/RequestQueryValues.h"
#include "ruvia/http/detail/parser/MultipartBoundary.h"
#include "ruvia/http/detail/RequestBodyDecoding.h"
#include "ruvia/web/detail/http/RequestBodyLoader.h"
#include "ruvia/http/detail/AsciiCase.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/web/detail/http/RequestFieldParsing.h"
#include "ruvia/web/detail/http/RequestFormBodyParse.h"
#include "ruvia/web/detail/model/Parser.h"

#include <algorithm>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ruvia {

ConnInfo getConnInfo(const Context& context) noexcept {
    return context.connInfo_;
}

namespace detail {

// A media-type mismatch is the client speaking the wrong format at a valid
// endpoint: RFC 9110 15.5.16 assigns that 415, distinct from the 400 a
// malformed body of the RIGHT type earns below.
[[noreturn]] void throwInvalidJsonContentType() {
    throw HttpError(
        http_status::kUnsupportedMediaType,
        "unsupported_media_type",
        "request body must be application/json");
}

[[noreturn]] void throwInvalidJsonBody() {
    throw std::invalid_argument("invalid json body");
}

[[noreturn]] void throwInvalidFormContentType() {
    throw HttpError(
        http_status::kUnsupportedMediaType,
        "unsupported_media_type",
        "request body must be application/x-www-form-urlencoded");
}

[[noreturn]] void throwInvalidFormBody() {
    throw std::invalid_argument("invalid form body");
}

[[noreturn]] void throwTooManyFormFields() {
    throw HttpError(
        http_status::kContentTooLarge,
        "too_many_form_fields",
        "request form has too many fields");
}

[[noreturn]] void throwInvalidQuery() {
    throw std::invalid_argument("invalid query");
}

[[noreturn]] void throwInvalidParam() {
    throw std::invalid_argument("invalid route parameter");
}

[[noreturn]] void throwInvalidHeader() {
    throw std::invalid_argument("invalid request header");
}

[[noreturn]] void throwInvalidCookie() {
    throw std::invalid_argument("invalid cookie");
}

}  // namespace detail

Task<JsonValue> ContextRequest::jsonTask(const Context* context) {
    if (!contextContentTypeMatches(context, "application/json")) {
        detail::throwInvalidJsonContentType();
    }
    const auto requestBody = co_await contextTextTask(context);
    auto parsed = JsonValue::parse(requestBody, contextResource(context));
    if (!parsed) {
        detail::throwInvalidJsonBody();
    }
    co_return std::move(*parsed);
}

ScopedOperation<JsonValue> ContextRequest::json() const {
    return detail::makeScopedOperation(
        context_->operationScope_, jsonTask(context_));
}

Task<std::optional<JsonValue>> ContextRequest::jsonIfTask(const Context* context) {
    if (!contextContentTypeMatches(context, "application/json")) {
        co_return std::nullopt;
    }
    const auto requestBody = co_await contextTextTask(context);
    auto parsed = JsonValue::parse(requestBody, contextResource(context));
    if (!parsed) {
        co_return std::nullopt;
    }
    co_return std::move(*parsed);
}

ScopedOperation<std::optional<JsonValue>> ContextRequest::jsonIf() const {
    return detail::makeScopedOperation(
        context_->operationScope_, jsonIfTask(context_));
}


const RequestNameValueList& Context::requestHeaders() const {
    auto& cache = requestStorage().headers;
    if (!cache) {
        const auto rawHeaders = request_.headers();
        std::pmr::vector<std::pmr::string> names(resource());
        auto headers = detail::RequestNameValueListAccess::make(resource());
        names.reserve(rawHeaders.size());
        detail::RequestNameValueListAccess::reserve(headers, rawHeaders.size());
        for (const auto& rawHeader : rawHeaders) {
            auto& name = names.emplace_back();
            name.reserve(rawHeader.name().size());
            detail::appendLowerAscii(name, rawHeader.name());
            detail::RequestNameValueListAccess::pushBack(
                headers,
                detail::RequestNameValueViewAccess::make(
                    std::string_view(name),
                    rawHeader.value()));
        }
        cache.emplace(std::move(names), std::move(headers));
    }
    return cache->fields;
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
    if (!detail::validateUrlEncoding(request_.queryString())) {
        requestStorage_->queryInvalid = true;
        detail::throwInvalidQuery();
    }

    const auto pairCount = detail::delimitedFieldCount(request_.queryString(), '&');
    std::pmr::vector<std::pmr::string> storage(resource());
    storage.reserve(detail::boundedFieldReserve(pairCount * 2));
    bool valid = true;
    const bool completed = detail::visitUrlEncodedPairs(
        request_.queryString(),
        [this, &storage, &valid](std::string_view key, std::string_view value) {
            std::pmr::string decodedName(resource());
            std::pmr::string decodedValue(resource());
            if (!detail::assignUrlDecodedOrCopy(decodedName, key, detail::UrlDecodeMode::kForm) ||
                !detail::assignUrlDecodedOrCopy(decodedValue, value, detail::UrlDecodeMode::kForm)) {
                valid = false;
                return false;
            }

            storage.push_back(std::move(decodedName));
            storage.push_back(std::move(decodedValue));
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

    const auto order = detail::sortedPairOrder(storage, resource());
    std::pmr::vector<QueryBuild> builds(resource());
    builds.reserve(order.size());
    for (std::size_t offset = 0; offset < order.size();) {
        const auto begin = offset;
        const auto firstIndex = order[offset];
        const auto name = detail::pairNameAt(storage, firstIndex);
        do {
            ++offset;
        } while (offset < order.size() && detail::pairNameAt(storage, order[offset]) == name);
        builds.push_back(QueryBuild{.firstIndex = firstIndex, .begin = begin, .end = offset});
    }
    std::ranges::sort(builds, [](const QueryBuild& left, const QueryBuild& right) noexcept {
        return left.firstIndex < right.firstIndex;
    });

    auto query = detail::RequestNameValueListAccess::make(resource());
    detail::RequestQueryValues groups{resource()};
    detail::RequestNameValueListAccess::reserve(query, builds.size());
    groups.reserve(builds.size());
    for (const auto& build : builds) {
        // A duplicated query name resolves to its LAST value, matching every other
        // duplicate-resolution path: Context::requestQuery(name), HttpRequest::query,
        // the parsed-form scalar compaction, and the raw cookie lookup all take the
        // last occurrence (commit 5523295). This flattened list was the lone holdout
        // keeping the first, so requestQuery("a") and iterating requestQuery()
        // disagreed on ?a=1&a=2. The group is name-sorted with stable ties, so
        // order[build.end - 1] is the last occurrence. requestQueries() below still
        // lists every value in order.
        const auto lastIndex = order[build.end - 1];
        detail::RequestNameValueListAccess::pushBack(
            query,
            detail::RequestNameValueViewAccess::make(
                detail::storedStringView(storage[lastIndex * 2]),
                detail::storedStringView(storage[lastIndex * 2 + 1])));

        auto& group = groups.append(detail::pairNameAt(storage, build.firstIndex));
        for (std::size_t i = build.begin; i < build.end; ++i) {
            const auto pairIndex = order[i];
            group.add(detail::storedStringView(storage[pairIndex * 2 + 1]));
        }
    }

    cache.emplace(
        std::move(storage),
        std::move(query),
        std::move(groups));
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
    const auto headers = request_.headers();
    for (std::size_t i = headers.size(); i > 0; --i) {
        const auto& header = headers[i - 1];
        if (!detail::httpAsciiEqualsIgnoreCase(header.name(), "Cookie")) {
            continue;
        }
        if (auto value = detail::httpFindSemicolonParameter(header.value(), name)) {
            return value;
        }
    }
    return std::nullopt;
}

const RequestNameValueList& Context::requestCookies() const {
    auto& cache = requestStorage().cookies;
    if (!cache) {
        std::size_t cookieCount = 0;
        for (const auto& header : request_.headers()) {
            if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Cookie")) {
                cookieCount += detail::delimitedFieldCount(header.value(), ';');
            }
        }

        auto cookies = detail::RequestNameValueListAccess::make(resource());
        detail::RequestNameValueListAccess::reserve(cookies, detail::boundedFieldReserve(cookieCount));
        for (const auto& header : request_.headers()) {
            if (!detail::httpAsciiEqualsIgnoreCase(header.name(), "Cookie")) {
                continue;
            }
            detail::httpVisitSemicolonParameters(
                header.value(),
                [&cookies](std::string_view key, std::string_view value) {
                    detail::RequestNameValueListAccess::pushBack(
                        cookies,
                        detail::RequestNameValueViewAccess::make(key, value));
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
    std::size_t encodedValueCount = 0;
    for (std::size_t i = 0; i < paramCount_; ++i) {
        if (!detail::validateUrlEncoding(paramValues_[i])) {
            requestStorage_->routeParamsInvalid = true;
            detail::throwInvalidParam();
        }
        if (detail::hasUrlEncoding(paramValues_[i], detail::UrlDecodeMode::kPercent)) {
            ++encodedValueCount;
        }
    }

    // Route names and unencoded captures already borrow stable route/request
    // storage. Own only decoded values, keeping the cache compact while making
    // every returned view stable for the whole Context lifetime.
    std::pmr::vector<std::pmr::string> storage(resource());
    auto params = detail::RequestNameValueListAccess::make(resource());
    storage.reserve(encodedValueCount);
    detail::RequestNameValueListAccess::reserve(params, paramCount_);
    for (std::size_t i = 0; i < paramCount_; ++i) {
        auto value = paramValues_[i];
        if (detail::hasUrlEncoding(value, detail::UrlDecodeMode::kPercent)) {
            auto decoded = detail::decodeUrlComponent(
                value,
                detail::UrlDecodeMode::kPercent,
                resource());
            if (!decoded) {
                requestStorage_->routeParamsInvalid = true;
                detail::throwInvalidParam();
            }
            auto& owned = storage.emplace_back(std::move(*decoded));
            value = detail::storedStringView(owned);
        }
        detail::RequestNameValueListAccess::pushBack(
            params,
            detail::RequestNameValueViewAccess::make(paramNames_[i], value));
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
    int bestSpecificity = -1;
    int bestQuality = 0;
    bool sawAccept = false;
    for (const auto& header : request_.headers()) {
        if (!detail::httpAsciiEqualsIgnoreCase(header.name(), "Accept")) {
            continue;
        }
        sawAccept = true;
        if (!header.value().empty()) {
            detail::httpAccumulateMediaTypeAcceptance(
                header.value(), mediaType, bestSpecificity, bestQuality);
        }
    }
    // Only absence means no preference. A present but empty Accept field is an
    // empty media-range list and therefore matches no representation.
    if (!sawAccept) {
        return true;
    }
    return bestSpecificity >= 0 && bestQuality > 0;
}

Task<std::string_view> Context::requestBody() const {
    if (bodyDecoded_) {
        const auto& decoded = *requestStorage_->decodedBody;
        co_return std::string_view(decoded);
    }

    std::string_view raw;
    if (const auto* lazy = requestBodySource_.lazy()) {
        raw = co_await lazy->loader().readAll();
    } else if (requestBodySource_.streaming() != nullptr) {
        throw std::logic_error("streaming request body cannot be buffered");
    } else {
        raw = detail::requestBodyBytes(request_);
    }

    // Transparently decode a request body whose Content-Encoding we understand,
    // so handlers always see the decoded representation (RFC 9110 §8.4).
    const auto parsedCoding = detail::requestContentCoding(request_);
    if (const auto* invalid = parsedCoding.invalid()) {
        throw HttpProtocolError(
            invalid->status(), "invalid request Content-Encoding");
    }
    if (const auto* unsupported = parsedCoding.unsupported()) {
        throw detail::UnsupportedRequestContentCoding(*unsupported);
    }
    const auto coding = *parsedCoding.coding();
    if (coding == detail::HttpContentCoding::kIdentity) {
        co_return raw;
    }
    auto decodeResult = detail::decodeHttpRequestContent(
        coding,
        raw,
        maxDecodedBodyBytes_,
        resource());
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
    std::string_view name,
    std::string_view secret) const {
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
    return detail::contentTypeMatches(
        detail::requestKnownHeader(request_, detail::RequestKnownHeader::kContentType),
        expected);
}

Task<std::pmr::vector<MultipartPart>> Context::requestMultipart() const {
    const auto boundary = multipartBoundary();
    const auto requestBody = co_await this->requestBody();
    co_return detail::parseCompleteMultipartBody(requestBody, boundary, resource());
}

Task<ContextRequest::RequestFormData> Context::parseRequestBody(
    ContextRequest::ParseBodyOptions options) const {
    const auto requestBody = co_await this->requestBody();
    co_return detail::parseFormBodyFromView(
        detail::requestKnownHeader(request_, detail::RequestKnownHeader::kContentType),
        requestBody,
        resource(),
        options);
}

Task<void> Context::requestDiscardBody() const {
    if (const auto* lazy = requestBodySource_.lazy()) {
        co_await lazy->loader().discard();
        co_return;
    }
    if (const auto* streaming = requestBodySource_.streaming()) {
        while (co_await streaming->reader().read()) {}
    }
}

BodyReader& Context::requestBodyReader() const {
    const auto* streaming = requestBodySource_.streaming();
    if (streaming == nullptr) {
        throw std::logic_error("request body is not streamable");
    }
    return streaming->reader();
}

MultipartReader Context::requestMultipartReader() const {
    return MultipartReader(requestBodyReader(), multipartBoundary(), resource());
}

MultipartBoundary Context::multipartBoundary() const {
    const auto boundary = detail::httpParseMultipartBoundary(
        detail::requestKnownHeader(request_, detail::RequestKnownHeader::kContentType));
    if (const auto* parsed = boundary.boundary()) {
        return *parsed;
    }
    if (boundary.notApplicable() != nullptr) {
        throw std::invalid_argument("invalid multipart content type");
    }
    if (const auto* failure = boundary.failure()) {
        throw failure->protocolError();
    }
    throw std::logic_error("unexpected multipart boundary parse result");
}

}  // namespace ruvia
