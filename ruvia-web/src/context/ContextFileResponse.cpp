#include "ruvia/web/Context.h"

#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/response/HttpResponseFileAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/http/detail/response/ResponseHeaderUtils.h"
#include "ruvia/http/detail/field/HttpByteRange.h"
#include "ruvia/http/detail/field/HttpConditionalRequest.h"
#include "ruvia/http/detail/field/HttpDate.h"
#include "ruvia/web/detail/http/static/StaticFileMetadata.h"
#include "ruvia/web/detail/http/static/StaticRootIndex.h"
#include "ruvia/web/detail/http/static/FileConditionalRequest.h"
#include "ruvia/web/detail/http/static/StaticFileVariant.h"
#include "ruvia/web/detail/server/file/HttpNativeFile.h"
#include "ruvia/web/detail/http/static/StaticPathNormalization.h"
#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/http/UrlEncoding.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace ruvia {
namespace {

inline constexpr std::size_t kFileResponseHeaderReserve = 7;

// The path travels by value until HttpResponse takes its own copy. StaticRoot is
// a public value whose lifetime is not coupled to the returned response, so an
// indexed entry must never leak its internal native-path pointer into the body.
class FileResponsePath final {
public:
    [[nodiscard]] static FileResponsePath copying(
        std::filesystem::path path, detail::ResponseFileIdentity identity) {
        return FileResponsePath(std::move(path), identity);
    }

    [[nodiscard]] static FileResponsePath copyingNative(
        const detail::NativePathChar* path, detail::ResponseFileIdentity identity) {
        if (path == nullptr || *path == detail::NativePathChar{}) {
            throw std::logic_error("static file entry has no native path");
        }
        return copying(std::filesystem::path(path), identity);
    }

    [[nodiscard]] std::string_view guessedContentType() const noexcept {
        return detail::guessStaticFileContentType(path_);
    }

    [[nodiscard]] detail::ResponseFileIdentity identity() const noexcept {
        return identity_;
    }

    void setBody(
        HttpResponse& response, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
        detail::setResponseFileBody(response, takePath(), size, offset, length, identity_);
    }

    void setFullBody(HttpResponse& response, std::uint64_t size) {
        setBody(response, size, 0, size);
    }

private:
    explicit FileResponsePath(
        std::filesystem::path path, detail::ResponseFileIdentity identity) noexcept
        : path_(std::move(path)),
          identity_(identity) {}

    [[nodiscard]] std::filesystem::path takePath() {
        if (consumed_) {
            throw std::logic_error("file response path was consumed more than once");
        }
        consumed_ = true;
        return std::move(path_);
    }

    std::filesystem::path path_;
    detail::ResponseFileIdentity identity_;
    bool consumed_{false};
};

class FileResponseBodySource final {
public:
    [[nodiscard]] static FileResponseBodySource file(FileResponsePath path) {
        return FileResponseBodySource(std::move(path));
    }

    [[nodiscard]] static FileResponseBodySource bytes(std::string_view bytes) {
        return FileResponseBodySource(bytes);
    }

    [[nodiscard]] std::string_view guessedContentType() const noexcept {
        if (const auto* path = std::get_if<FileResponsePath>(&value_)) {
            return path->guessedContentType();
        }
        return "application/octet-stream";
    }

    [[nodiscard]] detail::ResponseFileIdentity identity() const noexcept {
        if (const auto* path = std::get_if<FileResponsePath>(&value_)) {
            return path->identity();
        }
        return detail::ResponseFileIdentity::unchecked();
    }

    void setBody(HttpResponse& response, std::pmr::memory_resource* resource, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length) {
        if (auto* path = std::get_if<FileResponsePath>(&value_)) {
            path->setBody(response, size, offset, length);
            return;
        }
        const auto bytes = std::get<std::string_view>(value_);
        if (offset > bytes.size() || length > bytes.size() - offset) {
            throw std::logic_error("static memory response slice is out of range");
        }
        std::pmr::string owned(
            bytes.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(length)),
            resource);
        detail::setResponseBodyOwned(response, std::move(owned));
    }

    void setFullBody(
        HttpResponse& response, std::pmr::memory_resource* resource, std::uint64_t size) {
        setBody(response, resource, size, 0, size);
    }

private:
    explicit FileResponseBodySource(FileResponsePath path) noexcept
        : value_(std::move(path)) {}

    explicit FileResponseBodySource(std::string_view bytes) noexcept
        : value_(bytes) {}

    std::variant<FileResponsePath, std::string_view> value_;
};

// What one file response describes: which bytes, when they last changed, and
// the policy the serving route attached to them. Fifteen positional arguments
// at a call site said none of that; designated initializers do.
struct FileResponseSource final {
    FileResponseBodySource body;
    std::uint64_t size{0};
    std::uint64_t modifiedToken{0};
    std::time_t modifiedSeconds{0};
    std::string_view contentType;
    std::string_view cacheControl;
    StaticRangeRequestPolicy rangeRequests{StaticRangeRequestPolicy::kIgnore};
    StaticResponseValidatorPolicy responseValidators{StaticResponseValidatorPolicy::kOmit};
    std::string_view precomputedEtag;
    std::string_view precomputedLastModified;
    HttpContentCoding contentCoding{HttpContentCoding::kIdentity};
    bool negotiatesEncoding{false};
};

template <typename ApplyResponseState>
[[nodiscard]] HttpResponse makeFileResponse(const Context& context, const HttpRequest& request,
    FileResponseSource source, ApplyResponseState applyResponseState) {
    std::pmr::string etagStorage(context.resource());
    std::pmr::string lastModifiedStorage(context.resource());
    std::string_view etag;
    std::string_view lastModified;
    const bool honorRangeRequests = source.rangeRequests == StaticRangeRequestPolicy::kHonor;
    const bool emitResponseValidators =
        source.responseValidators == StaticResponseValidatorPolicy::kEmit;
    // RFC 9110 §8.8.2.1 forbids an origin server from emitting a
    // Last-Modified value later than the message origination time. Filesystems
    // can legitimately contain future mtimes (clock skew, archives, or an
    // explicit timestamp), so use this response's current second for the wire
    // validator and every date precondition evaluated against it. The clamped
    // value is not the representation's actual validator and therefore cannot
    // be a strong If-Range validator (RFC 9110 §13.1.5).
    const auto responseSeconds = std::time(nullptr);
    const bool lastModifiedIsActual = source.modifiedSeconds <= responseSeconds;
    const auto validatorModifiedSeconds =
        lastModifiedIsActual ? source.modifiedSeconds : responseSeconds;
    if (emitResponseValidators) {
        if (source.precomputedEtag.empty()) {
            etagStorage = detail::makeStaticFileSnapshotEtag(
                context.resource(), source.size, source.modifiedToken, source.body.identity());
            etag = etagStorage;
        } else {
            etag = source.precomputedEtag;
        }
        if (source.precomputedLastModified.empty() || !lastModifiedIsActual) {
            lastModifiedStorage =
                detail::httpFormatDate(context.resource(), validatorModifiedSeconds);
            lastModified = lastModifiedStorage;
        } else {
            lastModified = source.precomputedLastModified;
        }
    }

    auto addFileHeaders = [&](HttpResponse& response) {
        detail::reserveResponseHeaders(response, kFileResponseHeaderReserve);
        if (source.contentType.empty()) {
            detail::setResponseHeaderStableView(
                response, "Content-Type", source.body.guessedContentType());
        } else {
            response.header("Content-Type", source.contentType);
        }
        if (!source.cacheControl.empty()) {
            response.header("Cache-Control", source.cacheControl);
        }
        // A precompressed variant carries the original Content-Type with the
        // encoding declared here.
        if (source.contentCoding != HttpContentCoding::kIdentity) {
            detail::setResponseHeaderStableView(
                response, "Content-Encoding", httpContentCodingToken(source.contentCoding));
        }
        if (honorRangeRequests) {
            detail::setResponseHeaderStableView(response, "Accept-Ranges", "bytes");
        }
        if (emitResponseValidators) {
            response.header("ETag", etag);
            response.header("Last-Modified", lastModified);
        }
    };
    auto applyFileResponseState = [&](HttpResponse& response,
                                      std::optional<HttpStatusCode> statusCode) {
        applyResponseState(response, statusCode);
        // Declare the negotiation dimension after Context response metadata is
        // applied. A caller-provided Vary value must be merged, not allowed to
        // overwrite Accept-Encoding and make differently encoded variants share
        // one cache entry (RFC 9110 12.5.5 / RFC 9111 4.1). Context::file does no
        // Accept-Encoding negotiation and stays Vary-free.
        if (source.negotiatesEncoding) {
            detail::addVaryToken(response, "Accept-Encoding");
        }
    };
    auto setFileBody = [&](HttpResponse& response, std::uint64_t offset, std::uint64_t length) {
        source.body.setBody(response, context.resource(), source.size, offset, length);
    };
    auto setFullFileBody = [&](HttpResponse& response) {
        source.body.setFullBody(response, context.resource(), source.size);
    };
    auto makeHeaderOnlyResponse = [&](std::optional<HttpStatusCode> statusCode) {
        HttpResponse response({.resource = context.resource()});
        addFileHeaders(response);
        applyFileResponseState(response, statusCode);
        return response;
    };
    auto makeFullFileResponse = [&](std::optional<HttpStatusCode> statusCode) {
        HttpResponse response({.resource = context.resource()});
        addFileHeaders(response);
        setFullFileBody(response);
        applyFileResponseState(response, statusCode);
        return response;
    };

    const auto method = request.knownMethod();
    const auto methodPlan = detail::httpConditionalMethodPlan(method);
    const auto conditional = fileConditionalHeaders(request);
    // Response validator generation is optional, but request preconditions are
    // method semantics. In particular, If-Match / If-None-Match "*" test the
    // existence of this current representation without needing an ETag, and
    // date conditions can use the file metadata without emitting Last-Modified.
    if (methodPlan.evaluatesPreconditions) {
        const auto etagConditions = fileEtagConditions(request, etag);
        if (etagConditions.ifMatch.present && !etagConditions.ifMatch.matches()) {
            throw HttpError({.status = ruvia::http_status::kPreconditionFailed,
                .code = "precondition_failed",
                .message = "file precondition failed"});
        }
        // RFC 9110 §13.2.2 step 2: If-Unmodified-Since is evaluated only when If-Match
        // is absent -- a present If-Match takes precedence and the (weaker) date
        // condition MUST be ignored, exactly as If-Modified-Since is ignored below
        // when If-None-Match is present. Presence is tracked separately because an
        // empty list is still a present field and must take precedence over the date.
        if (!etagConditions.ifMatch.present && !conditional.ifUnmodifiedSince.empty() &&
            !httpDateUnmodified(conditional.ifUnmodifiedSince, validatorModifiedSeconds)) {
            throw HttpError({.status = ruvia::http_status::kPreconditionFailed,
                .code = "precondition_failed",
                .message = "file precondition failed"});
        }

        if (etagConditions.ifNoneMatch.matches()) {
            if (methodPlan.usesNotModifiedResponse) {
                return makeHeaderOnlyResponse(http_status::kNotModified);
            }
            throw HttpError({.status = ruvia::http_status::kPreconditionFailed,
                .code = "precondition_failed",
                .message = "file precondition failed"});
        }

        if (methodPlan.evaluatesIfModifiedSince && !etagConditions.ifNoneMatch.present &&
            !conditional.ifModifiedSince.empty() &&
            httpDateNotModified(conditional.ifModifiedSince, validatorModifiedSeconds)) {
            return makeHeaderOnlyResponse(http_status::kNotModified);
        }
    }

    // RFC 9110 §14.2 defines Range only for GET. In particular, HEAD must
    // describe the full selected representation rather than returning partial
    // response metadata for content that will never be sent.
    if (methodPlan.evaluatesRange && honorRangeRequests && !conditional.range.empty()) {
        // RFC 9110 13.1.5: honor the Range only if a present If-Range matches
        // the current representation. When response validators are omitted,
        // this root exposes no ETag/Last-Modified, so an If-Range can never be
        // confirmed -- the condition MUST be treated as not matching and the
        // full representation served, rather than a 206 stitched from bytes the
        // client cannot verify it still holds. Gating on validator emission (as
        // before) skipped the check entirely and returned a 206. A range with
        // no If-Range is still honored without response validator headers.
        if (conditional.hasIfRange &&
            (!emitResponseValidators || !ifRangeAllows(conditional.ifRange, etag,
                                            validatorModifiedSeconds, lastModifiedIsActual))) {
            return makeFullFileResponse(std::nullopt);
        }

        const auto rangeResolution = detail::resolveHttpByteRange(conditional.range, source.size);
        if (rangeResolution.ignored()) {
            // Unknown units, invalid/unsupported sets, and ranges over an
            // empty representation follow the RFC 9110 §14.2 ignore policy.
            return makeFullFileResponse(std::nullopt);
        }
        if (rangeResolution.unsatisfiable()) {
            HttpResponse response({.resource = context.resource()});
            detail::setResponseContentRangeUnsatisfied(response, source.size);
            addFileHeaders(response);
            applyFileResponseState(response, http_status::kRangeNotSatisfiable);
            return response;
        }

        const auto& resolved = *rangeResolution.resolved();
        HttpResponse response({.resource = context.resource()});
        addFileHeaders(response);
        detail::setResponseContentRange(
            response, resolved.offset(), resolved.length(), source.size);
        setFileBody(response, resolved.offset(), resolved.length());
        applyFileResponseState(response, http_status::kPartialContent);
        return response;
    }

    return makeFullFileResponse(std::nullopt);
}

}  // namespace

HttpResponse Context::file(FileResponseOptions options) const {
    std::error_code ec;
    const auto snapshot = detail::snapshotResponseFile(options.path.c_str(), ec);
    if (ec) {
        throw HttpError({.status = ruvia::http_status::kNotFound,
            .code = "not_found",
            .message = "file not found"});
    }

    const auto contentType = options.contentType.view();
    const auto applyState = [this](
                                HttpResponse& response, std::optional<HttpStatusCode> statusCode) {
        applyResponseState(response, statusCode);
    };
    return makeFileResponse(*this, request_,
        FileResponseSource{
            .body = FileResponseBodySource::file(
                FileResponsePath::copying(std::move(options.path), snapshot.identity)),
            .size = snapshot.size,
            .modifiedToken = snapshot.modifiedToken,
            .modifiedSeconds = snapshot.modifiedSeconds,
            .contentType = contentType,
            .cacheControl = {},
            .rangeRequests = StaticRangeRequestPolicy::kHonor,
            .responseValidators = StaticResponseValidatorPolicy::kEmit,
            .precomputedEtag = {},
            .precomputedLastModified = {},
            .contentCoding = HttpContentCoding::kIdentity,
            .negotiatesEncoding = false,
        },
        applyState);
}

HttpResponse Context::staticFile(const StaticRoot& root, StaticFileResponseOptions options) const {
    const auto mode = precompressedStaticFiles_ ? detail::StaticFileSelectionMode::kPrecompressed
                                                : detail::StaticFileSelectionMode::kIdentityOnly;
    return staticFile(root, options, mode);
}

HttpResponse Context::staticFile(const StaticRoot& root, StaticFileResponseOptions options,
    detail::StaticFileSelectionMode mode) const {
    const auto relativePath = options.relativePath.view();
    const auto contentType = options.contentType.view();
    // Percent-decode the request path before matching it against the static index,
    // whose keys are the real (decoded) on-disk names -- so a file whose name holds
    // an encoded octet (a space "%20", UTF-8, parentheses, ...) resolves instead of
    // 404ing, per RFC 3986 2.1 / 6.2.2.2 percent-encoding equivalence. Decoding is
    // safe here: normalizeStaticRelativePath still clamps ".." at the root and
    // rejects absolute paths, and the lookup is a byte-exact index compare that
    // never joins the client path onto the filesystem, so the worst case is a miss
    // (404). A "%00" would inject a NUL that cannot occur in a filename, so reject
    // it; a malformed escape falls back to the raw bytes (which simply miss).
    std::optional<std::pmr::string> decodedPath;
    if (detail::hasUrlEncoding(relativePath, detail::UrlDecodeMode::kPercent)) {
        decodedPath = detail::decodeUrlComponent(
            relativePath, {.mode = detail::UrlDecodeMode::kPercent, .resource = resource()});
    }
    const std::string_view lookupPath =
        decodedPath.has_value() ? std::string_view(*decodedPath) : relativePath;
    if (lookupPath.find('\0') != std::string_view::npos) {
        throw HttpError({.status = ruvia::http_status::kForbidden,
            .code = "forbidden",
            .message = "invalid static file path"});
    }
    auto relative = detail::normalizeStaticRelativePath(lookupPath, allocator<char>());

    if (relative.empty() && !detail::StaticRootAccess::hasDirectoryIndex(root)) {
        throw HttpError({.status = ruvia::http_status::kForbidden,
            .code = "forbidden",
            .message = "invalid static file path"});
    }

    auto entry = detail::StaticRootAccess::find(root, relative);
    if (!entry.has_value() && detail::StaticRootAccess::isIndexedDirectory(root, relative)) {
        if (!relative.empty() && relative.back() != '/') {
            relative.push_back('/');
        }
        const auto indexFile = detail::StaticRootAccess::indexFile(root);
        relative.append(indexFile.data(), indexFile.size());
        entry = detail::StaticRootAccess::find(root, relative);
    }
    if (!entry.has_value()) {
        throw HttpError({.status = ruvia::http_status::kNotFound,
            .code = "not_found",
            .message = "file not found"});
    }
    const auto& baseEntry = *entry;

    // Serve a precompressed variant when the client accepts one; the bytes and
    // validators come from the variant, the Content-Type from the base entry.
    const auto served =
        selectStaticFileRepresentation(root, relative, request_, resource(), baseEntry, mode);
    if (!served.has_value()) {
        throw HttpError({.status = ruvia::http_status::kNotAcceptable,
            .code = "not_acceptable",
            .message = "no acceptable response content coding"});
    }
    const auto& servedEntry = served->entry();
    const auto* const memoryVariant = served->memoryVariant();
    const auto responseSize = memoryVariant == nullptr ? servedEntry.size() : memoryVariant->size();
    const auto responseModifiedToken =
        memoryVariant == nullptr ? servedEntry.modifiedToken() : memoryVariant->modifiedToken();
    const auto responseModifiedSeconds =
        memoryVariant == nullptr ? servedEntry.modifiedSeconds() : memoryVariant->modifiedSeconds();
    const auto responseEtag = memoryVariant == nullptr ? servedEntry.etag() : memoryVariant->etag();
    const auto responseLastModified =
        memoryVariant == nullptr ? servedEntry.lastModified() : memoryVariant->lastModified();

    const auto applyState = [this](
                                HttpResponse& response, std::optional<HttpStatusCode> statusCode) {
        applyResponseState(response, statusCode);
    };
    return makeFileResponse(*this, request_,
        FileResponseSource{
            .body = memoryVariant == nullptr
                        ? FileResponseBodySource::file(FileResponsePath::copyingNative(
                              servedEntry.filePath(), servedEntry.identity()))
                        : FileResponseBodySource::bytes(memoryVariant->bytes()),
            .size = responseSize,
            .modifiedToken = responseModifiedToken,
            .modifiedSeconds = responseModifiedSeconds,
            .contentType = contentType.empty() ? baseEntry.contentType() : contentType,
            .cacheControl = baseEntry.cacheControl(),
            .rangeRequests = baseEntry.rangeRequests(),
            .responseValidators = baseEntry.responseValidators(),
            .precomputedEtag = responseEtag,
            .precomputedLastModified = responseLastModified,
            .contentCoding = served->contentCoding(),
            // staticFile negotiates the representation by Accept-Encoding.
            .negotiatesEncoding = true,
        },
        applyState);
}

}  // namespace ruvia
