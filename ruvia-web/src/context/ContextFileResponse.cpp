#include <array>
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

#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/http/HttpDate.h"
#include "ruvia/http/HttpRepresentationResponsePlan.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/web/Context.h"

#include "context/ContextResponseState.h"
#include "context/ContextServices.h"
#include "http/SecureToken.h"
#include "http/StaticFileMetadata.h"
#include "http/StaticFileVariant.h"
#include "http/StaticPathNormalization.h"
#include "http/StaticRootIndex.h"
#include "server/HttpNativeFile.h"

namespace ruvia {
namespace {

inline constexpr std::size_t kFileResponseHeaderReserve = 7;

// The path travels by value until HttpResponse takes its own copy. StaticRoot is
// a public value whose lifetime is not coupled to the returned response, so an
// indexed entry must never leak its internal native-path pointer into the body.
class FileResponsePath final {
public:
    [[nodiscard]] static FileResponsePath copying(
        std::filesystem::path path, HttpResponseFileIdentity identity) {
        return FileResponsePath(std::move(path), identity);
    }

    [[nodiscard]] static FileResponsePath copyingNative(
        const ruvia::NativePathChar* path, HttpResponseFileIdentity identity) {
        if (path == nullptr || *path == ruvia::NativePathChar{}) {
            throw std::logic_error("static file entry has no native path");
        }
        return copying(std::filesystem::path(path), identity);
    }

    [[nodiscard]] std::string_view guessedContentType() const noexcept {
        return detail::guessStaticFileContentType(path_);
    }

    [[nodiscard]] HttpResponseFileIdentity identity() const noexcept {
        return identity_;
    }

    void validateCurrent(std::uint64_t size) const {
        if (!identity_.requiresValidation()) {
            return;
        }
        std::error_code ec;
        const auto snapshot = detail::snapshotResponseFile(path_.c_str(), ec);
        if (ec || snapshot.identity != identity_ || snapshot.size != size) {
            throw HttpError({.status = ruvia::http_status::kInternalServerError,
                .code = "static_file_changed",
                .message = "static file changed since its index was built"});
        }
    }

    void setBody(
        HttpResponse& response, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
        response.fileBody(takePath(), size, offset, length, identity_);
    }

    void setFullBody(HttpResponse& response, std::uint64_t size) {
        setBody(response, size, 0, size);
    }

    void set_multipart_body(HttpResponse& response, std::uint64_t size,
        http_multipart_byte_range_plan&& plan) {
        response.multipart_file_body(takePath(), size, identity_, std::move(plan));
    }

private:
    explicit FileResponsePath(
        std::filesystem::path path, HttpResponseFileIdentity identity) noexcept
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
    HttpResponseFileIdentity identity_;
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

    [[nodiscard]] HttpResponseFileIdentity identity() const noexcept {
        if (const auto* path = std::get_if<FileResponsePath>(&value_)) {
            return path->identity();
        }
        return HttpResponseFileIdentity::unchecked();
    }

    void validateCurrent(std::uint64_t size) const {
        if (const auto* path = std::get_if<FileResponsePath>(&value_)) {
            path->validateCurrent(size);
        }
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
        response.ownedBody(std::move(owned));
    }

    void setFullBody(
        HttpResponse& response, std::pmr::memory_resource* resource, std::uint64_t size) {
        setBody(response, resource, size, 0, size);
    }

    void set_multipart_body(HttpResponse& response, std::pmr::memory_resource* resource,
        std::uint64_t size, http_multipart_byte_range_plan&& plan) {
        if (auto* path = std::get_if<FileResponsePath>(&value_)) {
            path->set_multipart_body(response, size, std::move(plan));
            return;
        }
        const auto source = std::get<std::string_view>(value_);
        std::pmr::string body(resource);
        for (const auto& segment : plan.segments()) {
            if (segment.kind == http_multipart_byte_range_plan::segment_kind::file) {
                body.append(source.substr(static_cast<std::size_t>(segment.file_offset),
                    static_cast<std::size_t>(segment.file_length)));
            } else {
                body.append(plan.metadata().substr(segment.metadata_offset, segment.metadata_length));
            }
        }
        response.ownedBody(std::move(body));
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
    bool validateIndexedFileForBodylessResponse{false};
};

template <typename ApplyResponseState>
[[nodiscard]] HttpResponse makeFileResponse(const Context& context, const HttpRequest& request,
    HttpStatusCode normalStatus, FileResponseSource source, ApplyResponseState applyResponseState) {
    std::pmr::string etagStorage(context.pool());
    std::array<char, kHttpImfFixdateSize> lastModifiedStorage{};
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
    const bool hasResponseTime = responseSeconds != std::time_t{-1};
    const bool lastModifiedIsActual = hasResponseTime && source.modifiedSeconds <= responseSeconds;
    const auto validatorModifiedSeconds =
        lastModifiedIsActual ? source.modifiedSeconds : responseSeconds;
    if (emitResponseValidators) {
        if (source.precomputedEtag.empty()) {
            etagStorage = detail::makeStaticFileSnapshotEtag(
                context.pool(), source.size, source.modifiedToken, source.body.identity());
            etag = etagStorage;
        } else {
            etag = source.precomputedEtag;
        }
    }
    // Date preconditions also apply when response validators are not emitted.
    // An unrepresentable date is unavailable, not a truncated wire validator.
    if (hasResponseTime) {
        if (source.precomputedLastModified.empty() || !lastModifiedIsActual) {
            if (const auto date = formatHttpDate(validatorModifiedSeconds)) {
                lastModifiedStorage = *date;
                lastModified = std::string_view(lastModifiedStorage.data(), lastModifiedStorage.size());
            }
        } else {
            lastModified = source.precomputedLastModified;
        }
    }

    auto addFileHeaders = [&](HttpResponse& response) {
        response.reserveHeaders(kFileResponseHeaderReserve);
        if (source.contentType.empty()) {
            response.header("Content-Type", source.body.guessedContentType());
        } else {
            response.header("Content-Type", source.contentType);
        }
        if (!source.cacheControl.empty()) {
            response.header("Cache-Control", source.cacheControl);
        }
        // A precompressed variant carries the original Content-Type with the
        // encoding declared here.
        if (source.contentCoding != HttpContentCoding::kIdentity) {
            response.header("Content-Encoding", httpContentCodingToken(source.contentCoding));
        }
        if (honorRangeRequests) {
            response.header("Accept-Ranges", "bytes");
        }
        if (emitResponseValidators) {
            response.header("ETag", etag);
            if (!lastModified.empty()) {
                response.header("Last-Modified", lastModified);
            }
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
            response.addVaryToken("Accept-Encoding");
        }
    };
    auto setFileBody = [&](HttpResponse& response, std::uint64_t offset, std::uint64_t length) {
        source.body.setBody(response, context.arena(), source.size, offset, length);
    };
    auto setFullFileBody = [&](HttpResponse& response) {
        source.body.setFullBody(response, context.arena(), source.size);
    };
    // Body writes validate the indexed identity when opening the file. Responses
    // without file bytes (HEAD, 304, 412, 416) need that check here instead.
    bool indexedFileValidated = false;
    auto validateIndexedFileForBodylessResponse = [&] {
        if (source.validateIndexedFileForBodylessResponse && !indexedFileValidated) {
            source.body.validateCurrent(source.size);
            indexedFileValidated = true;
        }
    };
    auto makeHeaderOnlyResponse = [&](std::optional<HttpStatusCode> statusCode) {
        validateIndexedFileForBodylessResponse();
        HttpResponse response({.resource = context.arena()});
        addFileHeaders(response);
        applyFileResponseState(response, statusCode);
        return response;
    };
    auto makeFullFileResponse = [&](std::optional<HttpStatusCode> statusCode) {
        HttpResponse response({.resource = context.arena()});
        addFileHeaders(response);
        setFullFileBody(response);
        applyFileResponseState(response, statusCode);
        return response;
    };
    auto make_multipart_response = [&](const http_byte_range_set& ranges) {
        std::array<char, 48> boundary_token{};
        const auto token_result = detail::generateSecureToken(boundary_token);
        const auto* token = token_result.ready();
        if (token == nullptr) {
            throw std::runtime_error("secure multipart boundary generation failed");
        }
        std::pmr::string boundary(token->value(), context.arena());
        HttpResponse response({.resource = context.arena()});
        addFileHeaders(response);
        applyFileResponseState(response, http_status::kPartialContent);
        const auto media_type = response.header("Content-Type");
        const auto selected_media_type = media_type.value_or(source.body.guessedContentType());
        const auto content_encoding = source.contentCoding == HttpContentCoding::kIdentity
                                          ? std::string_view{}
                                          : httpContentCodingToken(source.contentCoding);
        auto multipart_plan = make_http_multipart_byte_range_plan(ranges, source.size,
            selected_media_type, boundary, content_encoding, context.arena());

        // The outer representation is multipart, not the encoded file bytes.
        // Carry the selected representation's coding on each part instead.
        response.header("Content-Type", multipart_plan.content_type());
        response.removeHeader("Content-Range");
        response.removeHeader("Content-Encoding");
        response.removeHeader("Content-Length");
        source.body.set_multipart_body(response, context.arena(), source.size, std::move(multipart_plan));
        return response;
    };

    if (request.knownMethod() == HttpKnownMethod::kHead) {
        validateIndexedFileForBodylessResponse();
    }
    const auto plan = planHttpRepresentationResponse(request,
        HttpSelectedRepresentationMetadata{
            .length = source.size,
            .etag = etag,
            .lastModified = lastModified.empty() ? std::nullopt : std::optional(validatorModifiedSeconds),
            .strongDateValidator = emitResponseValidators && lastModifiedIsActual && !lastModified.empty(),
        },
        HttpRepresentationResponseOptions{
            .normalStatus = normalStatus,
            .rangePolicy = honorRangeRequests ? HttpRangeRequestPolicy::honor_byte_ranges
                                              : HttpRangeRequestPolicy::kIgnore,
        });
    if (plan.preconditionFailed()) {
        validateIndexedFileForBodylessResponse();
        throw HttpError({.status = plan.status(),
            .code = "precondition_failed",
            .message = "file precondition failed"});
    }
    if (plan.notModified()) {
        return makeHeaderOnlyResponse(plan.status());
    }
    if (plan.rangeUnsatisfiable()) {
        validateIndexedFileForBodylessResponse();
        HttpResponse response({.resource = context.arena()});
        response.contentRangeUnsatisfied(source.size);
        addFileHeaders(response);
        applyFileResponseState(response, plan.status());
        return response;
    }
    if (const auto* ranges = plan.multipart_ranges()) {
        return make_multipart_response(*ranges);
    }
    if (const auto* range = plan.partial()) {
        HttpResponse response({.resource = context.arena()});
        addFileHeaders(response);
        response.contentRange(range->offset(), range->length(), source.size);
        setFileBody(response, range->offset(), range->length());
        applyFileResponseState(response, plan.status());
        return response;
    }
    return makeFullFileResponse(plan.status());
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
    return makeFileResponse(*this, request_, responseState().activeResponse().status(),
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
    const auto mode = services().precompressedStaticFiles() ? detail::StaticFileSelectionMode::kPrecompressed
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
    if (hasUrlEncoding(relativePath, UrlDecodeMode::kPercent)) {
        decodedPath = decodeUrlComponent(
            relativePath, {.mode = UrlDecodeMode::kPercent, .resource = pool()});
    }
    const std::string_view lookupPath =
        decodedPath.has_value() ? std::string_view(*decodedPath) : relativePath;
    if ((lookupPath.find('\0') != std::string_view::npos)) {
        throw HttpError({.status = ruvia::http_status::kForbidden,
            .code = "forbidden",
            .message = "invalid static file path"});
    }
    auto relative = detail::normalizeStaticRelativePath(
        lookupPath, std::pmr::polymorphic_allocator<char>(pool()));

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
        selectStaticFileRepresentation(root, relative, request_, pool(), baseEntry, mode);
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
    return makeFileResponse(*this, request_, responseState().activeResponse().status(),
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
            .validateIndexedFileForBodylessResponse = memoryVariant == nullptr,
        },
        applyState);
}

}  // namespace ruvia
