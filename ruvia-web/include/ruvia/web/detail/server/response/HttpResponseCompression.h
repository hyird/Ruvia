#pragma once

#include <cstdint>

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/coding/HttpAcceptEncoding.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/ServerConfig.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

// Runtime capability is deliberately separate from Accept-Encoding
// negotiation. A precompressed file sidecar can satisfy a selected coding
// without this process owning an incremental encoder.
enum class HttpResponseCodingAvailability : std::uint8_t {
    kIdentityOnly,
    kIdentityAndCompression,
};

// Pure response-policy result. It deliberately does not inspect the body
// alternative: a static file has a separate deferred path, while buffered and
// streaming responses share the same metadata policy. Identity is considered
// eligible here when the representation may vary; callers that need to create
// an encoder must still reject identity explicitly.
enum class HttpResponseCompressionEligibility : std::uint8_t {
    kIneligible,
    kEligible,
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
[[nodiscard]] inline bool httpKnownResponseContentEncodingStackAccepted(const HttpResponseCodingSelection& selection, const HttpResponse& response) noexcept {
    bool sawKnownCoding = false;
    bool sawUnknownCoding = false;
    bool accepted = true;
    for (const auto& header : response.headers()) {
        if (responseHeaderKnownBit(header) != kResponseHeaderContentEncoding) {
            continue;
        }
        httpVisitCommaSeparatedQuotedItems(header.value(), [&](std::string_view item) noexcept {
            if (item.empty()) {
                sawUnknownCoding = true;
            } else if (httpAsciiEqualsIgnoreCase(item, "gzip") || httpAsciiEqualsIgnoreCase(item, "x-gzip")) {
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
[[nodiscard]] inline bool httpResponseCodingFallbackForbidden(const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod, const HttpResponse& response) noexcept {
    if (!httpResponseBodyPlan(requestMethod, response.status()).statusAllowsBody()) {
        return false;
    }
    if (responseHasKnownHeader(response, kResponseHeaderContentEncoding)) {
        // An already-encoded response is a valid representation source, but it
        // still cannot bypass Accept-Encoding. Ruvia can classify a single
        // coding and a stack made entirely from its known codings without
        // taking ownership of arbitrary application coding registries; stacks
        // containing a custom coding remain application-managed.
        const auto contentCoding = httpContentCodingFromHeaders(response.headers());
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

[[nodiscard]] HttpResponseCompressionResult applyResponseCompression(const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod, HttpResponse& response, const CompressionConfig& options);

[[nodiscard]] HttpResponseCompressionEligibility httpResponseCompressionEligibility(const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod, const HttpResponse& response, ResponseStreamKind kind) noexcept;

// Select and annotate a streaming representation before the protocol-owned
// stream head is committed. The returned value tells the runtime whether it
// should own an incremental encoder for the body bytes.
[[nodiscard]] bool prepareStreamingResponseCompression(const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod, HttpResponse& response, ResponseStreamKind kind);

}  // namespace ruvia::detail
