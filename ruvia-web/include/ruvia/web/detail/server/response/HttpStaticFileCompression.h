#pragma once

#include <cstddef>
#include <cstdint>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/coding/HttpAcceptEncoding.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/server/response/HttpResponseCompression.h"
#include "ruvia/web/ServerConfig.h"

namespace ruvia {
class HttpResponse;
}

namespace ruvia::detail {

// A boolean cannot describe the terminal state of deferred static-file
// compression: the response may be intentionally left as identity, the
// blocking pool may reject the work, the file/encoder may fail, or the body may
// actually be replaced by compressed bytes. These outcomes have different HTTP
// policy consequences when the client forbids identity.
enum class HttpStaticFileCompressionStatus : std::uint8_t {
    kCompressed,
    kNotApplicable,
    kUnavailable,
    kFailed,
};

class HttpStaticFileCompressionResult final {
public:
    [[nodiscard]] HttpStaticFileCompressionStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool compressed() const noexcept {
        return status_ == HttpStaticFileCompressionStatus::kCompressed;
    }

    [[nodiscard]] bool notApplicable() const noexcept {
        return status_ == HttpStaticFileCompressionStatus::kNotApplicable;
    }

    [[nodiscard]] bool unavailable() const noexcept {
        return status_ == HttpStaticFileCompressionStatus::kUnavailable;
    }

    [[nodiscard]] bool failed() const noexcept {
        return status_ == HttpStaticFileCompressionStatus::kFailed;
    }

private:
    friend Task<HttpStaticFileCompressionResult> tryCompressStaticFileResponse(HttpResponse&, const HttpResponseCodingSelection&, HttpKnownMethod, CompressionConfig, std::size_t, BlockingPool*, const WorkerHandle&);

    explicit constexpr HttpStaticFileCompressionResult(HttpStaticFileCompressionStatus status) noexcept
        : status_(status) {}

    HttpStaticFileCompressionStatus status_;
};

// Convert only after the caller has confirmed that identity is forbidden. A
// policy miss is a client-facing 406; pool capacity/lifecycle is a temporary
// 503; a file or encoder failure is a server-side 500.
[[nodiscard]] inline HttpErrorInfo httpStaticFileCompressionError(const HttpStaticFileCompressionResult& result) noexcept {
    switch (result.status()) {
        case HttpStaticFileCompressionStatus::kNotApplicable:
            return HttpErrorInfo(ruvia::http_status::kNotAcceptable, "not_acceptable", "no acceptable response content coding");
        case HttpStaticFileCompressionStatus::kUnavailable:
            return HttpErrorInfo(ruvia::http_status::kServiceUnavailable, "service_unavailable", "static response compression is temporarily unavailable");
        case HttpStaticFileCompressionStatus::kFailed:
            return HttpErrorInfo(ruvia::http_status::kInternalServerError, "static_response_compression_failed", "static response compression failed");
        case HttpStaticFileCompressionStatus::kCompressed:
            return HttpErrorInfo(ruvia::http_status::kInternalServerError, "invalid_static_compression_outcome", "static response compression returned an invalid error outcome");
    }
    return HttpErrorInfo(ruvia::http_status::kInternalServerError, "invalid_static_compression_outcome", "static response compression returned an unknown outcome");
}

// Compress a small full static-file response when no precompressed sidecar was
// selected. The server compression minBytes threshold is applied before any
// blocking work. File I/O and whole-file encoding run on the process blocking
// pool; larger files, ranges, pool rejection, stale identities, and
// incompressible results keep the original zero-copy file response.
[[nodiscard]] Task<HttpStaticFileCompressionResult> tryCompressStaticFileResponse(HttpResponse& response, const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod, CompressionConfig compression, std::size_t maxBytes, BlockingPool* pool, const WorkerHandle& worker);

}  // namespace ruvia::detail
