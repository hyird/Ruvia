#include "ruvia/web/Context.h"

#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/HttpResponseFileAccess.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/detail/FileResponseHelpers.h"
#include "ruvia/web/detail/StaticFilesInternal.h"
#include "ruvia/http/detail/StaticPathNormalization.h"
#include "ruvia/http/detail/HeaderAcceptUtils.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/UrlEncoding.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>

namespace ruvia {
namespace {

inline constexpr std::size_t kFileResponseHeaderReserve = 7;

struct FileConditionalHeaders final {
    std::string_view ifMatch;
    std::string_view ifUnmodifiedSince;
    std::string_view ifNoneMatch;
    std::string_view ifModifiedSince;
    std::string_view range;
    std::string_view ifRange;
};

[[nodiscard]] bool etagListMatches(
    std::string_view values,
    std::string_view expected,
    bool strong) noexcept {
    while (!values.empty()) {
        const auto comma = values.find(',');
        const auto value = detail::httpTrimOws(
            comma == std::string_view::npos ? values : values.substr(0, comma));
        if (strong ? detail::httpStrongEtagEquals(value, expected) : detail::httpWeakEtagEquals(value, expected)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        values.remove_prefix(comma + 1);
    }
    return false;
}

[[nodiscard]] bool ifMatchAllows(std::string_view header, std::string_view etag) noexcept {
    if (header.empty()) {
        return true;
    }
    if (detail::httpTrimOws(header) == "*") {
        return true;
    }
    return etagListMatches(header, etag, true);
}

[[nodiscard]] bool ifNoneMatchMatches(std::string_view header, std::string_view etag) noexcept {
    if (header.empty()) {
        return false;
    }
    if (detail::httpTrimOws(header) == "*") {
        return true;
    }
    return etagListMatches(header, etag, false);
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
    std::time_t modifiedSeconds) noexcept {
    if (header.empty()) {
        return true;
    }
    const auto value = detail::httpTrimOws(header);
    if (!value.empty() && (value.front() == '"' || value.starts_with("W/"))) {
        return detail::httpStrongEtagEquals(value, etag);
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
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfMatch),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfUnmodifiedSince),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfNoneMatch),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfModifiedSince),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kRange),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfRange)};
}

struct FileResponsePath final {
    const std::filesystem::path* path{nullptr};
    const detail::NativePathChar* nativePath{nullptr};
    bool borrowNativePath{false};
};

template <typename ApplyResponseState>
[[nodiscard]] HttpResponse makeFileResponse(
    const Context& context,
    FileResponsePath filePath,
    std::uint64_t size,
    std::filesystem::file_time_type modified,
    std::string_view contentType,
    std::string_view cacheControl,
    bool enableRanges,
    bool enableValidators,
    std::string_view precomputedEtag,
    std::string_view precomputedLastModified,
    std::string_view contentEncoding,
    bool negotiatesEncoding,
    ApplyResponseState applyResponseState) {
    std::pmr::string etagStorage(context.resource());
    std::pmr::string lastModifiedStorage(context.resource());
    std::string_view etag;
    std::string_view lastModified;
    std::time_t modifiedSeconds{};
    if (enableValidators) {
        modifiedSeconds = detail::httpFileTimeToTimeT(modified);
        if (precomputedEtag.empty() || precomputedLastModified.empty()) {
            etagStorage = detail::httpMakeFileEtag(context.resource(), size, modified);
            lastModifiedStorage = detail::httpFormatDate(context.resource(), modifiedSeconds);
            etag = etagStorage;
            lastModified = lastModifiedStorage;
        } else {
            etag = precomputedEtag;
            lastModified = precomputedLastModified;
        }
    }

    auto addFileHeaders = [&](HttpResponse& response) {
        detail::reserveResponseHeaders(response, kFileResponseHeaderReserve);
        if (contentType.empty()) {
            detail::setResponseHeaderStableView(response, "Content-Type", detail::httpGuessContentType(*filePath.path));
        } else {
            response.header("Content-Type", contentType);
        }
        if (!cacheControl.empty()) {
            response.header("Cache-Control", cacheControl);
        }
        // A precompressed variant carries the original Content-Type with the
        // encoding declared here.
        if (!contentEncoding.empty()) {
            detail::setResponseHeaderStableView(response, "Content-Encoding", contentEncoding);
        }
        // Declare Vary: Accept-Encoding on every response from an endpoint that
        // negotiates the representation by Accept-Encoding -- even when the
        // identity variant was served. Gating it on a chosen compressed variant
        // left the identity 200/206/304 with no Vary, so a shared cache keyed only
        // on the URL would serve that identity body to an encoding-capable client
        // (RFC 9110 12.5.5 / RFC 9111 4.1). Context::file does no negotiation and
        // stays Vary-free.
        if (negotiatesEncoding) {
            detail::setResponseHeaderStableView(response, "Vary", "Accept-Encoding");
        }
        if (enableRanges) {
            detail::setResponseHeaderStableView(response, "Accept-Ranges", "bytes");
        }
        if (enableValidators) {
            response.header("ETag", etag);
            response.header("Last-Modified", lastModified);
        }
    };
    auto setFileBody = [&](HttpResponse& response, std::uint64_t offset, std::uint64_t length) {
        if (filePath.borrowNativePath) {
            detail::setResponseBorrowedNativeFileBody(response, filePath.nativePath, size, offset, length);
        } else {
            detail::setResponseFileBody(response, *filePath.path, size, offset, length);
        }
    };
    auto setFullFileBody = [&](HttpResponse& response) {
        if (filePath.borrowNativePath) {
            detail::setResponseBorrowedNativeFileBody(response, filePath.nativePath, size);
        } else {
            detail::setResponseFileBody(response, *filePath.path, size);
        }
    };
    auto makeHeaderOnlyResponse = [&](std::uint16_t statusCode) {
        HttpResponse response(context.resource());
        addFileHeaders(response);
        applyResponseState(response, statusCode);
        return response;
    };
    auto makeFullFileResponse = [&](std::uint16_t statusCode) {
        HttpResponse response(context.resource());
        addFileHeaders(response);
        setFullFileBody(response);
        applyResponseState(response, statusCode);
        return response;
    };

    const auto contextRequest = context.req();
    const auto& request = contextRequest.raw();
    if (request.method() == HttpMethod::kGet || request.method() == HttpMethod::kHead) {
        const auto conditional = fileConditionalHeaders(request);
        if (enableValidators && !ifMatchAllows(conditional.ifMatch, etag)) {
            throw HttpError(412, "precondition_failed", "file precondition failed");
        }
        // RFC 9110 §13.2.2 step 2: If-Unmodified-Since is evaluated only when If-Match
        // is absent -- a present If-Match takes precedence and the (weaker) date
        // condition MUST be ignored, exactly as If-Modified-Since is ignored below
        // when If-None-Match is present. Without the ifMatch.empty() guard, a request
        // whose strong validator matched (If-Match ok) but whose date is older than
        // Last-Modified would draw a spurious 412.
        if (enableValidators && conditional.ifMatch.empty() &&
            !conditional.ifUnmodifiedSince.empty() &&
            !httpDateUnmodified(conditional.ifUnmodifiedSince, modifiedSeconds)) {
            throw HttpError(412, "precondition_failed", "file precondition failed");
        }

        if (enableValidators && ifNoneMatchMatches(conditional.ifNoneMatch, etag)) {
            return makeHeaderOnlyResponse(304);
        }

        if (enableValidators && conditional.ifNoneMatch.empty() &&
            !conditional.ifModifiedSince.empty() &&
            httpDateNotModified(conditional.ifModifiedSince, modifiedSeconds)) {
            return makeHeaderOnlyResponse(304);
        }

        if (enableRanges && !conditional.range.empty()) {
            if (detail::httpByteRangeSetHasMultiple(conditional.range)) {
                return makeFullFileResponse(0);
            }

            // RFC 9110 13.1.5: honor the Range only if a present If-Range matches
            // the current representation. When validators are disabled this root
            // exposes no ETag/Last-Modified, so an If-Range can never be confirmed
            // -- the condition MUST be treated as not matching and the full
            // representation served, rather than a 206 stitched from bytes the
            // client cannot verify it still holds. Gating on enableValidators (as
            // before) skipped the check entirely and returned a 206. A range with
            // no If-Range is still honored without validators.
            if (!conditional.ifRange.empty() &&
                (!enableValidators || !ifRangeAllows(conditional.ifRange, etag, modifiedSeconds))) {
                return makeFullFileResponse(0);
            }

            const auto parsedRange = detail::httpParseByteRange(conditional.range, size);
            if (parsedRange.outcome == detail::HttpRangeOutcome::kIgnore) {
                // Unknown unit or malformed range: RFC 9110 §14.2 -- ignore it and
                // serve the full representation rather than a spurious 416.
                return makeFullFileResponse(0);
            }
            if (parsedRange.outcome == detail::HttpRangeOutcome::kUnsatisfiable) {
                HttpResponse response(context.resource());
                detail::setResponseContentRangeUnsatisfied(response, size);
                addFileHeaders(response);
                applyResponseState(response, 416);
                return response;
            }

            const auto [offset, length] = parsedRange.range;
            HttpResponse response(context.resource());
            addFileHeaders(response);
            detail::setResponseContentRange(response, offset, length, size);
            setFileBody(response, offset, length);
            applyResponseState(response, 206);
            return response;
        }
    }

    return makeFullFileResponse(0);
}

}  // namespace

HttpResponse Context::file(
    const std::filesystem::path& path,
    std::string_view contentType) const {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        throw HttpError(404, "not_found", "file not found");
    }

    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw HttpError(404, "not_found", "file not found");
    }

    const auto modified = std::filesystem::last_write_time(path, ec);
    if (ec) {
        throw HttpError(404, "not_found", "file not found");
    }

    const auto applyState = [this](HttpResponse& response, std::uint16_t statusCode) {
        applyResponseState(response, statusCode, {});
    };
    return makeFileResponse(
        *this,
        FileResponsePath{.path = &path, .nativePath = path.c_str(), .borrowNativePath = false},
        static_cast<std::uint64_t>(size),
        modified,
        contentType,
        {},
        true,
        true,
        {},
        {},
        {},
        false,  // Context::file serves one path with no Accept-Encoding negotiation
        applyState);
}

// Selects the best precompressed sidecar (foo.js.br / .gz / .zst) the client
// accepts and that exists in the index — highest Accept-Encoding q-value wins,
// ties resolve br > zstd > gzip. The served bytes are the variant's, so its
// size/etag/modified describe the wire representation; the caller keeps the
// original Content-Type. Index lookups only (no per-request filesystem stat).
[[nodiscard]] bool selectStaticEncodingVariant(
    const StaticRoot& root,
    std::string_view relative,
    const HttpRequest& request,
    std::pmr::memory_resource* resource,
    detail::StaticRootEntryView& variant,
    std::string_view& contentEncoding) {
    const auto acceptEncoding =
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kAcceptEncoding);
    if (acceptEncoding.empty()) {
        return false;
    }
    detail::HttpAcceptedEncodingQuality brotli;
    detail::HttpAcceptedEncodingQuality zstd;
    detail::HttpAcceptedEncodingQuality gzip;
    detail::httpUpdateResponseCodingQualities(acceptEncoding, gzip, brotli, zstd);

    struct Candidate final {
        std::string_view suffix;
        std::string_view encoding;
        int score;
    };
    const Candidate candidates[] = {
        {".br", "br", detail::httpAcceptedEncodingScore(brotli)},
        {".zst", "zstd", detail::httpAcceptedEncodingScore(zstd)},
        {".gz", "gzip", detail::httpAcceptedEncodingScore(gzip)},
    };

    int best = 0;
    bool found = false;
    for (const auto& candidate : candidates) {
        if (candidate.score <= best) {
            continue;
        }
        std::pmr::string variantPath(resource);
        variantPath.reserve(relative.size() + candidate.suffix.size());
        variantPath.assign(relative.data(), relative.size());
        variantPath.append(candidate.suffix.data(), candidate.suffix.size());
        if (const auto entry = detail::StaticRootAccess::find(root, variantPath); entry.found()) {
            best = candidate.score;
            variant = entry;
            contentEncoding = candidate.encoding;
            found = true;
        }
    }
    return found;
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
    std::pmr::string decodedPath(allocator<char>());
    const std::string_view lookupPath =
        detail::decodeUrlComponent(relativePath, decodedPath, detail::UrlDecodeMode::kPercent)
            ? std::string_view(decodedPath)
            : relativePath;
    if (lookupPath.find('\0') != std::string_view::npos) {
        throw HttpError(403, "forbidden", "invalid static file path");
    }
    auto relative = detail::normalizeStaticRelativePath(lookupPath, allocator<char>());

    if (relative.empty() && !detail::StaticRootAccess::hasDirectoryIndex(root)) {
        throw HttpError(403, "forbidden", "invalid static file path");
    }

    auto entry = detail::StaticRootAccess::find(root, relative);
    if (!entry.found() && detail::StaticRootAccess::isIndexedDirectory(root, relative)) {
        if (!relative.empty() && relative.back() != '/') {
            relative.push_back('/');
        }
        const auto indexFile = detail::StaticRootAccess::indexFile(root);
        relative.append(indexFile.data(), indexFile.size());
        entry = detail::StaticRootAccess::find(root, relative);
    }
    if (!entry.found()) {
        throw HttpError(404, "not_found", "file not found");
    }

    // Serve a precompressed sidecar when the client accepts one; the bytes and
    // validators come from the variant, the Content-Type from the base entry.
    detail::StaticRootEntryView variant;
    std::string_view contentEncoding;
    const auto& served = selectStaticEncodingVariant(root, relative, request_, resource(), variant, contentEncoding)
        ? variant
        : entry;

    const auto applyState = [this](HttpResponse& response, std::uint16_t statusCode) {
        applyResponseState(response, statusCode, {});
    };
    return makeFileResponse(
        *this,
        FileResponsePath{.nativePath = served.filePath, .borrowNativePath = true},
        served.size,
        served.modified,
        contentType.empty() ? entry.contentType : contentType,
        entry.cacheControl,
        entry.enableRanges,
        entry.enableValidators,
        served.etag,
        served.lastModified,
        contentEncoding,
        true,  // staticFile negotiates the representation by Accept-Encoding
        applyState);
}

}  // namespace ruvia
