#include "ruvia/http/detail/server/HttpResponseHead.h"

#include <charconv>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "ruvia/http/detail/server/HttpDateCache.h"
#include "ruvia/http/detail/HttpContentLength.h"
#include "ruvia/http/detail/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia::detail {

namespace {

struct ResponseHeadFlags {
    HttpProtocolVersion protocolVersion{HttpProtocolVersion::kHttp11};
    bool emitChunkedTransferEncoding{false};
    bool emitContentLength{false};
    std::uint64_t canonicalContentLength{0};
};

inline constexpr std::string_view kChunkedTransferEncodingHeader =
    "Transfer-Encoding: chunked\r\n";

[[nodiscard]] std::optional<std::uint64_t> explicitContentLength(
    const HttpResponse& response) {
    // A kOk parseField always populates the state's value, and a Content-Length
    // that fails to parse throws below, so the accumulated optional already
    // encodes presence: empty means no Content-Length line was seen.
    HttpContentLengthState state;
    for (const auto& header : response.headers()) {
        if (responseHeaderKnownBit(header) != kResponseHeaderContentLength) {
            continue;
        }
        if (state.parseField(header.value()) !=
            HttpContentLengthParseStatus::kOk) {
            throw std::invalid_argument(
                "invalid explicit HTTP response Content-Length");
        }
    }
    const auto value = state.value();
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(*value);
}

// Unchecked sink writing through a raw cursor; the caller guarantees capacity
// via ResponseHeadBuffer::stackCursor. Constant-size appends inline to stores.
struct RawHeadSink {
    char* out;

    void append(std::string_view value) noexcept {
        // A default-constructed empty string_view may carry a null data pointer.
        // libc annotates memcpy arguments as nonnull even when the byte count is
        // zero, so avoid passing that representation across the C boundary.
        if (value.empty()) {
            return;
        }
        std::memcpy(out, value.data(), value.size());
        out += value.size();
    }

    void append(char value) noexcept {
        *out++ = value;
    }

    void appendUnsigned(std::uint64_t value) noexcept {
        out = std::to_chars(out, out + 20, value).ptr;
    }
};

void addResponseHeadBytes(std::size_t& total, std::size_t bytes) {
    if (bytes > kMaxHttpHeaderBytes - total) {
        throw std::length_error("HTTP response head is too large");
    }
    total += bytes;
}

[[nodiscard]] std::size_t decimalDigits(std::uint64_t value) noexcept {
    std::size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

template <typename Sink>
void emitResponseHead(
    const HttpResponse& response,
    Sink& sink,
    HttpStatusCode responseStatus,
    std::string_view reasonPhrase,
    std::string_view dateHeader,
    ResponseHeadFlags flags) {
    sink.append(
        flags.protocolVersion == HttpProtocolVersion::kHttp10
            ? std::string_view("HTTP/1.0 ")
            : std::string_view("HTTP/1.1 "));
    const auto statusToken = httpStatusCodeToken(responseStatus);
    sink.append(httpStatusCodeTokenView(statusToken));
    // RFC 9112 requires this SP even when the optional reason phrase is empty.
    sink.append(' ');
    sink.append(reasonPhrase);
    sink.append(std::string_view("\r\n"));

    for (const auto& header : response.headers()) {
        const auto knownBit = responseHeaderKnownBit(header);
        // HTTP/1 framing belongs exclusively to Http1ResponseHeadPlan. A handler
        // cannot override canonical chunked framing, invent Transfer-Encoding on
        // an HTTP/1.0 response, or attach Content-Length to a body-open
        // close-delimited stream.
        if (knownBit == kResponseHeaderTransferEncoding ||
            knownBit == kResponseHeaderContentLength) {
            continue;
        }
        sink.append(header.name());
        sink.append(std::string_view(": "));
        sink.append(header.value());
        sink.append(std::string_view("\r\n"));
    }

    const auto knownBits = responseKnownHeaderBits(response);
    if ((knownBits & kResponseHeaderDate) == 0) {
        sink.append(dateHeader);
    }
    if (flags.emitChunkedTransferEncoding) {
        sink.append(kChunkedTransferEncodingHeader);
    }
    if (flags.emitContentLength) {
        sink.append(std::string_view("Content-Length: "));
        sink.appendUnsigned(flags.canonicalContentLength);
        sink.append(std::string_view("\r\n"));
    }
    sink.append(std::string_view("\r\n"));
}

}  // namespace

void appendResponseHead(
    const HttpResponse& response,
    ResponseHeadBuffer& head,
    const Http1ResponseHeadPlan& plan) {
    const auto bodyPlan = plan.bodyPlan();
    if (response.status() != bodyPlan.responseStatus()) {
        throw std::invalid_argument(
            "HTTP/1 response plan status does not match response");
    }
    const auto* buffered = plan.buffered();
    if (buffered != nullptr &&
        buffered->contentLength() !=
            bodyPlan.bufferedRepresentationLength(response)) {
        throw std::invalid_argument(
            "HTTP/1 response plan representation does not match response");
    }
    const auto responseStatus = bodyPlan.responseStatus();
    const auto policy = bodyPlan.policy();
    const bool emitChunkedTransferEncoding =
        plan.chunkedStream() != nullptr && policy.transferEncodingAllowed();
    const bool autoContentLengthOwnedByWriter =
        policy.autoContentLengthAllowed() &&
        !emitChunkedTransferEncoding &&
        (plan.buffered() != nullptr || !policy.bodyAllowed());
    const bool explicitContentLengthAllowed =
        policy.explicitContentLengthAllowed() &&
        !emitChunkedTransferEncoding &&
        !autoContentLengthOwnedByWriter &&
        (plan.closeDelimitedStream() == nullptr || bodyPlan.bodySuppressed());
    const auto knownBits = responseKnownHeaderBits(response);
    const auto declaredContentLength =
        explicitContentLengthAllowed &&
            (knownBits & kResponseHeaderContentLength) != 0
        ? explicitContentLength(response)
        : std::nullopt;
    const ResponseHeadFlags flags{
        .protocolVersion = plan.protocolVersion(),
        .emitChunkedTransferEncoding = emitChunkedTransferEncoding,
        .emitContentLength =
            autoContentLengthOwnedByWriter ||
            declaredContentLength.has_value(),
        // Buffered HEAD metadata retains the selected representation length.
        // A status-level no-content policy that still owns framing (205) is
        // canonicalized to zero for both buffered and streaming heads.
        .canonicalContentLength = declaredContentLength.value_or(
            buffered != nullptr && policy.bodyAllowed()
                ? buffered->contentLength()
                : std::uint64_t{0})};

    const auto reasonPhrase = httpReasonPhrase(responseStatus);
    const auto dateHeader = cachedDateHeader();

    // Measure the exact emitted head before touching reusable output storage.
    // This both bounds the unchecked raw stack sink and enforces the same 64 KiB
    // field-section ceiling used by request and HTTP/2 paths.
    std::size_t headBytes = 9;
    addResponseHeadBytes(headBytes, kHttpStatusCodeTokenSize);
    addResponseHeadBytes(headBytes, 1);
    addResponseHeadBytes(headBytes, reasonPhrase.size());
    addResponseHeadBytes(headBytes, 2);
    std::size_t fieldCount = 0;
    for (const auto& header : response.headers()) {
        const auto knownBit = responseHeaderKnownBit(header);
        if (knownBit == kResponseHeaderTransferEncoding ||
            knownBit == kResponseHeaderContentLength) {
            continue;
        }
        ++fieldCount;
        addResponseHeadBytes(headBytes, header.name().size());
        addResponseHeadBytes(headBytes, header.value().size());
        addResponseHeadBytes(headBytes, 4);
    }
    if ((knownBits & kResponseHeaderDate) == 0) {
        ++fieldCount;
        addResponseHeadBytes(headBytes, dateHeader.size());
    }
    if (emitChunkedTransferEncoding) {
        ++fieldCount;
        addResponseHeadBytes(
            headBytes, kChunkedTransferEncodingHeader.size());
    }
    if (flags.emitContentLength) {
        ++fieldCount;
        addResponseHeadBytes(headBytes, 16);
        addResponseHeadBytes(
            headBytes, decimalDigits(flags.canonicalContentLength));
        addResponseHeadBytes(headBytes, 2);
    }
    if (fieldCount > kMaxHttpHeaderFields) {
        throw std::length_error("too many HTTP response headers");
    }
    addResponseHeadBytes(headBytes, 2);

    if (char* cursor = head.stackCursor(headBytes); cursor != nullptr) {
        RawHeadSink sink{cursor};
        emitResponseHead(
            response,
            sink,
            responseStatus,
            reasonPhrase,
            dateHeader,
            flags);
        head.commitStack(sink.out);
        return;
    }
    head.reserveAdditional(headBytes);
    emitResponseHead(
        response,
        head,
        responseStatus,
        reasonPhrase,
        dateHeader,
        flags);
}

}  // namespace ruvia::detail
