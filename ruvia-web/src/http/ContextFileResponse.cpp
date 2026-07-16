#include "ruvia/web/Context.h"

#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/HttpResponseFileAccess.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/detail/ResponseHeaderUtils.h"
#include "ruvia/http/detail/HttpByteRange.h"
#include "ruvia/http/detail/HttpConditionalRequest.h"
#include "ruvia/http/detail/HttpDate.h"
#include "ruvia/http/detail/HttpEntityTag.h"
#include "ruvia/web/detail/StaticFilesInternal.h"
#include "ruvia/web/detail/StaticFileMetadata.h"
#include "ruvia/web/detail/server/HttpNativeFile.h"
#include "ruvia/web/detail/StaticPathNormalization.h"
#include "ruvia/http/detail/HeaderAcceptUtils.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpContentCoding.h"
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
#include <variant>

namespace ruvia {
namespace {

inline constexpr std::size_t kFileResponseHeaderReserve = 7;

struct FileConditionalHeaders final {
    std::string_view ifUnmodifiedSince;
    std::string_view ifModifiedSince;
    std::string_view range;
    std::string_view ifRange;
    bool hasIfRange;
};

struct EtagFieldCondition final {
    bool present{false};
    bool valid{true};
    bool matched{false};
    bool wildcard{false};
    std::size_t lineCount{0};

    void update(
        std::string_view value,
        std::string_view expected,
        bool strong) noexcept {
        present = true;
        ++lineCount;
        const auto trimmed = detail::httpTrimOws(value);
        if (trimmed == "*") {
            wildcard = true;
            if (lineCount != 1) {
                valid = false;
            }
            return;
        }
        if (wildcard) {
            valid = false;
        }
        const auto result = detail::httpParseEtagListMatches(
            value, expected, strong);
        valid = valid && result.valid;
        matched = matched || result.matched;
    }

    [[nodiscard]] bool matches() const noexcept {
        return valid && ((wildcard && lineCount == 1) || (!wildcard && matched));
    }
};

struct FileEtagConditions final {
    EtagFieldCondition ifMatch;
    EtagFieldCondition ifNoneMatch;
};

[[nodiscard]] FileEtagConditions fileEtagConditions(
    const HttpRequest& request,
    std::string_view etag) noexcept {
    FileEtagConditions result;
    const bool hasIfMatch = detail::requestHasKnownHeader(
        request, detail::RequestKnownHeader::kIfMatch);
    const bool hasIfNoneMatch = detail::requestHasKnownHeader(
        request, detail::RequestKnownHeader::kIfNoneMatch);
    if (!hasIfMatch && !hasIfNoneMatch) {
        return result;
    }

    // Both conditions are RFC list fields. Multiple field lines are equivalent
    // to comma-joining their values (RFC 9110 §5.3), but the request keeps
    // zero-copy views into separate wire lines. Fold them in one header scan and
    // retain whole-list validity without allocating a joined string.
    for (const auto& header : request.headers()) {
        if (hasIfMatch && detail::httpAsciiEqualsIgnoreCase(
                header.name(), "If-Match")) {
            result.ifMatch.update(header.value(), etag, true);
        } else if (hasIfNoneMatch && detail::httpAsciiEqualsIgnoreCase(
                       header.name(), "If-None-Match")) {
            result.ifNoneMatch.update(header.value(), etag, false);
        }
    }
    return result;
}

[[nodiscard]] bool httpDateNotModified(std::string_view header, std::time_t modifiedSeconds) noexcept {
    const auto date = detail::httpParseHttpDate(detail::httpTrimOws(header));
    return date.has_value() && modifiedSeconds <= *date;
}

[[nodiscard]] bool httpDateUnmodified(std::string_view header, std::time_t modifiedSeconds) noexcept {
    const auto date = detail::httpParseHttpDate(detail::httpTrimOws(header));
    return !date.has_value() || modifiedSeconds <= *date;
}

[[nodiscard]] bool ifRangeAllows(
    std::string_view header,
    std::string_view etag,
    std::time_t modifiedSeconds,
    bool dateValidatorStrong) noexcept {
    if (header.empty()) {
        return false;
    }
    const auto value = detail::httpTrimOws(header);
    if (!value.empty() && (value.front() == '"' || value.starts_with("W/"))) {
        return detail::httpStrongEtagEquals(value, etag);
    }
    if (!dateValidatorStrong) {
        return false;
    }
    // An If-Range date requires an EXACT match against Last-Modified (RFC 9110
    // §13.1.5 / RFC 7233 §3.2: "the comparison ... uses an exact match"), NOT the
    // "<=" not-modified-since comparison. If-Range's job is to confirm the client
    // still holds the byte-identical representation before a range is stitched in;
    // a representation whose Last-Modified is merely older (a rollback or a restore
    // that moves mtime backwards) is a DIFFERENT entity, and serving a 206 from it
    // would corrupt the client's reassembled copy. Only equality means "unchanged".
    const auto date = detail::httpParseHttpDate(value);
    return date.has_value() && modifiedSeconds == *date;
}

[[nodiscard]] FileConditionalHeaders fileConditionalHeaders(const HttpRequest& request) noexcept {
    return FileConditionalHeaders{
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfUnmodifiedSince),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfModifiedSince),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kRange),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfRange),
        detail::requestHasKnownHeader(request, detail::RequestKnownHeader::kIfRange)};
}

// The path travels by value until HttpResponse takes its own copy. StaticRoot is
// a public value whose lifetime is not coupled to the returned response, so an
// indexed entry must never leak its internal native-path pointer into the body.
class FileResponsePath final {
public:
    [[nodiscard]] static FileResponsePath copying(
        std::filesystem::path path,
        detail::ResponseFileIdentity identity) {
        return FileResponsePath(std::move(path), identity);
    }

    [[nodiscard]] static FileResponsePath copyingNative(
        const detail::NativePathChar* path,
        detail::ResponseFileIdentity identity) {
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
        HttpResponse& response,
        std::uint64_t size,
        std::uint64_t offset,
        std::uint64_t length) {
        detail::setResponseFileBody(
            response,
            takePath(),
            size,
            offset,
            length,
            identity_);
    }

    void setFullBody(
        HttpResponse& response,
        std::uint64_t size) {
        setBody(response, size, 0, size);
    }

private:
    explicit FileResponsePath(
        std::filesystem::path path,
        detail::ResponseFileIdentity identity) noexcept
        : path_(std::move(path)), identity_(identity) {}

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

template <typename ApplyResponseState>
[[nodiscard]] HttpResponse makeFileResponse(
    const Context& context,
    const HttpRequest& request,
    FileResponsePath filePath,
    std::uint64_t size,
    std::uint64_t modifiedToken,
    std::time_t modifiedSeconds,
    std::string_view contentType,
    std::string_view cacheControl,
    bool enableRanges,
    bool enableValidators,
    std::string_view precomputedEtag,
    std::string_view precomputedLastModified,
    detail::HttpContentCoding contentCoding,
    bool negotiatesEncoding,
    ApplyResponseState applyResponseState) {
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
    const bool lastModifiedIsActual = modifiedSeconds <= responseSeconds;
    const auto validatorModifiedSeconds = lastModifiedIsActual
        ? modifiedSeconds
        : responseSeconds;
    if (enableValidators) {
        if (precomputedEtag.empty()) {
            etagStorage = detail::makeStaticFileSnapshotEtag(
                context.resource(),
                size,
                modifiedToken,
                filePath.identity());
            etag = etagStorage;
        } else {
            etag = precomputedEtag;
        }
        if (precomputedLastModified.empty() || !lastModifiedIsActual) {
            lastModifiedStorage = detail::httpFormatDate(
                context.resource(), validatorModifiedSeconds);
            lastModified = lastModifiedStorage;
        } else {
            lastModified = precomputedLastModified;
        }
    }

    auto addFileHeaders = [&](HttpResponse& response) {
        detail::reserveResponseHeaders(response, kFileResponseHeaderReserve);
        if (contentType.empty()) {
            detail::setResponseHeaderStableView(
                response,
                "Content-Type",
                filePath.guessedContentType());
        } else {
            response.header("Content-Type", contentType);
        }
        if (!cacheControl.empty()) {
            response.header("Cache-Control", cacheControl);
        }
        // A precompressed variant carries the original Content-Type with the
        // encoding declared here.
        const auto contentEncoding =
            detail::httpContentCodingToken(contentCoding);
        if (!contentEncoding.empty()) {
            detail::setResponseHeaderStableView(
                response,
                "Content-Encoding",
                contentEncoding);
        }
        if (enableRanges) {
            detail::setResponseHeaderStableView(response, "Accept-Ranges", "bytes");
        }
        if (enableValidators) {
            response.header("ETag", etag);
            response.header("Last-Modified", lastModified);
        }
    };
    auto applyFileResponseState = [&](
        HttpResponse& response,
        std::optional<std::uint16_t> statusCode) {
        applyResponseState(response, statusCode);
        // Declare the negotiation dimension after Context response metadata is
        // applied. A caller-provided Vary value must be merged, not allowed to
        // overwrite Accept-Encoding and make differently encoded variants share
        // one cache entry (RFC 9110 12.5.5 / RFC 9111 4.1). Context::file does no
        // Accept-Encoding negotiation and stays Vary-free.
        if (negotiatesEncoding) {
            detail::addVaryToken(response, "Accept-Encoding");
        }
    };
    auto setFileBody = [&](HttpResponse& response, std::uint64_t offset, std::uint64_t length) {
        filePath.setBody(response, size, offset, length);
    };
    auto setFullFileBody = [&](HttpResponse& response) {
        filePath.setFullBody(response, size);
    };
    auto makeHeaderOnlyResponse = [&](
        std::optional<std::uint16_t> statusCode) {
        HttpResponse response(context.resource());
        addFileHeaders(response);
        applyFileResponseState(response, statusCode);
        return response;
    };
    auto makeFullFileResponse = [&](
        std::optional<std::uint16_t> statusCode) {
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
        if (etagConditions.ifMatch.present &&
            !etagConditions.ifMatch.matches()) {
            throw HttpError(412, "precondition_failed", "file precondition failed");
        }
        // RFC 9110 §13.2.2 step 2: If-Unmodified-Since is evaluated only when If-Match
        // is absent -- a present If-Match takes precedence and the (weaker) date
        // condition MUST be ignored, exactly as If-Modified-Since is ignored below
        // when If-None-Match is present. Presence is tracked separately because an
        // empty list is still a present field and must take precedence over the date.
        if (!etagConditions.ifMatch.present &&
            !conditional.ifUnmodifiedSince.empty() &&
            !httpDateUnmodified(
                conditional.ifUnmodifiedSince, validatorModifiedSeconds)) {
            throw HttpError(412, "precondition_failed", "file precondition failed");
        }

        if (etagConditions.ifNoneMatch.matches()) {
            if (methodPlan.usesNotModifiedResponse) {
                return makeHeaderOnlyResponse(304);
            }
            throw HttpError(412, "precondition_failed", "file precondition failed");
        }

        if (methodPlan.evaluatesIfModifiedSince &&
            !etagConditions.ifNoneMatch.present &&
            !conditional.ifModifiedSince.empty() &&
            httpDateNotModified(
                conditional.ifModifiedSince, validatorModifiedSeconds)) {
            return makeHeaderOnlyResponse(304);
        }
    }

    // RFC 9110 §14.2 defines Range only for GET. In particular, HEAD must
    // describe the full selected representation rather than returning partial
    // response metadata for content that will never be sent.
    if (methodPlan.evaluatesRange &&
        enableRanges && !conditional.range.empty()) {
        // RFC 9110 13.1.5: honor the Range only if a present If-Range matches
        // the current representation. When validators are disabled this root
        // exposes no ETag/Last-Modified, so an If-Range can never be confirmed
        // -- the condition MUST be treated as not matching and the full
        // representation served, rather than a 206 stitched from bytes the
        // client cannot verify it still holds. Gating on enableValidators (as
        // before) skipped the check entirely and returned a 206. A range with
        // no If-Range is still honored without validators.
        if (conditional.hasIfRange &&
            (!enableValidators || !ifRangeAllows(
                conditional.ifRange,
                etag,
                validatorModifiedSeconds,
                lastModifiedIsActual))) {
            return makeFullFileResponse(std::nullopt);
        }

        const auto rangeResolution = detail::resolveHttpByteRange(
            conditional.range, size);
        if (rangeResolution.ignored()) {
            // Unknown units, invalid/unsupported sets, and ranges over an
            // empty representation follow the RFC 9110 §14.2 ignore policy.
            return makeFullFileResponse(std::nullopt);
        }
        if (rangeResolution.unsatisfiable()) {
            HttpResponse response(context.resource());
            detail::setResponseContentRangeUnsatisfied(response, size);
            addFileHeaders(response);
            applyFileResponseState(response, 416);
            return response;
        }

        const auto& resolved = *rangeResolution.resolved();
        HttpResponse response(context.resource());
        addFileHeaders(response);
        detail::setResponseContentRange(
            response, resolved.offset(), resolved.length(), size);
        setFileBody(response, resolved.offset(), resolved.length());
        applyFileResponseState(response, 206);
        return response;
    }

    return makeFullFileResponse(std::nullopt);
}

}  // namespace

HttpResponse Context::file(
    const std::filesystem::path& path,
    std::string_view contentType) const {
    std::error_code ec;
    const auto snapshot = detail::snapshotResponseFile(path.c_str(), ec);
    if (ec) {
        throw HttpError(404, "not_found", "file not found");
    }

    const auto applyState = [this](
        HttpResponse& response,
        std::optional<std::uint16_t> statusCode) {
        applyResponseState(response, statusCode);
    };
    return makeFileResponse(
        *this,
        request_,
        FileResponsePath::copying(path, snapshot.identity),
        snapshot.size,
        snapshot.modifiedToken,
        snapshot.modifiedSeconds,
        contentType,
        {},
        true,
        true,
        {},
        {},
        detail::HttpContentCoding::kIdentity,
        false,  // Context::file serves one path with no Accept-Encoding negotiation
        applyState);
}

// Selects the best precompressed sidecar (foo.js.br / .gz / .zst) the client
// accepts and that exists in the index — highest Accept-Encoding q-value wins,
// ties resolve br > zstd > gzip. The served bytes are the variant's, so its
// size/etag/modified describe the wire representation; the caller keeps the
// original Content-Type. Index lookups only (no per-request filesystem stat).
class StaticFileRepresentation final {
public:
    StaticFileRepresentation(
        detail::StaticRootEntryView entry,
        detail::HttpContentCoding contentCoding) noexcept
        : entry_(entry),
          contentCoding_(contentCoding) {}

    [[nodiscard]] const detail::StaticRootEntryView& entry() const noexcept {
        return entry_;
    }

    [[nodiscard]] detail::HttpContentCoding contentCoding() const noexcept {
        return contentCoding_;
    }

private:
    detail::StaticRootEntryView entry_;
    detail::HttpContentCoding contentCoding_;
};

[[nodiscard]] StaticFileRepresentation selectStaticFileRepresentation(
    const StaticRoot& root,
    std::string_view relative,
    const HttpRequest& request,
    std::pmr::memory_resource* resource,
    detail::StaticRootEntryView identity) {
    StaticFileRepresentation selected(
        identity,
        detail::HttpContentCoding::kIdentity);
    detail::HttpResponseCodingQualities qualities;
    for (const auto& header : request.headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Accept-Encoding")) {
            qualities.update(header.value());
        }
    }

    struct Candidate final {
        std::string_view suffix;
        detail::HttpContentCoding contentCoding;
        int score;
    };
    const Candidate candidates[] = {
        {".br",
         detail::HttpContentCoding::kBrotli,
         detail::httpAcceptedEncodingScore(qualities.brotli)},
        {".zst",
         detail::HttpContentCoding::kZstd,
         detail::httpAcceptedEncodingScore(qualities.zstd)},
        {".gz",
         detail::HttpContentCoding::kGzip,
         detail::httpAcceptedEncodingScore(qualities.gzip)},
    };

    int best = detail::httpAcceptedIdentityScore(qualities.identity);
    for (const auto& candidate : candidates) {
        if (candidate.score < best ||
            (candidate.score == best &&
             selected.contentCoding() != detail::HttpContentCoding::kIdentity)) {
            continue;
        }
        std::pmr::string variantPath(resource);
        variantPath.reserve(relative.size() + candidate.suffix.size());
        variantPath.assign(relative.data(), relative.size());
        variantPath.append(candidate.suffix.data(), candidate.suffix.size());
        if (const auto entry =
                detail::StaticRootAccess::findVariant(root, variantPath);
            entry.has_value()) {
            best = candidate.score;
            selected = StaticFileRepresentation(
                *entry,
                candidate.contentCoding);
        }
    }
    return selected;
}

HttpResponse Context::staticFile(
    const StaticRoot& root,
    std::string_view relativePath,
    std::string_view contentType) const {
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
    if (detail::hasUrlEncoding(
            relativePath,
            detail::UrlDecodeMode::kPercent)) {
        decodedPath = detail::decodeUrlComponent(
            relativePath,
            detail::UrlDecodeMode::kPercent,
            resource());
    }
    const std::string_view lookupPath = decodedPath.has_value()
        ? std::string_view(*decodedPath)
        : relativePath;
    if (lookupPath.find('\0') != std::string_view::npos) {
        throw HttpError(403, "forbidden", "invalid static file path");
    }
    auto relative = detail::normalizeStaticRelativePath(lookupPath, allocator<char>());

    if (relative.empty() && !detail::StaticRootAccess::hasDirectoryIndex(root)) {
        throw HttpError(403, "forbidden", "invalid static file path");
    }

    auto entry = detail::StaticRootAccess::find(root, relative);
    if (!entry.has_value() &&
        detail::StaticRootAccess::isIndexedDirectory(root, relative)) {
        if (!relative.empty() && relative.back() != '/') {
            relative.push_back('/');
        }
        const auto indexFile = detail::StaticRootAccess::indexFile(root);
        relative.append(indexFile.data(), indexFile.size());
        entry = detail::StaticRootAccess::find(root, relative);
    }
    if (!entry.has_value()) {
        throw HttpError(404, "not_found", "file not found");
    }
    const auto& baseEntry = *entry;

    // Serve a precompressed sidecar when the client accepts one; the bytes and
    // validators come from the variant, the Content-Type from the base entry.
    const auto served = selectStaticFileRepresentation(
        root,
        relative,
        request_,
        resource(),
        baseEntry);
    const auto& servedEntry = served.entry();

    const auto applyState = [this](
        HttpResponse& response,
        std::optional<std::uint16_t> statusCode) {
        applyResponseState(response, statusCode);
    };
    return makeFileResponse(
        *this,
        request_,
        FileResponsePath::copyingNative(
            servedEntry.filePath(), servedEntry.identity()),
        servedEntry.size(),
        servedEntry.modifiedToken(),
        servedEntry.modifiedSeconds(),
        contentType.empty() ? baseEntry.contentType() : contentType,
        baseEntry.cacheControl(),
        baseEntry.rangesEnabled(),
        baseEntry.validatorsEnabled(),
        servedEntry.etag(),
        servedEntry.lastModified(),
        served.contentCoding(),
        true,  // staticFile negotiates the representation by Accept-Encoding
        applyState);
}

}  // namespace ruvia
