#include "ruvia/web/Context.h"

#include "ruvia/http/detail/request/HttpRequestAccess.h"
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
#include "ruvia/http/detail/coding/HttpContentCoding.h"
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

namespace ruvia {
namespace {

inline constexpr std::size_t kFileResponseHeaderReserve = 7;

// The path travels by value until HttpResponse takes its own copy. StaticRoot is
// a public value whose lifetime is not coupled to the returned response, so an
// indexed entry must never leak its internal native-path pointer into the body.
class FileResponsePath final {
public:
    [[nodiscard]] static FileResponsePath copying(std::filesystem::path path, detail::ResponseFileIdentity identity) {
        return FileResponsePath(std::move(path), identity);
    }

    [[nodiscard]] static FileResponsePath copyingNative(const detail::NativePathChar* path, detail::ResponseFileIdentity identity) {
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

    void setBody(HttpResponse& response, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
        detail::setResponseFileBody(response, takePath(), size, offset, length, identity_);
    }

    void setFullBody(HttpResponse& response, std::uint64_t size) {
        setBody(response, size, 0, size);
    }

private:
    explicit FileResponsePath(std::filesystem::path path, detail::ResponseFileIdentity identity) noexcept
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

// What one file response describes: which bytes, when they last changed, and
// the policy the serving route attached to them. Fifteen positional arguments
// at a call site said none of that; designated initializers do.
struct FileResponseSource final {
    FileResponsePath path;
    std::uint64_t size{0};
    std::uint64_t modifiedToken{0};
    std::time_t modifiedSeconds{0};
    std::string_view contentType;
    std::string_view cacheControl;
    bool enableRanges{false};
    bool enableValidators{false};
    std::string_view precomputedEtag;
    std::string_view precomputedLastModified;
    detail::HttpContentCoding contentCoding{detail::HttpContentCoding::kIdentity};
    bool negotiatesEncoding{false};
};

template <typename ApplyResponseState>
[[nodiscard]] HttpResponse makeFileResponse(const Context& context, const HttpRequest& request, FileResponseSource source, ApplyResponseState applyResponseState) {
    std::pmr::string etagStorage(context.resource());
    std::pmr::string lastModifiedStorage(context.resource());
    std::string_view etag;
    std::string_view lastModified;
    // RFC 9110 §8.8.2.1 forbids an origin server from emitting a
    // Last-Modified value later than the message origination time. Filesystems
    // can legitimately contain future mtimes (clock skew, archives, or an
    // explicit timestamp), so use this response's current second for the wire
    // validator and every date precondition evaluated against it. The clamped
    // value is not the representation's actual validator and therefore cannot
    // be a strong If-Range validator (RFC 9110 §13.1.5).
    const auto responseSeconds = std::time(nullptr);
    const bool lastModifiedIsActual = source.modifiedSeconds <= responseSeconds;
    const auto validatorModifiedSeconds = lastModifiedIsActual ? source.modifiedSeconds : responseSeconds;
    if (source.enableValidators) {
        if (source.precomputedEtag.empty()) {
            etagStorage = detail::makeStaticFileSnapshotEtag(context.resource(), source.size, source.modifiedToken, source.path.identity());
            etag = etagStorage;
        } else {
            etag = source.precomputedEtag;
        }
        if (source.precomputedLastModified.empty() || !lastModifiedIsActual) {
            lastModifiedStorage = detail::httpFormatDate(context.resource(), validatorModifiedSeconds);
            lastModified = lastModifiedStorage;
        } else {
            lastModified = source.precomputedLastModified;
        }
    }

    auto addFileHeaders = [&](HttpResponse& response) {
        detail::reserveResponseHeaders(response, kFileResponseHeaderReserve);
        if (source.contentType.empty()) {
            detail::setResponseHeaderStableView(response, "Content-Type", source.path.guessedContentType());
        } else {
            response.header("Content-Type", source.contentType);
        }
        if (!source.cacheControl.empty()) {
            response.header("Cache-Control", source.cacheControl);
        }
        // A precompressed variant carries the original Content-Type with the
        // encoding declared here.
        const auto contentEncoding = detail::httpContentCodingToken(source.contentCoding);
        if (!contentEncoding.empty()) {
            detail::setResponseHeaderStableView(response, "Content-Encoding", contentEncoding);
        }
        if (source.enableRanges) {
            detail::setResponseHeaderStableView(response, "Accept-Ranges", "bytes");
        }
        if (source.enableValidators) {
            response.header("ETag", etag);
            response.header("Last-Modified", lastModified);
        }
    };
    auto applyFileResponseState = [&](HttpResponse& response, std::optional<HttpStatusCode> statusCode) {
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
    auto setFileBody = [&](HttpResponse& response, std::uint64_t offset, std::uint64_t length) { source.path.setBody(response, source.size, offset, length); };
    auto setFullFileBody = [&](HttpResponse& response) { source.path.setFullBody(response, source.size); };
    auto makeHeaderOnlyResponse = [&](std::optional<HttpStatusCode> statusCode) {
        HttpResponse response(context.resource());
        addFileHeaders(response);
        applyFileResponseState(response, statusCode);
        return response;
    };
    auto makeFullFileResponse = [&](std::optional<HttpStatusCode> statusCode) {
        HttpResponse response(context.resource());
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
            throw HttpError(ruvia::http_status::kPreconditionFailed, "precondition_failed", "file precondition failed");
        }
        // RFC 9110 §13.2.2 step 2: If-Unmodified-Since is evaluated only when If-Match
        // is absent -- a present If-Match takes precedence and the (weaker) date
        // condition MUST be ignored, exactly as If-Modified-Since is ignored below
        // when If-None-Match is present. Presence is tracked separately because an
        // empty list is still a present field and must take precedence over the date.
        if (!etagConditions.ifMatch.present && !conditional.ifUnmodifiedSince.empty() && !httpDateUnmodified(conditional.ifUnmodifiedSince, validatorModifiedSeconds)) {
            throw HttpError(ruvia::http_status::kPreconditionFailed, "precondition_failed", "file precondition failed");
        }

        if (etagConditions.ifNoneMatch.matches()) {
            if (methodPlan.usesNotModifiedResponse) {
                return makeHeaderOnlyResponse(http_status::kNotModified);
            }
            throw HttpError(ruvia::http_status::kPreconditionFailed, "precondition_failed", "file precondition failed");
        }

        if (methodPlan.evaluatesIfModifiedSince && !etagConditions.ifNoneMatch.present && !conditional.ifModifiedSince.empty() && httpDateNotModified(conditional.ifModifiedSince, validatorModifiedSeconds)) {
            return makeHeaderOnlyResponse(http_status::kNotModified);
        }
    }

    // RFC 9110 §14.2 defines Range only for GET. In particular, HEAD must
    // describe the full selected representation rather than returning partial
    // response metadata for content that will never be sent.
    if (methodPlan.evaluatesRange && source.enableRanges && !conditional.range.empty()) {
        // RFC 9110 13.1.5: honor the Range only if a present If-Range matches
        // the current representation. When validators are disabled this root
        // exposes no ETag/Last-Modified, so an If-Range can never be confirmed
        // -- the condition MUST be treated as not matching and the full
        // representation served, rather than a 206 stitched from bytes the
        // client cannot verify it still holds. Gating on source.enableValidators (as
        // before) skipped the check entirely and returned a 206. A range with
        // no If-Range is still honored without validators.
        if (conditional.hasIfRange && (!source.enableValidators || !ifRangeAllows(conditional.ifRange, etag, validatorModifiedSeconds, lastModifiedIsActual))) {
            return makeFullFileResponse(std::nullopt);
        }

        const auto rangeResolution = detail::resolveHttpByteRange(conditional.range, source.size);
        if (rangeResolution.ignored()) {
            // Unknown units, invalid/unsupported sets, and ranges over an
            // empty representation follow the RFC 9110 §14.2 ignore policy.
            return makeFullFileResponse(std::nullopt);
        }
        if (rangeResolution.unsatisfiable()) {
            HttpResponse response(context.resource());
            detail::setResponseContentRangeUnsatisfied(response, source.size);
            addFileHeaders(response);
            applyFileResponseState(response, http_status::kRangeNotSatisfiable);
            return response;
        }

        const auto& resolved = *rangeResolution.resolved();
        HttpResponse response(context.resource());
        addFileHeaders(response);
        detail::setResponseContentRange(response, resolved.offset(), resolved.length(), source.size);
        setFileBody(response, resolved.offset(), resolved.length());
        applyFileResponseState(response, http_status::kPartialContent);
        return response;
    }

    return makeFullFileResponse(std::nullopt);
}

}  // namespace

HttpResponse Context::file(const std::filesystem::path& path, std::string_view contentType) const {
    std::error_code ec;
    const auto snapshot = detail::snapshotResponseFile(path.c_str(), ec);
    if (ec) {
        throw HttpError(ruvia::http_status::kNotFound, "not_found", "file not found");
    }

    const auto applyState = [this](HttpResponse& response, std::optional<HttpStatusCode> statusCode) { applyResponseState(response, statusCode); };
    return makeFileResponse(*this, request_,
        FileResponseSource{
            .path = FileResponsePath::copying(path, snapshot.identity),
            .size = snapshot.size,
            .modifiedToken = snapshot.modifiedToken,
            .modifiedSeconds = snapshot.modifiedSeconds,
            .contentType = contentType,
            .cacheControl = {},
            .enableRanges = true,
            .enableValidators = true,
            .precomputedEtag = {},
            .precomputedLastModified = {},
            .contentCoding = detail::HttpContentCoding::kIdentity,
            // A server-enabled static-file policy may transform this file in
            // the protocol session after the handler returns. Standalone
            // Context::file keeps its historical byte-for-byte behavior.
            .negotiatesEncoding = deferredStaticFileCompression_,
        },
        applyState);
}

HttpResponse Context::staticFile(const StaticRoot& root, std::string_view relativePath, std::string_view contentType) const {
    const auto mode = deferredStaticFileCompression_ ? detail::StaticFileSelectionMode::kAllowDeferredCompression : detail::StaticFileSelectionMode::kStrict;
    return staticFile(root, relativePath, contentType, mode);
}

HttpResponse Context::staticFile(const StaticRoot& root, std::string_view relativePath, std::string_view contentType, detail::StaticFileSelectionMode mode) const {
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
        decodedPath = detail::decodeUrlComponent(relativePath, detail::UrlDecodeMode::kPercent, resource());
    }
    const std::string_view lookupPath = decodedPath.has_value() ? std::string_view(*decodedPath) : relativePath;
    if (lookupPath.find('\0') != std::string_view::npos) {
        throw HttpError(ruvia::http_status::kForbidden, "forbidden", "invalid static file path");
    }
    auto relative = detail::normalizeStaticRelativePath(lookupPath, allocator<char>());

    if (relative.empty() && !detail::StaticRootAccess::hasDirectoryIndex(root)) {
        throw HttpError(ruvia::http_status::kForbidden, "forbidden", "invalid static file path");
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
        throw HttpError(ruvia::http_status::kNotFound, "not_found", "file not found");
    }
    const auto& baseEntry = *entry;

    // Serve a precompressed sidecar when the client accepts one; the bytes and
    // validators come from the variant, the Content-Type from the base entry.
    const auto served = selectStaticFileRepresentation(root, relative, request_, resource(), baseEntry, mode);
    if (!served.has_value()) {
        throw HttpError(ruvia::http_status::kNotAcceptable, "not_acceptable", "no acceptable response content coding");
    }
    const auto& servedEntry = served->entry();

    const auto applyState = [this](HttpResponse& response, std::optional<HttpStatusCode> statusCode) { applyResponseState(response, statusCode); };
    return makeFileResponse(*this, request_,
        FileResponseSource{
            .path = FileResponsePath::copyingNative(servedEntry.filePath(), servedEntry.identity()),
            .size = servedEntry.size(),
            .modifiedToken = servedEntry.modifiedToken(),
            .modifiedSeconds = servedEntry.modifiedSeconds(),
            .contentType = contentType.empty() ? baseEntry.contentType() : contentType,
            .cacheControl = baseEntry.cacheControl(),
            .enableRanges = baseEntry.rangesEnabled(),
            .enableValidators = baseEntry.validatorsEnabled(),
            .precomputedEtag = servedEntry.etag(),
            .precomputedLastModified = servedEntry.lastModified(),
            .contentCoding = served->contentCoding(),
            // staticFile negotiates the representation by Accept-Encoding.
            .negotiatesEncoding = true,
        },
        applyState);
}

}  // namespace ruvia
