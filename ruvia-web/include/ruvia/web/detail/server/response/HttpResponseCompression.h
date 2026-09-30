#pragma once

#include <cstdint>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/http/HttpAcceptEncoding.h"
#include "ruvia/http/HttpFieldValues.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpResponseServer.h"
#include "ruvia/web/ServerConfig.h"

namespace ruvia::detail {

[[nodiscard]] inline bool responseHasHeaderName(
    const HttpResponse& response, std::string_view name) noexcept {
    for (const auto& header : response.headers()) {
        if (httpAsciiEqualsIgnoreCase(header.name(), name)) {
            return true;
        }
    }
    return false;
}

// Runtime capability is deliberately separate from Accept-Encoding
// negotiation. A precompressed file sidecar can satisfy a selected coding
// without this process owning an incremental encoder.
enum class HttpResponseCodingAvailability : std::uint8_t {
    kIdentityOnly,
    kIdentityAndCompression,
};

// The representation source affects compression eligibility (notably SSE's
// media type), while static-file representations remain owned by the static
// response path.
enum class HttpResponseCompressionSource : std::uint8_t {
    kBuffered,
    kStream,
    kSse,
};

enum class HttpResponseCompressionDecision : std::uint8_t {
    kFixedRepresentation,
    kNegotiatedIdentity,
    kEncode,
};

// Compression policy and encoder execution are separate outcomes. A response
// may intentionally remain identity (body too small, no-transform, an
// incompressible media type, or an unsupported runtime capability), or the
// encoder may actually fail. Those cases have different meanings when the
// client has forbidden identity and must not collapse back to a bool.
enum class HttpResponseCompressionStatus : std::uint8_t {
    kCompressed,
    kNotApplicable,
    kFailed,
};

class HttpResponseCompressionResult final {
public:
    [[nodiscard]] static constexpr HttpResponseCompressionResult makeCompressed() noexcept {
        return HttpResponseCompressionResult(HttpResponseCompressionStatus::kCompressed);
    }

    [[nodiscard]] static constexpr HttpResponseCompressionResult makeNotApplicable() noexcept {
        return HttpResponseCompressionResult(HttpResponseCompressionStatus::kNotApplicable);
    }

    [[nodiscard]] static constexpr HttpResponseCompressionResult makeFailed() noexcept {
        return HttpResponseCompressionResult(HttpResponseCompressionStatus::kFailed);
    }

    [[nodiscard]] constexpr HttpResponseCompressionStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr bool compressed() const noexcept {
        return status_ == HttpResponseCompressionStatus::kCompressed;
    }

    [[nodiscard]] constexpr bool notApplicable() const noexcept {
        return status_ == HttpResponseCompressionStatus::kNotApplicable;
    }

    [[nodiscard]] constexpr bool failed() const noexcept {
        return status_ == HttpResponseCompressionStatus::kFailed;
    }

private:
    constexpr explicit HttpResponseCompressionResult(HttpResponseCompressionStatus status) noexcept
        : status_(status) {}

    HttpResponseCompressionStatus status_;
};

// The protocol parser deliberately reports a coding stack as unsupported when
// it contains more than one member, even when every member is a coding Ruvia
// knows. That is the right result for decoding (the runtime owns no stack
// decoder), but response negotiation still has to inspect a known stack: a
// client that accepts gzip but rejects br cannot be sent `gzip, br` merely
// because the stack is not executable by this process.
[[nodiscard]] inline bool httpKnownResponseContentEncodingStackAccepted(
    const HttpResponseCodingSelection& selection, const HttpResponse& response) noexcept {
    bool sawKnownCoding = false;
    bool sawUnknownCoding = false;
    bool accepted = true;
    for (const auto& header : response.headers()) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "Content-Encoding")) {
            continue;
        }
        httpVisitCommaSeparatedQuotedFieldItems(header.value(), [&](std::string_view item) noexcept {
            if (item.empty()) {
                sawUnknownCoding = true;
            } else if (httpAsciiEqualsIgnoreCase(item, "gzip") ||
                       httpAsciiEqualsIgnoreCase(item, "x-gzip")) {
                sawKnownCoding = true;
                accepted = accepted && selection.accepts(HttpContentCoding::kGzip);
            } else if (httpAsciiEqualsIgnoreCase(item, "br")) {
                sawKnownCoding = true;
                accepted = accepted && selection.accepts(HttpContentCoding::kBrotli);
            } else if (httpAsciiEqualsIgnoreCase(item, "zstd")) {
                sawKnownCoding = true;
                accepted = accepted && selection.accepts(HttpContentCoding::kZstd);
            } else if (httpAsciiEqualsIgnoreCase(item, "identity")) {
                sawKnownCoding = true;
                accepted = accepted && selection.accepts(HttpContentCoding::kIdentity);
            } else {
                // A custom coding (or malformed sender-side list member) is
                // outside the framework's decoder/registry. Leave that full
                // stack under application ownership instead of guessing.
                sawUnknownCoding = true;
            }
            return true;
        });
    }
    return !sawKnownCoding || sawUnknownCoding || accepted;
}

// Return true when the response cannot satisfy the client's coding policy:
// either a framework-selected coding fell back to forbidden identity, or a
// handler supplied a known pre-encoded representation the client excluded.
// Bodyless statuses are representation-free and therefore never need a
// content-coding fallback.
[[nodiscard]] inline bool httpResponseCodingFallbackForbidden(
    const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod,
    const HttpResponse& response) noexcept {
    if (!planHttpResponseBody(requestMethod, response.status()).statusAllowsBody()) {
        return false;
    }
    if (responseHasHeaderName(response, "Content-Encoding")) {
        // An already-encoded response is a valid representation source, but it
        // still cannot bypass Accept-Encoding. Ruvia can classify a single
        // coding and a stack made entirely from its known codings without
        // taking ownership of arbitrary application coding registries; stacks
        // containing a custom coding remain application-managed.
        const auto contentCoding = parseHttpContentCodingHeaders(response.headers());
        if (const auto* coding = contentCoding.coding(); coding != nullptr) {
            return !selection.accepts(*coding);
        }
        if (contentCoding.unsupported() != nullptr) {
            return !httpKnownResponseContentEncodingStackAccepted(selection, response);
        }
        return false;
    }
    if (selection.coding() == HttpContentCoding::kIdentity || selection.identityAccepted()) {
        return false;
    }
    // This guard targets only the framework's silent identity fallback.
    return true;
}

[[nodiscard]] HttpResponseCompressionResult applyResponseCompression(
    const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod,
    HttpResponse& response, const CompressionConfig& options);

// Applies the same policy as applyResponseCompression(), but offloads an
// in-memory body above syncBytes to the bounded blocking pool. A disabled pool
// extends synchronous compression through maxBytes; rejection by an existing
// pool is an intentional identity fallback. Encoder/commit failures remain
// typed failures so a client that forbids identity receives a terminal error.
[[nodiscard]] Task<HttpResponseCompressionResult> applyResponseCompressionAsync(
    const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod,
    HttpResponse& response, CompressionConfig options, BlockingPool* pool,
    const WorkerHandle& worker);

// Shared Web response policy. Fixed representations do not vary by coding;
// eligible representations are annotated before deciding between identity
// and an encoder. Encoder availability never changes Accept-Encoding parsing.
[[nodiscard]] HttpResponseCompressionDecision prepareResponseCompression(
    const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod,
    HttpResponse& response, HttpResponseCompressionSource source,
    HttpResponseCodingAvailability availability);

}  // namespace ruvia::detail
