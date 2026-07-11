#include "ruvia/web/Context.h"

#include "ruvia/web/detail/CookieSignature.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HeaderAcceptUtils.h"
#include "ruvia/http/detail/HttpCommonInternal.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/web/detail/http/ContextRequestInternal.h"
#include "ruvia/http/detail/MultipartParsing.h"
#include "ruvia/http/detail/RequestBodyDecoding.h"
#include "ruvia/web/detail/http/RequestBodyLoader.h"
#include "ruvia/http/detail/AsciiCase.h"
#include "ruvia/web/Error.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/web/detail/model/Parser.h"

#include <algorithm>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ruvia {

ConnInfo getConnInfo(const Context& context) noexcept {
    return ConnInfo(
        context.remoteAddress_,
        context.clientCertificateSubject_,
        context.secure_);
}

namespace detail {

[[noreturn]] void throwInvalidJsonContentType() {
    throw std::invalid_argument("invalid json content type");
}

[[noreturn]] void throwInvalidJsonBody() {
    throw std::invalid_argument("invalid json body");
}

[[noreturn]] void throwInvalidFormContentType() {
    throw std::invalid_argument("invalid form content type");
}

[[noreturn]] void throwInvalidFormBody() {
    throw std::invalid_argument("invalid form body");
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

namespace {

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

// Cap on the up-front reservation for a parsed name/value vector. delimitedFieldCount
// counts every delimiter, including the empty segments that the parser then skips
// (visitUrlEncodedPairs / httpVisitSemicolonParameters), so an untrusted input of
// only delimiters -- e.g. a 16 MiB body of '&' at the buffered-body limit -- would
// reserve millions of heavy field objects while producing none, amplifying a small
// body into a huge allocation. Bound the reservation: growth past it is amortized
// O(1), so a legitimate large input is unaffected while the attacker-controlled
// over-reservation is capped.
inline constexpr std::size_t kMaxParsedFieldReserve = 4096;

[[nodiscard]] std::size_t boundedFieldReserve(std::size_t count) noexcept {
    return count < kMaxParsedFieldReserve ? count : kMaxParsedFieldReserve;
}

void appendLowerAscii(std::pmr::string& output, std::string_view input) {
    for (const char ch : input) {
        output.push_back(static_cast<char>(detail::httpAsciiToLower(static_cast<unsigned char>(ch))));
    }
}

void assignUrlDecodedOrCopy(
    std::pmr::string& output,
    std::string_view input,
    detail::UrlDecodeMode mode) {
    if (detail::hasUrlEncoding(input, mode)) {
        if (detail::decodeUrlComponent(input, output, mode)) {
            return;
        }
    }
    output.assign(input.data(), input.size());
}

[[nodiscard]] bool fieldNameIsArray(std::string_view name) noexcept {
    return name.size() >= 2 && name.substr(name.size() - 2) == "[]";
}

[[nodiscard]] bool fieldNameHasProtoObject(std::string_view name) noexcept {
    std::size_t offset = 0;
    for (;;) {
        const auto found = name.find("__proto__.", offset);
        if (found == std::string_view::npos) {
            return false;
        }
        if (found == 0 || name[found - 1] == '.') {
            return true;
        }
        offset = found + 1;
    }
}

void assignDotPath(
    ContextRequest::RequestFormField& field,
    std::pmr::memory_resource* resource) {
    auto& path = detail::RequestFormFieldAccess::path(field);
    path.clear();
    std::string_view remaining = field.name();
    while (!remaining.empty()) {
        const auto dot = remaining.find('.');
        const auto segment = dot == std::string_view::npos
            ? remaining
            : remaining.substr(0, dot);
        path.emplace_back(std::pmr::string(segment.data(), segment.size(), resource));
        if (dot == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(dot + 1);
    }
}

[[nodiscard]] std::string_view storedStringView(const std::pmr::string& value) noexcept {
    return std::string_view(value.data(), value.size());
}

[[nodiscard]] std::string_view pairNameAt(
    const std::pmr::vector<std::pmr::string>& storage,
    std::size_t index) noexcept {
    return storedStringView(storage[index * 2]);
}

[[nodiscard]] std::pmr::vector<std::size_t> sortedPairOrder(
    const std::pmr::vector<std::pmr::string>& storage,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<std::size_t> order(resource);
    const auto count = storage.size() / 2;
    order.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&storage](std::size_t left, std::size_t right) noexcept {
        const auto leftName = pairNameAt(storage, left);
        const auto rightName = pairNameAt(storage, right);
        if (leftName == rightName) {
            return left < right;
        }
        return leftName < rightName;
    });
    return order;
}

[[nodiscard]] std::pmr::vector<std::size_t> sortedFormFieldOrder(
    const std::pmr::vector<ContextRequest::RequestFormField>& fields,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<std::size_t> order(resource);
    order.reserve(fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&fields](std::size_t left, std::size_t right) noexcept {
        const auto leftName = fields[left].name();
        const auto rightName = fields[right].name();
        if (leftName == rightName) {
            return left < right;
        }
        return leftName < rightName;
    });
    return order;
}

void appendParsedBodyField(
    std::pmr::vector<ContextRequest::RequestFormField>& fields,
    ContextRequest::RequestFormField&& field,
    ContextRequest::ParseBodyOptions options) {
    if (options.dot) {
        if (fieldNameHasProtoObject(field.name())) {
            return;
        }
        assignDotPath(field, fields.get_allocator().resource());
    }

    fields.emplace_back(std::move(field));
}

void compactParsedBodyFields(
    std::pmr::vector<ContextRequest::RequestFormField>& fields,
    ContextRequest::ParseBodyOptions options) {
    if (options.all || fields.size() < 2) {
        return;
    }

    auto* const resource = fields.get_allocator().resource();
    const auto order = sortedFormFieldOrder(fields, resource);
    std::pmr::vector<unsigned char> keep(resource);
    keep.resize(fields.size(), 0);

    for (std::size_t offset = 0; offset < order.size();) {
        const auto name = fields[order[offset]].name();
        std::optional<std::size_t> lastScalar;
        do {
            const auto index = order[offset];
            if (fields[index].array()) {
                keep[index] = 1;
            } else {
                lastScalar = index;
            }
            ++offset;
        } while (offset < order.size() && fields[order[offset]].name() == name);
        if (lastScalar.has_value()) {
            keep[*lastScalar] = 1;
        }
    }

    std::size_t write = 0;
    for (std::size_t read = 0; read < fields.size(); ++read) {
        if (keep[read] == 0) {
            continue;
        }
        if (write != read) {
            fields[write] = std::move(fields[read]);
        }
        ++write;
    }
    fields.erase(fields.begin() + write, fields.end());
}

[[nodiscard]] ContextRequest::RequestFormData parseUrlEncodedFormBody(
    std::string_view requestBody,
    std::pmr::memory_resource* resource,
    ContextRequest::ParseBodyOptions options,
    ContextRequest::RequestFormData::SingleValueSelection singleValueSelection) {
    std::pmr::vector<ContextRequest::RequestFormField> fields(resource);
    fields.reserve(boundedFieldReserve(delimitedFieldCount(requestBody, '&')));
    bool valid = true;
    const bool ok = detail::visitUrlEncodedPairs(
        requestBody,
        [resource, &fields, &valid, options](std::string_view key, std::string_view value) {
            auto decodedName = detail::decodeUrlComponentToString(key, resource, detail::UrlDecodeMode::kForm);
            auto decodedValue = detail::decodeUrlComponentToString(value, resource, detail::UrlDecodeMode::kForm);
            if (!decodedName || !decodedValue) {
                valid = false;
                return false;
            }

            const bool array = fieldNameIsArray(std::string_view(decodedName->data(), decodedName->size()));
            appendParsedBodyField(
                fields,
                detail::RequestFormFieldAccess::make(
                    resource,
                    std::move(*decodedName),
                    std::move(*decodedValue),
                    std::pmr::string(resource),
                    std::pmr::string(resource),
                    false,
                    array),
                options);
            return true;
        });
    if (!ok || !valid) {
        throw std::invalid_argument("invalid form body");
    }
    compactParsedBodyFields(fields, options);
    return ContextRequest::RequestFormData(std::move(fields), singleValueSelection);
}

[[nodiscard]] ContextRequest::RequestFormData parseMultipartFormBody(
    std::string_view requestBody,
    MultipartBoundary boundary,
    std::pmr::memory_resource* resource,
    ContextRequest::ParseBodyOptions options,
    ContextRequest::RequestFormData::SingleValueSelection singleValueSelection) {
    auto parts = parseMultipartBody(requestBody, std::move(boundary), resource);
    std::pmr::vector<ContextRequest::RequestFormField> fields(resource);
    fields.reserve(parts.size());
    for (const auto& part : parts) {
        const auto partName = part.name();
        const auto partBody = part.body();
        const auto partFilename = part.filename();
        const auto partContentType = part.contentType();
        std::pmr::string name(partName.data(), partName.size(), resource);
        const bool array = fieldNameIsArray(std::string_view(name.data(), name.size()));
        appendParsedBodyField(
            fields,
            detail::RequestFormFieldAccess::make(
                resource,
                std::move(name),
                std::pmr::string(partBody.data(), partBody.size(), resource),
                std::pmr::string(partFilename.data(), partFilename.size(), resource),
                std::pmr::string(partContentType.data(), partContentType.size(), resource),
                !partFilename.empty(),
                array),
            options);
    }
    compactParsedBodyFields(fields, options);
    return ContextRequest::RequestFormData(std::move(fields), singleValueSelection);
}

[[nodiscard]] ContextRequest::RequestFormData parseFormBodyFromView(
    std::string_view contentType,
    std::string_view requestBody,
    std::pmr::memory_resource* resource,
    ContextRequest::ParseBodyOptions options,
    ContextRequest::RequestFormData::SingleValueSelection singleValueSelection =
        ContextRequest::RequestFormData::SingleValueSelection::kLast) {
    if (detail::contentTypeMatches(contentType, "application/x-www-form-urlencoded")) {
        return parseUrlEncodedFormBody(requestBody, resource, options, singleValueSelection);
    }

    const auto boundary = detail::httpParseMultipartBoundary(contentType);
    if (const auto* parsed = boundary.boundary()) {
        return parseMultipartFormBody(
            requestBody,
            *parsed,
            resource,
            options,
            singleValueSelection);
    }
    if (const auto* failure = boundary.failure()) {
        switch (failure->error()) {
        case detail::HttpMultipartBoundaryParseError::kInvalidContentType:
            return ContextRequest::RequestFormData(resource, singleValueSelection);
        case detail::HttpMultipartBoundaryParseError::kInvalidBoundary:
            throw std::invalid_argument("invalid multipart boundary");
        }
    }
    throw std::logic_error("unexpected multipart boundary parse result");
}

}  // namespace

std::string_view ContextRequest::RawRequestClone::header(std::string_view name) const noexcept {
    for (auto it = headers_.rbegin(); it != headers_.rend(); ++it) {
        if (detail::httpAsciiEqualsIgnoreCase(it->name(), name)) {
            return it->value();
        }
    }
    return {};
}

ContextRequest::RequestFormData ContextRequest::RawRequestClone::parseBody(ParseBodyOptions options) const {
    return parseFormBodyFromView(
        header("Content-Type"),
        body(),
        body_.get_allocator().resource(),
        options);
}

ContextRequest::RequestFormData ContextRequest::RawRequestClone::formData() const {
    return parseFormBodyFromView(
        header("Content-Type"),
        body(),
        body_.get_allocator().resource(),
        ParseBodyOptions{.all = true},
        ContextRequest::RequestFormData::SingleValueSelection::kFirst);
}

const RequestNameValueList& Context::requestHeaders() const {
    if (requestHeaders_ == nullptr) {
        const auto rawHeaders = request_.headers();
        auto& names = memory_.emplace<std::pmr::vector<std::pmr::string>>(resource());
        auto& headers = memory_.emplace<RequestNameValueList>(
            detail::RequestNameValueListAccess::make(resource()));
        names.reserve(rawHeaders.size());
        detail::RequestNameValueListAccess::reserve(headers, rawHeaders.size());
        for (const auto& rawHeader : rawHeaders) {
            auto& name = names.emplace_back();
            name.reserve(rawHeader.name().size());
            appendLowerAscii(name, rawHeader.name());
            detail::RequestNameValueListAccess::pushBack(
                headers,
                detail::RequestNameValueViewAccess::make(
                    std::string_view(name.data(), name.size()),
                    rawHeader.value()));
        }
        requestHeaders_ = &headers;
    }
    return *requestHeaders_;
}

std::optional<std::string_view> Context::requestHeader(std::string_view name) const {
    // Case-insensitive scan directly over the raw header span — no per-lookup allocation and no
    // forced materialization of the full lowercased header map (requestHeaders() builds that only
    // for the list accessor). Last match wins, mirroring RequestNameValueList::get()'s reverse scan.
    const auto rawHeaders = request_.headers();
    for (auto it = rawHeaders.rbegin(); it != rawHeaders.rend(); ++it) {
        if (detail::httpAsciiEqualsIgnoreCase(it->name(), name)) {
            return it->value();
        }
    }
    return std::nullopt;
}

void Context::ensureRequestQuery() const {
    if (requestQuery_ != nullptr) {
        return;
    }

    const auto pairCount = delimitedFieldCount(request_.queryString(), '&');
    auto& storage = memory_.emplace<std::pmr::vector<std::pmr::string>>(resource());
    storage.reserve(boundedFieldReserve(pairCount * 2));
    (void)detail::visitUrlEncodedPairs(
        request_.queryString(),
        [this, &storage](std::string_view key, std::string_view value) {
            std::pmr::string decodedName(resource());
            std::pmr::string decodedValue(resource());
            assignUrlDecodedOrCopy(decodedName, key, detail::UrlDecodeMode::kForm);
            assignUrlDecodedOrCopy(decodedValue, value, detail::UrlDecodeMode::kForm);

            storage.push_back(std::move(decodedName));
            storage.push_back(std::move(decodedValue));
            return true;
        });

    struct QueryBuild final {
        std::size_t firstIndex;
        std::size_t begin;
        std::size_t end;
    };

    const auto order = sortedPairOrder(storage, resource());
    std::pmr::vector<QueryBuild> builds(resource());
    builds.reserve(order.size());
    for (std::size_t offset = 0; offset < order.size();) {
        const auto begin = offset;
        const auto firstIndex = order[offset];
        const auto name = pairNameAt(storage, firstIndex);
        do {
            ++offset;
        } while (offset < order.size() && pairNameAt(storage, order[offset]) == name);
        builds.push_back(QueryBuild{.firstIndex = firstIndex, .begin = begin, .end = offset});
    }
    std::stable_sort(builds.begin(), builds.end(), [](const QueryBuild& left, const QueryBuild& right) noexcept {
        return left.firstIndex < right.firstIndex;
    });

    auto& query = memory_.emplace<RequestNameValueList>(detail::RequestNameValueListAccess::make(resource()));
    auto& groups = memory_.emplace<RequestValueGroupList>(detail::RequestValueGroupListAccess::make(resource()));
    detail::RequestNameValueListAccess::reserve(query, builds.size());
    detail::RequestValueGroupListAccess::reserve(groups, builds.size());
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
                storedStringView(storage[lastIndex * 2]),
                storedStringView(storage[lastIndex * 2 + 1])));

        auto group = detail::RequestValueGroupAccess::make(resource(), pairNameAt(storage, build.firstIndex));
        for (std::size_t i = build.begin; i < build.end; ++i) {
            const auto pairIndex = order[i];
            detail::RequestValueGroupAccess::add(group, storedStringView(storage[pairIndex * 2 + 1]));
        }
        detail::RequestValueGroupListAccess::pushBack(groups, std::move(group));
    }

    requestQueryStorage_ = &storage;
    requestQueriesStorage_ = &storage;
    requestQuery_ = &query;
    requestQueries_ = &groups;
}

const RequestNameValueList& Context::requestQuery() const {
    ensureRequestQuery();
    return *requestQuery_;
}

std::optional<std::string_view> Context::requestQuery(std::string_view name) const {
    std::optional<std::string_view> result;
    (void)detail::visitUrlEncodedPairs(
        request_.queryString(),
        [this, name, &result](std::string_view encodedName, std::string_view encodedValue) {
            if (!detail::urlComponentEquals(encodedName, name, detail::UrlDecodeMode::kForm)) {
                return true;
            }
            if (!detail::hasUrlEncoding(encodedValue, detail::UrlDecodeMode::kForm)) {
                result = encodedValue;
                return true;
            }

            auto& decoded = memory_.emplace<std::pmr::string>(resource());
            assignUrlDecodedOrCopy(decoded, encodedValue, detail::UrlDecodeMode::kForm);
            result = storedStringView(decoded);
            return true;
        });
    return result;
}

const RequestValueGroupList& Context::requestQueries() const {
    ensureRequestQuery();
    return *requestQueries_;
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
    if (requestCookies_ == nullptr) {
        std::size_t cookieCount = 0;
        for (const auto& header : request_.headers()) {
            if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Cookie")) {
                cookieCount += delimitedFieldCount(header.value(), ';');
            }
        }

        auto& cookies = memory_.emplace<RequestNameValueList>(
            detail::RequestNameValueListAccess::make(resource()));
        detail::RequestNameValueListAccess::reserve(cookies, boundedFieldReserve(cookieCount));
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
        requestCookies_ = &cookies;
    }
    return *requestCookies_;
}

const std::pmr::vector<ContextRequest::MatchedRoute>& Context::requestMatchedRoutes() const {
    if (matchedRoutes_ == nullptr) {
        auto& routes = memory_.emplace<std::pmr::vector<ContextRequest::MatchedRoute>>(resource());
        if (!routePath_.empty() && routeMethod_ != HttpKnownMethod::kUnknown) {
            const auto method = knownHttpMethodToken(routeMethod_);
            routes.reserve(routeMiddlewareCount_ + 1);
            for (std::size_t i = 0; i < routeMiddlewareCount_; ++i) {
                routes.push_back(ContextRequest::MatchedRoute{
                    .method = method,
                    .path = routePath_,
                    .kind = ContextRequest::MatchedRouteKind::kMiddleware});
            }
            routes.push_back(ContextRequest::MatchedRoute{
                .method = method,
                .path = routePath_,
                .kind = ContextRequest::MatchedRouteKind::kHandler});
        }
        matchedRoutes_ = &routes;
    }
    return *matchedRoutes_;
}

const RequestNameValueList& Context::routeParams() const {
    if (routeParams_ == nullptr) {
        auto& storage = memory_.emplace<std::pmr::vector<std::pmr::string>>(resource());
        auto& params = memory_.emplace<RequestNameValueList>(
            detail::RequestNameValueListAccess::make(resource()));
        storage.reserve(paramCount_ * 2);
        detail::RequestNameValueListAccess::reserve(params, paramCount_);
        for (std::size_t i = 0; i < paramCount_; ++i) {
            std::pmr::string name(paramNames_[i].data(), paramNames_[i].size(), resource());
            std::pmr::string value(resource());
            assignUrlDecodedOrCopy(value, paramValues_[i], detail::UrlDecodeMode::kPercent);
            storage.push_back(std::move(name));
            storage.push_back(std::move(value));
            const auto nameIndex = storage.size() - 2;
            detail::RequestNameValueListAccess::pushBack(
                params,
                detail::RequestNameValueViewAccess::make(
                    std::string_view(storage[nameIndex].data(), storage[nameIndex].size()),
                    std::string_view(storage[nameIndex + 1].data(), storage[nameIndex + 1].size())));
        }
        routeParamStorage_ = &storage;
        routeParams_ = &params;
    }
    return *routeParams_;
}

std::optional<std::string_view> Context::routeParam(std::string_view name) const {
    for (std::size_t i = paramCount_; i > 0; --i) {
        const auto index = i - 1;
        if (paramNames_[index] != name) {
            continue;
        }
        const auto value = paramValues_[index];
        if (!detail::hasUrlEncoding(value, detail::UrlDecodeMode::kPercent)) {
            return value;
        }
        auto& decoded = memory_.emplace<std::pmr::string>(resource());
        assignUrlDecodedOrCopy(decoded, value, detail::UrlDecodeMode::kPercent);
        return storedStringView(decoded);
    }
    return std::nullopt;
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
        if (!detail::httpAsciiEqualsIgnoreCase(header.name(), "Accept") || header.value().empty()) {
            continue;
        }
        sawAccept = true;
        detail::httpAccumulateMediaTypeAcceptance(header.value(), mediaType, bestSpecificity, bestQuality);
    }
    // No (non-empty) Accept header means the client accepts any media type.
    if (!sawAccept) {
        return true;
    }
    return bestSpecificity >= 0 && bestQuality > 0;
}

Task<std::string_view> Context::requestBody() const {
    if (bodyDecoded_) {
        const auto& decoded = *decodedBody_;
        co_return std::string_view(decoded.data(), decoded.size());
    }

    std::string_view raw;
    if (bodyLoader_ != nullptr) {
        raw = co_await bodyLoader_->readAll();
    } else if (bodyReader_ != nullptr) {
        throw std::logic_error("streaming request body cannot be buffered");
    } else {
        raw = detail::requestBodyBytes(request_);
    }

    // Transparently decode a request body whose Content-Encoding we understand,
    // so handlers always see the decoded representation (RFC 9110 §8.4).
    const auto coding = detail::requestContentCoding(request_);
    if (coding == detail::HttpContentCoding::kNone || raw.empty()) {
        co_return raw;
    }
    auto& decoded = decodedBody();
    decoded.clear();
    // Keep the decoded buffer in the arena (not inline SSO) so the returned view
    // survives the Context if a handler hands it to c.text(). See assignStableString.
    decoded.reserve(32);
    if (!detail::decodeRequestContentEncoding(coding, raw, decoded, detail::kMaxDecodedRequestBodyBytes)) {
        throw HttpError(400, "bad_request", "failed to decode request body");
    }
    bodyDecoded_ = true;
    co_return std::string_view(decoded.data(), decoded.size());
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

Task<ContextRequest::RawRequestClone> ContextRequest::cloneRawRequest() const {
    RawRequestClone clone(context_->resource());
    clone.method_.assign(raw().method().data(), raw().method().size());
    const auto requestUrl = url();
    clone.url_.assign(requestUrl.data(), requestUrl.size());
    clone.path_.assign(raw().path().data(), raw().path().size());
    clone.headers_.reserve(raw().headers().size());
    for (const auto& header : raw().headers()) {
        clone.headers_.push_back(
            RawRequestClone::Header::make(context_->resource(), header.name(), header.value()));
    }
    const auto requestBody = co_await text();
    clone.body_.assign(requestBody.data(), requestBody.size());
    co_return std::move(clone);
}

bool Context::requestContentTypeMatches(std::string_view expected) const noexcept {
    return detail::contentTypeMatches(
        detail::requestKnownHeader(request_, detail::RequestKnownHeader::kContentType),
        expected);
}

Task<std::pmr::vector<MultipartPart>> Context::requestMultipart() const {
    const auto boundary = multipartBoundary();
    const auto requestBody = co_await this->requestBody();
    co_return parseMultipartBody(requestBody, boundary, resource());
}

Task<ContextRequest::RequestFormData> Context::parseRequestBody(
    ContextRequest::ParseBodyOptions options,
    ContextRequest::RequestFormData::SingleValueSelection singleValueSelection) const {
    const auto requestBody = co_await this->requestBody();
    co_return parseFormBodyFromView(
        detail::requestKnownHeader(request_, detail::RequestKnownHeader::kContentType),
        requestBody,
        resource(),
        options,
        singleValueSelection);
}

Task<void> Context::requestDiscardBody() const {
    if (bodyLoader_ != nullptr) {
        co_await bodyLoader_->discard();
        co_return;
    }
    if (bodyReader_ != nullptr) {
        while (co_await bodyReader_->read()) {}
    }
}

BodyReader& Context::requestBodyReader() const {
    if (bodyReader_ == nullptr) {
        throw std::logic_error("request body is not streamable");
    }
    return *bodyReader_;
}

MultipartReader Context::requestMultipartReader() const {
    return MultipartReader(requestBodyReader(), multipartBoundary(), resource());
}

WebSocket& Context::webSocket() const {
    if (webSocket_ == nullptr) {
        throw std::logic_error("websocket is not available");
    }
    return *webSocket_;
}

ResponseStreamWriter& Context::stream() const {
    if (responseStream_ == nullptr) {
        throw std::logic_error("response body is not streamable");
    }
    return *responseStream_;
}

ResponseStreamWriter& Context::streamText() {
    setStableResponseHeader("Content-Type", "text/plain; charset=UTF-8");
    setStableResponseHeader("X-Content-Type-Options", "nosniff");
    return stream();
}

SseWriter Context::streamSSE() const {
    return SseWriter(stream());
}

MultipartBoundary Context::multipartBoundary() const {
    const auto boundary = detail::httpParseMultipartBoundary(
        detail::requestKnownHeader(request_, detail::RequestKnownHeader::kContentType));
    if (const auto* parsed = boundary.boundary()) {
        return *parsed;
    }
    if (const auto* failure = boundary.failure()) {
        switch (failure->error()) {
        case detail::HttpMultipartBoundaryParseError::kInvalidContentType:
            throw std::invalid_argument("invalid multipart content type");
        case detail::HttpMultipartBoundaryParseError::kInvalidBoundary:
            throw std::invalid_argument("invalid multipart boundary");
        }
    }
    throw std::logic_error("unexpected multipart boundary parse result");
}

}  // namespace ruvia
