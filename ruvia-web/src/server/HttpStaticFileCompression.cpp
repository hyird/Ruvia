#include "ruvia/web/detail/server/response/HttpStaticFileCompression.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/response/HttpResponseFileBody.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/http/detail/response/ResponseHeaderUtils.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/web/detail/server/file/HttpFileOpen.h"
#include "ruvia/web/detail/server/response/HttpResponseCompression.h"

namespace ruvia::detail {
namespace {

struct StaticFileCompressionJob final {
    std::filesystem::path path;
    std::uint64_t size{0};
    std::uint64_t offset{0};
    std::uint64_t length{0};
    ResponseFileIdentity identity{ResponseFileIdentity::unchecked()};
    HttpContentCoding coding{HttpContentCoding::kIdentity};
};

enum class StaticFileCompressionAttemptStatus : std::uint8_t {
    kCompressed,
    kNotApplicable,
    kFailed,
};

struct StaticFileCompressionAttempt final {
    explicit StaticFileCompressionAttempt(StaticFileCompressionAttemptStatus status, std::pmr::memory_resource* resource) noexcept
        : status(status),
          bytes(resource) {}

    StaticFileCompressionAttempt(StaticFileCompressionAttemptStatus status, std::pmr::string bytes) noexcept
        : status(status),
          bytes(std::move(bytes)) {}

    [[nodiscard]] bool compressed() const noexcept {
        return status == StaticFileCompressionAttemptStatus::kCompressed;
    }

    [[nodiscard]] bool notApplicable() const noexcept {
        return status == StaticFileCompressionAttemptStatus::kNotApplicable;
    }

    [[nodiscard]] bool failed() const noexcept {
        return status == StaticFileCompressionAttemptStatus::kFailed;
    }

    StaticFileCompressionAttemptStatus status;
    std::pmr::string bytes;
};

[[nodiscard]] StaticFileCompressionAttempt readAndCompress(StaticFileCompressionJob job) {
    // This job outlives the request coroutine and runs on a foreign thread.
    // Keep both the input snapshot and the result on the process-lifetime PMR;
    // the request resource is worker-owned and must never cross this boundary.
    auto* const resource = processResource();
    if (job.length > (std::numeric_limits<std::size_t>::max)() || job.length > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
        return StaticFileCompressionAttempt(StaticFileCompressionAttemptStatus::kFailed, resource);
    }

    const auto descriptor = ResponseFileBodyAccess::make(job.path.c_str(), job.size, job.offset, job.length, job.identity);
    auto input = openResponseFileInput(descriptor);
    if (!input) {
        return StaticFileCompressionAttempt(StaticFileCompressionAttemptStatus::kFailed, resource);
    }
    input.seekg(static_cast<std::streamoff>(job.offset), std::ios::beg);
    if (!input) {
        return StaticFileCompressionAttempt(StaticFileCompressionAttemptStatus::kFailed, resource);
    }

    std::pmr::string plain(static_cast<std::size_t>(job.length), '\0', resource);
    std::size_t offset = 0;
    while (offset < plain.size()) {
        input.read(plain.data() + offset, static_cast<std::streamsize>(plain.size() - offset));
        const auto readBytes = input.gcount();
        if (readBytes <= 0) {
            return StaticFileCompressionAttempt(StaticFileCompressionAttemptStatus::kFailed, resource);
        }
        offset += static_cast<std::size_t>(readBytes);
    }

    // The descriptor remains valid when a writer edits the file in place. Do
    // not publish compressed bytes from a changed read under the old ETag and
    // Last-Modified snapshot; the caller will keep the original file response
    // or surface the typed failure according to coding policy.
    if (!input.matchesSnapshot(job.identity, job.size)) {
        return StaticFileCompressionAttempt(StaticFileCompressionAttemptStatus::kFailed, resource);
    }

    // An empty representation is valid. It cannot have a strictly smaller
    // content-coding, so use zero as the bound instead of rejecting it here or
    // underflowing the size limit.
    const auto maxEncodedBytes = plain.empty() ? 0 : plain.size() - 1;
    auto encoded = encodeHttpContent(job.coding, plain, maxEncodedBytes, resource);
    if (auto* content = encoded.encoded(); content != nullptr) {
        auto bytes = std::move(*content).takeBytes();
        if (bytes.empty()) {
            return StaticFileCompressionAttempt(StaticFileCompressionAttemptStatus::kFailed, std::move(bytes));
        }
        return StaticFileCompressionAttempt(StaticFileCompressionAttemptStatus::kCompressed, std::move(bytes));
    }

    const auto* failure = encoded.failure();
    if (failure != nullptr && failure->error() == HttpContentEncodeError::kEncodedSizeExceeded) {
        // A valid representation that is not smaller than the file is a
        // compression policy miss, not an internal serving failure. The
        // caller can keep the original zero-copy file body or map it to 406
        // when identity is forbidden.
        return StaticFileCompressionAttempt(StaticFileCompressionAttemptStatus::kNotApplicable, resource);
    }
    return StaticFileCompressionAttempt(StaticFileCompressionAttemptStatus::kFailed, resource);
}

}  // namespace

Task<HttpStaticFileCompressionResult> tryCompressStaticFileResponse(HttpResponse& response, const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod, CompressionConfig compression, std::size_t maxBytes, BlockingPool* pool, const WorkerHandle& worker) {
    if (pool == nullptr || maxBytes == 0 || selection.coding() == HttpContentCoding::kIdentity) {
        co_return HttpStaticFileCompressionResult(HttpStaticFileCompressionStatus::kNotApplicable);
    }
    // All checks below this point are either file metadata or compression
    // policy. Keep them before the blocking boundary: a request that cannot be
    // transformed must never occupy a pool thread just to discover that fact.
    if (httpResponseCompressionEligibility(selection, requestMethod, response, ResponseStreamKind::kGeneric) != HttpResponseCompressionEligibility::kEligible) {
        co_return HttpStaticFileCompressionResult(HttpStaticFileCompressionStatus::kNotApplicable);
    }
    const auto file = responseBody(response).file();
    if (!file.has_value() || file->offset() != 0 || file->length() != file->size() || file->length() < compression.minBytes || file->length() > maxBytes || responseHasKnownHeader(response, kResponseHeaderContentEncoding) || responseHasKnownHeader(response, kResponseHeaderContentRange)) {
        co_return HttpStaticFileCompressionResult(HttpStaticFileCompressionStatus::kNotApplicable);
    }

    try {
        auto result = co_await tryRunBlocking(*pool, worker, [job = StaticFileCompressionJob{file->toPath(), file->size(), file->offset(), file->length(), file->identity(), selection.coding()}]() mutable { return readAndCompress(std::move(job)); });
        if (result.failed()) {
            co_return HttpStaticFileCompressionResult(HttpStaticFileCompressionStatus::kFailed);
        }
        if (!result.completed()) {
            co_return HttpStaticFileCompressionResult(HttpStaticFileCompressionStatus::kUnavailable);
        }
        auto attempt = std::move(result).value();
        if (attempt.notApplicable()) {
            co_return HttpStaticFileCompressionResult(HttpStaticFileCompressionStatus::kNotApplicable);
        }
        if (attempt.failed() || !attempt.compressed() || attempt.bytes.empty()) {
            co_return HttpStaticFileCompressionResult(HttpStaticFileCompressionStatus::kFailed);
        }
        try {
            addVaryToken(response, "Accept-Encoding");
            replaceResponseBodyWithContentEncoding(response, std::move(attempt.bytes), httpContentCodingToken(selection.coding()));
        } catch (...) {
            // Keep the original file response intact when request-resource
            // allocation fails while preparing the compressed representation.
            // The caller can then map the typed failure to 500/406 without
            // sending a file body under compressed metadata.
            co_return HttpStaticFileCompressionResult(HttpStaticFileCompressionStatus::kFailed);
        }
        co_return HttpStaticFileCompressionResult(HttpStaticFileCompressionStatus::kCompressed);
    } catch (...) {
        // tryRunBlocking normally carries pool rejection and callable failures in
        // its result. Its one-shot setup/result transport can still throw (for
        // example on process-resource exhaustion); do not let that tear down
        // the request/session when the identity file is still available.
        co_return HttpStaticFileCompressionResult(HttpStaticFileCompressionStatus::kUnavailable);
    }
}

}  // namespace ruvia::detail
