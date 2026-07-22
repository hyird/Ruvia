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
#include "ruvia/http/detail/MultipartParsing.h"
#include "ruvia/http/detail/RequestBodyDecoding.h"
#include "ruvia/web/detail/http/RequestBodyLoader.h"
#include "ruvia/http/detail/AsciiCase.h"
#include "ruvia/http/UrlEncoding.h"
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

[[nodiscard]] bool assignUrlDecodedOrCopy(
    std::pmr::string& output,
    std::string_view input,
    detail::UrlDecodeMode mode) {
    if (detail::hasUrlEncoding(input, mode)) {
        auto decoded = detail::decodeUrlComponent(
            input,
            mode,
            output.get_allocator().resource());
        if (decoded.has_value()) {
            output = std::move(*decoded);
            return true;
        }
        return false;
    }
    output.assign(input.data(), input.size());
    return true;
}

[[nodiscard]] bool fieldNameIsArray(std::string_view name) noexcept {
    return name.ends_with("[]");
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
    return value;
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
    // The original position is an explicit tie-breaker, so an in-place sort has
    // the same deterministic order as stable_sort without its non-PMR scratch
    // allocation on the request path.
    std::ranges::sort(order, [&storage](std::size_t left, std::size_t right) noexcept {
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
    std::ranges::sort(order, [&fields](std::size_t left, std::size_t right) noexcept {
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
    if (options.dottedNames == ContextRequest::DottedNamePolicy::kExpandPath) {
        if (fieldNameHasProtoObject(field.name())) {
            return;
        }
        assignDotPath(field, fields.get_allocator().resource());
    }

    // Reject before the field vector (and the sorts over it) can grow without
    // bound from an attacker-supplied body of many tiny fields.
    if (fields.size() >= options.maxFields) {
        detail::throwTooManyFormFields();
    }
    fields.emplace_back(std::move(field));
}

void compactParsedBodyFields(
    std::pmr::vector<ContextRequest::RequestFormField>& fields,
    ContextRequest::ParseBodyOptions options) {
    if (options.repeatedScalars == ContextRequest::RepeatedScalarPolicy::kRetainAll ||
        fields.size() < 2) {
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
            // Retain every array ("name[]") field, and every file part: a
            // standard <input type=file multiple> emits several parts under one
            // non-"[]" name, and collapsing them as repeated scalars would
            // silently drop all but the last upload. Only true repeated scalars
            // (text fields) collapse to their last value.
            if (fields[index].array() || fields[index].file()) {
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
            std::destroy_at(&fields[write]);
            std::construct_at(&fields[write], std::move(fields[read]));
        }
        ++write;
    }
    while (fields.size() > write) {
        fields.pop_back();
    }
}

[[nodiscard]] ContextRequest::RequestFormData parseUrlEncodedFormBody(
    std::string_view requestBody,
    std::pmr::memory_resource* resource,
    ContextRequest::ParseBodyOptions options) {
    std::pmr::vector<ContextRequest::RequestFormField> fields(resource);
    fields.reserve(boundedFieldReserve(delimitedFieldCount(requestBody, '&')));
    bool valid = true;
    const bool ok = detail::visitUrlEncodedPairs(
        requestBody,
        [resource, &fields, &valid, options](std::string_view key, std::string_view value) {
            auto decodedName = detail::decodeUrlComponent(
                key,
                detail::UrlDecodeMode::kForm,
                resource);
            auto decodedValue = detail::decodeUrlComponent(
                value,
                detail::UrlDecodeMode::kForm,
                resource);
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
    return detail::RequestFormDataAccess::fromFields(std::move(fields));
}

[[nodiscard]] std::pmr::vector<MultipartPart> parseCompleteMultipartBody(
    std::string_view requestBody,
    MultipartBoundary boundary,
    std::pmr::memory_resource* resource) {
    auto parsed = parseMultipartBody(requestBody, std::move(boundary), resource);
    if (const auto* failure = parsed.failure()) {
        throw failure->protocolError();
    }
    auto* body = parsed.body();
    if (body == nullptr) {
        throw std::logic_error("unexpected multipart body parse result");
    }
    return std::move(*body).takeParts();
}

[[nodiscard]] ContextRequest::RequestFormData parseMultipartFormBody(
    std::string_view requestBody,
    MultipartBoundary boundary,
    std::pmr::memory_resource* resource,
    ContextRequest::ParseBodyOptions options) {
    auto parts = parseCompleteMultipartBody(
        requestBody, std::move(boundary), resource);
    std::pmr::vector<ContextRequest::RequestFormField> fields(resource);
    fields.reserve(parts.size());
    for (const auto& part : parts) {
        const auto partName = part.name();
        const auto partBody = part.body();
        const auto partFilename = part.filename();
        const auto partContentType = part.contentType();
        std::pmr::string name(partName.data(), partName.size(), resource);
        const bool array = fieldNameIsArray(std::string_view(name));
        // RFC 7578 section 4.4: a part without a Content-Type defaults to
        // text/plain. Surface that effective type to the form consumer rather
        // than an empty string (the raw multipart parts API stays faithful).
        std::pmr::string contentType = partContentType.empty()
            ? std::pmr::string("text/plain", resource)
            : std::pmr::string(partContentType.data(), partContentType.size(), resource);
        appendParsedBodyField(
            fields,
            detail::RequestFormFieldAccess::make(
                resource,
                std::move(name),
                std::pmr::string(partBody.data(), partBody.size(), resource),
                std::pmr::string(partFilename.data(), partFilename.size(), resource),
                std::move(contentType),
                !partFilename.empty(),
                array),
            options);
    }
    compactParsedBodyFields(fields, options);
    return detail::RequestFormDataAccess::fromFields(std::move(fields));
}

[[nodiscard]] ContextRequest::RequestFormData parseFormBodyFromView(
    std::string_view contentType,
    std::string_view requestBody,
    std::pmr::memory_resource* resource,
    ContextRequest::ParseBodyOptions options) {
    if (detail::contentTypeMatches(contentType, "application/x-www-form-urlencoded")) {
        return parseUrlEncodedFormBody(requestBody, resource, options);
    }

    const auto boundary = detail::httpParseMultipartBoundary(contentType);
    if (const auto* parsed = boundary.boundary()) {
        return parseMultipartFormBody(
            requestBody,
            *parsed,
            resource,
            options);
    }
    if (boundary.notApplicable() != nullptr) {
        return detail::RequestFormDataAccess::empty(resource);
    }
    if (const auto* failure = boundary.failure()) {
        throw failure->protocolError();
    }
    throw std::logic_error("unexpected multipart boundary parse result");
}

}  // namespace

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
            appendLowerAscii(name, rawHeader.name());
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

    const auto pairCount = delimitedFieldCount(request_.queryString(), '&');
    std::pmr::vector<std::pmr::string> storage(resource());
    storage.reserve(boundedFieldReserve(pairCount * 2));
    bool valid = true;
    const bool completed = detail::visitUrlEncodedPairs(
        request_.queryString(),
        [this, &storage, &valid](std::string_view key, std::string_view value) {
            std::pmr::string decodedName(resource());
            std::pmr::string decodedValue(resource());
            if (!assignUrlDecodedOrCopy(decodedName, key, detail::UrlDecodeMode::kForm) ||
                !assignUrlDecodedOrCopy(decodedValue, value, detail::UrlDecodeMode::kForm)) {
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
                storedStringView(storage[lastIndex * 2]),
                storedStringView(storage[lastIndex * 2 + 1])));

        auto& group = groups.append(pairNameAt(storage, build.firstIndex));
        for (std::size_t i = build.begin; i < build.end; ++i) {
            const auto pairIndex = order[i];
            group.add(storedStringView(storage[pairIndex * 2 + 1]));
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
                cookieCount += delimitedFieldCount(header.value(), ';');
            }
        }

        auto cookies = detail::RequestNameValueListAccess::make(resource());
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
            value = storedStringView(owned);
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
    co_return parseCompleteMultipartBody(requestBody, boundary, resource());
}

Task<ContextRequest::RequestFormData> Context::parseRequestBody(
    ContextRequest::ParseBodyOptions options) const {
    const auto requestBody = co_await this->requestBody();
    co_return parseFormBodyFromView(
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
